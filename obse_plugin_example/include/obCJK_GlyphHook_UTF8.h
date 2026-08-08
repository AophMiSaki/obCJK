#pragma once
// UTF-8 counterpart of obCJK_GlyphHook.h — hooks the identical three native
// "draw one glyph" call sites (Path A: sub_576670 @0x576919, Path B:
// sub_575870 @0x575A48, Path C: sub_576AB0 @0x576CD1) but decodes
// variable-length UTF-8 sequences (2/3/4 bytes) instead of assuming a fixed
// DBCS lead/trail pair. Per the 2026-07-18 architecture decision , 
// every hook/state symbol below is independently named (Utf8 suffix)
// so both headers coexist in the same translation unit; 
// only one path's Install*HookUtf8()/Install*Hook()
// is ever actually called (ini utf8-mode switch), so the two never run
// concurrently on the same VA.
//
// Reused from obCJK_GlyphHook.h (DBCS) without duplication — same
// "encoding-agnostic utility, not per-encoding disambiguation logic"
// rationale obCJK_WordWrapHook_UTF8.h already applies to that file's
// ObCJKFontIDFromInfoPtr reuse: ObCJKFontIDFromInfoPtr/ObCJKFontInfoPtrFromID
// (pure FontManager slot-array <-> fontID lookups), ObCJKIsPlausiblePtr
// (guard-page sanity check), ObCJKPathDiagEnabled/ObCJKPathDiagCap (ini
// diagnostic settings). Reused from obCJK_WordWrapHook_UTF8.h: the UTF-8
// decode helpers (ObCJKUtf8ValidSeqLenAt/ObCJKUtf8Decode/
// ObCJKUtf8CodeForGlyph/ObCJKUtf8FindGenuineLeadForIdx) — these ARE
// UTF-8-specific disambiguation logic, but reusing them across UTF-8 files
// only (never across the DBCS/UTF-8 boundary) keeps one correctness-critical
// implementation instead of three copies that could drift.
//
// node+0x05/0x06/0x07 3-byte continuation-byte stash consumed by Path C
// below (written by ObCJKWordWrapCheckUtf8/ObCJKSecondLoopCheckUtf8, carried
// by ObCJKNodeCopyUtf8, all in obCJK_WordWrapHook_UTF8.h) — IDA-verified safe
// range.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_TexUpload.h"        // ObCJKGlyphEntry / ObCJKNativeGlyphEntry / ObCJKTexUpload_GetOrPlaceGlyph
#include "obCJK_GlyphHook.h"        // ObCJKFontIDFromInfoPtr / ObCJKFontInfoPtrFromID / ObCJKIsPlausiblePtr / ObCJKPathDiagEnabled / ObCJKPathDiagCap
#include "obCJK_WordWrapHook_UTF8.h" // ObCJKUtf8ValidSeqLenAt / ObCJKUtf8Decode / ObCJKUtf8CodeForGlyph / ObCJKUtf8FindGenuineLeadForIdx

static const DWORD kVA_PathA_GlyphDrawUtf8 = 0x00576919;  // same hook VA as DBCS's kVA_PathA_GlyphDraw
static const DWORD kVA_PathB_GlyphDrawUtf8 = 0x00575A48;  // same hook VA as DBCS's kVA_PathB_GlyphDraw
static const DWORD kVA_PathC_GlyphDrawUtf8 = 0x00576CD1;  // same hook VA as DBCS's kVA_PathC_GlyphDraw

static BYTE* g_pathATrampUtf8 = nullptr;
static BYTE* g_pathBTrampUtf8 = nullptr;
static BYTE* g_pathCTrampUtf8 = nullptr;

// --- Path A: sub_576670, per-source-byte firing, no [idx+1] peek ----------
// DBCS's g_paPendingLead spans exactly 2 firings (1 lead + 1 trail). UTF-8
// sequences can be 2/3/4 bytes, so this needs a small buffer + running count
// instead of a single pending byte: firings 1..(N-1) arm/extend the buffer
// and draw a zero-size blank placeholder (native's own per-firing pen
// bookkeeping stays consistent, same reason DBCS blanks the lead firing);
// firing N (the last continuation byte) decodes the full buffer and
// substitutes the real merged-width glyph.
static ObCJKNativeGlyphEntry g_paBlankGlyphUtf8 = {};  // all-zero: width/height/advance=0

