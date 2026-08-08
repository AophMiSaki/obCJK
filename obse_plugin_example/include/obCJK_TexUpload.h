#pragma once
// Plan B "fill the spare room" half — dynamic on-demand glyph upload into
// the REAL native font D3D texture, via NiSourceTexture::rendererData ->
// IDirect3DTexture9::LockRect. Companion to obCJK_TexSwap.h's "make room"
// half. Full architecture story, NiTexturingProperty/NiTArray/Map/
// NiSourceTexture/NiDX9SourceTextureData field offsets (SDK-documented vs.
// IDA-confirmed) and why immediate SetTexture doesn't work here: D3D替換文字.md.
// obCJK_GlyphHook.h's Path A/B/C all
// call ObCJKTexUpload_GetOrPlaceGlyph() already, this is not a standalone
// unwired primitive.
//
// Every hop below is guarded (null checks return "not ready yet, retry next
// call" rather than assuming success) and the whole placement attempt runs
// inside an SEH firewall, same pattern as obCJK_TexSwap.h's
// ObCJKTexSwapCheck: a wrong assumption degrades to "this glyph stays
// unplaced for now" instead of crashing the draw call that triggered it.

#include <windows.h>
#include <d3d9.h>
#include "common/IDebugLog.h"
#include "obCJK_GlyphAtlas.h"
#include "obCJK_TexSwap.h"

// --- NiTexturingProperty / NiTArray<Map*> / Map (D3D替換文字.md「二、」) ---
static const DWORD kFontInfo_TexProp = 0x0C;  // NiTexturingProperty*
static const DWORD kTexProp_Maps     = 0x1C;  // NiTArray<Map*> m_maps
static const DWORD kNiTArray_Data    = 0x04;  // T* data
static const DWORD kNiTArray_NumObjs = 0x0C;  // UInt16 numObjs
static const int   kMap_Base         = 0;     // NiTexturingProperty::kMap_Base
static const DWORD kMap_SpTexture    = 0x08;  // Map::m_spTexture (void* in SDK; NiSourceTexture* in practice for a font's Base map)

// --- NiTexture / NiSourceTexture (同上「二、」) -----------------------------
static const DWORD kNiTexture_RendererData = 0x24;
static const DWORD kNiSrcTex_FileName      = 0x38;  // unused here, kept for reference
static const DWORD kNiSrcTex_PixelData     = 0x3C;  // unused here, obCJK_TexSwap.h operates one level up (the NiPixelData argument itself, before this field is even assigned)

// --- NiDX9SourceTextureData, IDA-confirmed not SDK-documented (D3D替換文字.md「三、」) ---
static const DWORD kRD_D3DTexture = 0x50;  // IDirect3DTexture9*, written at [esi+50h] by sub_760700 (NiDX9SourceTextureData::CreateSurface) right after CreateTexture succeeds

// Walks FontInfo -> NiTexturingProperty -> Map[kMap_Base] -> NiSourceTexture
// -> rendererData -> IDirect3DTexture9*. Returns nullptr at any hop that
// isn't populated yet — most commonly rendererData itself, which stays NULL
// until the engine's NiRenderer::InitTexture lazily realizes this specific
// font's D3D texture (first time that font is actually drawn, not at load).
// A nullptr return means "try again on a later draw call", not an error.
static IDirect3DTexture9* ObCJKGetRealFontTexture(void* fontInfo)
{
    if (!fontInfo) return nullptr;

    BYTE* prop = *(BYTE**)((BYTE*)fontInfo + kFontInfo_TexProp);
    if (!prop) return nullptr;

    BYTE* mapsData  = *(BYTE**)(prop + kTexProp_Maps + kNiTArray_Data);
    WORD  mapsCount = *(WORD*)(prop + kTexProp_Maps + kNiTArray_NumObjs);
    if (!mapsData || mapsCount <= kMap_Base) return nullptr;

    BYTE* baseMap = ((BYTE**)mapsData)[kMap_Base];
    if (!baseMap) return nullptr;

    BYTE* srcTex = *(BYTE**)(baseMap + kMap_SpTexture);
    if (!srcTex) return nullptr;

    BYTE* rendererData = *(BYTE**)(srcTex + kNiTexture_RendererData);
    if (!rendererData) return nullptr;  // InitTexture hasn't realized this font's D3D texture yet

    return *(IDirect3DTexture9**)(rendererData + kRD_D3DTexture);
}

