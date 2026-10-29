#pragma once
// UTF-8 word-wrap hooks. Same native VAs as the DBCS obCJK_WordWrapHook.h
// (sub_577C10 / sub_578960 / sub_575B40), but decodes variable-length UTF-8
// sequences instead of a fixed 2-byte lead/trail pair. Shares no logic with
// the DBCS version; every symbol has a Utf8 suffix so both headers compile
// into main.cpp, which installs only one set based on ActiveCodePage.
//
// Hooks:
//   WordWrap        sub_577C10 per-character width/height + continuation stash
//   NodeCopy        carries node+0x05..0x07 to the permanent node
//   SecondLoop      same as WordWrap for sub_578960's second loop
//   SmartQuoteGuard sub_575B40 0x91-0x94 continuation bytes not turned into '/"
//   LineSplitPair   forced line break doesn't split a UTF-8 sequence
//   PreMeasure      CENTER/RIGHT-align width pre-measurement
// TrailByteGuard/TrailByteGuard2 have no UTF-8 counterpart: 0x7E can never be
// a continuation byte, so native's '~' strip is already correct.
//
// Continuation bytes (0x80-0xBF) never overlap lead bytes (0xC2-0xF4) or
// ASCII, so a byte's role is determined by its value alone.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_GlyphAtlas.h"
// Needed only for ObCJKFontIDFromInfoPtr() (encoding-agnostic).
#include "obCJK_GlyphHook.h"

// --- UTF-8 decode helpers, local to this file ------------------------------

// Validates and returns the full sequence length starting at buf[idx]
// (2/3/4), or 0 if buf[idx] isn't a multi-byte lead or the following
// continuation bytes are missing/malformed/truncated (callers then fall back
// to native single-byte handling). Never reads past a NUL terminator, since
// NUL is not a continuation byte.
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

// Glyph-atlas lookup key for a decoded codepoint. The glyph table is
// WORD-indexed, so codepoints above U+FFFF are truncated.
static inline WORD ObCJKUtf8CodeForGlyph(DWORD codepoint)
{
    return (WORD)(codepoint & 0xFFFF);
}

// Finds the lead byte of the sequence that srcPtr[idx] is a continuation
// byte of: walks back through the continuation run (max 3 bytes) and checks
// that the lead's declared length reaches idx. Returns false if idx isn't a
// continuation byte, the run exceeds 3 bytes, or no covering lead is found.
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
static int   g_wwExtraAdvanceUtf8  = 0;  // (seqLen-1) when a glyph was substituted, else 0

// [1-a] ASCII/half-width single-byte advance override: writes the
// substituted glyph's advance to node+0x24.
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
        *(int*)(node + 0x28) = ObCJKNodeHeightFor(fontID, glyph);
        // Stash all (seqLen-1) continuation bytes in node padding +0x05..0x07
        // (never written by native) so Path C can rebuild the full sequence
        // from node[4..7]. Carried onward by ObCJKNodeCopyHookUtf8.
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
// Copies the +0x05..0x07 continuation-byte stash to the permanent node.

static const DWORD kVA_NodeCopyUtf8 = 0x005770CE;
static BYTE* g_nodeCopyTrampUtf8 = nullptr;

static void __cdecl ObCJKNodeCopyUtf8(BYTE* scratchNode, BYTE* permanentNode)
{
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
        *(int*)(node + 0x28) = ObCJKNodeHeightFor(fontID, glyph);
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

// --- TrailByteGuard/TrailByteGuard2: no UTF-8 counterpart (0x7E is never a
// --- continuation byte) ---------------------------------------------------

// --- Fifth hook: smart-quote substitution guard (kVA_SmartQuoteGuard) -----
// Fix: native's jump table replaces bytes 0x91-0x94 with ASCII '/", which
// corrupts UTF-8 continuation bytes in that range. Skips the substitution
// when the byte belongs to a valid sequence (ObCJKUtf8FindGenuineLeadForIdx).

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
        mov  byte ptr [esp+0x12], al   ; replay the native store the trampoline would have done
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
// Fix: native's forced break writes '-','\n' at p[-2]/p[-1], splitting an
// in-progress UTF-8 sequence. When p[-1] is a continuation byte, this
// moves the already-written N-byte run (N=2..4, may be partial) forward by
// 2 and writes '-'/'\n' in front of it. The far end lands at p[+1], within
// native's own write footprint (p[+2]), so no buffer growth is needed. The
// outer loop then appends any remaining continuation bytes contiguously.

static const DWORD kVA_LineSplitPairUtf8 = 0x0057616E;

static BYTE* g_lspTrampUtf8      = nullptr;
static DWORD g_lspSavedEspUtf8   = 0;
static bool  g_lspForceSkipUtf8  = false;

static void __cdecl ObCJKLineSplitPairCheckUtf8(BYTE* ediVal, DWORD esiVal, DWORD espSnapshot)
{
    g_lspForceSkipUtf8 = false;
    if (esiVal < 1) return;  // nothing written yet to examine

    // Only needed when p[-1] is a continuation byte; otherwise native's
    // relocation is harmless.
    DWORD leadIdx;
    int   seqLen;
    if (!ObCJKUtf8FindGenuineLeadForIdx(ediVal, esiVal - 1, &leadIdx, &seqLen)) return;

    DWORD N = esiVal - leadIdx;  // bytes of the run already committed (2..4)
    BYTE* p = ediVal + esiVal;   // &v6[v22]

    // Shift the N-byte run forward by 2, high-to-low (overlapping windows).
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
        mov  edx, 0x0057617F     ; continuation past both native writes
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
// Fix: sub_575B40's per-byte width accumulation (feeds CENTER/RIGHT-align
// start x) uses native single-byte widths. Overrides each byte's width:
//   Case 1: continuation byte of a counted sequence -> 0
//   Case 2: multi-byte lead -> substituted glyph's advance
//   Case 3: ASCII with AsciiRenderEnable -> substituted glyph's advance

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

    // Fix: replicate native's var_494 store unconditionally (re-read at
    // 0x57620D).
    *(DWORD*)(espSnapshot + 0x50) = (DWORD)current * 56;

    // --- Case 1: continuation byte of an already-counted sequence -> 0.
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
        return;
    }

    // --- Case 2: multi-byte lead byte ---
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
        return;
    }

    // --- Case 3: plain ASCII byte. current > 0x7F here = malformed
    // sequence, left to native.
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
