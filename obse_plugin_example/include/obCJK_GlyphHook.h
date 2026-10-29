#pragma once
// Hooks the three native "draw one glyph" call sites (Path A: sub_576670,
// Path B: sub_575870, Path C: sub_576AB0), replacing the native single-byte
// font-table lookup with a CJK (or substituted ASCII/half-width) glyph placed
// in the TexSwap-enlarged native font texture via
// ObCJKTexUpload_GetOrPlaceGlyph(), then continuing into sub_573F10 with
// &entry->native as the glyph pointer. No D3D device calls of its own.
//   Path A: lead/trail pair spans two hook firings (state machine).
//   Path B: peeks buf[idx+1] for the trail byte, skips it via esi+1.
//   Path C: reads lead/trail from the word-wrap node (+0x04/+0x05).
// Also hosts the Path A per-line height fix (VA 0x5768C4) and the
// PathDiagEnable / PathDiagCap / PathDiagSlot78Only diagnostic switches.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_TexUpload.h"  // pulls in obCJK_GlyphAtlas.h (ObCJKGlyphEntry) + obCJK_TexSwap.h
#include "obCJK_NorthernUICompat.h"  // NorthernUI xxnFontPath bridge

// [obCJK] PathDiagEnable(預設0)：開啟後印出Path A/B/C的glyph替換catch/miss訊息。
// PathDiagCap(300/500/1000)：每條路徑各自的印出行數上限。
static bool ObCJKPathDiagEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "PathDiagEnable", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

static DWORD ObCJKPathDiagCap()
{
    static int cached = -1;
    if (cached < 0) {
        cached = GetPrivateProfileIntA("obCJK", "PathDiagCap", 300, k_iniMain);
        if (cached != 300 && cached != 500 && cached != 1000) cached = 300;
    }
    return (DWORD)cached;
}

// [obCJK] PathDiagSlot78Only(PathDiagEnable子選項)：只印slot7/8(0-based
// fontID 6/7)，其餘slot不印也不消耗ObCJKPathDiagCap()上限。
// Fix: compares 0-based fontID 6/7 (was 7/8, which is slot8/9).
static bool ObCJKPathDiagSlot78Only()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "PathDiagSlot78Only", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

static inline bool ObCJKPathDiagSlotAllowed(int fontID)
{
    return !ObCJKPathDiagSlot78Only() || fontID == 6 || fontID == 7;
}

static const DWORD kVA_PathA_GlyphDraw = 0x00576919;  // sub_576670 loc_576919
static const DWORD kVA_PathB_GlyphDraw = 0x00575A48;  // sub_575870 loc_575A48
static const DWORD kVA_PathC_GlyphDraw = 0x00576CD1;  // sub_576AB0 movzx eax,byte ptr[esi+4]
static const DWORD kVA_VertexSubmit    = 0x00573F10;  // sub_573F10, reused as-is

static BYTE* g_pathATramp = nullptr;
static BYTE* g_pathBTramp = nullptr;
static BYTE* g_pathCTramp = nullptr;

// Reverse-looks-up 0-based fontID for a FontInfo* (Path A/B only get the
// pointer). Intentionally scans past fontInfos[5] up to kObCJKMaxFontIDNative,
// then falls back to the NorthernUI bridge.
static int ObCJKFontIDFromInfoPtr(void* fontInfoPtr)
{
    FontManager* mgr = FontManager::GetSingleton();
    if (mgr) {
        FontManager::FontInfo** slots = (FontManager::FontInfo**)mgr;  // fontInfos[] is offset 0
        for (int i = 0; i < kObCJKMaxFontIDNative; i++) {
            if ((void*)slots[i] == fontInfoPtr) return i;
        }
    }
    // Not a native slot — check the NorthernUI bridge. -1 if compat is off
    // or the pointer is unrecognized.
    return ObCJKNorthernUIFontIDFromInfoPtr(fontInfoPtr);
}

// Inverse of the above: fontID -> FontInfo* (Path C's node only carries a
// fontID; ObCJKTexUpload_GetOrPlaceGlyph() needs the FontInfo*).
static void* ObCJKFontInfoPtrFromID(int fontID)
{
    if (fontID < 0 || fontID >= kObCJKMaxFontID) return nullptr;
    if (fontID >= kObCJKMaxFontIDNative)
        return ObCJKNorthernUIFontInfoPtrFromCompactID(fontID);
    FontManager* mgr = FontManager::GetSingleton();
    if (!mgr) return nullptr;
    FontManager::FontInfo** slots = (FontManager::FontInfo**)mgr;
    return (void*)slots[fontID];
}

