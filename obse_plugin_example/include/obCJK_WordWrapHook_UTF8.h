#pragma once
// UTF-8 counterpart of obCJK_WordWrapHook.h — hooks the identical native VAs
// (same sub_577C10/sub_578960/sub_575B40 code, see that file's header
// comment for full derivation) but decodes variable-length UTF-8 sequences
// instead of assuming a fixed 2-byte DBCS lead/trail pair. Per the
// 2026-07-18 architecture decision (memory obcjk-utf8-plan), this file shares NO logic with the DBCS
// version — every symbol below is independently named (Utf8 suffix) so both
// headers can be compiled into the same translation unit (main.cpp) and the
// caller picks which set of Install*HookUtf8()/Install*Hook() functions to
// call at runtime based on the ini's utf8-mode switch. Only one set is ever
// actually installed (patches real game-code bytes), so the two files' hooks
// never run against each other.
//
// UTF-8 structurally simplifies several of the DBCS disambiguation tricks:
// continuation bytes (0x80-0xBF) never overlap any lead-byte range
// (0xC2-0xDF/0xE0-0xEF/0xF0-0xF4) or the ASCII range (0x00-0x7F), so a
// byte's own value unambiguously tells you whether it's a sequence start,
// mid-sequence, or plain ASCII — no odd/even "run length" parity walk is
// needed the way DBCS's overlapping lead/trail ranges require (see
// obCJK_WordWrapHook.h's ObCJKLineSplitPairIsGenuineLead /
// ObCJKIsGenuineLeadAtIdx).
//
// KNOWN GAP (documented at the relevant hook below, not silently skipped):
// the fourth/sixth DBCS hooks (TrailByteGuard/TrailByteGuard2, protecting a
// misidentified '~' hotkey marker) have no UTF-8 counterpart — 0x7E is
// always plain ASCII in UTF-8 (can never be a continuation byte), so
// native's own '~'-strip is already correct and needs no override. The
// seventh hook (LineSplitPair) only protects 2-byte UTF-8 sequences for a
// structural reason explained at that hook.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_GlyphAtlas.h"
// Needed only for ObCJKFontIDFromInfoPtr() (pure FontInfo*->fontID pointer
// arithmetic, no encoding logic) — same reuse obCJK_WordWrapHook.h makes of
// this file. Not a violation of the "no shared code" decision: that decision
// covers the per-encoding DISAMBIGUATION logic, not encoding-agnostic
// utilities. If obCJK_GlyphHook_UTF8.h changes this helper's shape later,
// revisit this include.
#include "obCJK_GlyphHook.h"

// --- UTF-8 decode helpers, local to this file ------------------------------

// Validates and returns the full sequence length starting at buf[idx]
// (2/3/4), or 0 if buf[idx] isn't a multi-byte lead or the following
// continuation bytes are missing/malformed/truncated. Mirrors the DBCS
// files' "trailOk" pattern: callers fall back to native single-byte handling
// on 0, same as a failed DBCS lead/trail check. Safe against running past a
// NUL terminator: NUL (0x00) never satisfies ObCJKIsUtf8Continuation
// (0x80-0xBF), so a truncated sequence at the buffer end always fails here
// rather than reading past the terminator.
static int ObCJKUtf8ValidSeqLenAt(const BYTE* buf, DWORD idx)
{
    BYTE lead = buf[idx];
    int len = ObCJKUtf8SeqLen(lead);
    if (len <= 1) return 0;  // ASCII (handled elsewhere) or not a valid lead byte
    for (int i = 1; i < len; i++) {
        if (!ObCJKIsUtf8Continuation(buf[idx + i])) return 0;
    }
    return len;
}

