#pragma once
// Font-texture swap-at-load hook: single 4-byte rel32 patch at VA 0x5749BB
// (sub_5744E0's call to sub_704800) swaps the first .tex page's NiPixelData
// for an enlarged copy (native pixels + reserved CJK region below), letting
// the engine's own CreateTexture path build CJK glyphs into the same native
// texture. Full insertion-point derivation, 2026-07-10 sizing/Plan-B
// decisions, and NiPixelData/FontInfo field offsets:
// D3D替換文字.md.

#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_GlyphAtlas.h"
#include "obCJK_Path.h"  // k_iniMain
#include "obCJK_NorthernUICompat.h"  // ObCJKNorthernUIResolveByOwnPath — see fontID0 fallback below

// --- Native addresses (Oblivion.exe 1.2.416) -------------------------------
// Call site inside sub_5744E0's per-page .tex loop. Byte at +0 must be E8;
// rel32 operand lives at +1..+4. Its return address 0x5749C0 is the exact
// fingerprint obtc.dll matched via SEH stack peeking — we patch the call
// operand directly instead (single 4-byte data patch, no trampoline, no
// ObCJKGetInstrLen involvement).
static const DWORD kVA_TexSwapCallSite  = 0x005749BB;
static const DWORD kVA_Sub704800        = 0x00704800; // NiTexturingProperty ctor (builds NiSourceTexture via sub_701FC0)
static const DWORD kVA_NiPixelDataCtor  = 0x0070E560; // __thiscall (this, W, H, NiPixelFormat*, mipLevels, numFaces), retn 14h
static const DWORD kVA_FormHeapAllocate = 0x00401F00; // void* __cdecl (UInt32 size) — xOBSE GameAPI.cpp:111

// --- NiPixelData field offsets (D3D替換文字.md「三、」, sub_70E560 完整解出) ---
static const DWORD kNiPD_Format  = 0x08; // embedded 0x44-byte NiPixelFormat copy
static const DWORD kNiPD_Buffer  = 0x50; // BYTE* raw pixels (same block as the arrays below)
static const DWORD kNiPD_Widths  = 0x54; // DWORD* per-mip widths
static const DWORD kNiPD_Heights = 0x58; // DWORD* per-mip heights
static const DWORD kNiPD_Bpp     = 0x64; // bytesPerPixel (4 for the font .tex RGBA format)
static const DWORD kNiPD_Size    = 0x70; // sizeof(NiPixelData)

// --- FontInfo field offsets (xOBSE GameAPI.h FontManager::FontInfo) --------
static const DWORD kFontInfo_Path   = 0x04;  // char* "Data\Fonts\XXX.fnt"
static const DWORD kFontInfo_ID     = 0x08;  // UInt16, 1-based engine font ID (MenuQue extras: 7/8)
static const DWORD kFontInfo_FntBuf = 0x38;  // void*, 0x3928-byte .fnt file buffer
static const DWORD kFntBuf_Entries  = 0x128; // buffer+0x128: 256 × 56-byte glyph entries (font_base+byte*56+0x128)

static const int kObCJKTexSwapBaseSize = 2048; // fixed default (user decision 2026-07-10)
static const int kObCJKTexSwapMaxSize  = 4096; // overflow fuse — 64MB RGBA per font, warn loudly

static bool  g_texSwapEnabled      = true;
static void* g_texSwapCont         = (void*)kVA_Sub704800; // stub's continuation target

// Per-fontID state for the reserved-but-lazily-filled CJK region inside the
// swapped (enlarged) native font texture. Populated in ObCJKTexSwapCheckImpl
// once a font's texture is actually enlarged, with a fresh shelf-pack cursor.
// Consumed by obCJK_TexUpload.h's Plan B on-demand placement — see that
// file's top comment. fontID here is 0-based (engineID - 1), matching
// obCJK_GlyphAtlas.h's convention.
struct ObCJKTexSwapRegion {
    bool active;
    int  texSize;
    int  startY;  // row just below the native ASCII strip — reset floor, never reclaim above this
    int  penX, penY, rowHeight;
    bool overflowWarned;
};
static ObCJKTexSwapRegion g_texSwapRegion[kObCJKMaxFontID] = {};