// Path A lead/trail state machine spans two consecutive hook firings: the
// lead-byte firing substitutes a zero-size placeholder (keeps sub_573F10's
// pen bookkeeping consistent), the trail-byte firing substitutes the real
// merged-width glyph.
static ObCJKNativeGlyphEntry g_paBlankGlyph = {};  // all-zero: width/height/advance=0

static BYTE  g_paPendingLead = 0;
static BYTE* g_paGlyphPtr    = nullptr;
static bool  g_paIsCJK       = false;
// diagnostic line counter for PathA CJK lines, gated against ObCJKPathDiagCap()
static DWORD g_paHits        = 0;

// [line-height] tallest substituted glyph (gmBlackBoxY, CJK or ASCII) since
// the last line step. Consumed+reset by ObCJKPathALineHeightHook.
static int g_paMaxLineHeight = 0;

// [1-a] ASCII/half-width substitution: single byte, no lead/trail merge.
// Reuses g_paGlyphPtr/g_paIsCJK. Separate diag counter so ASCII hits don't
// use up the CJK diagnostics' ObCJKPathDiagCap() budget.
static DWORD g_paAsciiHits = 0;

static void ObCJKPathAAsciiCheck(BYTE currentByte, void* fontInfoPtr)
{
    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0) return;
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;

    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, (WORD)currentByte, fontInfoPtr);
    bool ready = (glyph && glyph->texSwapReady);
    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_paAsciiHits++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_paAsciiHits <= ObCJKPathDiagCap()) {
        if (ready)
            _MESSAGE("obCJK:PathA:glyph(ascii) slot=%d code=0x%02X char='%c'",
                     ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte);
        else
            _WARNING("obCJK:PathA:MISS(ascii) slot=%d code=0x%02X byte='%c' valid=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte, glyph ? (int)glyph->valid : -1);
    }
    if (ready) {
        g_paGlyphPtr = (BYTE*)&glyph->native;
        g_paIsCJK = true;
        // [line-height] Fix: track substituted ASCII glyph height too, so a
        // taller substituted face doesn't overlap the next line.
        int h = (int)glyph->gm.gmBlackBoxY;
        if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
    }
}

