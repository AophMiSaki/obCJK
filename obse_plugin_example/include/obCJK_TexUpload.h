#pragma once
// On-demand glyph upload into the real native font D3D texture
// (NiSourceTexture::rendererData -> IDirect3DTexture9::LockRect), into the
// spare room TexSwap reserved when enlarging it. Called by GlyphHook's
// Path A/B/C via ObCJKTexUpload_GetOrPlaceGlyph().
//
// Every pointer hop is null-checked ("not ready yet, retry later") and the
// whole placement runs inside an SEH guard, so a failure leaves the glyph
// unplaced instead of crashing the draw call.

#include <windows.h>
#include <d3d9.h>
#include "common/IDebugLog.h"
#include "obCJK_GlyphAtlas.h"
#include "obCJK_TexSwap.h"

// --- NiTexturingProperty / NiTArray<Map*> / Map ---
static const DWORD kFontInfo_TexProp = 0x0C;  // NiTexturingProperty*
static const DWORD kTexProp_Maps     = 0x1C;  // NiTArray<Map*> m_maps
static const DWORD kNiTArray_Data    = 0x04;  // T* data
static const DWORD kNiTArray_NumObjs = 0x0C;  // UInt16 numObjs
static const int   kMap_Base         = 0;     // NiTexturingProperty::kMap_Base
static const DWORD kMap_SpTexture    = 0x08;  // Map::m_spTexture (void* in SDK; NiSourceTexture* in practice for a font's Base map)

// --- NiTexture / NiSourceTexture ---
static const DWORD kNiTexture_RendererData = 0x24;
static const DWORD kNiSrcTex_FileName      = 0x38;  // unused, kept for reference
static const DWORD kNiSrcTex_PixelData     = 0x3C;  // unused, kept for reference

// --- NiDX9SourceTextureData ---
static const DWORD kRD_D3DTexture = 0x50;  // IDirect3DTexture9*, written by sub_760700 (NiDX9SourceTextureData::CreateSurface)

// Walks FontInfo -> NiTexturingProperty -> Map[kMap_Base] -> NiSourceTexture
// -> rendererData -> IDirect3DTexture9*. nullptr = not populated yet
// (usually rendererData, created on the font's first draw) — retry later.
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

