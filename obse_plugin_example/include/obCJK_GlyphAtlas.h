#pragma once
// CPU-side glyph cache + VRAM atlas upload for CJK glyphs. See
// D3D替換文字.md、MenuQue與LootMenu相關.md、
// 01_文字繪製呼叫鏈.md「四、」for design/derivation detail.
#include <windows.h>
#include <d3d9.h>
#include <cstddef>  // offsetof, used by ObCJKNativeGlyphEntry's layout static_asserts
#include "common/IDebugLog.h"
#include "obCJK_Encoding.h"
#include "obCJK_Path.h"  // k_iniMain
#include "obCJK_LineBreakRule.h"  // ObCJKEncodeCharToCodePage, reused by GlyphXAlign Rule C

// Native engine font-ID space: vanilla (0-4) + MenuQue extras (5-31), synced
// to MenuQue's own verified cap (31). 
// This bound must ONLY be used to size/scan the real
// FontManager::fontInfos[] array (obCJK_GlyphHook.h's
// ObCJKFontIDFromInfoPtr scan loop and ObCJKFontInfoPtrFromID's native
// branch) — never for obCJK's own per-slot bookkeeping arrays below, which
// also need room for the NorthernUI virtual slots. See kObCJKMaxFontID.
static const int kObCJKMaxFontIDNative = 32;

// NorthernUI's xxnFontPath fonts (Normal/Large/MediumLargeUpper/Shadowed/
// Small) never appear in fontInfos[] — see obCJK_NorthernUICompat.h. obCJK
// gives each a virtual ID in its own compact space, right after the native
// range, so they can be configured via ordinary FontParam<N>_1/_2 ini keys
// (engineID = fontID+1) exactly like any other slot.
static const int kObCJKNorthernUIRoleCount = 5;

// Upper bound on obCJK's own compact font-ID space: every array below
// (g_fontSlotsAscii/Cjk, g_asciiNativeCached/Value, obCJK_TexSwap.h's
// g_texSwapRegion, and every ini-driven per-slot lookup) is sized/bounded by
// THIS constant, not kObCJKMaxFontIDNative.
static const int kObCJKMaxFontID = kObCJKMaxFontIDNative + kObCJKNorthernUIRoleCount;

// Font slot4 (engineID4 — ObCJKGlyphAtlas_GetSlotImpl below
// computes engineID = fontID + 1, so engineID4 == fontID 3 here) is a
// special non-CJK charset by design and must never be substituted. Excluded
// from every CJK check point (word-wrap width/height override + all three
// glyph-draw paths) so it falls through 100% to native handling.
static const int kObCJKExcludedFontID = 3;  // engineID4 - 1

static inline bool ObCJKIsFontIDExcluded(int fontID)
{
    return fontID == kObCJKExcludedFontID;
}

// fontID (0-based array index) -> "slot" (1-based, what obCJK_iniEdit.py
// labels "SLOT N" and what FontParam<N>_1/_2 ini keys use — same number as
// engineID above). All log/diagnostic output should print this, not the raw
// 0-based fontID, so log line numbers match what the user configures in
// iniEdit.
static inline int ObCJKSlotFromFontID(int fontID)
{
    return fontID + 1;
}

// NiDX9Renderer singleton. [2026-07-09] kVA_NiDX9RendererSingleton/
// kOffsetNiDX9RendererDevice were corrected from an own-disassembly guess
// (0x00B350D8/+0xFF8, wrong — log scans found zero d3d9.dll pointers there)
// to xOBSE's own vendored NiRenderer.h/.cpp values. 
// Note: this whole CPU atlas + EnsureVRAM()/SetTexture() 
// path is superseded by Plan B (obCJK_TexSwap.h +// obCJK_TexUpload.h) 
// and no longer called
// obCJK_GlyphAtlas.h 對應列. Kept for reference, not deleted.
static const DWORD kVA_NiDX9RendererSingleton = 0x00B3F928;
static const DWORD kOffsetNiDX9RendererDevice  = 0x280;
static const DWORD kNiDX9RendererObjectSize    = 0xB00;

// Reads d3d9.dll's loaded module [base, base+SizeOfImage) from its own PE
// header. Used to test whether a candidate pointer's first DWORD (a COM
// object's vtable pointer) lands inside d3d9.dll's code/data — that's the
// signature of "this points at a real IDirect3DDevice9". See the
// NiDX9Renderer singleton comment above.
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