// `currentByte`/`fontInfoPtr` = `bl`/`edi` at hook VA 0x576919 (edi =
// FontInfo*, set at sub_576670 entry and not reassigned before the hook).
static void __cdecl ObCJKPathACheck(BYTE currentByte, void* fontInfoPtr)
{
    g_paIsCJK = false;

    // slot4: leave native handling untouched. Fix: checked before arming the
    // lead-byte placeholder, otherwise the lead byte is already drawn blank
    // and the character disappears instead of staying native.
    {
        int fontIDGuard = ObCJKFontIDFromInfoPtr(fontInfoPtr);
        if (fontIDGuard >= 0 && ObCJKIsFontIDExcluded(fontIDGuard)) {
            g_paPendingLead = 0;  // don't leave a stale pending-lead from a
                                   // prior (non-excluded) string
            return;
        }
    }

    if (g_paPendingLead != 0) {
        BYTE lead = g_paPendingLead;
        g_paPendingLead = 0;

        bool trailOk = (currentByte != 0 && ObCJKIsTrailByte(currentByte, g_activeCodePage));
        if (trailOk) {
            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            WORD code = (WORD)((lead << 8) | currentByte);
            if (fontID >= 0) {
                ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, code, fontInfoPtr);
                bool ready = (glyph && glyph->texSwapReady);
                bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
                if (diagSlotOk) g_paHits++;
                if (ObCJKPathDiagEnabled() && diagSlotOk && g_paHits <= ObCJKPathDiagCap()) {
                    if (ready)
                        _MESSAGE("obCJK:PathA:glyph(cjk) slot=%d code=0x%04X bytes=%c%c",
                                 ObCJKSlotFromFontID(fontID), code, (char)lead, (char)currentByte);
                    else
                        _WARNING("obCJK:PathA:MISS(cjk) slot=%d code=0x%04X bytes=%c%c valid=%d texSwapReady=%d — falls back to native",
                                  ObCJKSlotFromFontID(fontID), code, (char)lead, (char)currentByte,
                                  glyph ? (int)glyph->valid : -1, glyph ? (int)glyph->texSwapReady : -1);
                }
                if (ready) {
                    g_paGlyphPtr = (BYTE*)&glyph->native;
                    g_paIsCJK = true;
                    // [line-height] track the tallest CJK glyph on this line
                    int h = (int)glyph->gm.gmBlackBoxY;
                    if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
                    return;
                }
            }
        } else {
            // fontID only needed here for the slot7/8 diag filter.
            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            if (ObCJKPathDiagEnabled() && ObCJKPathDiagSlotAllowed(fontID) && g_paHits <= ObCJKPathDiagCap()) {
                // lead byte not followed by a valid trail byte; its own draw
                // was already blanked on the prior firing.
                _WARNING("obCJK:PathA:MISS(lead-alone) lead=0x%02X nextByte=0x%02X — not a valid CJK pair, "
                          "lead byte's own draw was already blanked",
                          lead, currentByte);
            }
        }
        // pending lead didn't complete a pair — re-examine currentByte below.
    }

    if (ObCJKIsLeadByte(currentByte, g_activeCodePage)) {
        g_paPendingLead = currentByte;
        g_paGlyphPtr = (BYTE*)&g_paBlankGlyph;
        g_paIsCJK = true;
        return;
    }

    // [1-a] not part of a CJK pair — try ASCII/half-width substitution.
    if (ObCJKIsAsciiCandidate(currentByte))
        ObCJKPathAAsciiCheck(currentByte, fontInfoPtr);
}

// CJK branch (either half of the pair) replays the 6 forwarding
// instructions between hook VA 0x576919 and the native `push eax` at
// 0x57696F, then jumps to 0x57696F with eax = our glyph pointer, so native
// does `push eax` + `mov ecx,edi` + `call sub_573F10` unmodified. Offsets
// are esp-relative after `popad` (= native esp at hook entry, esp+70h frame).
static __declspec(naked) void ObCJKPathAGlyphHook()
{
    __asm {
        pushad
        push edi            // fontInfoPtr
        movzx eax, bl
        push eax            // currentByte
        call ObCJKPathACheck
        add  esp, 8
        popad
        cmp  byte ptr [g_paIsCJK], 0
        je   short patha_passthrough

        mov  ecx, [esp+90h]  ; native 576943: ecx = arg_1C (fix: was 8Ch = arg_18)
        push ecx             ; native 576959
        mov  ecx, [esp+98h]  ; native 57695A: ecx = arg_20
        lea  edx, [esp+34h]  ; native 576968: edx = &var_40
        push edx             ; native 57696C
        push ebp             ; native 57696D
        push ecx             ; native 57696E

        mov  eax, dword ptr [g_paGlyphPtr]
        mov  edx, 0x57696F   ; native "push eax" (arg_0) — falls through to
        jmp  edx             ; "mov ecx,edi" + "call sub_573F10" unmodified

patha_passthrough:
        jmp  dword ptr [g_pathATramp]
    }
}

// ---------------------------------------------------------------------
// Path A per-line height fix (encoding-agnostic, Path A only).
// Fix: sub_576670's per-line Y step (VA 0x5768C4-0x5768D6) computes
// `Y -= (HardcodedLineSpacing + FontInfo->fnt->fontSize)` with a fixed
// per-slot fontSize, so taller substituted glyphs overlap the next line.
// This replaces the fontSize term.
static BYTE* g_paLineHeightTramp = nullptr;  // unused: hook always returns to VA 0x5768D7

static float g_paLineHeightOverrideF = 0.0f;