static BYTE  g_paBufUtf8[4]       = { 0, 0, 0, 0 };
static int   g_paCollectedUtf8    = 0;   // bytes stashed in g_paBufUtf8 so far (includes the lead)
static int   g_paTargetLenUtf8    = 0;   // 0 = no sequence in progress; else ObCJKUtf8SeqLen() of the armed lead
static BYTE* g_paGlyphPtrUtf8     = nullptr;
static bool  g_paIsCJKUtf8        = false;
static DWORD g_paHitsUtf8         = 0;   // diagnostic line counter, gated against ObCJKPathDiagCap() — see DBCS g_paHits comment
static DWORD g_paAsciiHitsUtf8    = 0;

// [1-a] ASCII/half-width substitution — identical role to DBCS's
// ObCJKPathAAsciiCheck (single byte, no multi-byte merge, same TexUpload
// path). Duplicated rather than shared per the architecture decision; body
// is intentionally near-identical.
static void ObCJKPathAAsciiCheckUtf8(BYTE currentByte, void* fontInfoPtr)
{
    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0) return;
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;

    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, (WORD)currentByte, fontInfoPtr);
    bool ready = (glyph && glyph->texSwapReady);
    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_paAsciiHitsUtf8++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_paAsciiHitsUtf8 <= ObCJKPathDiagCap()) {
        if (ready)
            _MESSAGE("obCJK:PathA:glyph(ascii-utf8) slot=%d code=0x%02X char='%c'",
                     ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte);
        else
            _WARNING("obCJK:PathA:MISS(ascii-utf8) slot=%d code=0x%02X byte='%c' valid=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte, glyph ? (int)glyph->valid : -1);
    }
    if (ready) {
        g_paGlyphPtrUtf8 = (BYTE*)&glyph->native;
        g_paIsCJKUtf8 = true;
        // [line-height] shared with DBCS — see obCJK_GlyphHook.h's
        // ObCJKPathAAsciiCheck for why the ASCII branch tracks
        // g_paMaxLineHeight too (2026-07-31, widened alongside the CJK
        // branch below to match obCJK_LineBreakHook.h's line+0x20 fix).
        int h = (int)glyph->gm.gmBlackBoxY;
        if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
    }
}