// Decodes a length-len UTF-8 sequence starting at buf[idx] (already
// validated by ObCJKUtf8ValidSeqLenAt) into its Unicode codepoint.
static DWORD ObCJKUtf8Decode(const BYTE* buf, DWORD idx, int len)
{
    static const BYTE kLeadMask[5] = { 0, 0x7F, 0x1F, 0x0F, 0x07 };  // indexed by len
    DWORD cp = buf[idx] & kLeadMask[len];
    for (int i = 1; i < len; i++) cp = (cp << 6) | (buf[idx + i] & 0x3F);
    return cp;
}

// Glyph-atlas lookup key for a decoded codepoint. [2026-07-18] obCJK_GlyphAtlas.h's
// ObCJKGlyphAtlas_GetGlyph now calls GetGlyphOutlineW (not GetGlyphOutlineA)
// under kCP_UTF8, so BMP-range codepoints (U+0000-U+FFFF — every CJK
// Unified Ideograph, U+4E00-U+9FFF, lands here) rasterize correctly (Layer 4
// landed). The flat glyph table is still
// WORD-indexed though, so codepoints above U+FFFF (4-byte sequences, astral
// planes) still truncate here — extremely rare in game text; same "falls
// through, never worse than leaving it unmapped" fallback philosophy as
// every other unmapped case in this file.
static inline WORD ObCJKUtf8CodeForGlyph(DWORD codepoint)
{
    return (WORD)(codepoint & 0xFFFF);
}

// Finds the lead byte of the sequence that srcPtr[idx] is a continuation
// byte of, if any. Unlike DBCS (where a byte's lead/trail role is only
// resolvable by parity-walking, since the ranges overlap), a continuation
// byte can ONLY ever be mid/end-of-sequence — so this simply walks backward
// through the continuation run (bounded to 3 steps, the max continuation
// bytes in any valid sequence) and validates the byte where the run breaks
// is a lead byte whose declared length actually reaches idx. Returns false
// (not found) if idx isn't a continuation byte, the run exceeds 3 bytes, or
// the byte before the run isn't a valid covering lead byte.
static bool ObCJKUtf8FindGenuineLeadForIdx(const BYTE* srcPtr, DWORD idx, DWORD* outLeadIdx, int* outSeqLen)
{
    if (!ObCJKIsUtf8Continuation(srcPtr[idx])) return false;
    for (DWORD k = 1; k <= 3 && k <= idx; k++) {
        BYTE candidate = srcPtr[idx - k];
        if (ObCJKIsUtf8Continuation(candidate)) continue;  // still inside the run, keep walking back
        int seqLen = ObCJKUtf8SeqLen(candidate);
        if (seqLen >= 2 && (DWORD)(seqLen - 1) >= k) {
            *outLeadIdx = idx - k;
            *outSeqLen  = seqLen;
            return true;
        }
        return false;  // run breaks on a byte that isn't a covering lead
    }
    return false;  // walked 3 continuation bytes without finding a lead — malformed
}

// --- First hook: word-wrap width/height override (kVA_WordWrapLoop) --------

static const DWORD kVA_WordWrapLoopUtf8 = 0x00577F05;

static BYTE* g_wordWrapTrampUtf8   = nullptr;
static DWORD g_wwSavedEspUtf8      = 0;
static int   g_wwExtraAdvanceUtf8  = 0;  // (seqLen-1) when a glyph was substituted, else 0 — dynamic step vs DBCS's fixed +1

// [1-a] ASCII/half-width single-byte advance override — identical role and
// logic to obCJK_WordWrapHook.h's ObCJKWordWrapAsciiOverride (that function
// is itself encoding-agnostic, but duplicated here rather than included
// per the "no shared code between variants" decision — memory
// obcjk-utf8-plan). See that function's comment for the node+0x1C/0x00/0x24
// field derivation.
static void ObCJKWordWrapAsciiOverrideUtf8(BYTE* node, BYTE byteVal)
{
    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, (WORD)byteVal);
    if (glyph && glyph->valid) {
        *(int*)(node + 0x24) = (int)glyph->gm.gmCellIncX;
    }
}

