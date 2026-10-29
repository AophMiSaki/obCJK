#pragma once
// Font-texture swap-at-load hook: a 4-byte rel32 patch at VA 0x5749BB
// (sub_5744E0's call to sub_704800) swaps the first .tex page's NiPixelData
// for an enlarged copy (native pixels + reserved CJK region below), so the
// engine builds one native texture with room for CJK glyphs, which
// obCJK_TexUpload.h fills on demand.

#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_GlyphAtlas.h"
#include "obCJK_Path.h"  // k_iniMain
#include "obCJK_NorthernUICompat.h"  // ObCJKNorthernUIResolveByOwnPath

// --- Native addresses (Oblivion.exe 1.2.416) -------------------------------
// Call site inside sub_5744E0's per-page .tex loop: E8 + rel32; only the
// rel32 operand is patched (no trampoline).
static const DWORD kVA_TexSwapCallSite  = 0x005749BB;
static const DWORD kVA_Sub704800        = 0x00704800; // NiTexturingProperty ctor (builds NiSourceTexture via sub_701FC0)
static const DWORD kVA_NiPixelDataCtor  = 0x0070E560; // __thiscall (this, W, H, NiPixelFormat*, mipLevels, numFaces), retn 14h
static const DWORD kVA_FormHeapAllocate = 0x00401F00; // void* __cdecl (UInt32 size) — xOBSE GameAPI.cpp:111

// --- NiPixelData field offsets (from sub_70E560) ---
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

static const int kObCJKTexSwapBaseSize = 2048; // default enlarged size
static const int kObCJKTexSwapMaxSize  = 4096; // overflow fuse — 64MB RGBA per font, warn loudly

static bool  g_texSwapEnabled      = true;
static void* g_texSwapCont         = (void*)kVA_Sub704800; // stub's continuation target

// Per-fontID (0-based) CJK region inside the enlarged native texture, set up
// by ObCJKTexSwapCheckImpl and filled by TexUpload's shelf-pack cursor.
// Fix: the cursor never rewinds on font hot-reload — rewinding let new
// glyphs overwrite rects still referenced by UI text that isn't redrawn
// every frame, showing the wrong character. The region is consumed
// monotonically; when full, new glyphs stay blank (overflowWarned).
struct ObCJKTexSwapRegion {
    bool active;
    int  texSize;
    int  startY;  // row just below the native ASCII strip — reset floor, never reclaim above this
    int  penX, penY, rowHeight;
    bool overflowWarned;
};
static ObCJKTexSwapRegion g_texSwapRegion[kObCJKMaxFontID] = {};

// Returns the region for new glyph placement, or nullptr. Also gated on
// g_texSwapEnabled, since region.active is never cleared once set.
static ObCJKTexSwapRegion* ObCJKTexSwapGetRegion(int fontID0)
{
    if (!g_texSwapEnabled) return nullptr;
    if (fontID0 < 0 || fontID0 >= kObCJKMaxFontID) return nullptr;
    return g_texSwapRegion[fontID0].active ? &g_texSwapRegion[fontID0] : nullptr;
}

// Live turn-off for ini TexSwapEnable, called from main.cpp's editor-close
// handler: if it is now 0, clears g_texSwapEnabled and resets the glyph
// cache so already-placed CJK glyphs are dropped too. One-directional: it
// never turns TexSwap back on (that needs the load-time patch).
static void ObCJKTexSwapReloadEnableFlag()
{
    if (g_texSwapEnabled &&
        GetPrivateProfileIntA("obCJK", "TexSwapEnable", 1, k_iniMain) == 0) {
        g_texSwapEnabled = false;
        ObCJKGlyphAtlas_Reset();
        _MESSAGE("obCJK:TexSwap:ObCJKTexSwapReloadEnableFlag: disabled via ini after editor close, atlas reset to drop cached CJK placements");
    }
}

// Returns the NiPixelData* sub_704800 should consume: the original or the
// enlarged replacement. Called for every font page; only the first page per
// FontInfo is swapped (the UI text pipeline only binds texture index 0).
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
        // Not a native slot ID: resolve NorthernUI fonts by the FontInfo's own
        // path (rawID isn't written yet during construction).
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

    // Reserve the CJK region for TexUpload, fresh shelf-pack cursor.
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

    // The original NiPixelData is orphaned (accepted one-time leak per font load).
    return newPd;
}

// SEH guard: any fault in the swap logic passes the native pixelData through
// instead of crashing font loading. No C++ objects here, so __try is legal.
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
// sub_704800 constructs into, esi=FontInfo* (sub_5744E0 keeps `this` in esi).
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
