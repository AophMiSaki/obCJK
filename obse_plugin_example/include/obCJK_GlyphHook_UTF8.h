#pragma once
// UTF-8 counterpart of obCJK_GlyphHook.h — hooks the identical three native
// "draw one glyph" call sites (Path A: sub_576670 @0x576919, Path B:
// sub_575870 @0x575A48, Path C: sub_576AB0 @0x576CD1) but decodes
// variable-length UTF-8 sequences (2/3/4 bytes) instead of a DBCS lead/trail
// pair. All symbols carry a Utf8 suffix so both headers coexist; only one
// set of Install*Hook is called (by ActiveCodePage), never both.
//
// Reuses encoding-independent helpers from obCJK_GlyphHook.h (fontID
// lookups, ObCJKIsPlausiblePtr, PathDiag settings) and the UTF-8 decode
// helpers from obCJK_WordWrapHook_UTF8.h.
//
// Path C reads continuation bytes from node+0x05..0x07, written by
// obCJK_WordWrapHook_UTF8.h (ObCJKWordWrapCheckUtf8/ObCJKSecondLoopCheckUtf8,
// carried by ObCJKNodeCopyUtf8).
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
// Fires once per source byte. Bytes 1..N-1 of a sequence are buffered and
// drawn as a zero-size placeholder (keeps native pen bookkeeping
// consistent); byte N decodes the buffer and substitutes the real glyph.
static ObCJKNativeGlyphEntry g_paBlankGlyphUtf8 = {};  // all-zero: width/height/advance=0

static BYTE  g_paBufUtf8[4]       = { 0, 0, 0, 0 };
static int   g_paCollectedUtf8    = 0;   // bytes stashed in g_paBufUtf8 so far (includes the lead)
static int   g_paTargetLenUtf8    = 0;   // 0 = no sequence in progress; else ObCJKUtf8SeqLen() of the armed lead
static BYTE* g_paGlyphPtrUtf8     = nullptr;
static bool  g_paIsCJKUtf8        = false;
static DWORD g_paHitsUtf8         = 0;   // diag line counter, capped by ObCJKPathDiagCap()
static DWORD g_paAsciiHitsUtf8    = 0;

// ASCII/half-width substitution (single byte), same as DBCS's ObCJKPathAAsciiCheck.
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
        // Line height: shared g_paMaxLineHeight (obCJK_GlyphHook.h).
        int h = (int)glyph->gm.gmBlackBoxY;
        if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
    }
}

// `currentByte`/`fontInfoPtr` = `bl`/`edi` at hook VA 0x576919.
static void __cdecl ObCJKPathACheckUtf8(BYTE currentByte, void* fontInfoPtr)
{
    g_paIsCJKUtf8 = false;

    // Fix: slot4 is checked before buffering — otherwise its earlier bytes
    // were drawn as blank placeholders and the character disappeared. Also
    // drops any in-progress sequence.
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
                    // Line height: shared g_paMaxLineHeight, consumed by
                    // ObCJKPathALineHeightHook (encoding-independent).
                    int h = (int)glyph->gm.gmBlackBoxY;
                    if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
                    return;
                }
            }
            // Not ready: the last continuation byte falls through below and
            // matches neither check, so it is left to native.
        } else {
            // fontID only needed for the diag slot filter.
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

    // Not part of a multi-byte sequence — try ASCII/half-width substitution.
    if (ObCJKUtf8IsAsciiCandidate(currentByte))
        ObCJKPathAAsciiCheckUtf8(currentByte, fontInfoPtr);
}

// Same native replay as DBCS's ObCJKPathAGlyphHook (gap 0x576919-0x57696F,
// same esp offsets).
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

// `buf`/`idx`/`fontInfoPtr` = `eax`/`esi`/`edi` at hook VA 0x575A48.
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

// Same native replay as DBCS's ObCJKPathBGlyphHook, except `add esi,1`
// becomes `add esi,[g_pbExtraAdvanceUtf8]`.
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

// ASCII/half-width substitution. Each Path C node is one character (no index
// skip), so g_pcIsCJKUtf8 alone means "substitute".
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
// +0x05/+0x06/+0x07=up to 3 continuation bytes (see top comment),
// +0x1C=IMG-override pointer (non-zero -> skip).
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

    // Validate the continuation bytes: WordWrapHook_UTF8 may have skipped
    // the stash write for this node, leaving stale bytes.
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

// Same native replay as DBCS's ObCJKPathCGlyphHook (gap 0x576CD1-0x576CE9).
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