static void __cdecl ObCJKWordWrapCheckUtf8(BYTE* ediVal, DWORD esiVal, DWORD espSnapshot)
{
    g_wwExtraAdvanceUtf8 = 0;
    BYTE* node = (BYTE*)(espSnapshot + 0xBC);

    BYTE lead = ediVal[esiVal];
    int seqLen = ObCJKUtf8ValidSeqLenAt(ediVal, esiVal);
    if (seqLen < 2) {
        if (ObCJKUtf8IsAsciiCandidate(lead))
            ObCJKWordWrapAsciiOverrideUtf8(node, lead);
        return;  // ASCII, or an invalid/truncated multi-byte lead — native handles it
    }

    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched

    DWORD codepoint = ObCJKUtf8Decode(ediVal, esiVal, seqLen);
    WORD code = ObCJKUtf8CodeForGlyph(codepoint);
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
    if (glyph && glyph->valid) {
        *(int*)(node + 0x24) = (int)glyph->gm.gmCellIncX;
        *(int*)(node + 0x28) = (int)glyph->gm.gmBlackBoxY;
        // +0x05/+0x06/+0x07 padding stash, carried onward by
        // ObCJKNodeCopyHookUtf8 below. IDA-verified safe to use all 3 bytes
        // (memory obcjk-utf8-plan「IDA交叉驗證收尾」— sub_576F30/sub_577120/
        // sub_5772A0/sub_576AB0/sub_578960 never write +0x05~+0x07). Stash
        // ALL (seqLen-1) continuation bytes in order so
        // obCJK_GlyphHook_UTF8.h's Path C can reconstruct the full 2/3/4-byte
        // sequence from node[4](lead)+node[5..7], not just the last byte.
        for (int i = 1; i < seqLen; i++) {
            node[0x05 + (i - 1)] = ediVal[esiVal + i];
        }
        g_wwExtraAdvanceUtf8 = seqLen - 1;  // esi only advances the extra bytes when we actually substituted a glyph
    }
}

static __declspec(naked) void ObCJKWordWrapHookUtf8()
{
    __asm {
        mov  dword ptr [g_wwSavedEspUtf8], esp
        pushad
        push dword ptr [g_wwSavedEspUtf8]
        push esi
        push edi
        call ObCJKWordWrapCheckUtf8
        add  esp, 12
        popad
        mov  eax, dword ptr [g_wwExtraAdvanceUtf8]
        add  esi, eax
        jmp  dword ptr [g_wordWrapTrampUtf8]
    }
}

static void ObCJKInstallWordWrapHookUtf8()
{
    if (g_wordWrapTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_WordWrapLoopUtf8, (void*)ObCJKWordWrapHookUtf8, &g_wordWrapTrampUtf8, "WordWrapUtf8");
}

// --- Second hook: scratch-node -> permanent-node copy (kVA_NodeCopy) -------
// Byte copy only, encoding-agnostic in DBCS too — duplicated here (rather
// than shared) purely to keep this file self-contained per the architecture
// decision; the body is intentionally identical.

static const DWORD kVA_NodeCopyUtf8 = 0x005770CE;
static BYTE* g_nodeCopyTrampUtf8 = nullptr;

static void __cdecl ObCJKNodeCopyUtf8(BYTE* scratchNode, BYTE* permanentNode)
{
    // All 3 stash bytes (+0x05/+0x06/+0x07), not just +0x05 — native's own
    // node-copy code doesn't touch this padding range either way (IDA-verified,
    // see the stash comment in ObCJKWordWrapCheckUtf8 above), so nothing here
    // relies on or conflicts with native's copy.
    permanentNode[5] = scratchNode[5];
    permanentNode[6] = scratchNode[6];
    permanentNode[7] = scratchNode[7];
}

static __declspec(naked) void ObCJKNodeCopyHookUtf8()
{
    __asm {
        pushad
        push eax                 ; arg1 (permanentNode)
        push esi                 ; arg0 (scratchNode)
        call ObCJKNodeCopyUtf8
        add  esp, 8
        popad
        jmp  dword ptr [g_nodeCopyTrampUtf8]
    }
}