// `currentByte`/`fontInfoPtr` = `bl`/`edi` at hook VA 0x576919 — same
// register-safety proof as DBCS's ObCJKPathACheck (edi set once at function
// entry, unclobbered).
static void __cdecl ObCJKPathACheckUtf8(BYTE currentByte, void* fontInfoPtr)
{
    g_paIsCJKUtf8 = false;

    // slot4 exclusion guard, checked up front for the same reason as DBCS's
    // ObCJKPathACheck: fontID can't be resolved mid-sequence, but by the
    // final firing the earlier bytes would already have drawn as blank
    // placeholders, which would delete the character instead of leaving it
    // native. Abort any in-progress sequence so a subsequent (non-excluded)
    // string doesn't inherit stale buffer state.
    {
        int fontIDGuard = ObCJKFontIDFromInfoPtr(fontInfoPtr);
        if (fontIDGuard >= 0 && ObCJKIsFontIDExcluded(fontIDGuard)) {
            g_paCollectedUtf8 = 0;
            g_paTargetLenUtf8 = 0;
            return;
        }
    }

    if (g_paTargetLenUtf8 != 0) {
        if (ObCJKIsUtf8Continuation(currentByte)) {
            g_paBufUtf8[g_paCollectedUtf8] = currentByte;
            g_paCollectedUtf8++;

            if (g_paCollectedUtf8 < g_paTargetLenUtf8) {
                // still gathering — keep the placeholder up for this firing too
                g_paGlyphPtrUtf8 = (BYTE*)&g_paBlankGlyphUtf8;
                g_paIsCJKUtf8 = true;
                return;
            }

            // full sequence collected on this firing — decode & try the real glyph
            int seqLen = g_paTargetLenUtf8;
            g_paTargetLenUtf8 = 0;
            g_paCollectedUtf8 = 0;

            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            if (fontID >= 0) {
                DWORD codepoint = ObCJKUtf8Decode(g_paBufUtf8, 0, seqLen);
                WORD code = ObCJKUtf8CodeForGlyph(codepoint);
                ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, code, fontInfoPtr);
                bool ready = (glyph && glyph->texSwapReady);
                bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
                if (diagSlotOk) g_paHitsUtf8++;
                if (ObCJKPathDiagEnabled() && diagSlotOk && g_paHitsUtf8 <= ObCJKPathDiagCap()) {
                    if (ready)
                        _MESSAGE("obCJK:PathA:glyph(cjk-utf8) slot=%d code=0x%04X cp=0x%04X len=%d",
                                 ObCJKSlotFromFontID(fontID), code, codepoint, seqLen);
                    else
                        _WARNING("obCJK:PathA:MISS(cjk-utf8) slot=%d code=0x%04X cp=0x%04X len=%d valid=%d texSwapReady=%d — falls back to native",
                                  ObCJKSlotFromFontID(fontID), code, codepoint, seqLen,
                                  glyph ? (int)glyph->valid : -1, glyph ? (int)glyph->texSwapReady : -1);
                }
                if (ready) {
                    g_paGlyphPtrUtf8 = (BYTE*)&glyph->native;
                    g_paIsCJKUtf8 = true;
                    // [line-height] shared with DBCS — g_paMaxLineHeight
                    // (obCJK_GlyphHook.h) is consumed+reset by the same
                    // encoding-agnostic ObCJKPathALineHeightHook (VA
                    // 0x5768c4, native code doesn't branch on encoding
                    // mode). See that file's comment + PathA主要文字顯示.md
                    // 第25節.
                    int h = (int)glyph->gm.gmBlackBoxY;
                    if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
                    return;
                }
            }
            // glyph not ready / no fontID — currentByte (the sequence's LAST
            // continuation byte, always 0x80-0xBF) falls through to the fresh
            // lead/ascii checks below, same as DBCS's "pending lead didn't pan
            // out" path. Structurally a no-op there: a continuation byte can
            // never satisfy ObCJKUtf8SeqLen()>=2 nor ObCJKUtf8IsAsciiCandidate,
            // so this just leaves the byte to native (falls back to native).
        } else {
            // fontID not otherwise resolved on this branch — resolve it here
            // purely for the slot7/8-only diag filter below.
            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            if (ObCJKPathDiagEnabled() && ObCJKPathDiagSlotAllowed(fontID) && g_paHitsUtf8 <= ObCJKPathDiagCap()) {
                _WARNING("obCJK:PathA:MISS(broken-seq-utf8) expected continuation byte, got 0x%02X (targetLen=%d collected=%d) — "
                          "abandoning sequence; already-fired placeholder bytes are not retroactively fixed",
                          currentByte, g_paTargetLenUtf8, g_paCollectedUtf8);
            }
            g_paTargetLenUtf8 = 0;
            g_paCollectedUtf8 = 0;
            // fall through: re-examine currentByte fresh below (own lead/ascii check)
        }
    }

    int leadLen = ObCJKUtf8SeqLen(currentByte);
    if (leadLen >= 2) {
        g_paBufUtf8[0] = currentByte;
        g_paCollectedUtf8 = 1;
        g_paTargetLenUtf8 = leadLen;
        g_paGlyphPtrUtf8 = (BYTE*)&g_paBlankGlyphUtf8;
        g_paIsCJKUtf8 = true;
        return;
    }

    // [1-a] not part of a CJK sequence — try ASCII/half-width substitution.
    if (ObCJKUtf8IsAsciiCandidate(currentByte))
        ObCJKPathAAsciiCheckUtf8(currentByte, fontInfoPtr);
}

// Byte-for-byte identical replay to DBCS's ObCJKPathAGlyphHook (same hook
// VA, same native instruction gap 0x576919-0x57696F, same esp-relative
// offsets — this hook point's register layout doesn't depend on encoding).
// See that function's comment for the offset derivation.
static __declspec(naked) void ObCJKPathAGlyphHookUtf8()
{
    __asm {
        pushad
        push edi            // fontInfoPtr
        movzx eax, bl
        push eax            // currentByte
        call ObCJKPathACheckUtf8
        add  esp, 8
        popad
        cmp  byte ptr [g_paIsCJKUtf8], 0
        je   short patha_passthrough_utf8

        mov  ecx, [esp+90h]  ; native 576943: ecx = arg_1C
        push ecx             ; native 576959
        mov  ecx, [esp+98h]  ; native 57695A: ecx = arg_20
        lea  edx, [esp+34h]  ; native 576968: edx = &var_40
        push edx             ; native 57696C
        push ebp             ; native 57696D
        push ecx             ; native 57696E

        mov  eax, dword ptr [g_paGlyphPtrUtf8]
        mov  edx, 0x57696F   ; native "push eax" (arg_0) — falls through to
        jmp  edx             ; "mov ecx,edi" + "call sub_573F10" unmodified

patha_passthrough_utf8:
        jmp  dword ptr [g_pathATrampUtf8]
    }
}

