#pragma once
// CPU-side GDI glyph cache for CJK/ASCII glyphs, plus the per-slot font
// configuration (HFONT, metrics, density/contrast, outline) and the shared
// pixel/advance helpers that TexSwap/TexUpload use.
#include <windows.h>
#include <d3d9.h>
#include <cstddef>  // offsetof, used by ObCJKNativeGlyphEntry's layout static_asserts
#include "common/IDebugLog.h"
#include "obCJK_Encoding.h"
#include "obCJK_Path.h"  // k_iniMain
#include "obCJK_LineBreakRule.h"  // ObCJKEncodeCharToCodePage, reused by GlyphXAlign Rule C

// Native engine font-ID space: vanilla (0-4) + MenuQue extras (5-31).
// Only for sizing/scanning the real FontManager::fontInfos[] array — never
// for obCJK's own per-slot arrays, which use kObCJKMaxFontID.
static const int kObCJKMaxFontIDNative = 32;

// NorthernUI's xxnFontPath fonts (Normal/Large/MediumLargeUpper/Shadowed/
// Small) are not in fontInfos[]; each gets a virtual ID right after the
// native range so it is configured via ordinary FontParam<N>_1/_2 keys.
static const int kObCJKNorthernUIRoleCount = 5;

// Size/bound of every obCJK per-slot array (g_fontSlotsAscii/Cjk,
// g_asciiNativeCached/Value, g_texSwapRegion, ini per-slot lookups).
static const int kObCJKMaxFontID = kObCJKMaxFontIDNative + kObCJKNorthernUIRoleCount;

// Font slot4 (fontID 3) is a special non-CJK charset and is never
// substituted: excluded from every CJK check point (word-wrap width/height
// override + all three glyph-draw paths), so it runs 100% native.
static const int kObCJKExcludedFontID = 3;  // engineID4 - 1

static inline bool ObCJKIsFontIDExcluded(int fontID)
{
    return fontID == kObCJKExcludedFontID;
}

// fontID (0-based) -> slot (1-based, as used by iniEdit "SLOT N" and
// FontParam<N> keys). Logs print the slot number.
static inline int ObCJKSlotFromFontID(int fontID)
{
    return fontID + 1;
}

// NiDX9Renderer singleton and its IDirect3DDevice9* member.
// Fix: previous values 0x00B350D8/+0xFF8 were wrong (no d3d9 pointer there).
static const DWORD kVA_NiDX9RendererSingleton = 0x00B3F928;
static const DWORD kOffsetNiDX9RendererDevice  = 0x280;
static const DWORD kNiDX9RendererObjectSize    = 0xB00;

// d3d9.dll's loaded range [base, base+SizeOfImage), used to test whether a
// candidate pointer's vtable lands inside d3d9.dll.
static bool ObCJKGetD3D9ModuleRange(BYTE*& base, BYTE*& end)
{
    HMODULE h = GetModuleHandleA("d3d9.dll");
    if (!h) return false;
    base = (BYTE*)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    end = base + nt->OptionalHeader.SizeOfImage;
    return true;
}