// Looks up (rasterizing on first miss, via the existing CPU-side GDI cache)
// the glyph for `code` under fontID0 (0-based), and if it isn't already
// living inside the swapped native texture, LockRects a freshly
// shelf-allocated rect of the REAL font texture and writes it in (native
// .tex is 4 bytes/pixel, R at byte[0]..A at byte[3]; RGB filled white,
// alpha = GGO coverage rescaled 0..64 -> 0..255).
//
// Fills *outEntry regardless of whether placement succeeded this call —
// callers must check entry->texSwapReady (or entry->valid for "this font
// has no glyph for this code at all", e.g. a space). A false/not-ready
// result is always safe to retry on a later draw call; nothing here leaves
// partial state that would corrupt a retry.
static void ObCJKTexUpload_PlaceGlyphImpl(int fontID0, WORD code, void* fontInfo, ObCJKGlyphEntry** outEntry)
{
    ObCJKGlyphEntry* entry = ObCJKGlyphAtlas_GetGlyph(fontID0, code);
    *outEntry = entry;
    // This path serves both ASCII and CJK codes, so resolve the slot per-code
    // (ObCJKGlyphAtlas_GetSlot picks ASCII vs CJK internally from `code`).
    ObCJKFontSlot* slot = ObCJKGlyphAtlas_GetSlot(fontID0, code);  // for slot->yPosOffset/density/contrast; already created by the GetGlyph call above
    if (!entry || !entry->valid) return;      // no visible glyph (e.g. space) or fontID out of range — nothing to place
    if (entry->texSwapReady) return;           // already placed by a previous call here

    ObCJKTexSwapRegion* region = ObCJKTexSwapGetRegion(fontID0);
    if (!region) return;  // this font's texture was never swapped/enlarged (TexSwap disabled or bad native format) — nothing to place into

    int w = (int)entry->gm.gmBlackBoxX;
    int h = (int)entry->gm.gmBlackBoxY;
    if (w <= 0 || h <= 0) return;

    // Shelf-pack allocate within the reserved region, continuing from
    // wherever the previous dynamic placement left the cursor. 1px gap
    // between glyphs against filter bleed.
    int penX = region->penX, penY = region->penY, rowH = region->rowHeight;
    if (penX + w + 1 > region->texSize) { penY += rowH + 1; penX = 0; rowH = 0; }
    if (penY + h + 1 > region->texSize) {
        if (!region->overflowWarned) {
            region->overflowWarned = true;
            _WARNING("obCJK:TexUpload: CJK region full for slot=%d (texSize=%d) — further glyphs stay unplaced (blank) until the font reloads",
                      ObCJKSlotFromFontID(fontID0), region->texSize);
        }
        return;
    }

    IDirect3DTexture9* tex = ObCJKGetRealFontTexture(fontInfo);
    if (!tex) return;  // InitTexture hasn't realized this font's D3D texture yet — not an error, retry next call

    D3DLOCKED_RECT locked;
    RECT rect = { penX, penY, penX + w, penY + h };
    HRESULT hr = tex->LockRect(0, &locked, &rect, 0);
    if (FAILED(hr)) {
        _WARNING("obCJK:TexUpload: LockRect failed hr=0x%08X slot=%d code=0x%04X", hr, ObCJKSlotFromFontID(fontID0), code);
        return;
    }

    // __finally guarantees UnlockRect runs even if the pixel-composite loop
    // below faults (bad entry->bitmap/dst assumption) — otherwise the outer
    // SEH firewall in ObCJKTexUpload_GetOrPlaceGlyph would catch the
    // exception but skip UnlockRect, leaving this font's D3D texture level 0
    // permanently locked.
    __try {
        int srcPitch = (w + 3) & ~3;  // GGO_GRAY8_BITMAP rows are DWORD-aligned
        int bgOpacityPct = ObCJKBackgroundOpacityPercent();
        BYTE* dstBase = (BYTE*)locked.pBits;
        for (int row = 0; row < h; row++) {
            BYTE* src = entry->bitmap + row * srcPitch;
            BYTE* dst = dstBase + row * locked.Pitch;
            for (int col = 0; col < w; col++) {
                BYTE v = slot ? ObCJKApplyDensityContrast(src[col], slot->density, slot->contrastLevel)
                              : ObCJKApplyDensityContrast(src[col], 0, 0);
                ObCJKCompositeGlyphPixel(v, bgOpacityPct,
                    &dst[col * 4 + 0], &dst[col * 4 + 1], &dst[col * 4 + 2], &dst[col * 4 + 3]);
            }
        }
    } __finally {
        tex->UnlockRect(0);
    }

    entry->tsU0 = (float)penX / (float)region->texSize;
    entry->tsV0 = (float)penY / (float)region->texSize;
    entry->tsU1 = (float)(penX + w) / (float)region->texSize;
    entry->tsV1 = (float)(penY + h) / (float)region->texSize;

    // Mirrors obCJK_GlyphAtlas.h's EnsureVRAM population of `native` (the
    // same 56-byte struct sub_573F10 reads as arg_0), just sourced from the
    // swapped-native-texture UV rect (ts*) instead of the standalone A8
    // atlas rect (u0/v0/u1/v1) — obCJK_GlyphHook.h's Path A/B/C consume
    // `&entry->native` directly regardless of which route filled it.
    entry->native.reserved00 = 0;
    entry->native.u0_topLeft  = entry->tsU0; entry->native.v0_topLeft  = entry->tsV0;
    entry->native.u1_topRight = entry->tsU1; entry->native.v0_topRight = entry->tsV0;
    entry->native.u0_botLeft  = entry->tsU0; entry->native.v1_botLeft  = entry->tsV1;
    entry->native.u1_botRight = entry->tsU1; entry->native.v1_botRight = entry->tsV1;
    entry->native.width           = (float)w;
    entry->native.height          = (float)h;
    // sub_573F10 sums THREE separate terms into pen.x (advance/+0x2C first,
    // then width/+0x24 + advanceNaNGuard/+0x30 at the end — advanceNaNGuard
    // is always added in the normal, non-NaN path). Filling both with the
    // full gmCellIncX double-counts the cell width; split via
    // ObCJKComputeGlyphXTerms so the three terms sum back to exactly
    // gmCellIncX (left/right split honors GlyphXAlign). See
    // obCJK_GlyphAtlas.h's EnsureVRAM (same fix) for the full
    // disassembly-backed rationale.
    ObCJKComputeGlyphXTerms(entry->gm.gmptGlyphOrigin.x, w, (int)entry->gm.gmCellIncX,
                            code, slot ? slot->cellWidth : 0,
                            &entry->native.advance, &entry->native.advanceNaNGuard);
    // Per-glyph baseline — see obCJK_GlyphAtlas.h's EnsureVRAM (same fix) /
    // ObCJKFontSlot::yPosOffset comment for the disassembly-backed rationale.
    // GlyphXAlign Rule C Y-centering: topOffsetFromBaseline/glyphTopFromCellTop
    // naming + the ascent-bridges-cell-top-and-baseline derivation are the
    // same as obCJK_GlyphAtlas.h's EnsureVRAM — see that function's comment
    // (and obcjk_glyphxalign_rulecd_design memory / 2026-07-31 conversation)
    // for the full rationale; keep both copies in sync.
    float topOffsetFromBaseline = (float)entry->gm.gmptGlyphOrigin.y;
    if (slot && ObCJKGlyphXAlignForceCenter(code) && ObCJKGlyphXAlignMode() == 2) {
        float glyphTopFromCellTop = ((float)slot->cellHeight - (float)h) / 2.0f;
        topOffsetFromBaseline = (float)slot->ascent - glyphTopFromCellTop;
    }
    entry->native.baseline        = topOffsetFromBaseline + (slot ? (float)slot->yPosOffset : 0.0f);

    entry->texSwapReady = true;

    region->penX      = penX + w + 1;
    region->penY      = penY;
    region->rowHeight = (h > rowH) ? h : rowH;
}

// SEH firewall: this is first use of the entire NiTexturingProperty/NiTArray/
// Map/rendererData/LockRect chain above. Any wrong assumption degrades to
// "glyph stays unplaced" (caller sees texSwapReady==false and should fall
// back, e.g. skip drawing or leave the native single-byte glyph) instead of
// crashing whatever draw call triggered the first placement attempt. Kept
// free of C++ objects with destructors so __try is legal here, matching
// obCJK_TexSwap.h's ObCJKTexSwapCheck.
static ObCJKGlyphEntry* ObCJKTexUpload_GetOrPlaceGlyph(int fontID0, WORD code, void* fontInfo)
{
    ObCJKGlyphEntry* entry = nullptr;
    __try {
        ObCJKTexUpload_PlaceGlyphImpl(fontID0, code, fontInfo, &entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _WARNING("obCJK:TexUpload: EXCEPTION placing glyph slot=%d code=0x%04X fontInfo=%p", ObCJKSlotFromFontID(fontID0), code, fontInfo);
    }
    return entry;
}