static void ObCJKInstallPathAGlyphHookUtf8()
{
    if (g_pathATrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_PathA_GlyphDrawUtf8, (void*)ObCJKPathAGlyphHookUtf8, &g_pathATrampUtf8, "PathA_GlyphDrawUtf8");
}

// --- Path B: sub_575870, direct [idx+1..idx+N-1] peek, dynamic skip -------

static BYTE* g_pbGlyphPtrUtf8      = nullptr;
static bool  g_pbIsCJKUtf8         = false;   // real multi-byte sequence -> needs the extra esi skip
static bool  g_pbGlyphReadyUtf8    = false;   // substitute a glyph at all (CJK or ASCII)
static int   g_pbExtraAdvanceUtf8  = 0;       // (seqLen-1) when CJK, else 0 — dynamic vs DBCS's fixed add esi,1
static DWORD g_pbHitsUtf8          = 0;
static DWORD g_pbAsciiHitsUtf8     = 0;

static void ObCJKPathBAsciiCheckUtf8(BYTE currentByte, void* fontInfoPtr)
{
    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0) return;
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;

    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, (WORD)currentByte, fontInfoPtr);
    bool ready = (glyph && glyph->texSwapReady);
    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pbAsciiHitsUtf8++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pbAsciiHitsUtf8 <= ObCJKPathDiagCap()) {
        if (ready)
            _MESSAGE("obCJK:PathB:glyph(ascii-utf8) slot=%d code=0x%02X char='%c'",
                     ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte);
        else
            _WARNING("obCJK:PathB:MISS(ascii-utf8) slot=%d code=0x%02X byte='%c' valid=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte, glyph ? (int)glyph->valid : -1);
    }
    if (ready) {
        g_pbGlyphPtrUtf8 = (BYTE*)&glyph->native;
        g_pbGlyphReadyUtf8 = true;  // NOT g_pbIsCJKUtf8 — single byte, no extra skip
    }
}

// `buf`/`idx`/`fontInfoPtr` = `eax`/`esi`/`edi` at hook VA 0x575A48, same
// register-safety proof as DBCS's ObCJKPathBCheck.
static void __cdecl ObCJKPathBCheckUtf8(BYTE* buf, int idx, void* fontInfoPtr)
{
    g_pbIsCJKUtf8 = false;
    g_pbGlyphReadyUtf8 = false;
    g_pbExtraAdvanceUtf8 = 0;

    if (!ObCJKIsPlausiblePtr(buf) || idx < 0) return;

    BYTE lead = buf[idx];
    int seqLen = ObCJKUtf8ValidSeqLenAt(buf, idx);
    if (seqLen < 2) {
        if (ObCJKUtf8IsAsciiCandidate(lead))
            ObCJKPathBAsciiCheckUtf8(lead, fontInfoPtr);
        return;
    }

    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0) return;  // shouldn't happen — edi should always be one of fontInfos[]
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched

    DWORD codepoint = ObCJKUtf8Decode(buf, idx, seqLen);
    WORD code = ObCJKUtf8CodeForGlyph(codepoint);
    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, code, fontInfoPtr);
    bool glyphReady = (glyph && glyph->texSwapReady);

    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pbHitsUtf8++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pbHitsUtf8 <= ObCJKPathDiagCap()) {
        if (glyphReady)
            _MESSAGE("obCJK:PathB:glyph(cjk-utf8) slot=%d code=0x%04X cp=0x%04X len=%d",
                     ObCJKSlotFromFontID(fontID), code, codepoint, seqLen);
        else
            _WARNING("obCJK:PathB:MISS(cjk-utf8) slot=%d code=0x%04X cp=0x%04X len=%d valid=%d texSwapReady=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), code, codepoint, seqLen,
                      glyph ? (int)glyph->valid : -1, glyph ? (int)glyph->texSwapReady : -1);
    }
    if (!glyphReady) return;

    g_pbGlyphPtrUtf8 = (BYTE*)&glyph->native;
    g_pbIsCJKUtf8 = true;
    g_pbGlyphReadyUtf8 = true;
    g_pbExtraAdvanceUtf8 = seqLen - 1;
}