static void ObCJKInstallNodeCopyHookUtf8()
{
    if (g_nodeCopyTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_NodeCopyUtf8, (void*)ObCJKNodeCopyHookUtf8, &g_nodeCopyTrampUtf8, "NodeCopyUtf8");
}

// --- Third hook: second independent per-character loop (kVA_SecondLoop) ---

static const DWORD kVA_SecondLoopUtf8 = 0x00578B2A;

static BYTE* g_secondLoopTrampUtf8  = nullptr;
static DWORD g_slSavedEspUtf8       = 0;
static int   g_slExtraAdvanceUtf8   = 0;

static void __cdecl ObCJKSecondLoopCheckUtf8(BYTE* ediVal, DWORD ebpVal, DWORD espSnapshot)
{
    g_slExtraAdvanceUtf8 = 0;
    BYTE* node = (BYTE*)(espSnapshot + 0x24);

    BYTE* buf = *(BYTE**)ediVal;
    BYTE lead = buf[ebpVal];

    int seqLen = ObCJKUtf8ValidSeqLenAt(buf, ebpVal);
    if (seqLen < 2) {
        if (ObCJKUtf8IsAsciiCandidate(lead))
            ObCJKWordWrapAsciiOverrideUtf8(node, lead);
        return;
    }

    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched

    DWORD codepoint = ObCJKUtf8Decode(buf, ebpVal, seqLen);
    WORD code = ObCJKUtf8CodeForGlyph(codepoint);
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
    if (glyph && glyph->valid) {
        *(int*)(node + 0x24) = (int)glyph->gm.gmCellIncX;
        *(int*)(node + 0x28) = (int)glyph->gm.gmBlackBoxY;
        // Same +0x05/+0x06/+0x07 3-byte stash as ObCJKWordWrapCheckUtf8 above.
        for (int i = 1; i < seqLen; i++) {
            node[0x05 + (i - 1)] = buf[ebpVal + i];
        }
        g_slExtraAdvanceUtf8 = seqLen - 1;
    }
}

static __declspec(naked) void ObCJKSecondLoopHookUtf8()
{
    __asm {
        mov  dword ptr [g_slSavedEspUtf8], esp
        pushad
        push dword ptr [g_slSavedEspUtf8]
        push ebp
        push edi
        call ObCJKSecondLoopCheckUtf8
        add  esp, 12
        popad
        mov  eax, dword ptr [g_slExtraAdvanceUtf8]
        add  ebp, eax
        jmp  dword ptr [g_secondLoopTrampUtf8]
    }
}

static void ObCJKInstallSecondLoopHookUtf8()
{
    if (g_secondLoopTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_SecondLoopUtf8, (void*)ObCJKSecondLoopHookUtf8, &g_secondLoopTrampUtf8, "SecondLoopUtf8");
}

// --- Fourth/sixth DBCS hooks (TrailByteGuard/TrailByteGuard2): NO UTF-8 ----
// --- counterpart, intentionally omitted -----------------------------------
//
// obCJK_WordWrapHook.h's fourth and sixth hooks exist to stop native's '~'
// hotkey-marker strip from firing on a byte that's really the trailing half
// of a DBCS pair — that ambiguity exists ONLY because DBCS trail-byte ranges
// (e.g. BIG5/GBK 0x40-0xFE) overlap 0x7E. UTF-8 continuation bytes are
// strictly 0x80-0xBF, which never includes 0x7E — a 0x7E byte in UTF-8 text
// can only ever be a genuine, standalone ASCII '~'. Native's own strip is
// therefore always correct here and installing a guard hook would be a
// no-op wrapped in dead code. obCJK_LootMenuTrailByteFixHook.h's paragraph
// makes the same observation for a sibling bug.