// Writes the line-step height into g_paLineHeightOverrideF and resets the
// tracker. Line with an obCJK glyph on a taken-over font: exactly
// ObCJKLineHeightForFont (cellHeight + LineGap). Otherwise:
// max(native fontSize, tallest glyph).
static void __cdecl ObCJKPathALineHeightPrepare(float* fontSizePtr, void* fontInfoPtr)
{
    float nativeFontSize = *fontSizePtr;
    float tracked = (float)g_paMaxLineHeight;
    float result = (tracked > nativeFontSize) ? tracked : nativeFontSize;
    if (g_paMaxLineHeight > 0) {
        int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
        if (fontID >= 0 && !ObCJKIsFontIDExcluded(fontID)) {
            int lh = ObCJKLineHeightForFont(fontID);
            if (lh > 0) result = (float)lh;
        }
    }
    g_paLineHeightOverrideF = result;
    g_paMaxLineHeight = 0;
}

// Replaces the 19-byte FPU sequence at VA 0x5768C4-0x5768D6 (6 complete
// instructions, no jump target inside) with the same computation using
// g_paLineHeightOverrideF, then jumps back to 0x5768D7. pushad/popad keeps
// esp at the native frame for the esp-relative FPU operands.
static __declspec(naked) void ObCJKPathALineHeightHook()
{
    __asm {
        pushad
        mov  edx, [edi + 0x38]   // edi = FontInfo* ("this"), same as native's own [edi+0x38] read here
        push edi                 // arg2: fontInfoPtr (for fontID)
        push edx                 // arg1: fontSizePtr
        call ObCJKPathALineHeightPrepare
        add  esp, 8
        popad

        fld  dword ptr [esp + 0x38]                ; ST0 = Y (var_38)
        fild dword ptr [esp + 0x7C]                 ; ST0 = HardcodedLineSpacing, ST1 = Y
        fadd dword ptr [g_paLineHeightOverrideF]    ; ST0 += line height override
        fsubp st(1), st                              ; ST0 = Y - (spacing + override)
        fstp dword ptr [esp + 0x38]                  ; var_38 = result

        mov  eax, 0x5768D7
        jmp  eax
    }
}

static void ObCJKInstallPathALineHeightHook()
{
    if (g_paLineHeightTramp) return;
    ObCJKInstallHook((BYTE*)0x005768C4, (void*)ObCJKPathALineHeightHook, &g_paLineHeightTramp, "PathA_LineHeight");
}

// Sanity guard for pointers read from native registers: Win32 user-mode
// never maps below 0x10000. On failure the byte is treated as non-CJK
// (passthrough).
static inline bool ObCJKIsPlausiblePtr(const void* p)
{
    return (UInt32)p >= 0x10000;
}

// Resolved by ObCJKPathBCheck(): substitute glyph pointer + CJK-pair flag.
// Fires once per glyph draw, no reentrancy risk.
static BYTE* g_pbGlyphPtr = nullptr;
static bool  g_pbIsCJK    = false;
static DWORD g_pbHits     = 0;  // diagnostic line counter, gated against ObCJKPathDiagCap()

// [1-a] Fix: Path B's hook does an extra `add esi, 1` to skip the trail
// byte, which would desync a single ASCII byte. g_pbGlyphReady = substitute
// a glyph (CJK or ASCII); g_pbIsCJK = real pair, do the extra skip.
static bool g_pbGlyphReady = false;

// ASCII/half-width substitution: plain single-byte lookup.
static DWORD g_pbAsciiHits = 0;

static void ObCJKPathBAsciiCheck(BYTE currentByte, void* fontInfoPtr)
{
    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0) return;
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;

    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, (WORD)currentByte, fontInfoPtr);
    bool ready = (glyph && glyph->texSwapReady);
    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pbAsciiHits++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pbAsciiHits <= ObCJKPathDiagCap()) {
        if (ready)
            _MESSAGE("obCJK:PathB:glyph(ascii) slot=%d code=0x%02X char='%c'",
                     ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte);
        else
            _WARNING("obCJK:PathB:MISS(ascii) slot=%d code=0x%02X byte='%c' valid=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte, glyph ? (int)glyph->valid : -1);
    }
    if (ready) {
        g_pbGlyphPtr = (BYTE*)&glyph->native;
        g_pbGlyphReady = true;  // NOT g_pbIsCJK — single byte, no extra skip
    }
}