// Byte-for-byte identical native replay to DBCS's ObCJKPathBGlyphHook,
// except the fixed `add esi,1` becomes a dynamic `add esi,[g_pbExtraAdvanceUtf8]`
// — same dynamic-step pattern obCJK_WordWrapHook_UTF8.h already uses for its
// first/third hooks.
static __declspec(naked) void ObCJKPathBGlyphHookUtf8()
{
    __asm {
        pushad
        push edi            // fontInfoPtr
        push esi             // idx
        push eax             // buf
        call ObCJKPathBCheckUtf8
        add  esp, 12
        popad
        cmp  byte ptr [g_pbGlyphReadyUtf8], 0
        je   short pathb_passthrough_utf8

        cmp  byte ptr [g_pbIsCJKUtf8], 0    ; only a real multi-byte sequence needs
        je   short pathb_no_extra_utf8       ; the extra skip — see g_pbGlyphReadyUtf8 comment
        mov  eax, dword ptr [g_pbExtraAdvanceUtf8]
        add  esi, eax        // extra advance to skip the continuation bytes
pathb_no_extra_utf8:

        mov  edx, dword ptr [esp+44h]
        push edx
        mov  edx, dword ptr [esp+64h]
        lea  ecx, dword ptr [esp+28h]
        push ecx
        push ebp
        push edx

        mov  eax, dword ptr [g_pbGlyphPtrUtf8]
        mov  edx, 0x575A98  ; edx's own value was already pushed above, free to reuse as jump target
        jmp  edx

pathb_passthrough_utf8:
        jmp  dword ptr [g_pathBTrampUtf8]
    }
}

static void ObCJKInstallPathBGlyphHookUtf8()
{
    if (g_pathBTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_PathB_GlyphDrawUtf8, (void*)ObCJKPathBGlyphHookUtf8, &g_pathBTrampUtf8, "PathB_GlyphDrawUtf8");
}

// --- Path C: sub_576AB0, pre-built node tree, node[4]=lead node[5..7]=cont -

static BYTE* g_pcGlyphPtrUtf8 = nullptr;
static bool  g_pcIsCJKUtf8    = false;
static DWORD g_pcHitsUtf8     = 0;
static DWORD g_pcAsciiHitsUtf8 = 0;

// [1-a] ASCII/half-width substitution, same idea as ObCJKPathAAsciiCheckUtf8.
// Path C has no esi/index bookkeeping (each node already represents exactly
// one logical character), so reusing g_pcIsCJKUtf8 for both "substitute" and
// "is a real sequence" is safe — no extra-skip side effect gated on it.
static void ObCJKPathCAsciiCheckUtf8(BYTE* node, BYTE currentByte)
{
    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;
    void* fontInfoPtr = ObCJKFontInfoPtrFromID(fontID);
    if (!fontInfoPtr) return;

    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, (WORD)currentByte, fontInfoPtr);
    bool ready = (glyph && glyph->texSwapReady);
    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pcAsciiHitsUtf8++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pcAsciiHitsUtf8 <= ObCJKPathDiagCap()) {
        if (ready)
            _MESSAGE("obCJK:PathC:glyph(ascii-utf8) slot=%d code=0x%02X char='%c'",
                     ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte);
        else
            _WARNING("obCJK:PathC:MISS(ascii-utf8) slot=%d code=0x%02X byte='%c' valid=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte, glyph ? (int)glyph->valid : -1);
    }
    if (ready) {
        g_pcGlyphPtrUtf8 = (BYTE*)&glyph->native;
        g_pcIsCJKUtf8 = true;
    }
}