// --- Fifth hook: smart-quote substitution guard (kVA_SmartQuoteGuard) -----
// Native's jump table unconditionally substitutes byte values 0x91-0x94
// with ASCII '/" — those values DO fall inside UTF-8's continuation-byte
// range (0x80-0xBF), so (unlike the '~' guards above) this ambiguity is
// real: a genuine continuation byte can legitimately hold 0x91-0x94.
// Disambiguation is backward-looking (continuation bytes only ever appear
// mid/end-of-sequence, never as a sequence start) via
// ObCJKUtf8FindGenuineLeadForIdx, replacing DBCS's forward lead+trail pair
// check.

static const DWORD kVA_SmartQuoteGuardUtf8      = 0x00575F8A;
static const DWORD kVA_SmartQuoteSkipTargetUtf8 = 0x00575FAE;  // def_575F9B convergence point (also kVA_PreMeasureUtf8, the eighth hook below)

static BYTE* g_sqTrampUtf8     = nullptr;
static DWORD g_sqSavedEspUtf8  = 0;
static bool  g_sqTakeSkipUtf8  = false;

static void __cdecl ObCJKSmartQuoteGuardCheckUtf8(DWORD espSnapshot)
{
    DWORD idx    = *(DWORD*)(espSnapshot + 0x44);
    BYTE* srcPtr = *(BYTE**)(espSnapshot + 0x54);

    DWORD leadIdx = 0;
    int   seqLen  = 0;
    g_sqTakeSkipUtf8 = ObCJKUtf8FindGenuineLeadForIdx(srcPtr, idx, &leadIdx, &seqLen);
}

static __declspec(naked) void ObCJKSmartQuoteGuardHookUtf8()
{
    __asm {
        mov  dword ptr [g_sqSavedEspUtf8], esp
        pushad
        push dword ptr [g_sqSavedEspUtf8]
        call ObCJKSmartQuoteGuardCheckUtf8
        add  esp, 4
        popad
        cmp  byte ptr [g_sqTakeSkipUtf8], 0
        jz   short sq_native_utf8
        mov  byte ptr [esp+0x12], al   ; replay the native store the trampoline would have done — same rationale as DBCS's ObCJKSmartQuoteGuardHook
        mov  edx, kVA_SmartQuoteSkipTargetUtf8
        jmp  edx
sq_native_utf8:
        jmp  dword ptr [g_sqTrampUtf8]
    }
}

static void ObCJKInstallSmartQuoteGuardHookUtf8()
{
    if (g_sqTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_SmartQuoteGuardUtf8, (void*)ObCJKSmartQuoteGuardHookUtf8, &g_sqTrampUtf8, "SmartQuoteGuardUtf8");
}

// --- Seventh hook: forced line-split mid-pair guard (kVA_LineSplitPair) ---
// Native writes exactly 2 bytes ('-','\n') into p[-2]/p[-1] and (on its own
// unmodified path) relocates 2 more into p[0]/p[1] — i.e. native's own
// footprint already reaches p[+2]. Generalizing to an N-byte in-progress
// UTF-8 run (N=2/3/4, possibly only partially collected so far — see below)
// by relocating the whole run to just past '-'/'\n' always lands the far end
// at p[-N+2 + (N-1)] = p[+1], REGARDLESS of N — one byte short of what
// native's own unmodified branch already requires. So no buffer growth is
// ever needed; the earlier "no mechanism to grow the destination buffer"
// concern doesn't apply once the write footprint is derived this way.
//
// The run doesn't need to be a COMPLETE sequence either: if only the lead +
// some prefix of its continuation bytes have been written so far (e.g. a
// 3-byte CJK sequence caught after 2 of 3 bytes), relocating that partial
// prefix is enough. The outer per-byte loop resumes
// writing the remaining continuation byte(s) right after the relocated
// prefix (same "esi += 2" native continuation this hook already jumps to),
// so the sequence ends up contiguous either way.
//
// Reuses ObCJKUtf8FindGenuineLeadForIdx (already used by PreMeasure) to
// find the lead byte covering p[-1], instead of the old hardcoded
// p[-2]/p[-1] 2-byte-only check.