// `buf`/`idx`/`fontInfoPtr` = `eax`/`esi`/`edi` at hook VA 0x575A48.
static void __cdecl ObCJKPathBCheck(BYTE* buf, int idx, void* fontInfoPtr)
{
    g_pbIsCJK = false;
    g_pbGlyphReady = false;

    if (!ObCJKIsPlausiblePtr(buf) || idx < 0) return;

    BYTE lead = buf[idx];
    if (!ObCJKIsLeadByte(lead, g_activeCodePage)) {
        if (ObCJKIsAsciiCandidate(lead))
            ObCJKPathBAsciiCheck(lead, fontInfoPtr);
        return;
    }

    BYTE trail = buf[idx + 1];
    bool trailOk = (trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage));
    if (!trailOk) return;

    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0) return;  // shouldn't happen — edi should always be one of fontInfos[]
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched

    WORD code = (WORD)((lead << 8) | trail);
    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, code, fontInfoPtr);
    bool glyphReady = (glyph && glyph->texSwapReady);

    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pbHits++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pbHits <= ObCJKPathDiagCap()) {
        if (glyphReady)
            _MESSAGE("obCJK:PathB:glyph(cjk) slot=%d code=0x%04X bytes=%c%c",
                     ObCJKSlotFromFontID(fontID), code, (char)lead, (char)trail);
        else
            _WARNING("obCJK:PathB:MISS(cjk) slot=%d code=0x%04X bytes=%c%c valid=%d texSwapReady=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), code, (char)lead, (char)trail,
                      glyph ? (int)glyph->valid : -1, glyph ? (int)glyph->texSwapReady : -1);
    }
    if (!glyphReady) return;

    g_pbGlyphPtr = (BYTE*)&glyph->native;
    g_pbIsCJK = true;
    g_pbGlyphReady = true;
}

// CJK branch replays the native instructions between hook VA 0x575A48 and
// `call sub_573F10` at 0x575A98, substituting `eax` with our glyph pointer.
static __declspec(naked) void ObCJKPathBGlyphHook()
{
    __asm {
        pushad
        push edi            // fontInfoPtr
        push esi            // idx
        push eax            // buf
        call ObCJKPathBCheck
        add  esp, 12
        popad
        cmp  byte ptr [g_pbGlyphReady], 0
        je   short pathb_passthrough

        cmp  byte ptr [g_pbIsCJK], 0    ; [1-a] only a real CJK pair needs the
        je   short pathb_no_extra       ; extra skip
        add  esi, 1         // extra advance to skip the trail byte
pathb_no_extra:

        mov  edx, dword ptr [esp+44h]
        push edx
        mov  edx, dword ptr [esp+64h]
        lea  ecx, dword ptr [esp+28h]
        push ecx
        push ebp
        push edx

        mov  eax, dword ptr [g_pbGlyphPtr]
        mov  edx, 0x575A98  ; edx's own value was already pushed above, free to reuse as jump target
        jmp  edx

pathb_passthrough:
        jmp  dword ptr [g_pathBTramp]
    }
}

// Resolved by ObCJKPathCCheck(): substitute glyph pointer + CJK-pair flag.
static BYTE* g_pcGlyphPtr = nullptr;
static bool  g_pcIsCJK    = false;
static DWORD g_pcHits     = 0;  // diagnostic line counter, gated against ObCJKPathDiagCap()

// [1-a] ASCII/half-width substitution. Each Path C node is one character,
// so g_pcIsCJK is reused as the substitute flag (no extra skip).
static DWORD g_pcAsciiHits = 0;

static void ObCJKPathCAsciiCheck(BYTE* node, BYTE currentByte)
{
    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int rawFontID = *(int*)(node + 0x00);
    int fontID = ObCJKCompactFontIDFromRaw(rawFontID);  // native passthrough, or NorthernUI bridge
    if (fontID < 0) return;  // unrecognized by-path font (compat off or no match)
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;
    void* fontInfoPtr = ObCJKFontInfoPtrFromID(fontID);
    if (!fontInfoPtr) return;

    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, (WORD)currentByte, fontInfoPtr);
    bool ready = (glyph && glyph->texSwapReady);
    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pcAsciiHits++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pcAsciiHits <= ObCJKPathDiagCap()) {
        if (ready)
            _MESSAGE("obCJK:PathC:glyph(ascii) slot=%d code=0x%02X char='%c'",
                     ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte);
        else
            _WARNING("obCJK:PathC:MISS(ascii) slot=%d code=0x%02X byte='%c' valid=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), currentByte, (char)currentByte, glyph ? (int)glyph->valid : -1);
    }
    if (ready) {
        g_pcGlyphPtr = (BYTE*)&glyph->native;
        g_pcIsCJK = true;
    }
}

