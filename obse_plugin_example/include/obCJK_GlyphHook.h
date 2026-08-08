#pragma once
// Hooks the three native "draw one glyph" call sites (Path A: sub_576670,
// Path B: sub_575870, Path C: sub_576AB0), replacing the native single-byte
// font-table lookup with a Plan B CJK glyph (obCJK_TexUpload.h) before
// tail-calling sub_573F10. This file only calls
// ObCJKTexUpload_GetOrPlaceGlyph() and reads back &entry->native — no
// SetTexture/D3D device calls of this file's own (the old immediate
// SetTexture route was proven architecturally inert; see below).
//
// Plan B背景（為何immediate SetTexture無效、TexSwap/TexUpload兩檔分工）
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_TexUpload.h"  // pulls in obCJK_GlyphAtlas.h (ObCJKGlyphEntry) + obCJK_TexSwap.h
#include "obCJK_NorthernUICompat.h"  // NorthernUI xxnFontPath bridge, see its top comment

// ini key PathDiagEnable ([obCJK] 節，obCJK_iniEdit.py 功能開關) — 預設關閉
// （0）。開啟後下面Path A/B/C各自的glyph替換catch(成功套用自選字型)/
// miss(退回native)訊息才會印出；PathDiagCap（同節，300/500/1000三選一下拉
// 選單）是每條路徑各自的印出行數上限，避免掃過整個選單畫面時洗版obCJK.log。
// 沿用obCJK_GlyphAtlas.h ObCJKAsciiRenderEnabled()同款lazy-static快取。
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

// ini key PathDiagSlot78Only ([obCJK] 節，PathDiagEnable 的子選項，只在上面
// 開關開啟時才有意義) — 開啟後 Path A/B/C 的glyph替換catch/miss訊息只印
// slot7/8（obCJK_iniEdit.py slot7/8＝MenuQue 額外字型，即 FontParam7/8，
// engineID 7/8，0-based fontID 6/7 — 換算見 obCJK_GlyphAtlas.h
// ObCJKSlotFromFontID）那幾行，其餘 slot 的命中既不印也不消耗
// ObCJKPathDiagCap() 的行數上限，讓上限全部留給 slot7/8。
//
// [2026-07-19] Fixed off-by-one: this used to compare the raw 0-based
// fontID against 7/8, which is actually slot8/9, not slot7/8 — slot7
// (fontID 6) never passed the filter and so never printed even with
// PathDiagSlot78Only enabled.
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

static const DWORD kVA_PathA_GlyphDraw = 0x00576919;  // sub_576670 loc_576919 (obtc Hook #23)
static const DWORD kVA_PathB_GlyphDraw = 0x00575A48;  // sub_575870 loc_575A48 (obja Hook #24)
static const DWORD kVA_PathC_GlyphDraw = 0x00576CD1;  // sub_576AB0 movzx eax,byte ptr[esi+4]
static const DWORD kVA_VertexSubmit    = 0x00573F10;  // sub_573F10 — reused as-is (方案A)

static BYTE* g_pathATramp = nullptr;
static BYTE* g_pathBTramp = nullptr;
static BYTE* g_pathCTramp = nullptr;

// Reverse-looks-up 0-based fontID for a FontInfo* (Path B/A only get the
// pointer, not the int; Path C's node already carries fontID at +0x00).
// Intentional OOB scan past fontInfos[5] to kObCJKMaxFontID — see
// Hook內容與規範.md「三、」. Moved above Path A (2026-07) since Path A's
// state machine now needs it too and C++ has no forward-hoisting for this.
static int ObCJKFontIDFromInfoPtr(void* fontInfoPtr)
{
    FontManager* mgr = FontManager::GetSingleton();
    if (mgr) {
        FontManager::FontInfo** slots = (FontManager::FontInfo**)mgr;  // fontInfos[] is offset 0
        for (int i = 0; i < kObCJKMaxFontIDNative; i++) {
            if ((void*)slots[i] == fontInfoPtr) return i;
        }
    }
    // Not a native slot — check the NorthernUI bridge (obCJK_NorthernUICompat.h)
    // before giving up. -1 if compat is off or the pointer is unrecognized.
    return ObCJKNorthernUIFontIDFromInfoPtr(fontInfoPtr);
}

// Inverse of the above: Path C's node only carries a 0-based fontID
// (node+0x00), not a FontInfo* — but ObCJKTexUpload_GetOrPlaceGlyph() needs
// the FontInfo* to walk FontInfo+0x0C -> NiTexturingProperty -> ... -> the
// real D3D texture (obCJK_TexUpload.h). Same fontInfos[] array/indexing as
// ObCJKFontIDFromInfoPtr, just read forward instead of scanned.
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