// g_texSwapEnabled gate here too (not just in ObCJKTexSwapCheckImpl): once
// ObCJKTexSwapReloadEnableFlag() below flips it false at runtime, this must
// stop handing out the region so obCJK_TexUpload.h's PlaceGlyphImpl treats
// EVERY fontID as "never swapped" for any *new* placement — plain
// region.active alone can't express that, because a font that was already
// swapped earlier this session keeps region.active==true forever (nothing
// ever clears it back to false).
static ObCJKTexSwapRegion* ObCJKTexSwapGetRegion(int fontID0)
{
    if (!g_texSwapEnabled) return nullptr;
    if (fontID0 < 0 || fontID0 >= kObCJKMaxFontID) return nullptr;
    return g_texSwapRegion[fontID0].active ? &g_texSwapRegion[fontID0] : nullptr;
}

// Live "turn off" for the TexSwapEnable debug switch (obCJK_iniEdit.py's
// "一鍵關閉CJK顯示" checkbox): re-reads ini after the editor closes and, if
// the user flipped it to 0, (1) clears g_texSwapEnabled so
// ObCJKTexSwapGetRegion() above stops handing out regions for any *new*
// glyph placement, and (2) calls ObCJKGlyphAtlas_Reset() so every already-
// cached ObCJKGlyphEntry (texSwapReady==true from earlier this session) gets
// freed and re-placed from scratch on next draw — otherwise
// ObCJKTexUpload_PlaceGlyphImpl's "already placed by a previous call here"
// early-out (obCJK_TexUpload.h:90) would keep showing every character the
// player has already seen this session as CJK forever, since that check
// never looks at g_texSwapEnabled. One-directional by design — if
// TexSwapEnable was already 0 at plugin load, ObCJKInstallTexSwapHook()
// never patched the call site at all, so there is no live path back to 1
// without re-doing that patch; this function only ever sets the flag false,
// never true. Called from main.cpp's editor-close handler.
static void ObCJKTexSwapReloadEnableFlag()
{
    if (g_texSwapEnabled &&
        GetPrivateProfileIntA("obCJK", "TexSwapEnable", 1, k_iniMain) == 0) {
        g_texSwapEnabled = false;
        ObCJKGlyphAtlas_Reset();
        _MESSAGE("obCJK:TexSwap:ObCJKTexSwapReloadEnableFlag: disabled via ini after editor close, atlas reset to drop cached CJK placements");
    }
}

// [2026-07-17] There used to be an ObCJKTexSwapResetRegions() here that
// rewound every active region's shelf-pack cursor back to its starting row
// on a font-settings hot-reload, so repeated in-session reloads would reuse
// the same swapped-texture space instead of growing forever. Removed: some
// already-visible UI text (e.g. the main menu's static bottom hotkey row)
// isn't redrawn every frame, so its baked UV keeps pointing at its old rect
// in the shared native font texture even after ObCJKGlyphAtlas_Reset()
// invalidates every ObCJKGlyphEntry. Rewinding the cursor let a freshly
// placed (but unrelated) glyph land in that same rect, flipping the stale,
// never-redrawn text to a different — still valid-looking, but wrong — CJK
// glyph (obcjk_refresh_bug_001.png). The cursor now only ever advances
// forward (see ObCJKTexUpload_PlaceGlyphImpl in obCJK_TexUpload.h), so an
// old rect is never overwritten by a new placement. Trade-off: the
// fixed-size reserved region is consumed monotonically across repeated
// in-session font reloads instead of being recycled — already handled
// gracefully by TexUpload's overflow fuse (that file's overflowWarned path
// just leaves further new glyphs blank until the next level/texture reload).