// `node` (`esi` at hook VA 0x576CD1): +0x00=font ID, +0x04=char byte,
// +0x05=trail byte (stashed by NodeCopyHook), +0x1C=IMG-override pointer
// (non-zero → skip). FontInfo* resolved via ObCJKFontInfoPtrFromID().
static void __cdecl ObCJKPathCCheck(BYTE* node)
{
    g_pcIsCJK = false;

    if (!ObCJKIsPlausiblePtr(node)) return;

    BYTE lead = node[4];
    if (!ObCJKIsLeadByte(lead, g_activeCodePage)) {
        if (ObCJKIsAsciiCandidate(lead))
            ObCJKPathCAsciiCheck(node, lead);
        return;
    }

    BYTE trail = node[5];
    bool trailOk = (trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage));
    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (!trailOk) return;
    if (imgField != 0) return;  // IMG-override node, leave untouched (legitimate skip, not a failure)

    int rawFontID = *(int*)(node + 0x00);
    int fontID = ObCJKCompactFontIDFromRaw(rawFontID);  // native passthrough, or NorthernUI bridge
    if (fontID < 0) return;  // unrecognized by-path font (compat off or no match)
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    void* fontInfoPtr = ObCJKFontInfoPtrFromID(fontID);
    if (!fontInfoPtr) return;  // fontID out of range or slot never created — shouldn't happen

    WORD code = (WORD)((lead << 8) | trail);
    ObCJKGlyphEntry* glyph = ObCJKTexUpload_GetOrPlaceGlyph(fontID, code, fontInfoPtr);
    bool glyphReady = (glyph && glyph->texSwapReady);

    bool diagSlotOk = ObCJKPathDiagSlotAllowed(fontID);
    if (diagSlotOk) g_pcHits++;
    if (ObCJKPathDiagEnabled() && diagSlotOk && g_pcHits <= ObCJKPathDiagCap()) {
        if (glyphReady)
            _MESSAGE("obCJK:PathC:glyph(cjk) slot=%d code=0x%04X bytes=%c%c",
                     ObCJKSlotFromFontID(fontID), code, (char)lead, (char)trail);
        else
            _WARNING("obCJK:PathC:MISS(cjk) slot=%d code=0x%04X bytes=%c%c valid=%d texSwapReady=%d — falls back to native",
                      ObCJKSlotFromFontID(fontID), code, (char)lead, (char)trail,
                      glyph ? (int)glyph->valid : -1, glyph ? (int)glyph->texSwapReady : -1);
    }
    if (!glyphReady) return;

    g_pcGlyphPtr = (BYTE*)&glyph->native;
    g_pcIsCJK = true;
}

// CJK branch replays `push edx` (the 0x576CD1-0x576CE9 gap), then jumps to
// native VA 0x576CE9 with `edx` pointing at our glyph struct.
static __declspec(naked) void ObCJKPathCGlyphHook()
{
    __asm {
        pushad
        push esi
        call ObCJKPathCCheck
        add  esp, 4
        popad
        cmp  byte ptr [g_pcIsCJK], 0
        je   short pathc_passthrough
        push edx
        mov  edx, dword ptr [g_pcGlyphPtr]
        mov  eax, 0x576CE9
        jmp  eax
pathc_passthrough:
        jmp  dword ptr [g_pathCTramp]
    }
}

static void ObCJKInstallGlyphHooks()
{
    if (!g_pathATramp)
        ObCJKInstallHook((BYTE*)kVA_PathA_GlyphDraw, (void*)ObCJKPathAGlyphHook, &g_pathATramp, "PathA_GlyphDraw");
    if (!g_pathBTramp)
        ObCJKInstallHook((BYTE*)kVA_PathB_GlyphDraw, (void*)ObCJKPathBGlyphHook, &g_pathBTramp, "PathB_GlyphDraw");
    if (!g_pathCTramp)
        ObCJKInstallHook((BYTE*)kVA_PathC_GlyphDraw, (void*)ObCJKPathCGlyphHook, &g_pathCTramp, "PathC_GlyphDraw");
}