static const DWORD kVA_LineSplitPairUtf8 = 0x0057616E;

static BYTE* g_lspTrampUtf8      = nullptr;
static DWORD g_lspSavedEspUtf8   = 0;
static bool  g_lspForceSkipUtf8  = false;

static void __cdecl ObCJKLineSplitPairCheckUtf8(BYTE* ediVal, DWORD esiVal, DWORD espSnapshot)
{
    g_lspForceSkipUtf8 = false;
    if (esiVal < 1) return;  // nothing written yet to examine

    // p[-1] must itself be a continuation byte for there to be anything to
    // protect — if it's ASCII or a fresh lead byte with no continuation
    // written yet, native's unmodified relocation is already harmless (the
    // "don't care" byte it moves to p[+2] gets overwritten by the very next
    // real byte the outer loop appends there anyway).
    DWORD leadIdx;
    int   seqLen;
    if (!ObCJKUtf8FindGenuineLeadForIdx(ediVal, esiVal - 1, &leadIdx, &seqLen)) return;

    DWORD N = esiVal - leadIdx;  // bytes of the run already committed (2..4)
    BYTE* p = ediVal + esiVal;   // &v6[v22]

    // Shift the N-byte run forward by 2, high-to-low so N==3/4's overlapping
    // source/destination windows don't self-clobber (equivalent to memmove
    // moving to a higher address).
    for (DWORD i = N; i-- > 0; ) {
        p[(int)i - (int)N + 2] = p[(int)i - (int)N];
    }
    p[-(int)N]     = ObCJKLineBreakSpaceEnabledPathA() ? 0x20 : 0x2D;  // '-' or ' ', per LineBreakSpaceEnablePathA
    p[-(int)N + 1] = 0x0A;  // '\n'
    g_lspForceSkipUtf8 = true;
}

static __declspec(naked) void ObCJKLineSplitPairHookUtf8()
{
    __asm {
        mov  dword ptr [g_lspSavedEspUtf8], esp
        pushad
        push dword ptr [g_lspSavedEspUtf8]
        push esi
        push edi
        call ObCJKLineSplitPairCheckUtf8
        add  esp, 12
        popad
        cmp  byte ptr [g_lspForceSkipUtf8], 0
        jz   short lsp_native_utf8
        mov  edx, 0x0057617F     ; shared continuation past both native writes, same target as DBCS's ObCJKLineSplitPairHook
        jmp  edx
lsp_native_utf8:
        jmp  dword ptr [g_lspTrampUtf8]
    }
}

static void ObCJKInstallLineSplitPairHookUtf8()
{
    if (g_lspTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_LineSplitPairUtf8, (void*)ObCJKLineSplitPairHookUtf8, &g_lspTrampUtf8, "LineSplitPairUtf8");
}

// --- Eighth hook: CENTER/RIGHT-align pre-measurement (kVA_PreMeasure) -----
// Same three-case structure as DBCS's ObCJKPreMeasureCheck, but the
// "structurally the trailing half of a real pair" case (Case 1) is now a
// genuine backward continuation-byte lookup (any position within a 2/3/4
// byte sequence, not just position 1-of-2) via
// ObCJKUtf8FindGenuineLeadForIdx, and Case 2's lead check needs no parity
// walk (ObCJKUtf8ValidSeqLenAt on a non-continuation byte is unambiguous).

static const DWORD kVA_PreMeasureUtf8           = 0x00575FAE;
static const DWORD kVA_PreMeasureAccumulateUtf8 = 0x00575FDF;  // native "add ebx,eax", reused unmodified

static BYTE* g_pmTrampUtf8       = nullptr;
static DWORD g_pmSavedEspUtf8    = 0;
static bool  g_pmForceWidthUtf8  = false;
static int   g_pmWidthUtf8       = 0;