// Path A lead/trail state machine spans TWO consecutive hook firings (no
// [eax+1] trail-byte peek here, unlike Path B/C — see derivation): lead-byte
// firing substitutes a zero-size placeholder so sub_573F10's own pen
// bookkeeping stays consistent; trail-byte firing substitutes the real
// merged-width glyph. Design rationale: D3D替換文字.md「一、」.
static ObCJKNativeGlyphEntry g_paBlankGlyph = {};  // all-zero: width/height/advance=0

static BYTE  g_paPendingLead = 0;
static BYTE* g_paGlyphPtr    = nullptr;
static bool  g_paIsCJK       = false;
// diagnostic line counter — gates PathA:glyph(cjk)/PathA:MISS(cjk|lead-alone)
// against ObCJKPathDiagCap() when ObCJKPathDiagEnabled() is on (ini
// PathDiagEnable/PathDiagCap, obCJK_iniEdit.py 功能開關). Previously a fixed
// 300 (raised from 40, same story as obCJK_WordWrapHook.h's kWWDiagCap —
// the failure branches (pending lead's trail byte didn't pan out / fontID
// lookup failed / glyph not yet placed) log via _WARNING so a desync (lead
// byte draws a blank placeholder, then its trail byte gets re-examined as a
// fresh byte instead of completing the pair) is actually visible in the
// log); now user-configurable (300/500/1000).
static DWORD g_paHits        = 0;

// [line-height] tallest glyph (gmBlackBoxY) hit since the last line step
// — CJK pairs (below) and substituted ASCII/half-width faces
// (ObCJKPathAAsciiCheck above, widened 2026-07-31 to match
// obCJK_LineBreakHook.h's line+0x20 fix, which covers the analogous
// Path C case for the same reason: a taller GDI face substituted for an
// ASCII slot can underrun the per-line Y-step just like a CJK glyph
// can). Consumed+reset by ObCJKPathALineHeightHook (VA 0x5768c4, defined
// after ObCJKPathAGlyphHook below), which take-maxes it against
// sub_576670's own fixed per-font-slot fontSize before that function's
// per-line Y-cursor subtraction.
static int g_paMaxLineHeight = 0;

// [1-a] ASCII/half-width substitution — same TexUpload path as CJK, no
// lead/trail merge needed (single byte; Path A's underlying native loop
// already advances exactly 1 byte per firing regardless, see the "no += 1
// skip trick needed" comment on g_paIsCJK's declaration above). Reuses
// g_paGlyphPtr/g_paIsCJK — the naked asm below doesn't care whether the
// substituted glyph came from a CJK pair or a single ASCII byte, it just
// pushes whatever pointer this call left there. Separate diag counter from
// g_paHits so the much higher frequency of ASCII hits doesn't crowd out the
// CJK diagnostics that share the same ObCJKPathDiagCap() budget.
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
        // [line-height] same take-max tracking as the CJK branch below —
        // widened to cover this ASCII/half-width branch too (2026-07-31,
        // user-requested): if AsciiRenderEnable substitutes a GDI face
        // taller than the native western fontSize for this slot, the same
        // per-line Y-step underrun/overlap bug can recur even on an
        // ASCII-only or mixed line. Mirrors obCJK_LineBreakHook.h's
        // line+0x20 fix, which was widened the same way for the same
        // reason. 
        int h = (int)glyph->gm.gmBlackBoxY;
        if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
    }
}