// `node` (`esi` at hook VA 0x576CD1): +0x00=font ID, +0x04=lead byte,
// +0x05/+0x06/+0x07=up to 3 continuation bytes (stashed by
// ObCJKWordWrapCheckUtf8/ObCJKSecondLoopCheckUtf8, carried across scratch->
// permanent copy by ObCJKNodeCopyUtf8 — all in obCJK_WordWrapHook_UTF8.h),
// +0x1C=IMG-override pointer (non-zero -> skip). Same node-format proof as
// DBCS's ObCJKPathCCheck (Hook內容與規範.md「一、」), extended per the
// IDA cross-check documented in memory obcjk-utf8-plan.
static void __cdecl ObCJKPathCCheckUtf8(BYTE* node)
{
    g_pcIsCJKUtf8 = false;

    if (!ObCJKIsPlausiblePtr(node)) return;

    BYTE lead = node[4];
    int seqLen = ObCJKUtf8SeqLen(lead);
    if (seqLen < 2) {
        if (ObCJKUtf8IsAsciiCandidate(lead))
            ObCJKPathCAsciiCheckUtf8(node, lead);
        return;
    }

    // Validate node[5..7]'s (seqLen-1) continuation bytes before trusting
    // them — node is populated by a different hook (WordWrapHook_UTF8) and
    // could carry stale bytes from a prior character if that hook never
    // armed this particular node (e.g. it hit its own IMG-override/slot4
    // early-return and skipped the stash write).
    bool contOk = true;
    for (int i = 1; i < seqLen; i++) {
        if (!ObCJKIsUtf8Continuation(node[4 + i])) { contOk = false; break; }
    }
    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (!contOk) return;
    if (imgField != 0) return;  // IMG-override node, leave untouched (legitimate skip, not a failure)

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    void* fontInfoPtr = ObCJKFontInfoPtrFromID(fontID);
    if (!fontInfoPtr) return;  // fontID out of range or slot never created — shouldn't happen

    // node+4 is the lead byte position; ObCJKUtf8Decode(buf, idx, len) reads
    // buf[idx]=lead then buf[idx+1..idx+len-1] — exactly node[5..7].
    DWORD codepoint = ObCJKUtf8Decode(node, 4, seqLen);
    WORD code = ObCJKUtf8CodeForGlyph(codepoint);
    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, code, fontInfoPtr);
    bool glyphReady = (glyph && glyph->texSwapReady);

    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pcHitsUtf8++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pcHitsUtf8 <= ObCJKPathDiagCap()) {
        if (glyphReady)
            _MESSAGE("obCJK:PathC:glyph(cjk-utf8) slot=%d code=0x%04X cp=0x%04X len=%d",
                     ObCJKSlotFromFontID(fontID), code, codepoint, seqLen);
        else
            _WARNING("obCJK:PathC:MISS(cjk-utf8) slot=%d code=0x%04X cp=0x%04X len=%d valid=%d texSwapReady=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), code, codepoint, seqLen,
                      glyph ? (int)glyph->valid : -1, glyph ? (int)glyph->texSwapReady : -1);
    }
    if (!glyphReady) return;

    g_pcGlyphPtrUtf8 = (BYTE*)&glyph->native;
    g_pcIsCJKUtf8 = true;
}

// Byte-for-byte identical replay to DBCS's ObCJKPathCGlyphHook (same hook VA,
// same 0x576CD1-0x576CE9 native gap).
static __declspec(naked) void ObCJKPathCGlyphHookUtf8()
{
    __asm {
        pushad
        push esi
        call ObCJKPathCCheckUtf8
        add  esp, 4
        popad
        cmp  byte ptr [g_pcIsCJKUtf8], 0
        je   short pathc_passthrough_utf8
        push edx
        mov  edx, dword ptr [g_pcGlyphPtrUtf8]
        mov  eax, 0x576CE9
        jmp  eax
pathc_passthrough_utf8:
        jmp  dword ptr [g_pathCTrampUtf8]
    }
}

static void ObCJKInstallPathCGlyphHookUtf8()
{
    if (g_pathCTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_PathC_GlyphDrawUtf8, (void*)ObCJKPathCGlyphHookUtf8, &g_pathCTrampUtf8, "PathC_GlyphDrawUtf8");
}

static void ObCJKInstallGlyphHooksUtf8()
{
    ObCJKInstallPathAGlyphHookUtf8();
    ObCJKInstallPathBGlyphHookUtf8();
    ObCJKInstallPathCGlyphHookUtf8();
}