static void __cdecl ObCJKPreMeasureCheckUtf8(DWORD espSnapshot)
{
    g_pmForceWidthUtf8 = false;
    g_pmWidthUtf8 = 0;

    DWORD idx         = *(DWORD*)(espSnapshot + 0x44);  // var_4A0
    BYTE* srcPtr       = *(BYTE**)(espSnapshot + 0x54);  // var_490
    void* fontInfoPtr  = *(void**)(espSnapshot + 0x38);  // var_4AC

    BYTE current = srcPtr[idx];
    if (current == 0) return;  // terminator — native handles it

    // Same unconditional var_494 replicate as DBCS's ObCJKPreMeasureCheck —
    // see that function's comment for the downstream 0x57620D re-read this
    // guards against desyncing.
    *(DWORD*)(espSnapshot + 0x50) = (DWORD)current * 56;

    // --- Case 1: idx sits on a continuation byte of an already-counted
    // sequence (its width was added when the sequence's LEAD position
    // fired) -> contribute 0. Checked first, unconditionally on current's
    // own value, same ordering rationale as DBCS Case 1.
    DWORD leadIdx = 0;
    int   leadSeqLen = 0;
    if (ObCJKUtf8FindGenuineLeadForIdx(srcPtr, idx, &leadIdx, &leadSeqLen)) {
        int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
        if (fontID >= 0 && !ObCJKIsFontIDExcluded(fontID)) {
            DWORD codepoint = ObCJKUtf8Decode(srcPtr, leadIdx, leadSeqLen);
            WORD code = ObCJKUtf8CodeForGlyph(codepoint);
            ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
            if (glyph && glyph->valid) {
                g_pmWidthUtf8 = 0;  // already counted on the lead firing
                g_pmForceWidthUtf8 = true;
            }
        }
        return;  // structurally a continuation byte -> never also an ASCII candidate
    }

    // --- Case 2: genuine, currently-unpaired multi-byte lead byte ---
    int fullSeqLen = ObCJKUtf8ValidSeqLenAt(srcPtr, idx);
    if (fullSeqLen >= 2) {
        int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
        if (fontID >= 0 && !ObCJKIsFontIDExcluded(fontID)) {
            DWORD codepoint = ObCJKUtf8Decode(srcPtr, idx, fullSeqLen);
            WORD code = ObCJKUtf8CodeForGlyph(codepoint);
            ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
            if (glyph && glyph->valid) {
                g_pmWidthUtf8 = (int)glyph->gm.gmCellIncX;
                g_pmForceWidthUtf8 = true;
            }
        }
        return;  // lead byte -> never also an ASCII candidate
    }

    // --- Case 3: plain ASCII byte. current > 0x7F here only via a
    // malformed/truncated sequence (both cases above failed) — leave to
    // native rather than guessing.
    if (current > 0x7F) return;
    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0 || ObCJKIsFontIDExcluded(fontID)) return;
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, (WORD)current);
    if (!glyph || !glyph->valid) return;

    g_pmWidthUtf8 = (int)glyph->gm.gmCellIncX;
    g_pmForceWidthUtf8 = true;
}

static __declspec(naked) void ObCJKPreMeasureHookUtf8()
{
    __asm {
        mov  dword ptr [g_pmSavedEspUtf8], esp
        pushad
        push dword ptr [g_pmSavedEspUtf8]
        call ObCJKPreMeasureCheckUtf8
        add  esp, 4
        popad
        cmp  byte ptr [g_pmForceWidthUtf8], 0
        jz   short pm_native_utf8
        mov  eax, dword ptr [g_pmWidthUtf8]
        mov  edx, kVA_PreMeasureAccumulateUtf8
        jmp  edx
pm_native_utf8:
        jmp  dword ptr [g_pmTrampUtf8]
    }
}

static void ObCJKInstallPreMeasureHookUtf8()
{
    if (g_pmTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_PreMeasureUtf8, (void*)ObCJKPreMeasureHookUtf8, &g_pmTrampUtf8, "PreMeasureUtf8");
}