// `currentByte`/`fontInfoPtr` = `bl`/`edi` at hook VA 0x576919. `edi` is set
// once at function entry (sub_576670+0x27 `mov edi,ecx`) and never
// reassigned before the hook point, and `[edi+0x38]`=font_base is read by
// the very table-lookup formula we're replacing — same FontInfo*-shaped
// pointer ObCJKFontIDFromInfoPtr() already resolves for Path B.
//
// No device-ready guard before arming the lead-byte placeholder — it's an
// all-zero struct with no texture reference, nothing here touches D3D state.
static void __cdecl ObCJKPathACheck(BYTE currentByte, void* fontInfoPtr)
{
    g_paIsCJK = false;

    // slot4: leave native handling untouched. Must be checked here, before
    // the lead-byte branch below has a chance to arm the blank placeholder
    // state machine — fontID can't be resolved until the trail-byte firing,
    // but by then the lead byte would already have been drawn as zero-width
    // (see "lead byte already drew blank" comment below), which would delete
    // the character instead of leaving it native. Checking fontInfoPtr's
    // fontID up front avoids ever entering the pending-lead machinery for
    // this slot.
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
                    // [line-height] track the tallest CJK glyph seen since
                    // the last line step, consumed+reset by
                    // ObCJKPathALineHeightHook below (VA 0x5768c4). See
                    // PathA主要文字顯示.md 第25節.
                    int h = (int)glyph->gm.gmBlackBoxY;
                    if (h > g_paMaxLineHeight) g_paMaxLineHeight = h;
                    return;
                }
            }
        } else {
            // fontID not otherwise resolved on this branch (trailOk was
            // false, so the CJK-hit code above never ran) — resolve it here
            // purely for the slot7/8-only diag filter below.
            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            if (ObCJKPathDiagEnabled() && ObCJKPathDiagSlotAllowed(fontID) && g_paHits <= ObCJKPathDiagCap()) {
                // lead byte (0x81-0xFE for BIG5/GBK) wasn't followed by a valid
                // trail byte — not a real CJK pair. Its own draw was already
                // replaced by a zero-size blank placeholder on the PRIOR firing.
                _WARNING("obCJK:PathA:MISS(lead-alone) lead=0x%02X nextByte=0x%02X — not a valid CJK pair, "
                          "lead byte's own draw was already blanked",
                          lead, currentByte);
            }
        }
        // pending lead didn't pan out (bad trail / no glyph / not yet placed
        // in the real texture) — its own placeholder draw already happened
        // on the prior firing, so just fall through and re-examine
        // currentByte fresh below.
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