// The actual per-call decision + swap. Returns the NiPixelData* that
// sub_704800 should consume: the original (pass through) or our enlarged
// replacement. Called on EVERY font page's texture build; only the first
// page per FontInfo is swapped (the UI text pipeline only ever binds
// texture array index 0 — 多張texture page機制查明).
static void* ObCJKTexSwapCheckImpl(BYTE* pixelData, BYTE* fontInfo)
{
    // Font loads are strictly sequential on the main thread, so "same
    // FontInfo as last call" == "next page of the same font". A reload
    // constructs a fresh FontInfo (FormHeap allocation), which resets this.
    static BYTE* s_lastFontInfo = nullptr;
    static int   s_pageIdx = -1;
    if (fontInfo != s_lastFontInfo) { s_lastFontInfo = fontInfo; s_pageIdx = 0; }
    else s_pageIdx++;

    DWORD* widths  = *(DWORD**)(pixelData + kNiPD_Widths);
    DWORD* heights = *(DWORD**)(pixelData + kNiPD_Heights);
    DWORD  oldW = widths[0], oldH = heights[0];
    DWORD  bpp  = *(DWORD*)(pixelData + kNiPD_Bpp);
    WORD   engineID = *(WORD*)(fontInfo + kFontInfo_ID);

    if (!g_texSwapEnabled) return pixelData;
    if (s_pageIdx != 0) return pixelData;

    int fontID0 = (int)engineID - 1;  // FontInfo::ID is 1-based; atlas slots are 0-based
    if (fontID0 < 0 || fontID0 >= kObCJKMaxFontID) {
        // Not a native slot ID. This hook fires *during* font construction
        // (mid .tex-file read), before FontShim finishes writing its own
        // rawID/broadcast into this FontInfo — see
        // ObCJKNorthernUIResolveByOwnPath's comment in
        // obCJK_NorthernUICompat.h. So unlike Path A/B/C's draw-time lookup
        // (ObCJKFontIDFromInfoPtr), we can't use the rawID-based bridge here
        // and must resolve via the FontInfo's own path field instead.
        int roleIndex = ObCJKNorthernUIResolveByOwnPath(fontInfo);
        if (roleIndex >= 0) {
            fontID0 = kObCJKMaxFontIDNative + roleIndex;
        } else {
            _WARNING("obCJK:TexSwap: engineID=%u out of range and NorthernUI path bridge did not resolve — passing native pixelData through (fontInfo=%p)", engineID, fontInfo);
            return pixelData;
        }
    }
    if (bpp != 4 || oldW == 0 || oldH == 0 || oldW > (DWORD)kObCJKTexSwapMaxSize || oldH > (DWORD)kObCJKTexSwapMaxSize) {
        _WARNING("obCJK:TexSwap: unexpected native format (%ux%u bpp=%u) — passing through", oldW, oldH, bpp);
        return pixelData;
    }

    // CJK region sits below the native strip; 1px gap against filter bleed.
    int startY = (int)oldH + 1;

    // Overflow fuse: escalate to 4096 only if the native strip itself doesn't
    // fit at the fixed 2048 base size.
    int texSize = kObCJKTexSwapBaseSize;
    if ((int)oldW > texSize || (int)oldH > texSize) {
        _WARNING("obCJK:TexSwap: fuse tripped for engineID=%u (native %ux%u) — escalating to %d (64MB RGBA, watch FormHeap pressure)",
                 engineID, oldW, oldH, kObCJKTexSwapMaxSize);
        texSize = kObCJKTexSwapMaxSize;
    }

    // Build the enlarged NiPixelData with the engine's own allocator+ctor so
    // downstream refcount/destruction handles it exactly like the native one
    // (FormHeap_Allocate(0x70) + sub_70E560, mirroring sub_5744E0 itself).
    // NiPixelFormat is copied from the original's embedded block; the font
    // path always uses mipLevels=1, numFaces=1.
    typedef void* (__cdecl* FormHeapAlloc_t)(DWORD size);
    typedef void  (__fastcall* NiPixelDataCtor_t)(void* self, void* edxUnused,
                                                  DWORD w, DWORD h, const void* fmt,
                                                  DWORD mips, DWORD faces);
    BYTE* newPd = (BYTE*)((FormHeapAlloc_t)kVA_FormHeapAllocate)(kNiPD_Size);
    ((NiPixelDataCtor_t)kVA_NiPixelDataCtor)(newPd, NULL,
        (DWORD)texSize, (DWORD)texSize, pixelData + kNiPD_Format, 1, 1);

    BYTE* newBuf = *(BYTE**)(newPd + kNiPD_Buffer);
    BYTE* oldBuf = *(BYTE**)(pixelData + kNiPD_Buffer);
    memset(newBuf, 0, (DWORD)texSize * texSize * 4);
    for (DWORD y = 0; y < oldH; y++)
        memcpy(newBuf + (DWORD)y * texSize * 4, oldBuf + (DWORD)y * oldW * 4, oldW * 4);

    // Reserve the CJK region for Plan B dynamic upload (obCJK_TexUpload.h) —
    // fresh shelf-pack cursor, no load-time pre-bake anymore.
    ObCJKTexSwapRegion& region = g_texSwapRegion[fontID0];
    region.active        = true;
    region.texSize        = texSize;
    region.startY          = startY;
    region.penX           = 0;
    region.penY           = startY;
    region.rowHeight      = 0;
    region.overflowWarned = false;

    // Native glyph UVs are normalized against the OLD texture size — rescale
    // all 256 entries so ASCII keeps rendering from the (now smaller) native
    // corner. Entries belonging to pages >0 of multi-page fonts get scaled
    // too, but those pages never render anyway (pipeline binds index 0 only).
    // Done LAST so an exception earlier can't leave a half-swapped font with
    // half-scaled UVs.
    BYTE* fntBuf = *(BYTE**)(fontInfo + kFontInfo_FntBuf);
    if (fntBuf) {
        float su = (float)oldW / (float)texSize;
        float sv = (float)oldH / (float)texSize;
        static const DWORD kUOffs[4] = { 0x04, 0x0C, 0x14, 0x1C };
        static const DWORD kVOffs[4] = { 0x08, 0x10, 0x18, 0x20 };
        for (int i = 0; i < 256; i++) {
            BYTE* e = fntBuf + kFntBuf_Entries + (DWORD)i * 56;
            for (int k = 0; k < 4; k++) {
                *(float*)(e + kUOffs[k]) *= su;
                *(float*)(e + kVOffs[k]) *= sv;
            }
        }
    } else {
        _WARNING("obCJK:TexSwap: FontInfo+0x38 fnt buffer is NULL at swap time — native UVs NOT rescaled, ASCII will look wrong (engineID=%u)", engineID);
    }

    // Original NiPixelData orphaned from here on — accepted one-time leak
    // (D3D替換文字.md「二、」, user decision 2026-07-10).
    return newPd;
}