// SEH-guarded DWORD read for probing candidate pointers.
static bool ObCJKSafeReadDword(const void* addr, DWORD& out)
{
    __try {
        out = *(const DWORD*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns nullptr before the render device exists — callers treat that as
// "try again later", not an error.
static IDirect3DDevice9* ObCJKGetD3DDevice()
{
    static bool s_loggedRenderer = false;
    static bool s_loggedDevice   = false;
    static bool s_dumpedRegion   = false;

    BYTE* renderer = *(BYTE**)kVA_NiDX9RendererSingleton;
    if (!renderer) return nullptr;
    if (!s_loggedRenderer) {
        _VMESSAGE("obCJK:GlyphAtlas:ObCJKGetD3DDevice: renderer became available=%p", renderer);
        s_loggedRenderer = true;
    }

    IDirect3DDevice9* device = *(IDirect3DDevice9**)(renderer + kOffsetNiDX9RendererDevice);
    if (device && !s_loggedDevice) {
        _VMESSAGE("obCJK:GlyphAtlas:ObCJKGetD3DDevice: device became available=%p", device);
        s_loggedDevice = true;
    }

    if (!device && !s_dumpedRegion) {
        s_dumpedRegion = true;
        BYTE* d3d9Base = nullptr;
        BYTE* d3d9End  = nullptr;
        bool haveD3D9Range = ObCJKGetD3D9ModuleRange(d3d9Base, d3d9End);
        _VMESSAGE("obCJK:GlyphAtlas:ObCJKGetD3DDevice: d3d9.dll range=[%p,%p) resolved=%d",
                 d3d9Base, d3d9End, haveD3D9Range ? 1 : 0);
        // Debug log: device still null — scan the renderer object for a
        // DWORD whose target's vtable lies in d3d9.dll; only HITs are logged.
        DWORD nonZeroCount = 0, readableCount = 0, hitCount = 0;
        for (DWORD off = 0; off < kNiDX9RendererObjectSize; off += 4) {
            DWORD val = *(DWORD*)(renderer + off);
            if (!val || val <= 0x10000) continue;
            ++nonZeroCount;
            if (!haveD3D9Range) continue;
            DWORD vtbl = 0;
            if (!ObCJKSafeReadDword((const void*)val, vtbl)) continue;
            ++readableCount;
            bool inRange = (BYTE*)vtbl >= d3d9Base && (BYTE*)vtbl < d3d9End;
            if (inRange) {
                ++hitCount;
                _VMESSAGE("obCJK:GlyphAtlas:ObCJKGetD3DDevice: HIT renderer+0x%03X = 0x%08X, *val = 0x%08X (vtbl-in-d3d9=1)",
                         off, val, vtbl);
            }
        }
        _VMESSAGE("obCJK:GlyphAtlas:ObCJKGetD3DDevice: scan summary: candidates(>0x10000)=%u readable=%u hits(vtbl-in-d3d9)=%u",
                 nonZeroCount, readableCount, hitCount);
    }
    return device;
}

// Scale from desktop-DC pixels (GDI metrics) to the game's backbuffer:
// backBufferHeight / desktopHeight, one uniform factor so glyphs stay
// undistorted. Resolved once the device exists, then cached.
// Fix: GDI metrics used unscaled overflowed text boxes when the backbuffer
// resolution differed from the desktop.
static float ObCJKGetRenderScale()
{
    static float s_scale    = 1.0f;
    static bool  s_resolved = false;
    if (s_resolved) return s_scale;

    IDirect3DDevice9* device = ObCJKGetD3DDevice();
    if (!device) return 1.0f;  // renderer not up yet — try again next call

    IDirect3DSwapChain9* swapChain = nullptr;
    if (FAILED(device->GetSwapChain(0, &swapChain)) || !swapChain) return 1.0f;
    D3DPRESENT_PARAMETERS pp = {};
    HRESULT hr = swapChain->GetPresentParameters(&pp);
    swapChain->Release();
    if (FAILED(hr) || pp.BackBufferHeight == 0) return 1.0f;

    HDC hdc = CreateCompatibleDC(NULL);
    int desktopW = GetDeviceCaps(hdc, HORZRES);
    int desktopH = GetDeviceCaps(hdc, VERTRES);
    DeleteDC(hdc);
    if (desktopH <= 0) return 1.0f;

    s_scale    = (float)pp.BackBufferHeight / (float)desktopH;
    s_resolved = true;
    _VMESSAGE("obCJK:GlyphAtlas:ObCJKGetRenderScale: gameBackBuffer=%ux%u desktopGDI=%dx%d scale=%.4f",
              pp.BackBufferWidth, pp.BackBufferHeight, desktopW, desktopH, s_scale);
    return s_scale;
}

// Rounds value*scale half-away-from-zero (value can be negative, e.g. ypos).
static inline int ObCJKScaleRound(int value, float scale)
{
    float f = (float)value * scale;
    return (int)(f >= 0.0f ? (f + 0.5f) : (f - 0.5f));
}

// Native-compatible 56-byte glyph struct, mirrors the font_base+byte*56+0x128
// table entries sub_573F10 reads as arg_0. Offsets must match native exactly.
// Fix: width/height/advance/advanceNaNGuard/baseline are float, not int —
// sub_573F10 reads them via x87 fld, and int bit patterns became near-zero
// floats (invisible glyphs). The producer (TexUpload) must store float.
struct ObCJKNativeGlyphEntry {
    DWORD reserved00;         // +0x00 — never read by sub_573F10
    float u0_topLeft,  v0_topLeft;      // +0x04 / +0x08
    float u1_topRight, v0_topRight;     // +0x0C / +0x10
    float u0_botLeft,  v1_botLeft;      // +0x14 / +0x18
    float u1_botRight, v1_botRight;     // +0x1C / +0x20
    float width;              // +0x24 — X extent, pixels (float)
    float height;             // +0x28 — Y extent, pixels (float)
    float advance;            // +0x2C — accumulated into caller's pen.x by sub_573F10 itself (float)
    float advanceNaNGuard;    // +0x30 — also added to pen.x after the quad; see ObCJKComputeGlyphXTerms (float)
    float baseline;           // +0x34 — baseline / Y offset (float)
};
#define OBCJK_NATIVE_GLYPH_CHECK_OFFSET(field, expected) \
    static_assert(offsetof(ObCJKNativeGlyphEntry, field) == (expected), \
                  "ObCJKNativeGlyphEntry." #field " must sit at native offset " #expected)
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(u0_topLeft, 0x04);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(u1_topRight, 0x0C);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(u0_botLeft, 0x14);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(u1_botRight, 0x1C);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(width, 0x24);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(height, 0x28);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(advance, 0x2C);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(advanceNaNGuard, 0x30);
OBCJK_NATIVE_GLYPH_CHECK_OFFSET(baseline, 0x34);
#undef OBCJK_NATIVE_GLYPH_CHECK_OFFSET
static_assert(sizeof(ObCJKNativeGlyphEntry) == 56, "ObCJKNativeGlyphEntry must be exactly 56 bytes to match native layout");

// One rasterized glyph. CPU fields (gm/bitmap/valid) fill on first GDI
// rasterization; `native` and the ts* UV rect are filled later by TexUpload.
struct ObCJKGlyphEntry {
    GLYPHMETRICS gm;
    BYTE*        bitmap;      // GetGlyphOutlineA buffer, owned by this entry
    DWORD        bitmapSize;
    bool         valid;       // false = rasterization failed (no glyph in font)
    WORD         code;        // lookup code (lead<<8|trail, or codepoint under UTF8), set in
                               // GetGlyph(); used by GlyphXAlign Rule C's punctuation check.

    ObCJKNativeGlyphEntry native;      // ready-to-push arg_0 replacement for sub_573F10

    // UV rect within the swapped (enlarged) native font texture, filled by TexUpload.
    bool  texSwapReady;
    float tsU0, tsV0, tsU1, tsV1;
};

// Per font-ID slot: GDI HFONT + flat glyph cache indexed by 2-byte code
// (lead<<8|trail), allocated lazily on first touch.
struct ObCJKFontSlot {
    HFONT             hFont;
    BYTE              charset;
    int               yPosOffset;  // ini ypos (p3), scaled — added on top of the per-glyph
                                  // baseline (native.baseline from gm.gmptGlyphOrigin.y;
                                  // computed in TexUpload).
                                  // Fix: used to be the whole baseline as one font-wide
                                  // constant; baseline is per-glyph.
    int               spacing;    // FontParam p2 (字距), added into gm.gmCellIncX at rasterization
                                   // time, so every downstream advance consumer includes it.
    int               density;    // FontParam p4 (濃度), decoded to -8..+8 — see ObCJKApplyDensityContrast().
    int               contrastLevel; // FontParam p6 (對比), -1/0/+1 — see ObCJKApplyDensityContrast().
    int               cellWidth;  // FontParam p0 (字寬), raw cfgWidth (0 = auto). Only used by
                                   // GlyphXAlign Rule C as the centering cell width; falls back
                                   // to gm.gmCellIncX when 0.
    int               cellHeight; // FontParam p1 (高), post-scale cfgHeight, always >0. Rule C's
                                   // Y centering cell height, measured from the cell top down
                                   // (0=cell top), unlike native.baseline/gmptGlyphOrigin.y
                                   // (0=baseline, up=positive); `ascent` converts between them.
    int               ascent;     // tmAscent - round(tmInternalLeading/2): baseline up to cell
                                   // top, in the baseline-zero/up-positive frame. Only used by
                                   // Rule C's Y centering to convert a cell-top-relative center
                                   // into native.baseline.
    int               outlineMode;  // FontParam<N>_OutlineMode: 0=off, 1=shadow (right/bottom), 2=outline (all sides).
                                    // Slot-level key shared by the ASCII and CJK slots of the same engineID.
    int               outlineSize;  // FontParam<N>_OutlineSize (ini px 1..3) scaled by ObCJKGetRenderScale(), min 1 when mode!=0.
    int               outlineAlpha; // FontParam<N>_OutlineAlpha, 0..100 (black layer opacity, not scaled).
    ObCJKGlyphEntry** glyphs;   // VirtualAlloc'd on first use, 0x10000 entries
};

// Separate ASCII (半角, FontParam<N>_1) and CJK (全角, FontParam<N>_2) slots
// per engineID.
static ObCJKFontSlot* g_fontSlotsAscii[kObCJKMaxFontID] = {};
static ObCJKFontSlot* g_fontSlotsCjk[kObCJKMaxFontID]   = {};

// Comma-separated "Name,p0,p1,...,p7" parser, mirrors obCJK_iniEdit.py's
// parse_font_param(): first token is the face name, up to 8 remaining tokens
// are integers, missing trailing tokens default to 0.
static void ObCJKParseFontParam(const char* raw, char* outName, size_t outNameSize, int outNums[8])
{
    for (int i = 0; i < 8; i++) outNums[i] = 0;
    outName[0] = '\0';
    if (!raw || !raw[0]) return;

    const char* comma = strchr(raw, ',');
    size_t nameLen = comma ? (size_t)(comma - raw) : strlen(raw);
    if (nameLen >= outNameSize) nameLen = outNameSize - 1;
    memcpy(outName, raw, nameLen);
    outName[nameLen] = '\0';

    const char* p = comma ? comma + 1 : nullptr;
    for (int i = 0; i < 8 && p; i++) {
        outNums[i] = atoi(p);
        const char* next = strchr(p, ',');
        p = next ? next + 1 : nullptr;
    }
}

// GDI GGO_GRAY8_BITMAP coverage (0..64) -> native .tex alpha (0..255), shared
// by TexUpload. density (-8..+8) is an additive alpha
// bias with a built-in +3 step (effective bias about -40..+88); contrastLevel
// (-1/0/+1, weak/normal/strong) pushes coverage away from/toward mid-gray.
static BYTE ObCJKApplyDensityContrast(BYTE gray0to64, int density, int contrastLevel)
{
    // Fix: zero coverage stays alpha 0 regardless of density/contrast;
    // otherwise background pixels got non-zero alpha and glyphs showed a box.
    if (gray0to64 == 0) return 0;

    int v = gray0to64 * 255 / 64;
    if (v > 255) v = 255;
    // +3-step baseline so density 0 isn't too faint for thin CJK strokes.
    v += (density + 3) * 8;
    if (contrastLevel != 0) {
        float factor = (contrastLevel > 0) ? 1.4f : 0.75f;
        v = 128 + (int)((v - 128) * factor);
    }
    if (v < 1) v = 1;  // stay off true-0 so ink pixels are never mistaken for background
    if (v > 255) v = 255;
    return (BYTE)v;
}

// ini [obCJK] BackgroundOpacity, 0..100 (default 0): opacity of the black
// backdrop behind each glyph; 0 = transparent, 100 = opaque.
static int ObCJKBackgroundOpacityPercent()
{
    static int cached = -1;
    if (cached < 0) {
        cached = GetPrivateProfileIntA("obCJK", "BackgroundOpacity", 0, k_iniMain);
        if (cached < 0)   cached = 0;
        if (cached > 100) cached = 100;
    }
    return cached;
}

// Composites one glyph pixel (ink coverage `v`, 0..255, already through
// ObCJKApplyDensityContrast) over a translucent black backdrop whose opacity
// is `bgOpacityPct` (0..100), writing straight (non-premultiplied) RGBA into
// *outR/*outG/*outB/*outA. Standard "ink OVER backdrop" compositing:
//   aOut = aInk + aBackdrop*(1-aInk)
//   cOut = 255*aInk / aOut   (ink is white 255, backdrop is black 0)
// 0% = white ink with alpha v on transparent; 100% = grayscale v on opaque black.
static void ObCJKCompositeGlyphPixel(BYTE v, int bgOpacityPct, BYTE* outR, BYTE* outG, BYTE* outB, BYTE* outA)
{
    if (bgOpacityPct <= 0) {
        *outR = *outG = *outB = 0xFF;
        *outA = v;
        return;
    }
    float aInk  = v / 255.0f;
    float aBg   = bgOpacityPct / 100.0f;
    float aOut  = aInk + aBg * (1.0f - aInk);
    BYTE  alpha = (BYTE)(aOut * 255.0f + 0.5f);
    BYTE  color = 0;
    if (aOut > 0.0f) {
        float cOut = 255.0f * aInk / aOut;
        if (cOut > 255.0f) cOut = 255.0f;
        color = (BYTE)(cOut + 0.5f);
    }
    *outR = *outG = *outB = color;
    *outA = alpha;
}

// Outline/shadow: padding in px the black layer adds around
// the ink box. Shadow = right/bottom only, outline = all four sides. All zero
// when the slot has no outline configured.
static void ObCJKOutlinePads(const ObCJKFontSlot* slot, int* padL, int* padT, int* padR, int* padB)
{
    *padL = *padT = *padR = *padB = 0;
    if (!slot || slot->outlineMode == 0) return;
    int s = slot->outlineSize;
    if (slot->outlineMode == 1) { *padR = *padB = s; }
    else                        { *padL = *padT = *padR = *padB = s; }
}

// Black-layer coverage (0..255) at ink-space position (x,y), where `ink` is the
// w*h density/contrast-processed coverage buffer (out-of-range = 0).
// Shadow: the ink shifted (size,size) toward right/bottom.
// Outline: max coverage over the pixel itself and the 8 directions at
// distance 1..size (same sample set as the iniEdit preview).
static BYTE ObCJKOutlineCoverage(const BYTE* ink, int w, int h, int x, int y, int mode, int size)
{
    int best = 0;
    if (mode == 1) {
        int sx = x - size, sy = y - size;
        if (sx >= 0 && sx < w && sy >= 0 && sy < h) best = ink[sy * w + sx];
        return (BYTE)best;
    }
    if (x >= 0 && x < w && y >= 0 && y < h) best = ink[y * w + x];
    for (int r = 1; r <= size; r++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int sx = x + dx * r, sy = y + dy * r;
                if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;
                int c = ink[sy * w + sx];
                if (c > best) best = c;
            }
        }
    }
    return (BYTE)best;
}

// Same "white ink OVER black" compositing as ObCJKCompositeGlyphPixel, with the
// black layer = BackgroundOpacity backdrop unioned with the outline/shadow
// layer (blackCov 0..255 scaled by outlineAlphaPct 0..100).
static void ObCJKCompositeGlyphPixelOutline(BYTE v, int bgOpacityPct, BYTE blackCov, int outlineAlphaPct,
                                            BYTE* outR, BYTE* outG, BYTE* outB, BYTE* outA)
{
    float aInk   = v / 255.0f;
    float aBg    = (bgOpacityPct > 0) ? bgOpacityPct / 100.0f : 0.0f;
    float aOl    = (blackCov / 255.0f) * (outlineAlphaPct / 100.0f);
    float aBlack = aBg + aOl * (1.0f - aBg);
    float aOut   = aInk + aBlack * (1.0f - aInk);
    BYTE  color  = 0;
    if (aOut > 0.0f) {
        float cOut = 255.0f * aInk / aOut;
        if (cOut > 255.0f) cOut = 255.0f;
        color = (BYTE)(cOut + 0.5f);
    }
    *outR = *outG = *outB = color;
    *outA = (BYTE)(aOut * 255.0f + 0.5f);
}

// ini [obCJK] GlyphXAlign: horizontal glyph placement within its advance
// cell (only visible for glyphs narrower than the cell):
//   0 (Rule A, default) — use the font's gmptGlyphOrigin.x (left bearing).
//   1 (Rule B) — center the black box in the cell.
//   2 (Rule C) — center only the GlyphXAlignCChars punctuation list.
static int ObCJKGlyphXAlignMode()
{
    static int cached = -1;
    if (cached < 0) {
        cached = GetPrivateProfileIntA("obCJK", "GlyphXAlign", 0, k_iniMain);
        if (cached < 0 || cached > 2) cached = 0;
    }
    return cached;
}

// Rule C punctuation list: ini [obCJK] GlyphXAlignCChars (UTF-8); this
// default set is used when the key is empty/missing.
static const wchar_t kObCJKGlyphXAlignCDefaultChars[] = L"。，、；：·"; // 。，、；：·

struct ObCJKGlyphXAlignCEntry { WORD code; };
static ObCJKGlyphXAlignCEntry g_glyphXAlignCTable[64];
static int g_glyphXAlignCCount = -1;  // -1 = not built yet

static void ObCJKBuildGlyphXAlignCTable()
{
    g_glyphXAlignCCount = 0;
    char raw[256] = {};
    GetPrivateProfileStringA("obCJK", "GlyphXAlignCChars", "", raw, sizeof(raw), k_iniMain);

    WCHAR wide[128] = {};
    const wchar_t* src = kObCJKGlyphXAlignCDefaultChars;
    if (raw[0]) {
        MultiByteToWideChar(CP_UTF8, 0, raw, -1, wide, 128);
        src = wide;
    }

    UINT winCP = (UINT)g_activeCodePage;
    bool isUtf8 = (g_activeCodePage == kCP_UTF8);
    int cap = (int)(sizeof(g_glyphXAlignCTable) / sizeof(g_glyphXAlignCTable[0]));
    for (const wchar_t* p = src; *p && g_glyphXAlignCCount < cap; p++) {
        // Fix: under UTF-8 the render-side `code` is the BMP codepoint, not
        // a DBCS byte pair, so store the codepoint directly instead of
        // encoding (which dropped every 3-byte CJK char).
        WORD code = isUtf8 ? (WORD)(*p & 0xFFFF)
                            : ObCJKEncodeCharToCodePage(*p, winCP);  // obCJK_LineBreakRule.h
        if (code == 0) continue;
        bool dup = false;
        for (int i = 0; i < g_glyphXAlignCCount; i++)
            if (g_glyphXAlignCTable[i].code == code) { dup = true; break; }
        if (!dup) g_glyphXAlignCTable[g_glyphXAlignCCount++].code = code;
    }
}

static bool ObCJKGlyphXAlignCContains(WORD code)
{
    if (g_glyphXAlignCCount < 0) ObCJKBuildGlyphXAlignCTable();
    for (int i = 0; i < g_glyphXAlignCCount; i++)
        if (g_glyphXAlignCTable[i].code == code) return true;
    return false;
}

// Whether this glyph is force-centered (Rule B, or Rule C list member);
// shared by the X (ObCJKComputeGlyphXTerms) and Y (baseline) paths.
static bool ObCJKGlyphXAlignForceCenter(WORD code)
{
    int mode = ObCJKGlyphXAlignMode();
    if (mode == 1) return true;
    if (mode == 2) return ObCJKGlyphXAlignCContains(code);
    return false;
}

// Splits a glyph's advance (cellAdvance, from gm.gmCellIncX) into left/right
// blank space around its black box (width w). sub_573F10 adds advance/+0x2C
// to pen.x before drawing the quad and width/+0x24 + advanceNaNGuard/+0x30
// after, so *outAdvance is the left gap. leftGap is clamped to
// [0, cell - w] so a bad font bearing can't overlap neighbors. Under Rule C
// a nonzero cfgWidth (slot cellWidth) replaces cellAdvance as the cell and
// total advance for list members.
static void ObCJKComputeGlyphXTerms(int originX, int w, int cellAdvance, WORD code, int cfgWidth,
                                    float* outAdvance, float* outAdvanceNaNGuard)
{
    bool forceCenter = ObCJKGlyphXAlignForceCenter(code);
    int mode = ObCJKGlyphXAlignMode();
    int effectiveCell = (forceCenter && mode == 2 && cfgWidth > 0) ? cfgWidth : cellAdvance;

    float leftGap = forceCenter
        ? (float)(effectiveCell - w) / 2.0f
        : (float)originX;

    float maxLeftGap = (float)effectiveCell - (float)w;
    if (maxLeftGap < 0.0f) maxLeftGap = 0.0f;
    if (leftGap < 0.0f) leftGap = 0.0f;
    if (leftGap > maxLeftGap) leftGap = maxLeftGap;

    *outAdvance         = leftGap;
    *outAdvanceNaNGuard = (float)effectiveCell - leftGap - (float)w;
}

// GDI charset for the CJK slots of the active codepage; every slot uses the
// same one. UTF-8 uses DEFAULT_CHARSET (no UTF-8 charset exists;
// GetGlyphOutlineW takes the codepoint directly) — it only affects font
// matching when the face name is empty.
static BYTE ObCJKCharsetForActiveCodePage()
{
    switch (g_activeCodePage) {
    case kCP_GBK:    return GB2312_CHARSET;
    case kCP_SJIS:   return SHIFTJIS_CHARSET;
    case kCP_KOREAN: return HANGEUL_CHARSET;
    case kCP_UTF8:   return DEFAULT_CHARSET;
    default:         return CHINESEBIG5_CHARSET;
    }
}

// ini [obCJK] AsciiRenderEnable (default 1): global switch for rendering
// ASCII/half-width characters through obCJK instead of the native .fnt table.
static bool ObCJKAsciiRenderEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "AsciiRenderEnable", 1, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// Per-slot 半形 override: ini FontParam<engineID>_1_Native (0/1, default 0,
// active codepage's section) keeps that slot's ASCII on the native bitmap
// font. Cached per fontID; cleared by ObCJKGlyphAtlas_Reset() (hot reload).
static bool g_asciiNativeCached[kObCJKMaxFontID] = {};
static bool g_asciiNativeValue[kObCJKMaxFontID]  = {};

static bool ObCJKAsciiIsNativeSlot(int fontID)
{
    if (fontID < 0 || fontID >= kObCJKMaxFontID) return false;
    if (!g_asciiNativeCached[fontID]) {
        int engineID = fontID + 1;
        char iniKey[32] = {};
        wsprintfA(iniKey, "FontParam%d_1_Native", engineID);
        g_asciiNativeValue[fontID] =
            GetPrivateProfileIntA(ObCJKCodePageName(g_activeCodePage), iniKey, 0, k_iniMain) != 0;
        g_asciiNativeCached[fontID] = true;
    }
    return g_asciiNativeValue[fontID];
}

// Global switch + per-slot override; ASCII call sites gate on this.
static bool ObCJKAsciiRenderEnabledForFont(int fontID)
{
    return ObCJKAsciiRenderEnabled() && !ObCJKAsciiIsNativeSlot(fontID);
}

// Per-slot 全形 disable: ini FontParam<engineID>_2_Disable (0/1, default 0,
// active codepage's section). Disabled = GetSlotImpl(fontID, false) returns
// nullptr, so full-width characters stay native; the 半形 slot is unaffected.
// Cached per fontID; cleared by ObCJKGlyphAtlas_Reset().
static bool g_cjkDisableCached[kObCJKMaxFontID] = {};
static bool g_cjkDisableValue[kObCJKMaxFontID]  = {};

static bool ObCJKCjkIsDisabledSlot(int fontID)
{
    if (fontID < 0 || fontID >= kObCJKMaxFontID) return false;
    if (!g_cjkDisableCached[fontID]) {
        int engineID = fontID + 1;
        char iniKey[32] = {};
        wsprintfA(iniKey, "FontParam%d_2_Disable", engineID);
        g_cjkDisableValue[fontID] =
            GetPrivateProfileIntA(ObCJKCodePageName(g_activeCodePage), iniKey, 0, k_iniMain) != 0;
        g_cjkDisableCached[fontID] = true;
    }
    return g_cjkDisableValue[fontID];
}

// ini [obCJK] LineBreakSpaceEnablePathA (default 1): Path A (sub_575B40)
// forced word split inserts a space instead of the native hyphen '-'.
// Used by ObCJKApplyLineBreakSpaceSettingPathA() and both
// ObCJKLineSplitPairCheck variants.
static bool ObCJKLineBreakSpaceEnabledPathA()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "LineBreakSpaceEnablePathA", 1, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// ini [obCJK] LineBreakSpaceEnablePathBC (default 1): same as PathA, for the
// sub_5772A0 word-wrap tree shared by Path B and C (hyphen `push 2Dh` at
// 0x577470, see ObCJKApplyLineBreakSpaceSettingPathBC()). One switch covers
// both paths since native doesn't distinguish them at this stage.
static bool ObCJKLineBreakSpaceEnabledPathBC()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "LineBreakSpaceEnablePathBC", 1, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// Lazily creates the GDI font + glyph cache for fontID (any ID below
// kObCJKMaxFontID, all handled identically) and isAscii, from ini
// FontParam<engineID>_1 (半角) / FontParam<engineID>_2 (全角).
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlotImpl(int fontID, bool isAscii)
{
    if (fontID < 0 || fontID >= kObCJKMaxFontID) return nullptr;
    if (!isAscii && ObCJKCjkIsDisabledSlot(fontID)) return nullptr;  // FontParam<N>_2_Disable
    ObCJKFontSlot** slots = isAscii ? g_fontSlotsAscii : g_fontSlotsCjk;
    if (slots[fontID]) return slots[fontID];

    ObCJKFontSlot* slot = (ObCJKFontSlot*)calloc(1, sizeof(ObCJKFontSlot));
    slot->charset = ObCJKCharsetForActiveCodePage();

    // ini section = active codepage's name ("BIG5"/"GBK"/...).
    int engineID = fontID + 1;
    char iniKey[32] = {};
    wsprintfA(iniKey, "FontParam%d_%d", engineID, isAscii ? 1 : 2);

    char raw[256] = {};
    GetPrivateProfileStringA(ObCJKCodePageName(g_activeCodePage), iniKey, "", raw, sizeof(raw), k_iniMain);

    char cfgFace[LF_FACESIZE] = {};
    int  nums[8];
    if (raw[0]) {
        ObCJKParseFontParam(raw, cfgFace, sizeof(cfgFace), nums);
    } else {
        // Key absent: 16px, auto-match font by charset, upright, no offsets.
        static const int kDefaultNums[8] = { 0, 16, 0, 0, 16, 400, 0, 0 };  // p4=16 decodes to density 0
        memcpy(nums, kDefaultNums, sizeof(nums));
        cfgFace[0] = '\0';
    }

    int cfgWidth    = nums[0];
    int cfgHeight   = nums[1];
    int cfgSpacing  = nums[2];
    int cfgYPos     = nums[3];
    int cfgDensity  = nums[4] / 2 - 8;  // ini stores (density+8)*2
    int cfgWeight   = nums[5];
    int cfgContrast = nums[6];
    int cfgItalic   = nums[7];

    if (cfgHeight < 0) cfgHeight = -cfgHeight;  // ini value is a plain pixel size, sign doesn't matter

    // Scale ini px values (幅/字距/高/Y坐標) to the game's render resolution.
    float renderScale = ObCJKGetRenderScale();
    if (renderScale != 1.0f) {
        cfgHeight  = ObCJKScaleRound(cfgHeight, renderScale);
        if (cfgWidth > 0) cfgWidth = ObCJKScaleRound(cfgWidth, renderScale);
        cfgSpacing = ObCJKScaleRound(cfgSpacing, renderScale);
        cfgYPos    = ObCJKScaleRound(cfgYPos, renderScale);
    }

    // Fix: the ini face name is UTF-8; CreateFontIndirectA decoded it as
    // CP_ACP and non-ASCII names (e.g. "微軟正黑體") fell back to a substitute
    // font. Decode as UTF-8 and use the wide API.
    WCHAR wideFace[LF_FACESIZE] = {};
    MultiByteToWideChar(CP_UTF8, 0, cfgFace, -1, wideFace, LF_FACESIZE);

    LOGFONTW lf = {};
    lf.lfHeight         = -cfgHeight;
    lf.lfWidth          = (cfgWidth > 0) ? cfgWidth : 0;
    lf.lfWeight         = cfgWeight ? cfgWeight : FW_NORMAL;
    lf.lfItalic         = cfgItalic ? TRUE : FALSE;
    // Fix: ASCII slot requests ANSI_CHARSET; a Western face under a CJK
    // charset got linked to a substitute font with full-width Latin glyphs.
    lf.lfCharSet        = isAscii ? ANSI_CHARSET : slot->charset;
    lf.lfOutPrecision   = OUT_TT_PRECIS;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    (void)lstrcpynW(lf.lfFaceName, wideFace, LF_FACESIZE);  // empty string = GDI matches by charset alone
    slot->hFont = CreateFontIndirectW(&lf);
    if (!slot->hFont) {
        _WARNING("obCJK:GlyphAtlas:ObCJKGlyphAtlas_GetSlot: CreateFontIndirectW failed slot=%d isAscii=%d charset=%u",
                  engineID, isAscii ? 1 : 0, slot->charset);
        free(slot);
        return nullptr;
    }
    slot->yPosOffset = cfgYPos;
    slot->spacing       = cfgSpacing;
    slot->density       = cfgDensity;
    slot->contrastLevel = cfgContrast;
    slot->cellWidth      = cfgWidth;   // 0 = auto
    slot->cellHeight     = cfgHeight;  // always >0

    // Outline/shadow (slot-level keys, shared by _1/_2).
    {
        const char* section = ObCJKCodePageName(g_activeCodePage);
        char olKey[48] = {};
        wsprintfA(olKey, "FontParam%d_OutlineMode", engineID);
        int olMode = GetPrivateProfileIntA(section, olKey, 0, k_iniMain);
        wsprintfA(olKey, "FontParam%d_OutlineSize", engineID);
        int olSizeIni = GetPrivateProfileIntA(section, olKey, 1, k_iniMain);
        wsprintfA(olKey, "FontParam%d_OutlineAlpha", engineID);
        int olAlpha = GetPrivateProfileIntA(section, olKey, 100, k_iniMain);

        if (olMode < 0 || olMode > 2) olMode = 0;
        if (olSizeIni < 1) olSizeIni = 1;
        if (olSizeIni > 3) olSizeIni = 3;
        if (olAlpha < 0)   olAlpha = 0;
        if (olAlpha > 100) olAlpha = 100;

        int olSize = ObCJKScaleRound(olSizeIni, renderScale);
        if (olSize < 1) olSize = 1;

        slot->outlineMode  = olMode;
        slot->outlineSize  = olSize;
        slot->outlineAlpha = olAlpha;
        if (olMode != 0) {
            // debug log: outline config actually read for this slot
            _MESSAGE("obCJK:GlyphAtlas:OutlineCfg engineID=%d isAscii=%d section=%s mode=%d sizeIni=%d sizeScaled=%d alpha=%d renderScale=%.4f",
                     engineID, isAscii ? 1 : 0, section, olMode, olSizeIni, olSize, olAlpha, renderScale);
        }
    }

    // ascent for Rule C's Y centering (see ObCJKFontSlot::ascent).
    {
        HDC hdcAscent = CreateCompatibleDC(NULL);
        HGDIOBJ oldAscent = SelectObject(hdcAscent, slot->hFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(hdcAscent, &tm);
        slot->ascent = tm.tmAscent - (int)((tm.tmInternalLeading + 1) / 2);
        SelectObject(hdcAscent, oldAscent);
        DeleteDC(hdcAscent);
    }

    slot->glyphs = (ObCJKGlyphEntry**)VirtualAlloc(NULL, sizeof(ObCJKGlyphEntry*) * 0x10000,
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!slot->glyphs) {
        _WARNING("obCJK:GlyphAtlas:ObCJKGlyphAtlas_GetSlot: VirtualAlloc(glyphs) failed slot=%d isAscii=%d", engineID, isAscii ? 1 : 0);
        DeleteObject(slot->hFont);
        free(slot);
        return nullptr;
    }

    slots[fontID] = slot;
    return slot;
}

// code < 0x100 = ASCII (every DBCS lead<<8|trail code is >= 0x8100).
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlot(int fontID, WORD code)
{
    return ObCJKGlyphAtlas_GetSlotImpl(fontID, code < 0x100);
}
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlotAscii(int fontID) { return ObCJKGlyphAtlas_GetSlotImpl(fontID, true); }
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlotCJK(int fontID)   { return ObCJKGlyphAtlas_GetSlotImpl(fontID, false); }

// Slot-level line gap: ini FontParam<engineID>_LineGap (-10..+10, clamped,
// default 0, shared by the 半形/全形 slots). Cached per fontID; cleared by
// ObCJKGlyphAtlas_Reset().
static bool g_lineGapCached[kObCJKMaxFontID] = {};
static int  g_lineGapValue[kObCJKMaxFontID]  = {};

static int ObCJKLineGapForFont(int fontID)
{
    if (fontID < 0 || fontID >= kObCJKMaxFontID) return 0;
    if (!g_lineGapCached[fontID]) {
        int engineID = fontID + 1;
        char iniKey[32] = {};
        wsprintfA(iniKey, "FontParam%d_LineGap", engineID);
        int gap = GetPrivateProfileIntA(ObCJKCodePageName(g_activeCodePage), iniKey, 0, k_iniMain);
        if (gap < -10) gap = -10;
        if (gap > 10)  gap = 10;
        g_lineGapValue[fontID]  = gap;
        g_lineGapCached[fontID] = true;
    }
    return g_lineGapValue[fontID];
}

// Line height obCJK wants for fontID:
// max(半形 slot cellHeight, 全形 slot cellHeight) + LineGap, minimum 1.
// The 半形 slot only counts when obCJK actually draws it
// (ObCJKAsciiRenderEnabledForFont); the 全形 slot only counts when not disabled
// (FontParam<N>_2_Disable — its GetSlotCJK is nullptr). Returns 0 when neither
// slot counts, meaning "obCJK does not take over, keep the native line height".
// cellHeight is already scaled by ObCJKGetRenderScale(); the gap is not.
static int ObCJKLineHeightForFont(int fontID)
{
    int cell = 0;
    if (ObCJKAsciiRenderEnabledForFont(fontID)) {
        ObCJKFontSlot* s = ObCJKGlyphAtlas_GetSlotAscii(fontID);
        if (s && s->cellHeight > cell) cell = s->cellHeight;
    }
    ObCJKFontSlot* c = ObCJKGlyphAtlas_GetSlotCJK(fontID);
    if (c && c->cellHeight > cell) cell = c->cellHeight;
    if (cell <= 0) return 0;
    int h = cell + ObCJKLineGapForFont(fontID);
    return h < 1 ? 1 : h;
}

// Value for a laid-out node's height field (node+0x28): the font's line
// height when obCJK takes it over, otherwise the glyph's ink height.
static int ObCJKNodeHeightFor(int fontID, const ObCJKGlyphEntry* glyph)
{
    int lh = ObCJKLineHeightForFont(fontID);
    return lh > 0 ? lh : (int)glyph->gm.gmBlackBoxY;
}

// Looks up (rasterizing on first miss) the glyph for `code` under fontID.
// Caches misses too (valid=false) so repeated misses don't re-hit GDI.
static ObCJKGlyphEntry* ObCJKGlyphAtlas_GetGlyph(int fontID, WORD code)
{
    ObCJKFontSlot* slot = ObCJKGlyphAtlas_GetSlot(fontID, code);
    if (!slot) return nullptr;
    if (slot->glyphs[code]) return slot->glyphs[code];

    ObCJKGlyphEntry* entry = (ObCJKGlyphEntry*)calloc(1, sizeof(ObCJKGlyphEntry));
    if (!entry) return nullptr;  // C6011: allocation failure falls back to native like a missing slot
    entry->code = code;

    // Fix: 0x0A (LF) is never substituted — some fonts map it to a notdef
    // box, which was drawn in place of the line break.
    if (code == 0x0A) {
        entry->valid = false;
        slot->glyphs[code] = entry;
        return entry;
    }

    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(hdc, slot->hFont);
    static const MAT2 kIdentity = { {0,1},{0,0},{0,0},{0,1} };

    // UTF-8: `code` is a BMP codepoint -> GetGlyphOutlineW.
    // DBCS: `code` is lead<<8|trail -> GetGlyphOutlineA.
    bool isUtf8 = (g_activeCodePage == kCP_UTF8);
    DWORD size = isUtf8
        ? GetGlyphOutlineW(hdc, code, GGO_GRAY8_BITMAP, &entry->gm, 0, NULL, &kIdentity)
        : GetGlyphOutlineA(hdc, code, GGO_GRAY8_BITMAP, &entry->gm, 0, NULL, &kIdentity);
    if (size != GDI_ERROR && size > 0) {
        entry->bitmap = (BYTE*)malloc(size);
        if (isUtf8)
            GetGlyphOutlineW(hdc, code, GGO_GRAY8_BITMAP, &entry->gm, size, entry->bitmap, &kIdentity);
        else
            GetGlyphOutlineA(hdc, code, GGO_GRAY8_BITMAP, &entry->gm, size, entry->bitmap, &kIdentity);
        entry->bitmapSize = size;
        entry->valid = true;
        // Bake FontParam p2 (字距) into the advance once; all consumers read
        // gm.gmCellIncX.
        entry->gm.gmCellIncX += slot->spacing;
    } else {
        entry->valid = false;  // font has no glyph for this code — cache the miss too
    }

    SelectObject(hdc, old);
    DeleteDC(hdc);

    slot->glyphs[code] = entry;
    return entry;
}

// Frees all cached glyphs/fonts; next GetGlyph() rebuilds from ini.
// Called by the ini-editor close handler in main.cpp.
static void ObCJKGlyphAtlas_ResetArray(ObCJKFontSlot** slots)
{
    for (int i = 0; i < kObCJKMaxFontID; i++) {
        if (!slots[i]) continue;
        for (DWORD c = 0; c < 0x10000; c++) {
            // C6001 false positive: glyphs comes from VirtualAlloc(MEM_COMMIT), which is zero-filled.
#pragma warning(suppress: 6001)
            if (slots[i]->glyphs[c]) {
                free(slots[i]->glyphs[c]->bitmap);
                free(slots[i]->glyphs[c]);
            }
        }
        VirtualFree(slots[i]->glyphs, 0, MEM_RELEASE);
        DeleteObject(slots[i]->hFont);
        free(slots[i]);
        slots[i] = nullptr;
    }
}

static void ObCJKGlyphAtlas_Reset()
{
    ObCJKGlyphAtlas_ResetArray(g_fontSlotsAscii);
    ObCJKGlyphAtlas_ResetArray(g_fontSlotsCjk);
    memset(g_asciiNativeCached, 0, sizeof(g_asciiNativeCached));  // re-read FontParam<N>_1_Native next use
    memset(g_cjkDisableCached, 0, sizeof(g_cjkDisableCached));    // re-read FontParam<N>_2_Disable next use
    memset(g_lineGapCached, 0, sizeof(g_lineGapCached));          // re-read FontParam<N>_LineGap next use
    _VMESSAGE("obCJK:GlyphAtlas:ObCJKGlyphAtlas_Reset: all font slots freed (ascii+cjk)");
}