// CJK branch (either half of the pair) replays the 6 byte-independent
// forwarding instructions between hook VA 0x576919 and the native
// `push eax` at 0x57696F, substituting `eax` with our glyph pointer, then
// jumps into native code at 0x57696F itself so it does `push eax` (with our
// value) + `mov ecx,edi` + `call sub_573F10` unmodified. Offsets below are
// esp-relative to THIS naked function's esp right after `popad` (which
// equals the original function's esp at hook entry, IDA's "esp+70h" frame
// baseline) — derived by adding each push's 4 bytes to IDA's frame-constant
// math.
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

        mov  ecx, [esp+90h]  ; native 576943: ecx = arg_1C (0x70 frame + 0x20 arg_1C offset;
                              ; previously 8Ch = 0x70+0x1C, which is arg_18 by mistake
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
// Path A per-line height fix (encoding-agnostic — same native VA and
// same fix regardless of DBCS/UTF-8 mode; see g_paMaxLineHeight comment
// above and obCJK_GlyphHook_UTF8.h's matching CJK-ready + ASCII-ready
// branches).
//
// sub_576670's own per-line Y-cursor step (VA 0x5768c4-0x5768d6, pure
// FPU arithmetic, fires once per line break inside the same byte-by-byte
// draw loop Path A hooks above) computes
// `Y -= (HardcodedLineSpacing + FontInfo->fnt->fontSize)` — fontSize is
// a single constant baked in at .fnt load time for the whole font slot,
// never checked against any glyph actually drawn on that line. CJK
// glyphs are substituted at runtime (TexUpload/TexSwap) and can be
// taller than the native western fontSize this step still uses, so the
// next line's Y doesn't drop far enough and visually overlaps the
// previous one — the NorthernUI loading_menu.xml-class symptom. None of
// the affected XMLs set <ishtml>, so they run this Path A loop, not the
// Path C word-wrap tree obCJK_LineBreakHook.h's line+0x20 fix targets —
// that fix is for book/scroll (ishtml=true) content only and does not
// apply here.
//
// [Path A only] this patch touches VA 0x5768c4, inside sub_576670. It
// has no effect on Path B (sub_575870) or Path C (sub_576AB0/sub_577840)
// — confirmed via sub_592390's ishtml branch.
static BYTE* g_paLineHeightTramp = nullptr;  // allocated but never jumped
// to — this hook always redirects to the fixed VA 0x5768d7 itself, same
// pattern as obCJK_LineBreakHook.h's g_lineBreakTramp.

// Loads FontInfo->fnt->fontSize (native's own `[edx]` operand at this
// point, edx = [edi+0x38], edi = FontInfo* = "this" — same pointer shape
// ObCJKFontIDFromInfoPtr() resolves elsewhere in this file) and writes
// max(fontSize, g_paMaxLineHeight) into g_paLineHeightOverrideF for the
// naked hook below to fadd directly from memory, then resets the
// tracker for the next line.
static float g_paLineHeightOverrideF = 0.0f;

static void __cdecl ObCJKPathALineHeightPrepare(float* fontSizePtr)
{
    float nativeFontSize = *fontSizePtr;
    float tracked = (float)g_paMaxLineHeight;
    g_paLineHeightOverrideF = (tracked > nativeFontSize) ? tracked : nativeFontSize;
    g_paMaxLineHeight = 0;
}

// Replaces the 19-byte, branch-free FPU sequence at VA 0x5768c4-0x5768d6
// (get_bytes-verified: 6 complete instructions, nothing split, no jump
// target lands inside it) with an equivalent computation that
// substitutes the `fontSize` term, then jumps back to the native `jmp
// short loc_576919` at 0x5768d7 (unmodified, outside the patched range).
// pushad/popad wraps the C helper call so the esp-relative var_38/arg_8
// frame the FPU instructions below depend on (this hook enters via jmp,
// not call, so esp at entry is the surrounding native function's own
// frame — same reasoning as every other naked hook in this file) is back
// to its entry value by the time they run; edi (FontInfo*) survives
// pushad/popad unclobbered same as every register there.
static __declspec(naked) void ObCJKPathALineHeightHook()
{
    __asm {
        pushad
        mov  edx, [edi + 0x38]   // edi = FontInfo* ("this"), same as native's own [edi+0x38] read here
        push edx
        call ObCJKPathALineHeightPrepare
        add  esp, 4
        popad

        fld  dword ptr [esp + 0x38]                ; ST0 = Y (var_38)
        fild dword ptr [esp + 0x7C]                 ; ST0 = HardcodedLineSpacing, ST1 = Y
        fadd dword ptr [g_paLineHeightOverrideF]    ; ST0 += max(fontSize, tracked CJK height)
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

// Cheap sanity guard for pointers sourced from native registers at hook
// sites whose register-safety proof only covers call paths actually traced.
// Win32 user-mode never maps addresses below 0x10000 (guard page). On
// failure the byte is treated as non-CJK (safe passthrough), not a real fix
// for the root register mismatch. why this avoids re-deriving 23+ call sites.
static inline bool ObCJKIsPlausiblePtr(const void* p)
{
    return (UInt32)p >= 0x10000;
}

// Resolved by ObCJKPathBCheck(): substitute glyph pointer + CJK-pair flag.
// Fires once per glyph draw, no reentrancy risk.
static BYTE* g_pbGlyphPtr = nullptr;
static bool  g_pbIsCJK    = false;
static DWORD g_pbHits     = 0;  // diagnostic line counter, gated against ObCJKPathDiagCap() — see g_paHits comment

// [1-a] Unlike Path A/C, Path B's naked hook does an extra `add esi, 1` to
// skip the trail byte whenever it substitutes — that's correct for a real
// CJK pair (2 source bytes consumed) but would desync the byte stream for a
// single ASCII byte (1 source byte, native code already advances esi by 1
// on its own). So substitution and "needs the extra skip" must be two
// separate flags now: g_pbIsCJK keeps its original meaning (real pair, do
// the extra skip) and is ONLY set true by the CJK branch; g_pbGlyphReady is
// the general "do we substitute a glyph pointer at all" flag, set true by
// either branch. The asm below checks g_pbGlyphReady first, then gates the
// extra skip on g_pbIsCJK specifically.
static bool g_pbGlyphReady = false;

// ASCII/half-width substitution, same idea as ObCJKPathAAsciiCheck — see
// that function's comment. Path B has no pending-lead state machine to
// interact with (its CJK branch peeks buf[idx+1] directly), so this is a
// plain single-byte lookup.
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

// `buf`/`idx`/`fontInfoPtr` = `eax`/`esi`/`edi` at hook VA 0x575A48, all
// unclobbered at this point. Register-safety proof: Hook內容與規範.md「五、」.
// [2026-07-10] Proof only covers the call paths traced so far.
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
// Blueprint/esp-offset derivation: Hook內容與規範.md「七、」.
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
        je   short pathb_no_extra       ; extra skip — see g_pbGlyphReady comment
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
static DWORD g_pcHits     = 0;  // diagnostic line counter, gated against ObCJKPathDiagCap() — see g_paHits comment

// [1-a] ASCII/half-width substitution, same idea as ObCJKPathAAsciiCheck.
// Path C's naked hook (below) has no esi/index bookkeeping of its own at
// all — it walks a pre-built node tree where each node already represents
// exactly one logical character, CJK or ASCII — so unlike Path B, reusing
// g_pcIsCJK directly for both "substitute" and "is a real pair" is safe:
// there is no extra-skip side effect gated on it here.
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
// (non-zero → skip).
//
// node+0x00 is a plain fontID int, not a FontInfo* — ObCJKTexUpload_GetOrPlaceGlyph()
// needs the FontInfo*, resolved via ObCJKFontInfoPtrFromID() first.
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