// Gets the glyph for `code` under fontID0 (rasterizing on first miss) and,
// if not yet placed, shelf-allocates a rect in the real font texture and
// writes it in (4 bytes/pixel, R..A; see ObCJKCompositeGlyphPixel*), then
// fills the UV rect and `native`.
//
// Always fills *outEntry; callers check entry->texSwapReady (entry->valid
// false = font has no glyph, e.g. space). Not-ready is safe to retry.
static void ObCJKTexUpload_PlaceGlyphImpl(int fontID0, WORD code, void* fontInfo, ObCJKGlyphEntry** outEntry)
{
    ObCJKGlyphEntry* entry = ObCJKGlyphAtlas_GetGlyph(fontID0, code);
    *outEntry = entry;
    // ASCII or CJK slot, chosen from `code` (already created by GetGlyph above).
    ObCJKFontSlot* slot = ObCJKGlyphAtlas_GetSlot(fontID0, code);
    if (!entry || !entry->valid) return;      // no visible glyph (e.g. space) or fontID out of range — nothing to place
    if (entry->texSwapReady) return;           // already placed by a previous call here

    ObCJKTexSwapRegion* region = ObCJKTexSwapGetRegion(fontID0);
    if (!region) return;  // this font's texture was never swapped/enlarged (TexSwap disabled or bad native format) — nothing to place into

    int w = (int)entry->gm.gmBlackBoxX;   // ink box
    int h = (int)entry->gm.gmBlackBoxY;
    if (w <= 0 || h <= 0) return;

    // Outline/shadow padding: the quad written to the texture is the ink box
    // plus padL/padT/padR/padB (all 0 when the slot has no outline).
    int padL, padT, padR, padB;
    ObCJKOutlinePads(slot, &padL, &padT, &padR, &padB);
    int qw = w + padL + padR;   // quad (ink + padding)
    int qh = h + padT + padB;

    // Shelf-pack within the reserved region from the current cursor; 1px gap
    // between glyphs against filter bleed.
    int penX = region->penX, penY = region->penY, rowH = region->rowHeight;
    if (penX + qw + 1 > region->texSize) { penY += rowH + 1; penX = 0; rowH = 0; }
    if (penY + qh + 1 > region->texSize) {
        if (!region->overflowWarned) {
            region->overflowWarned = true;
            _WARNING("obCJK:TexUpload: CJK region full for slot=%d (texSize=%d) — further glyphs stay unplaced (blank) until the font reloads",
                      ObCJKSlotFromFontID(fontID0), region->texSize);
        }
        return;
    }

    IDirect3DTexture9* tex = ObCJKGetRealFontTexture(fontInfo);
    if (!tex) return;  // InitTexture hasn't realized this font's D3D texture yet — not an error, retry next call

    // Density/contrast-processed ink coverage, needed by the outline/shadow
    // black layer (which samples neighbouring ink pixels).
    BYTE* inkBuf = nullptr;
    if (slot && slot->outlineMode != 0) {
        inkBuf = (BYTE*)malloc((size_t)w * (size_t)h);
        if (!inkBuf) return;
    }

    D3DLOCKED_RECT locked;
    RECT rect = { penX, penY, penX + qw, penY + qh };
    HRESULT hr = tex->LockRect(0, &locked, &rect, 0);
    if (FAILED(hr)) {
        _WARNING("obCJK:TexUpload: LockRect failed hr=0x%08X slot=%d code=0x%04X", hr, ObCJKSlotFromFontID(fontID0), code);
        free(inkBuf);
        return;
    }

    // Fix: __finally so UnlockRect runs even if the loop faults; otherwise the
    // outer SEH guard skipped it and the texture stayed locked.
    __try {
        int srcPitch = (w + 3) & ~3;  // GGO_GRAY8_BITMAP rows are DWORD-aligned
        int bgOpacityPct = ObCJKBackgroundOpacityPercent();
        BYTE* dstBase = (BYTE*)locked.pBits;
        if (!inkBuf) {
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
        } else {
            for (int row = 0; row < h; row++) {
                BYTE* src = entry->bitmap + row * srcPitch;
                for (int col = 0; col < w; col++)
                    inkBuf[row * w + col] = ObCJKApplyDensityContrast(src[col], slot->density, slot->contrastLevel);
            }
            for (int qrow = 0; qrow < qh; qrow++) {
                BYTE* dst = dstBase + qrow * locked.Pitch;
                int iy = qrow - padT;
                for (int qcol = 0; qcol < qw; qcol++) {
                    int ix = qcol - padL;
                    BYTE v = (ix >= 0 && ix < w && iy >= 0 && iy < h) ? inkBuf[iy * w + ix] : 0;
                    BYTE blackCov = ObCJKOutlineCoverage(inkBuf, w, h, ix, iy, slot->outlineMode, slot->outlineSize);
                    ObCJKCompositeGlyphPixelOutline(v, bgOpacityPct, blackCov, slot->outlineAlpha,
                        &dst[qcol * 4 + 0], &dst[qcol * 4 + 1], &dst[qcol * 4 + 2], &dst[qcol * 4 + 3]);
                }
            }
        }
    } __finally {
        tex->UnlockRect(0);
        free(inkBuf);
    }

    entry->tsU0 = (float)penX / (float)region->texSize;
    entry->tsV0 = (float)penY / (float)region->texSize;
    entry->tsU1 = (float)(penX + qw) / (float)region->texSize;
    entry->tsV1 = (float)(penY + qh) / (float)region->texSize;

    // `native`: the 56-byte struct sub_573F10 reads as arg_0; Path A/B/C
    // pass &entry->native.
    entry->native.reserved00 = 0;
    entry->native.u0_topLeft  = entry->tsU0; entry->native.v0_topLeft  = entry->tsV0;
    entry->native.u1_topRight = entry->tsU1; entry->native.v0_topRight = entry->tsV0;
    entry->native.u0_botLeft  = entry->tsU0; entry->native.v1_botLeft  = entry->tsV1;
    entry->native.u1_botRight = entry->tsU1; entry->native.v1_botRight = entry->tsV1;
    entry->native.width           = (float)qw;
    entry->native.height          = (float)qh;
    // Fix: sub_573F10 adds all three of advance, width and advanceNaNGuard
    // to pen.x; setting both advance terms to gmCellIncX double-counted the
    // cell. Split so the three sum to exactly gmCellIncX.
    ObCJKComputeGlyphXTerms(entry->gm.gmptGlyphOrigin.x, w, (int)entry->gm.gmCellIncX,
                            code, slot ? slot->cellWidth : 0,
                            &entry->native.advance, &entry->native.advanceNaNGuard);
    // Outline/shadow: the quad grew by padL/padR, so move the quad's left edge
    // left by padL and take padR from the trailing term — the three terms
    // still sum to gmCellIncX and the ink stays where it was.
    entry->native.advance         -= (float)padL;
    entry->native.advanceNaNGuard -= (float)padR;
    // Per-glyph baseline: gmptGlyphOrigin.y (baseline up to ink-box top,
    // 0=baseline, up=positive, negative for descenders) + ini ypos (p3).
    // Rule C list members are vertically centered instead:
    //   glyphTopFromCellTop   = (cellHeight - h) / 2   (cell-top-zero)
    //   topOffsetFromBaseline = ascent - glyphTopFromCellTop
    float topOffsetFromBaseline = (float)entry->gm.gmptGlyphOrigin.y;
    if (slot && ObCJKGlyphXAlignForceCenter(code) && ObCJKGlyphXAlignMode() == 2) {
        float glyphTopFromCellTop = ((float)slot->cellHeight - (float)h) / 2.0f;
        topOffsetFromBaseline = (float)slot->ascent - glyphTopFromCellTop;
    }
    // Outline/shadow: quad top is padT above the ink top.
    entry->native.baseline        = topOffsetFromBaseline + (float)padT + (slot ? (float)slot->yPosOffset : 0.0f);

    entry->texSwapReady = true;

    region->penX      = penX + qw + 1;
    region->penY      = penY;
    region->rowHeight = (qh > rowH) ? qh : rowH;
}

// SEH guard around placement: a fault leaves the glyph unplaced
// (texSwapReady==false, caller falls back to native) instead of crashing.
// No C++ objects with destructors here, so __try is legal.
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