// SEH firewall: any unexpected fault inside the swap logic (a wrong
// assumption about registers/offsets) degrades to "pass the native
// pixelData through untouched" instead of crashing font loading. Kept free
// of C++ objects so __try is legal here; all real work lives in Impl.
static void* __cdecl ObCJKTexSwapCheck(BYTE* pixelData, BYTE* fontInfo)
{
    void* result = pixelData;
    __try {
        result = ObCJKTexSwapCheckImpl(pixelData, fontInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _WARNING("obCJK:TexSwap: EXCEPTION in swap check (fontInfo=%p) — native pixelData passed through", fontInfo);
        result = pixelData;
    }
    return result;
}

// Replaces `call sub_704800` at 0x5749BB. On entry: [esp]=return address
// (0x5749C0), [esp+4]=NiPixelData* argument, ecx=the 0x30-byte object
// sub_704800 constructs into, esi=FontInfo* (sub_5744E0 keeps `this` in esi;
// diag log above verifies this on first run).
static __declspec(naked) void ObCJKTexSwapStub()
{
    __asm {
        pushad
        mov  eax, [esp + 24h]   // NiPixelData* arg ([esp+20h] is the return address after pushad)
        push esi                 // FontInfo*
        push eax
        call ObCJKTexSwapCheck
        add  esp, 8
        mov  [esp + 24h], eax   // hand sub_704800 whichever NiPixelData the check chose
        popad                    // restores ecx (sub_704800's this) untouched
        jmp  dword ptr [g_texSwapCont]
    }
}

// Single 4-byte rel32 patch. Verifies the site still holds the vanilla
// `E8 <rel32 to sub_704800>` before touching anything, so a wrong game
// build or a competing patcher aborts loudly instead of corrupting code.
static bool ObCJKInstallTexSwapHook()
{
    if (GetPrivateProfileIntA("obCJK", "TexSwapEnable", 1, k_iniMain) == 0) {
        g_texSwapEnabled = false;
        _MESSAGE("obCJK:TexSwap: disabled via ini (TexSwapEnable=0), call site left vanilla");
        return false;
    }

    BYTE* site = (BYTE*)kVA_TexSwapCallSite;
    if (site[0] != 0xE8) {
        _ERROR("obCJK:TexSwap: byte at %p is 0x%02X, expected E8 (call) — wrong build or conflicting patch, aborting install", site, site[0]);
        return false;
    }
    DWORD oldRel = *(DWORD*)(site + 1);
    DWORD expectRel = kVA_Sub704800 - (kVA_TexSwapCallSite + 5);
    if (oldRel != expectRel) {
        _ERROR("obCJK:TexSwap: call rel32 at %p is %08X, expected %08X (sub_704800) — conflicting patch, aborting install", site + 1, oldRel, expectRel);
        return false;
    }

    DWORD newRel = (DWORD)&ObCJKTexSwapStub - (kVA_TexSwapCallSite + 5);
    DWORD oldProt;
    VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &oldProt);
    *(DWORD*)(site + 1) = newRel;
    VirtualProtect(site + 1, 4, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    _MESSAGE("obCJK:TexSwap: ok (site=%p stub=%p)", site, &ObCJKTexSwapStub);
    return true;
}