// SEH-guarded DWORD read so a wrong guess about a candidate pointer can't
// crash the game while we're probing memory shapes.
static bool ObCJKSafeReadDword(const void* addr, DWORD& out)
{
    __try {
        out = *(const DWORD*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns nullptr before the render device exists yet — callers must treat
// that as "try again later", not an error. Diagnostic latches/dump below are
// from the same investigation as the NiDX9Renderer singleton comment above.
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
        // [2026-07-09] Fallback safety net, should no longer fire now that
        // the constants above come from xOBSE's vendored NiRenderer.h — see
        // that comment. Scans the whole object for a DWORD whose target
        // dereferences into d3d9.dll; only a real HIT is logged, everything
        // else is expected noise.
        DWORD nonZeroCount = 0, readableCount = 0, hitCount = 0;
        for (DWORD off = 0; off < kNiDX9RendererObjectSize; off += 4) {
            DWORD val = *(DWORD*)(renderer + off);
            if (!val || val <= 0x10000) continue;
            ++nonZeroCount;
            // Candidate pointer check: does *val (its presumed vtable slot)
            // land inside d3d9.dll? That would mean renderer+off itself
            // holds a live IDirect3DDevice9*/COM object, not raw data.
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

// GDI font metrics are measured in the DESKTOP DC's pixel unit, which is not
// guaranteed to equal Oblivion's actual D3D9 backbuffer resolution
// (fullscreen-exclusive bypasses DWM/desktop scaling; windowed can differ if
// DPI-virtualized) — full derivation and the case that exposed it (English
// text overflowing its box) in PathA主要文字顯示.md 第18節.
// Resolved once (device/swapchain becomes available partway through load,
// same "not ready yet" convention as ObCJKGetD3DDevice), then cached for the
// session. Uses the vertical axis as a single uniform scale so glyphs stay
// undistorted (no separate horizontal stretch).
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

// One atlas page: D3DPOOL_MANAGED texture, shelf-packed. See
// 01_文字繪製呼叫鏈.md「四、」第四輪第3點 for why MANAGED.
static const int kObCJKAtlasPageSize = 1024;
static const int kObCJKAtlasMaxPages = 8;  // 8×1024×1024 A8 = 8MB VRAM ceiling per font slot

struct ObCJKAtlasPage {
    IDirect3DTexture9* texture;
    int penX, penY, rowHeight;
};

// Native-compatible 56-byte glyph struct, mirrors font_base+byte*56+0x128
// table entries sub_573F10 reads as arg_0. Offsets MUST match native layout
// exactly. Field semantics/derivation: Hook內容與規範.md「三、」.
// [2026-07-10] width/height/advance/advanceNaNGuard/baseline are float, NOT
// int — sub_573F10 accesses them exclusively via x87 fld/fcom, so an int
// bit-pattern reads back as a near-zero denormal float (an effectively
// zero-size glyph quad). This was the root cause of "pipeline logs all
// green, screen still blank".
// All producers (this file's EnsureVRAM, obCJK_TexSwap.h,
// obCJK_TexUpload.h) must store these as float.
struct ObCJKNativeGlyphEntry {
    DWORD reserved00;         // +0x00 — never read by sub_573F10
    float u0_topLeft,  v0_topLeft;      // +0x04 / +0x08
    float u1_topRight, v0_topRight;     // +0x0C / +0x10
    float u0_botLeft,  v1_botLeft;      // +0x14 / +0x18
    float u1_botRight, v1_botRight;     // +0x1C / +0x20
    float width;              // +0x24 — X extent, pixels (float, see comment above)
    float height;             // +0x28 — Y extent, pixels (float)
    float advance;            // +0x2C — accumulated into caller's pen.x by sub_573F10 itself (float)
    float advanceNaNGuard;    // +0x30 — parity-flag fallback path; must equal `advance` (float)
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
// rasterization; VRAM fields + `native` fill lazily via EnsureVRAM() since
// rasterization can happen before the D3D device exists but upload can't.
struct ObCJKGlyphEntry {
    GLYPHMETRICS gm;
    BYTE*        bitmap;      // GetGlyphOutlineA buffer, owned by this entry
    DWORD        bitmapSize;
    bool         valid;       // false = rasterization failed (no glyph in font)
    WORD         code;        // this glyph's lookup code (lead<<8|trail, or raw byte/
                               // codepoint under UTF8) — set once in GetGlyph(), lets
                               // EnsureVRAM/TexUpload check GlyphXAlign Rule C's
                               // punctuation list without threading `code` through
                               // every call site. See ObCJKGlyphXAlignForceCenter().

    bool               vramReady;
    IDirect3DTexture9* atlasTexture;   // page texture holding this glyph (not owned — the page owns it)
    float              u0, v0, u1, v1; // normalized UV rect within atlasTexture
    ObCJKNativeGlyphEntry native;      // ready-to-push arg_0 replacement for sub_573F10, see struct comment above

    // Filled by obCJK_TexSwap.h's load-time pre-rasterization: UV rect
    // within the swapped (enlarged) NATIVE font texture — NOT the A8 atlas
    // pages above. This is the currently-used route 
    // (Plan B, obCJK_TexSwap.h + obCJK_TexUpload.h).
    bool  texSwapReady;
    float tsU0, tsV0, tsU1, tsV1;
};

// Per font-ID slot: GDI HFONT + flat glyph cache indexed by 2-byte code
// (lead<<8|trail), allocated lazily on first touch, plus this slot's atlas pages.
struct ObCJKFontSlot {
    HFONT             hFont;
    BYTE              charset;
    int               yPosOffset;  // ini ypos(p3) only, scaled — added on top of the
                                  // per-glyph natural baseline (ObCJKNativeGlyphEntry::baseline,
                                  // filled from gm.gmptGlyphOrigin.y in EnsureVRAM/TexSwap/
                                  // TexUpload, 3 copies, keep in sync). [2026-07-12] this field
                                  // used to BE the whole baseline value (a single font-wide
                                  // constant), which is wrong — native .fnt files show baseline
                                  // varies per-glyph with ascenders/descenders. 
    int               spacing;    // legacy FontParam p2 (字距) — added into gm.gmCellIncX once at
                                   // rasterization time (see ObCJKGlyphAtlas_GetGlyph), so every
                                   // advance consumer downstream picks it up automatically.
    int               density;    // legacy FontParam p4 (濃度), decoded to -8..+8 — see
                                   // ObCJKApplyDensityContrast() ([2026-07-16] range narrowed
                                   // from -15..+15, and the function now adds a +3-step
                                   // baseline so 0 looks like the old +3).
    int               contrastLevel; // legacy FontParam p6 (對比), -1/0/+1 — see ObCJKApplyDensityContrast().
    int               cellWidth;  // FontParam p0 (字寬), raw cfgWidth (0 = auto/未設定).
                                   // Only consumed by GlyphXAlign Rule C (mode==2) as the
                                   // forced-centering cell width for punctuation-list
                                   // glyphs; falls back to gm.gmCellIncX when 0. Rule A/B
                                   // never read this — see ObCJKComputeGlyphXTerms().
    int               cellHeight; // FontParam p1 (高), post-scale cfgHeight — always >0.
                                   // Rule C's forced-centering cell height for the Y axis
                                   // (never touched by Rule A/B). Measured from the CELL
                                   // TOP downward (0=cell top) — a DIFFERENT zero point
                                   // than native.baseline/gmptGlyphOrigin.y, which are
                                   // measured from the BASELINE (0=baseline, up=positive).
                                   // `ascent` below is the bridge between the two. See
                                   // EnsureVRAM/TexUpload baseline computation.
    int               ascent;     // GetTextMetricsW's tmAscent - round(tmInternalLeading/2)
                                   // the vertical distance from this row's BASELINE up to the
                                   // CELL TOP, in the same baseline-zero/up-positive frame
                                   // as gmptGlyphOrigin.y. Only consumed by GlyphXAlign
                                   // Rule C's Y-centering, to convert a cellHeight-relative
                                   // (cell-top-zero) center position into a
                                   // baseline-relative one before writing native.baseline.
                                   // Derivation/confirmation (descender ink correctly hangs
                                   // below baseline, proving baseline — not cell bottom —
                                   // is the true zero point).
    ObCJKGlyphEntry** glyphs;   // VirtualAlloc'd on first use, 0x10000 entries
    ObCJKAtlasPage*   pages[kObCJKAtlasMaxPages];
    int               pageCount;
};

// [2026-07-11] Split into ASCII (半角, legacy FontParam<N>_1) and CJK (全角,
// FontParam<N>_2) slots per engineID — previously ASCII and CJK shared one
// GDI font per fontID.
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

// Shared alpha-coverage post-process for GDI GGO_GRAY8_BITMAP (0..64) ->
// native .tex alpha (0..255), used by ObCJKGlyphAtlas_EnsureVRAM,
// obCJK_TexSwap.h, obCJK_TexUpload.h — kept in one place so the three copies
// can't drift. density (-8..+8, [2026-07-16] narrowed from -15..+15) is an
// additive alpha bias with a +3-step baseline folded in (so density==0 lands
// where the old formula's +3 used to), giving an effective bias range of
// roughly -40..+88 (was ±120 pre-narrowing); contrastLevel (-1/0/+1,
// weak/normal/strong) pushes coverage away from/toward mid-gray.
static BYTE ObCJKApplyDensityContrast(BYTE gray0to64, int density, int contrastLevel)
{
    // True background (zero GDI coverage) must stay alpha 0 no matter what
    // density/contrast are set to — otherwise density>0 or a "weak" contrast
    // leaks a non-zero value into every background pixel and the glyph gets
    // a visible box instead of a transparent background. Only pixels that
    // actually have ink get the density/contrast treatment.
    if (gray0to64 == 0) return 0;

    int v = gray0to64 * 255 / 64;
    if (v > 255) v = 255;
    // [2026-07-16] +3-step baseline: density==0 now renders like the old
    // (pre-narrowing) +3 did, since raw GDI coverage alone (density==0
    // pre-baseline) often looked faint for thin CJK strokes at typical
    // UI font sizes.
    v += (density + 3) * 8;
    if (contrastLevel != 0) {
        float factor = (contrastLevel > 0) ? 1.4f : 0.75f;
        v = 128 + (int)((v - 128) * factor);
    }
    if (v < 1) v = 1;  // stay off true-0 so ink pixels are never mistaken for background
    if (v > 255) v = 255;
    return (BYTE)v;
}

// [1-b] Global background-opacity slider: 0 = fully transparent (background
// alpha always 0, per ObCJKApplyDensityContrast above), 100 = fully opaque
// black backdrop behind the glyph. ini key BackgroundOpacity (obCJK_iniEdit.py
// slider), 0..100, default 0. Values in between give a translucent black box
// behind the glyph (subtitle-style background). See
// ObCJKCompositeGlyphPixel() for how this is actually applied per-pixel.
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
// bgOpacityPct=0 reduces to the original white-with-variable-alpha-on-
// transparent behavior; bgOpacityPct=100 reduces to solid grayscale-on-
// opaque-black (aOut always 1, cOut=v exactly) — both endpoints match what
// this pipeline did before the slider existed, this just fills in the
// continuum between them.
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

// [2026-07-16] Horizontal glyph alignment within its advance cell. Only
// glyphs whose ink (gmBlackBoxX) is narrower than their cell show a visible
// difference — full-width Hanzi get leftGap~=0 either way. Same "automatic
// per-glyph value, no manual-only knob" pattern baseline (+0x34) already
// uses for Y (see ObCJKFontSlot::yPosOffset comment), just for X:
//   0 (Rule A, default) — trust the font's own gmptGlyphOrigin.x (left
//     bearing). Matches font design but many CJK fonts draw narrow glyphs
//     (punctuation, small kana) flush-left by convention.
//   1 (Rule B) — force-center the black box within the cell instead.
// ini key GlyphXAlign, [obCJK] section (obCJK_iniEdit.py combobox).
static int ObCJKGlyphXAlignMode()
{
    static int cached = -1;
    if (cached < 0) {
        cached = GetPrivateProfileIntA("obCJK", "GlyphXAlign", 0, k_iniMain);
        if (cached < 0 || cached > 2) cached = 0;
    }
    return cached;
}

// [2026-07-28] Rule C (GlyphXAlign==2): like Rule B but scoped to a specific
// punctuation list instead of every narrow glyph — for fonts where most
// narrow glyphs already look fine under Rule A, and only a handful of
// specific marks need forcing. ini key GlyphXAlignCChars ([obCJK] section,
// obCJK_iniEdit.py text entry), UTF-8, user-editable; falls back to the
// default 6-punctuation set when empty/missing. Offered in the UI for
// BIG5/GBK/UTF8 — see obcjk_glyphxalign_rulecd_design memory for the encode
// verification behind this exact default set (each char independently
// verified to encode under BIG5 and GBK; under UTF8 the table is built from
// raw Unicode codepoints instead, see the isUtf8 branch in
// ObCJKBuildGlyphXAlignCTable() below, so no DBCS encode step applies there).
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
        // [UTF-8] `code` at the render call sites (ObCJKGlyphAtlas_GetGlyph
        // etc.) is the raw Unicode codepoint truncated to BMP (see
        // obCJK_WordWrapHook_UTF8.h's ObCJKUtf8CodeForGlyph), NOT a DBCS
        // lead<<8|trail byte pair — routing it through
        // ObCJKEncodeCharToCodePage(..., CP_UTF8) would instead try to
        // WideCharToMultiByte into UTF-8 BYTES (wrong thing to match
        // against, and would silently drop every 3-byte CJK sequence since
        // that helper's buf is only 2 bytes).
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

// Single decision point both the X (ObCJKComputeGlyphXTerms) and Y
// (EnsureVRAM/TexUpload baseline) forced-centering paths call, so Rule B/C's
// "should this glyph be forced centered" logic can't drift between axes.
static bool ObCJKGlyphXAlignForceCenter(WORD code)
{
    int mode = ObCJKGlyphXAlignMode();
    if (mode == 1) return true;
    if (mode == 2) return ObCJKGlyphXAlignCContains(code);
    return false;
}

// Splits a glyph's full advance width (cellAdvance, from gm.gmCellIncX) into
// left/right blank space around its ink black box (width w), writing
// straight into the two pen.x-contributing terms sub_573F10 reads (see
// ObCJKNativeGlyphEntry comment: it sums advance/+0x2C BEFORE drawing the
// quad, then width/+0x24 + advanceNaNGuard/+0x30 AFTER) — so *outAdvance is
// a genuine pre-glyph left shift, not a hack layered on top. Clamped so
// leftGap+w never exceeds cellAdvance (a font reporting an oversized/
// negative bearing would otherwise push the next character backwards or
// overlap it). `code` decides Rule C membership (ObCJKGlyphXAlignForceCenter);
// `cfgWidth` is the calling slot's cellWidth (0 = auto) — under Rule C, a
// nonzero cfgWidth REPLACES cellAdvance as the centering cell (and therefore
// as this glyph's actual total advance, so the three terms still sum to
// exactly one value), letting the user force a fixed cell for the
// punctuation list even when GDI's own gmCellIncX varies per glyph. Rule A/B
// are unaffected (cfgWidth only applies when mode==2).
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

// TODO: per-fontID charset override table if a MenuQue extra font ever needs
// one; every slot uses g_activeCodePage's charset for now.
//
// [UTF-8] Windows has no "UTF8_CHARSET" GDI constant — GetGlyphOutlineW
// (used for kCP_UTF8, see ObCJKGlyphAtlas_GetGlyph below) reads `code`
// directly as a Unicode codepoint and doesn't need a codepage-specific
// charset to interpret it. This value still matters for font *matching*
// when the ini leaves the face name empty (see the lfFaceName comment in
// ObCJKGlyphAtlas_GetSlotImpl) — DEFAULT_CHARSET lets GDI's font mapper pick
// whatever default face the system offers instead of forcing a specific
// legacy codepage's font, which is the best available option per
// (no better GDI primitive exists for "any Unicode script").
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

// [1-a] Global on/off switch for routing ASCII/half-width single-byte
// characters through this same glyph atlas (obCJK_WordWrapHook.h/
// obCJK_GlyphHook.h), instead of leaving them on the native single-byte
// .fnt table. Cached after first read, same lazy-static pattern as
// obCJK_TexSwap.h's TexSwapEnable, so a bad interaction can be ruled out via
// ini without a recompile. Default on (1).
static bool ObCJKAsciiRenderEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "AsciiRenderEnable", 1, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// [2026-07-27] Per-slot override: unlike ObCJKAsciiRenderEnabled() above
// (one global kill switch for every slot's ASCII rendering at once), this
// lets an individual SLOT's 半形 config opt back into Oblivion's own native
// bitmap font — e.g. no installed TrueType face is a good match for that
// particular slot, or the user just wants that one slot left vanilla. ini
// key FontParam<engineID>_1_Native (0/1, default 0) in the active codepage's
// section — kept as its own key rather than a magic sentinel written into
// the FontParam<N>_1 face-name string, so it can never collide with a real
// font name (obCJK_iniEdit.py checkbox next to the 半形 row). Lazily cached
// per fontID, same cheap-array idea as g_fontSlotsAscii/Cjk; cache is
// cleared by ObCJKGlyphAtlas_Reset() (font-param hot-reload after saving via
// the ini editor) so toggling the checkbox takes effect without a full game
// restart.
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

// Combines the global switch with the per-slot override — every ASCII call
// site below should gate on this (with fontID already resolved), not on
// ObCJKAsciiRenderEnabled() alone.
static bool ObCJKAsciiRenderEnabledForFont(int fontID)
{
    return ObCJKAsciiRenderEnabled() && !ObCJKAsciiIsNativeSlot(fontID);
}

// Whether sub_575B40's forced-line-break marker character (native inserts a
// hyphen '-' when word-wrap has to split a word with no natural break point)
// is a plain space instead. sub_575B40's only caller is sub_576670 (Path A,
// the menu-button text loop) — see obCJK_WordWrapHook.h — so this toggle is
// Path-A-only, hence the Enable*PathA* name. Same lazy-static cache pattern
// as ObCJKAsciiRenderEnabled() above. Default on (1). Consumed by
// obCJK_WordWrapHook.h's ObCJKApplyLineBreakSpaceSettingPathA() (native byte
// patch, DBCS+UTF8 share the same VAs) and both codepage variants'
// ObCJKLineSplitPairCheck (the CJK/UTF8-pair-safe insertion path).
static bool ObCJKLineBreakSpaceEnabledPathA()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "LineBreakSpaceEnablePathA", 1, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// Same idea as ObCJKLineBreakSpaceEnabledPathA() above, but for the shared
// word-wrap tree (sub_5772A0, recursively self-called and invoked from
// sub_577710/sub_577840 — the same tree the kinsoku LineBreakHook already
// hooks into) that lays out Path B (dialog/menu body text) and Path C
// (books/scrolls) text. That tree has its own, separate forced-split hyphen
// insertion (`push 2Dh` right before its sub_576F30 hyphen-node constructor
// call, VA 0x577470 — see obCJK_WordWrapHook.h's
// kVA_LineBreakMarkerImm_PathBC/ObCJKApplyLineBreakSpaceSettingPathBC()),
// unrelated to sub_575B40 entirely. Native does not distinguish Path B from
// Path C at this layout stage, so one switch necessarily covers both.
static bool ObCJKLineBreakSpaceEnabledPathBC()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "LineBreakSpaceEnablePathBC", 1, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// Lazily creates the GDI font + glyph cache for fontID (any ID up to
// kObCJKMaxFontID — native 0-4 or MenuQue extra 7/8 are handled identically,
// no branch on which kind of ID it is) and isAscii (half-width/ASCII vs
// full-width/CJK — [2026-07-11] these are now two independent GDI fonts per
// engineID, read from the legacy FontParam<engineID>_1 (半角/isAscii) /
// FontParam<engineID>_2 (全角/CJK) ini keys instead of the old single
// FontHeight<N>/FontFace<N>/FontItalic<N> [obCJK]-section keys. 
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlotImpl(int fontID, bool isAscii)
{
    if (fontID < 0 || fontID >= kObCJKMaxFontID) return nullptr;
    ObCJKFontSlot** slots = isAscii ? g_fontSlotsAscii : g_fontSlotsCjk;
    if (slots[fontID]) return slots[fontID];

    ObCJKFontSlot* slot = (ObCJKFontSlot*)calloc(1, sizeof(ObCJKFontSlot));
    slot->charset = ObCJKCharsetForActiveCodePage();

    // engineID matches the "engineID" already printed in obCJK:TexSwap:diag#N
    // log lines, so users can read a slot's engineID off the log and map it
    // straight to an ini key. Section = active codepage's own section
    // ("BIG5"/"GBK"/"SJIS"/"KOREAN"), same section obCJK_iniEdit.py writes
    // FontParam<N>_1/_2 into.
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
        // Key entirely absent (fresh install / never saved via obCJK_iniEdit.py
        // yet) — conservative defaults matching the old FontHeight/FontFace
        // fallback: 16px, auto-match font by charset, upright, no extra offsets.
        static const int kDefaultNums[8] = { 0, 16, 0, 0, 16, 400, 0, 0 };  // p4=16 decodes to density 0 (new (density+8)*2 encoding)
        memcpy(nums, kDefaultNums, sizeof(nums));
        cfgFace[0] = '\0';
    }

    int cfgWidth    = nums[0];
    int cfgHeight   = nums[1];
    int cfgSpacing  = nums[2];
    int cfgYPos     = nums[3];
    int cfgDensity  = nums[4] / 2 - 8;  // ini stores (density+8)*2, matches obCJK_iniEdit.py's encoding
    int cfgWeight   = nums[5];
    int cfgContrast = nums[6];
    int cfgItalic   = nums[7];

    if (cfgHeight < 0) cfgHeight = -cfgHeight;  // ini value is a plain pixel size, sign doesn't matter

    // [2026-07-12] Scale ini px values (幅/字距/高/Y坐標) to the game's actual
    // render resolution instead of trusting GDI's desktop-DC pixel unit
    // as-is — see ObCJKGetRenderScale() comment above for why these can
    // differ. No-op (scale==1.0f) whenever they already match, so this is
    // safe even if the mismatch theory turns out not to apply here.
    float renderScale = ObCJKGetRenderScale();
    if (renderScale != 1.0f) {
        cfgHeight  = ObCJKScaleRound(cfgHeight, renderScale);
        if (cfgWidth > 0) cfgWidth = ObCJKScaleRound(cfgWidth, renderScale);
        cfgSpacing = ObCJKScaleRound(cfgSpacing, renderScale);
        cfgYPos    = ObCJKScaleRound(cfgYPos, renderScale);
    }

    // cfgFace comes straight out of the ini's FontParam<N>_1/_2 value, which
    // obCJK_iniEdit.py writes as UTF-8 (see write_ini()). GetPrivateProfileStringA
    // above doesn't transcode (no-BOM ini files are passed through byte-for-byte),
    // so cfgFace still holds raw UTF-8 bytes here. LOGFONTA.lfFaceName is ANSI
    // (CP_ACP)-only, so CreateFontIndirectA would misdecode non-ASCII names
    // (e.g. "微軟正黑體") using the system codepage instead of UTF-8 and GDI
    // would silently fall back to a substitute font. Decode explicitly as
    // UTF-8 and go through the wide-char API instead.
    WCHAR wideFace[LF_FACESIZE] = {};
    MultiByteToWideChar(CP_UTF8, 0, cfgFace, -1, wideFace, LF_FACESIZE);

    LOGFONTW lf = {};
    lf.lfHeight         = -cfgHeight;
    lf.lfWidth          = (cfgWidth > 0) ? cfgWidth : 0;
    lf.lfWeight         = cfgWeight ? cfgWeight : FW_NORMAL;
    lf.lfItalic         = cfgItalic ? TRUE : FALSE;
    // [2026-07-12] ASCII/half-width slot must request ANSI_CHARSET, not the
    // active codepage's CJK charset (slot->charset) — a Western face name
    // under a CJK charset makes GDI silently font-link to a substitute font
    // whose Latin glyphs are often full-width, oversizing every English
    // character's advance.
    lf.lfCharSet        = isAscii ? ANSI_CHARSET : slot->charset;
    lf.lfOutPrecision   = OUT_TT_PRECIS;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, wideFace, LF_FACESIZE);  // empty string = GDI matches by charset alone
    slot->hFont = CreateFontIndirectW(&lf);
    if (!slot->hFont) {
        _WARNING("obCJK:GlyphAtlas:ObCJKGlyphAtlas_GetSlot: CreateFontIndirectW failed slot=%d isAscii=%d charset=%u",
                  engineID, isAscii ? 1 : 0, slot->charset);
        free(slot);
        return nullptr;
    }
    // [2026-07-14] TEMPORARILY silenced to cut log noise while chasing the
    // LootMenu slot7/8 off-by-one bug (obcjk_bug_garbled_lootmenu_slot78
    // memory) — this fires once per slot creation, not per-frame, so it's
    // safe to restore later.
    //_MESSAGE("obCJK:GlyphAtlas:ObCJKGlyphAtlas_GetSlot: fontID=%d engineID=%d isAscii=%d height=%d face=%s "
    //         "width=%d spacing=%d ypos=%d density=%d weight=%d contrast=%d italic=%d renderScale=%.4f (post-scale values)",
    //          fontID, engineID, isAscii ? 1 : 0, cfgHeight, cfgFace[0] ? cfgFace : "(auto)",
    //          cfgWidth, cfgSpacing, cfgYPos, cfgDensity, cfgWeight, cfgContrast, cfgItalic, renderScale);

    // [2026-07-12] Used to compute a font-level tmAscent-based constant here
    // and store it as the whole ObCJKNativeGlyphEntry::baseline value — see
    // ObCJKFontSlot::yPosOffset comment for why that's wrong (native baseline
    // is per-character). Now just the raw ini ypos(p3) knob; no GDI call
    // needed since gmptGlyphOrigin.y (per-glyph, see EnsureVRAM) supplies the
    // actual font-shape-derived offset.
    slot->yPosOffset = cfgYPos;
    slot->spacing       = cfgSpacing;
    slot->density       = cfgDensity;
    slot->contrastLevel = cfgContrast;
    slot->cellWidth      = cfgWidth;   // 0 = auto, see ObCJKFontSlot::cellWidth comment
    slot->cellHeight     = cfgHeight;  // always >0, see ObCJKFontSlot::cellHeight comment

    // GlyphXAlign Rule C's Y-centering needs `ascent` (baseline-to-cell-top
    // distance) to convert its cellHeight-relative (cell-top-zero) centered
    // position into native.baseline's baseline-relative (baseline-zero)
    // frame — see ObCJKFontSlot::ascent comment. One GetTextMetricsW call
    // per slot creation, not per-glyph, so this is negligible cost.
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
    // [2026-07-14] TEMPORARILY silenced, see comment above.
    //_MESSAGE("obCJK:GlyphAtlas:ObCJKGlyphAtlas_GetSlot: fontID=%d isAscii=%d created (charset=%u)", fontID, isAscii ? 1 : 0, slot->charset);
    return slot;
}

// code < 0x100 = ASCII (see obCJK_Encoding.h's lead-byte floor 0x81 for all 4
// codepages, confirmed all CJK lead<<8|trail codes are >= 0x8100).
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlot(int fontID, WORD code)
{
    return ObCJKGlyphAtlas_GetSlotImpl(fontID, code < 0x100);
}
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlotAscii(int fontID) { return ObCJKGlyphAtlas_GetSlotImpl(fontID, true); }
static ObCJKFontSlot* ObCJKGlyphAtlas_GetSlotCJK(int fontID)   { return ObCJKGlyphAtlas_GetSlotImpl(fontID, false); }

// Looks up (rasterizing on first miss) the glyph for `code` under fontID.
// Caches misses too (valid=false) so repeated misses don't re-hit GDI.
static ObCJKGlyphEntry* ObCJKGlyphAtlas_GetGlyph(int fontID, WORD code)
{
    ObCJKFontSlot* slot = ObCJKGlyphAtlas_GetSlot(fontID, code);
    if (!slot) return nullptr;
    if (slot->glyphs[code]) return slot->glyphs[code];

    ObCJKGlyphEntry* entry = (ObCJKGlyphEntry*)calloc(1, sizeof(ObCJKGlyphEntry));
    entry->code = code;

    // [2026-07-17] 0x0A (LF) must never be substituted, regardless of what
    // GDI says: some fonts' cmap happens to map this control code to a
    // non-empty glyph (their own tofu/notdef box, not an actual line-feed
    // glyph), so GetGlyphOutlineA reports success with size>0 and this
    // would otherwise be cached as entry->valid=true — drawing a visible
    // box in place of what should always be an invisible line break. Other
    // fonts correctly fail here (GDI_ERROR), which is the only reason this
    // wasn't visible with every font. Force native handling unconditionally
    // instead of trusting per-font GDI behavior for this code.
    if (code == 0x0A) {
        entry->valid = false;
        slot->glyphs[code] = entry;
        return entry;
    }

    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(hdc, slot->hFont);
    static const MAT2 kIdentity = { {0,1},{0,0},{0,0},{0,1} };

    // [UTF-8] Under kCP_UTF8, `code` is a decoded Unicode codepoint (see
    // obCJK_WordWrapHook_UTF8.h's ObCJKUtf8CodeForGlyph — truncated to BMP
    // range, WORD-sized; astral-plane codepoints are a known, documented,
    // deferred gap there, not something to fix here), NOT a DBCS
    // lead<<8|trail byte pair. GetGlyphOutlineA would run `code` back through
    // the ANSI codepage's byte interpretation and misdecode it, so it must go
    // through GetGlyphOutlineW instead, which takes `code` directly as a
    // Unicode codepoint. Every other DBCS codepage keeps GetGlyphOutlineA
    // unchanged.
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
        // Bake legacy FontParam p2 (字距/spacing) into the GDI-measured
        // advance ONCE here — every downstream consumer (WordWrapHook's
        // int32 write, EnsureVRAM/TexSwap/TexUpload's float native.advance)
        // just reads gm.gmCellIncX, so this single line propagates spacing
        // everywhere with zero other call-site changes.
        entry->gm.gmCellIncX += slot->spacing;
    } else {
        entry->valid = false;  // font has no glyph for this code — cache the miss too
    }

    SelectObject(hdc, old);
    DeleteDC(hdc);

    slot->glyphs[code] = entry;
    return entry;
}

// Allocates a w×h rect out of slot's atlas pages, creating/growing as needed.
static bool ObCJKGlyphAtlas_AllocRect(ObCJKFontSlot* slot, int w, int h,
                                      IDirect3DTexture9** outTex, int* outX, int* outY)
{
    IDirect3DDevice9* device = ObCJKGetD3DDevice();
    if (!device) return false;

    ObCJKAtlasPage* page = slot->pageCount ? slot->pages[slot->pageCount - 1] : nullptr;

    if (page && page->penX + w > kObCJKAtlasPageSize) {
        page->penY += page->rowHeight;
        page->penX = 0;
        page->rowHeight = 0;
    }
    if (page && page->penY + h > kObCJKAtlasPageSize)
        page = nullptr;  // current page has no room left — fall through to start a new one

    if (!page) {
        if (slot->pageCount >= kObCJKAtlasMaxPages) {
            _WARNING("obCJK:GlyphAtlas:ObCJKGlyphAtlas_AllocRect: all %d atlas pages full", kObCJKAtlasMaxPages);
            return false;
        }
        page = (ObCJKAtlasPage*)calloc(1, sizeof(ObCJKAtlasPage));
        HRESULT hr = device->CreateTexture(kObCJKAtlasPageSize, kObCJKAtlasPageSize, 1, 0,
                                            D3DFMT_A8, D3DPOOL_MANAGED, &page->texture, NULL);
        if (FAILED(hr)) {
            _WARNING("obCJK:GlyphAtlas:ObCJKGlyphAtlas_AllocRect: CreateTexture failed hr=0x%08X", hr);
            free(page);
            return false;
        }
        slot->pages[slot->pageCount++] = page;
        _VMESSAGE("obCJK:GlyphAtlas:ObCJKGlyphAtlas_AllocRect: created atlas page %d (%dx%d A8)",
                  slot->pageCount - 1, kObCJKAtlasPageSize, kObCJKAtlasPageSize);
    }

    *outTex = page->texture;
    *outX = page->penX;
    *outY = page->penY;
    page->penX += w;
    if (h > page->rowHeight) page->rowHeight = h;
    return true;
}

// Uploads entry's CPU bitmap into a VRAM atlas rect, fills atlasTexture/UV.
// Safe to call every draw: no-ops once vramReady, returns false (retry
// later) if device isn't up yet or glyph has no visible bitmap.
static bool ObCJKGlyphAtlas_EnsureVRAM(ObCJKFontSlot* slot, ObCJKGlyphEntry* entry)
{
    if (entry->vramReady) return true;
    if (!entry->valid)    return false;

    int w = (int)entry->gm.gmBlackBoxX;
    int h = (int)entry->gm.gmBlackBoxY;
    if (w <= 0 || h <= 0) return false;

    IDirect3DTexture9* tex = nullptr;
    int x = 0, y = 0;
    if (!ObCJKGlyphAtlas_AllocRect(slot, w, h, &tex, &x, &y))
        return false;

    D3DLOCKED_RECT locked;
    RECT rect = { x, y, x + w, y + h };
    HRESULT hr = tex->LockRect(0, &locked, &rect, 0);
    if (FAILED(hr)) {
        _WARNING("obCJK:GlyphAtlas:ObCJKGlyphAtlas_EnsureVRAM: LockRect failed hr=0x%08X", hr);
        return false;
    }

    // GGO_GRAY8_BITMAP rows are DWORD-aligned, 0..64 range — rescale to 0..255,
    // applying this slot's density/contrast (see ObCJKApplyDensityContrast).
    int srcPitch = (w + 3) & ~3;
    BYTE* dst = (BYTE*)locked.pBits;
    for (int row = 0; row < h; row++) {
        BYTE* srcRow = entry->bitmap + row * srcPitch;
        BYTE* dstRow = dst + row * locked.Pitch;
        for (int col = 0; col < w; col++) {
            dstRow[col] = ObCJKApplyDensityContrast(srcRow[col], slot->density, slot->contrastLevel);
        }
    }
    tex->UnlockRect(0);

    entry->atlasTexture = tex;
    entry->u0 = (float)x / (float)kObCJKAtlasPageSize;
    entry->v0 = (float)y / (float)kObCJKAtlasPageSize;
    entry->u1 = (float)(x + w) / (float)kObCJKAtlasPageSize;
    entry->v1 = (float)(y + h) / (float)kObCJKAtlasPageSize;

    entry->native.reserved00 = 0;
    entry->native.u0_topLeft   = entry->u0; entry->native.v0_topLeft   = entry->v0;
    entry->native.u1_topRight  = entry->u1; entry->native.v0_topRight  = entry->v0;
    entry->native.u0_botLeft   = entry->u0; entry->native.v1_botLeft   = entry->v1;
    entry->native.u1_botRight  = entry->u1; entry->native.v1_botRight  = entry->v1;
    entry->native.width           = (float)w;
    entry->native.height          = (float)h;
    // sub_573F10 accumulates pen.x via THREE separate terms: advance(+0x2C)
    // pre, then width(+0x24)+advanceNaNGuard(+0x30) post (all three always
    // execute — advanceNaNGuard is not an inert NaN-only fallback). Filling
    // both advance and advanceNaNGuard with the full gmCellIncX double-counts
    // the cell width (2*gmCellIncX + width instead of gmCellIncX). Split via
    // ObCJKComputeGlyphXTerms so the three terms sum back to exactly
    // gmCellIncX, with the left/right split honoring GlyphXAlign. Root-cause
    ObCJKComputeGlyphXTerms(entry->gm.gmptGlyphOrigin.x, w, (int)entry->gm.gmCellIncX,
                            entry->code, slot->cellWidth,
                            &entry->native.advance, &entry->native.advanceNaNGuard);
    // Per-glyph baseline (native .fnt entry[+0x34] is per-character, not
    // font-level — see ObCJKFontSlot::yPosOffset comment): gmptGlyphOrigin.y
    // is GDI's own per-glyph distance ABOVE the row's BASELINE (0=baseline,
    // up=positive; can go negative for ink hanging below baseline, e.g. a
    // lone descender mark) to this glyph's ink-box TOP, plus the user's
    // manual ini ypos(p3) knob on top.
    //
    // GlyphXAlign Rule C (mode==2, punctuation list) replaces this with a
    // forced vertical-center position. Centering itself is naturally
    // expressed in CELL-TOP-zero space (0=cell top, down=positive):
    // glyphTopFromCellTop = (cellHeight - h) / 2. But native.baseline is
    // baseline-zero space, not cell-top-zero — the two zero points are
    // `ascent` (slot->ascent, baseline-to-cell-top distance) apart. Convert
    // by flipping the cell-top-relative offset back into baseline-relative
    // terms: topOffsetFromBaseline = ascent - glyphTopFromCellTop.
    // [2026-07-31] Derived from sub_573F10 disassembly (baseline/-height
    // pair forms the ink box's top/bottom edges in baseline-zero space) +
    // confirmed against descender behavior (negative gmptGlyphOrigin.y
    // correctly hangs ink below baseline, proving 0 here is the baseline,
    // not the cell bottom — an earlier cell-bottom-zero assumption would
    // never need negative values).
    float topOffsetFromBaseline = (float)entry->gm.gmptGlyphOrigin.y;
    if (ObCJKGlyphXAlignForceCenter(entry->code) && ObCJKGlyphXAlignMode() == 2) {
        float glyphTopFromCellTop = ((float)slot->cellHeight - (float)h) / 2.0f;
        topOffsetFromBaseline = (float)slot->ascent - glyphTopFromCellTop;
    }
    entry->native.baseline        = topOffsetFromBaseline + (float)slot->yPosOffset;

    entry->vramReady = true;
    return true;
}

// Frees all cached glyphs/fonts/atlas textures; next GetGlyph() rebuilds
// from ini. Called by the ini-editor close handler in main.cpp.
static void ObCJKGlyphAtlas_ResetArray(ObCJKFontSlot** slots)
{
    for (int i = 0; i < kObCJKMaxFontID; i++) {
        if (!slots[i]) continue;
        for (DWORD c = 0; c < 0x10000; c++) {
            if (slots[i]->glyphs[c]) {
                free(slots[i]->glyphs[c]->bitmap);
                free(slots[i]->glyphs[c]);
            }
        }
        VirtualFree(slots[i]->glyphs, 0, MEM_RELEASE);
        for (int p = 0; p < slots[i]->pageCount; p++) {
            slots[i]->pages[p]->texture->Release();
            free(slots[i]->pages[p]);
        }
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
    _VMESSAGE("obCJK:GlyphAtlas:ObCJKGlyphAtlas_Reset: all font slots freed (ascii+cjk)");
}
