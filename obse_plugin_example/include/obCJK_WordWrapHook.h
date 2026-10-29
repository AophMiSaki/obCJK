#pragma once
// DBCS word-wrap / layout hooks.
//   1. WordWrap        sub_577C10, after sub_577120's per-byte metrics: CJK
//                      pair advance/height (node+0x24/+0x28), trail byte
//                      stash in node+0x05, esi+1 to skip the trail byte
//   2. NodeCopy        sub_577060: carries node+0x05 to the permanent node
//   3. SecondLoop      same as 1 for sub_578960's second per-character loop
//   4. TrailByteGuard  sub_575B40 0x576226: 0x7E trail byte not stripped as '~'
//   5. SmartQuoteGuard sub_575B40 0x575F8A: 0x91-0x94 lead bytes not turned into '/"
//   6. TrailByteGuard2 sub_575B40 0x575FFB: 0x7E trail byte not marked as a
//                      hyphenation point
//   7. LineSplitPair   sub_575B40 0x57616E: forced break doesn't split a pair
//   8. PreMeasure      sub_575B40 0x575FAE: CENTER/RIGHT-align width
// Also patches the Path A / Path B/C forced-break marker byte ('-' <-> ' ').
//
// sub_575B40 has a flat esp frame: idx(var_4A0) esp+0x44, srcPtr(var_490)
// esp+0x54, fontInfoPtr(var_4AC) esp+0x38, current byte(var_4D2) esp+0x12.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_GlyphAtlas.h"
// Needed for ObCJKFontIDFromInfoPtr() (PreMeasure).
#include "obCJK_GlyphHook.h"

static const DWORD kVA_WordWrapLoop = 0x00577F05;

static BYTE* g_wordWrapTramp = nullptr;
static DWORD g_wwSavedEsp    = 0;
static bool  g_wwIsCJK       = false;

// [1-a] ASCII/half-width single-byte advance override: writes the substituted
// glyph's advance to node+0x24 so layout width matches the draw side. Codes
// GDI can't rasterize (valid=false) keep the native advance.
static void ObCJKWordWrapAsciiOverride(BYTE* node, BYTE byteVal)
{
    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, (WORD)byteVal);
    if (glyph && glyph->valid) {
        // int32, not float — same node+0x24 field the CJK branch below writes.
        *(int*)(node + 0x24) = (int)glyph->gm.gmCellIncX;
    }
}

static void __cdecl ObCJKWordWrapCheck(BYTE* ediVal, DWORD esiVal, DWORD espSnapshot)
{
    g_wwIsCJK = false;
    BYTE* node = (BYTE*)(espSnapshot + 0xBC);

    BYTE lead = ediVal[esiVal];
    if (!ObCJKIsLeadByte(lead, g_activeCodePage)) {
        if (ObCJKIsAsciiCandidate(lead))
            ObCJKWordWrapAsciiOverride(node, lead);
        return;
    }

    BYTE trail = ediVal[esiVal + 1];  // safe: string buffer is NUL-terminated,
                                       // worst case this reads the terminator
    bool trailOk = (trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage));
    if (!trailOk) return;  // falls back to single-byte advance, native handles it

    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched

    WORD code = (WORD)((lead << 8) | trail);
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
    if (glyph && glyph->valid) {
        *(int*)(node + 0x24) = (int)glyph->gm.gmCellIncX;
        // Fix: node+0x28 is a fixed per-font height from sub_577120, so tall
        // CJK glyphs were under-measured. int32, not float.
        *(int*)(node + 0x28) = ObCJKNodeHeightFor(fontID, glyph);
        // +0x05 padding stash, carried onward by ObCJKNodeCopyHook.
        *(BYTE*)(node + 0x05) = trail;
        g_wwIsCJK = true;  // esi only advances by the extra +1 when we actually substituted a glyph
    }
}

static __declspec(naked) void ObCJKWordWrapHook()
{
    __asm {
        mov  dword ptr [g_wwSavedEsp], esp
        pushad
        push dword ptr [g_wwSavedEsp]
        push esi
        push edi
        call ObCJKWordWrapCheck
        add  esp, 12
        popad
        cmp  byte ptr [g_wwIsCJK], 0
        je   short no_extra_advance
        inc  esi
no_extra_advance:
        jmp  dword ptr [g_wordWrapTramp]
    }
}

static void ObCJKInstallWordWrapHook()
{
    if (g_wordWrapTramp) return;
    ObCJKInstallHook((BYTE*)kVA_WordWrapLoop, (void*)ObCJKWordWrapHook, &g_wordWrapTramp, "WordWrap");
}

// Second hook: sub_577060's scratch-node -> permanent-node copy point,
// carrying the +0x05 trail byte onward for the draw-side hooks.
static const DWORD kVA_NodeCopy = 0x005770CE;
static BYTE* g_nodeCopyTramp = nullptr;

static void __cdecl ObCJKNodeCopy(BYTE* scratchNode, BYTE* permanentNode)
{
    permanentNode[5] = scratchNode[5];
}

static __declspec(naked) void ObCJKNodeCopyHook()
{
    __asm {
        pushad
        push eax                 ; arg1 (permanentNode)
        push esi                 ; arg0 (scratchNode)
        call ObCJKNodeCopy
        add  esp, 8
        popad
        jmp  dword ptr [g_nodeCopyTramp]
    }
}

static void ObCJKInstallNodeCopyHook()
{
    if (g_nodeCopyTramp) return;
    ObCJKInstallHook((BYTE*)kVA_NodeCopy, (void*)ObCJKNodeCopyHook, &g_nodeCopyTramp, "NodeCopy");
}

// Third hook: sub_578960's second, independent per-character loop (VA
// 0x578AD4-0x578B4E). Fix: index desync on CJK pairs.
static const DWORD kVA_SecondLoop = 0x00578B2A;

static BYTE* g_secondLoopTramp = nullptr;
static DWORD g_slSavedEsp      = 0;
static bool  g_slIsCJK         = false;

static void __cdecl ObCJKSecondLoopCheck(BYTE* ediVal, DWORD ebpVal, DWORD espSnapshot)
{
    g_slIsCJK = false;
    BYTE* node = (BYTE*)(espSnapshot + 0x24);

    BYTE* buf = *(BYTE**)ediVal;
    BYTE lead = buf[ebpVal];

    if (!ObCJKIsLeadByte(lead, g_activeCodePage)) {
        // [1-a] same ASCII override as ObCJKWordWrapCheck (same node layout).
        if (ObCJKIsAsciiCandidate(lead))
            ObCJKWordWrapAsciiOverride(node, lead);
        return;
    }

    BYTE trail = buf[ebpVal + 1];  // safe: NUL-terminated buffer, worst case
                                    // this reads the terminator
    bool trailOk = (trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage));
    if (!trailOk) return;  // falls back to single-byte advance, native handles it

    DWORD imgField = *(DWORD*)(node + 0x1C);
    if (imgField != 0) return;  // IMG-override node, leave untouched

    int fontID = *(int*)(node + 0x00);
    if (ObCJKIsFontIDExcluded(fontID)) return;  // slot4: leave native handling untouched

    WORD code = (WORD)((lead << 8) | trail);
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
    if (glyph && glyph->valid) {
        *(int*)(node + 0x24) = (int)glyph->gm.gmCellIncX;
        // same height fix as ObCJKWordWrapCheck
        *(int*)(node + 0x28) = ObCJKNodeHeightFor(fontID, glyph);
        *(BYTE*)(node + 0x05) = trail;  // carried onward by NodeCopyHook
        g_slIsCJK = true;
    }
}

static __declspec(naked) void ObCJKSecondLoopHook()
{
    __asm {
        mov  dword ptr [g_slSavedEsp], esp
        pushad
        push dword ptr [g_slSavedEsp]
        push ebp
        push edi
        call ObCJKSecondLoopCheck
        add  esp, 12
        popad
        cmp  byte ptr [g_slIsCJK], 0
        je   short no_extra_advance_sl
        inc  ebp
no_extra_advance_sl:
        jmp  dword ptr [g_secondLoopTramp]
    }
}

static void ObCJKInstallSecondLoopHook()
{
    if (g_secondLoopTramp) return;
    ObCJKInstallHook((BYTE*)kVA_SecondLoop, (void*)ObCJKSecondLoopHook, &g_secondLoopTramp, "SecondLoop");
}

// Fourth hook: sub_575B40 (word-wrap token-copy scanner) VA 0x576226 —
// `cmp al,7Eh`/`jz`, which strips '~' hotkey markers. Fix: a CJK trail byte
// equal to 0x7E (e.g. Big5 "繼"=C4 7E) was stripped, orphaning the lead.
//
// The overwritten range ends with `jz rel8`, so the hook recomputes the
// branch and jumps to one of the two native VAs directly (trampoline
// unused). AL isn't loaded yet at the hook point; the byte is re-read from
// idx/srcPtr.
static const DWORD kVA_TrailByteGuard = 0x00576226;

static BYTE* g_tbgTramp    = nullptr;  // allocated but never jumped to
static DWORD g_tbgSavedEsp = 0;
static bool  g_tbgTakeSkip = false;
static BYTE  g_tbgByteVal  = 0;

static void __cdecl ObCJKTrailByteGuardCheck(DWORD espSnapshot)
{
    DWORD idx        = *(DWORD*)(espSnapshot + 0x44);
    BYTE* srcPtr      = *(BYTE**)(espSnapshot + 0x54);
    BYTE  currentByte = srcPtr[idx];

    g_tbgByteVal = currentByte;

    bool nativeSkip = (currentByte == 0x7E);
    // 0x7E preceded by a CJK lead byte is a trail byte, not a '~' marker.
    bool forceWrite = nativeSkip && idx > 0 &&
                       ObCJKIsLeadByte(srcPtr[idx - 1], g_activeCodePage);

    g_tbgTakeSkip = nativeSkip && !forceWrite;
}

static __declspec(naked) void ObCJKTrailByteGuardHook()
{
    __asm {
        mov  dword ptr [g_tbgSavedEsp], esp
        pushad
        push dword ptr [g_tbgSavedEsp]
        call ObCJKTrailByteGuardCheck
        add  esp, 4
        popad
        cmp  byte ptr [g_tbgTakeSkip], 0
        jnz  short tbg_skip
        mov  al, byte ptr [g_tbgByteVal]
        mov  edx, 0x57622E       ; native write path (mov [esi+edi],al)
        jmp  edx
tbg_skip:
        mov  edx, 0x576246       ; native jz target (real '~' hotkey strip)
        jmp  edx
    }
}

static void ObCJKInstallTrailByteGuardHook()
{
    if (g_tbgTramp) return;
    ObCJKInstallHook((BYTE*)kVA_TrailByteGuard, (void*)ObCJKTrailByteGuardHook, &g_tbgTramp, "TrailByteGuard");
}

// Fifth hook: sub_575B40 VA 0x575F8A — feeds the native smart-quote jump
// table (VA 0x575F91). Fix: bytes 0x91-0x94 were replaced with ASCII '/"
// even when they are CJK lead bytes, leaving an orphaned trail byte.
//
// 7-byte native span (575F8A `mov[esp+0x12],al` + 575F8E `movzx eax,al`),
// replayed via the standard trampoline on the native path. AL holds the raw
// current byte at hook entry.
static const DWORD kVA_SmartQuoteGuard      = 0x00575F8A;
static const DWORD kVA_SmartQuoteSkipTarget = 0x00575FAE;  // def_575F9B, jump-table convergence point (= kVA_PreMeasure)

static BYTE* g_sqTramp     = nullptr;
static DWORD g_sqSavedEsp  = 0;
static bool  g_sqTakeSkip  = false;

static void __cdecl ObCJKSmartQuoteGuardCheck(DWORD espSnapshot)
{
    DWORD idx    = *(DWORD*)(espSnapshot + 0x44);
    BYTE* srcPtr = *(BYTE**)(espSnapshot + 0x54);
    BYTE  lead   = srcPtr[idx];

    // Skip the substitution when the byte is a lead byte with a valid trail.
    BYTE trail = srcPtr[idx + 1];  // safe: NUL-terminated buffer, worst case reads terminator
    g_sqTakeSkip = ObCJKIsLeadByte(lead, g_activeCodePage) &&
                   trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage);
}

static __declspec(naked) void ObCJKSmartQuoteGuardHook()
{
    __asm {
        mov  dword ptr [g_sqSavedEsp], esp
        pushad
        push dword ptr [g_sqSavedEsp]
        call ObCJKSmartQuoteGuardCheck
        add  esp, 4
        popad
        cmp  byte ptr [g_sqTakeSkip], 0
        jz   short sq_native
        mov  byte ptr [esp+0x12], al   ; replay the native var_4D2 store the trampoline would have done
        mov  edx, kVA_SmartQuoteSkipTarget
        jmp  edx
sq_native:
        jmp  dword ptr [g_sqTramp]
    }
}

static void ObCJKInstallSmartQuoteGuardHook()
{
    if (g_sqTramp) return;
    ObCJKInstallHook((BYTE*)kVA_SmartQuoteGuard, (void*)ObCJKSmartQuoteGuardHook, &g_sqTramp, "SmartQuoteGuard");
}

// Sixth hook: sub_575B40 VA 0x575FFB — a second '~' hotkey-marker check that
// sets the line-break/hyphen candidate position (v56/v74/v77). Fix: a 0x7E
// trail byte was marked as a hyphenation point, so wrap overflow could split
// a CJK character and insert '-' inside it.
//
// 5-byte native `cmp` replayed via the standard trampoline; only the
// genuine-pair case jumps manually to the jnz's taken target.
static const DWORD kVA_TrailByteGuard2            = 0x00575FFB;
static const DWORD kVA_TrailByteGuard2SkipTarget  = 0x00576015;  // native jnz's taken target — "not a hotkey marker" path

static BYTE* g_tbg2Tramp      = nullptr;
static DWORD g_tbg2SavedEsp   = 0;
static bool  g_tbg2ForceSkip  = false;

static void __cdecl ObCJKTrailByteGuard2Check(DWORD espSnapshot)
{
    DWORD idx    = *(DWORD*)(espSnapshot + 0x44);
    BYTE* srcPtr = *(BYTE**)(espSnapshot + 0x54);
    BYTE  currentByte = srcPtr[idx];

    // 0x7E preceded by a CJK lead byte is a trail byte, not a '~' marker.
    g_tbg2ForceSkip = (currentByte == 0x7E) && idx > 0 &&
                       ObCJKIsLeadByte(srcPtr[idx - 1], g_activeCodePage);
}

static __declspec(naked) void ObCJKTrailByteGuard2Hook()
{
    __asm {
        mov  dword ptr [g_tbg2SavedEsp], esp
        pushad
        push dword ptr [g_tbg2SavedEsp]
        call ObCJKTrailByteGuard2Check
        add  esp, 4
        popad
        cmp  byte ptr [g_tbg2ForceSkip], 0
        jz   short tbg2_native
        mov  edx, kVA_TrailByteGuard2SkipTarget
        jmp  edx
tbg2_native:
        jmp  dword ptr [g_tbg2Tramp]
    }
}

static void ObCJKInstallTrailByteGuard2Hook()
{
    if (g_tbg2Tramp) return;
    ObCJKInstallHook((BYTE*)kVA_TrailByteGuard2, (void*)ObCJKTrailByteGuard2Hook, &g_tbg2Tramp, "TrailByteGuard2");
}

// Seventh hook: sub_575B40 VA 0x57616E — the "no candidate break point,
// force-break at current position" branch (v58==0). Fix: native writes
// "-\n" carrying only v6[v22-1] past the split; when that byte is a CJK
// trail byte the pair is split and the trail renders as a stray glyph.
//
// 8-byte native span (57616E/576172) replayed via the standard trampoline on
// the non-pair path. esi=v22 (write-cursor index), edi=v6 buffer.
// If v6[v22-2]/v6[v22-1] form a genuine pair, writes v6[v22-2]='-',
// v6[v22-1]='\n', v6[v22]=lead, v6[v22+1]=trail (same v22+2 endpoint).
//
// Fix: a value-range check alone gives false positives (Big5 lead and upper
// trail ranges overlap, e.g. 級/鈍 misread as one pair). Walks back counting
// consecutive bytes >= 0x81: odd run = genuine lead, even = trail of an
// earlier pair (left to native).
static const DWORD kVA_LineSplitPair           = 0x0057616E;
static const DWORD kVA_LineSplitPairSkipTarget = 0x00576176;  // native's own '\n'/'-' writes (not referenced)

// Path A forced-break marker: the two native '-' (0x2D) immediates in
// sub_575B40, flipped to 0x20 when LineBreakSpaceEnablePathA is on (no hook):
//   0x576079 "有候選斷點"        mov byte ptr [ebp+0],2Dh       (C6 45 00 2D)
//   0x57617A "無候選斷點強制斷行" mov byte ptr [esi+edi-1],2Dh  (C6 44 3E FF 2D)
// Codepage-independent.
static BYTE* const kVA_LineBreakMarkerImm_Candidate = (BYTE*)0x0057607C;  // "有候選斷點" branch's '-' immediate
static BYTE* const kVA_LineBreakMarkerImm_Forced    = (BYTE*)0x0057617E;  // "無候選斷點強制斷行" branch's '-' immediate

static void ObCJKApplyLineBreakSpaceSettingPathA()
{
    BYTE want = ObCJKLineBreakSpaceEnabledPathA() ? 0x20 : 0x2D;
    DWORD oldProt;
    BYTE* targets[] = { kVA_LineBreakMarkerImm_Candidate, kVA_LineBreakMarkerImm_Forced };
    for (BYTE* p : targets) {
        VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = want;
        VirtualProtect(p, 1, oldProt, &oldProt);
    }
}

// Path B/C forced-split hyphen marker: sub_5772A0's "no space candidate"
// branch builds a hyphen node via `push 2Dh` (VA 0x577470, 6A 2D) +
// `call sub_576F30`. The immediate is flipped to 0x20 when
// LineBreakSpaceEnablePathBC is on (no hook). Covers both Path B and C.
static BYTE* const kVA_LineBreakMarkerImm_PathBC = (BYTE*)0x00577470;  // sub_5772A0's forced-split hyphen immediate (push 2Dh)

static void ObCJKApplyLineBreakSpaceSettingPathBC()
{
    BYTE want = ObCJKLineBreakSpaceEnabledPathBC() ? 0x20 : 0x2D;
    DWORD oldProt;
    VirtualProtect(kVA_LineBreakMarkerImm_PathBC, 1, PAGE_EXECUTE_READWRITE, &oldProt);
    *kVA_LineBreakMarkerImm_PathBC = want;
    VirtualProtect(kVA_LineBreakMarkerImm_PathBC, 1, oldProt, &oldProt);
}

static BYTE* g_lspTramp      = nullptr;
static DWORD g_lspSavedEsp   = 0;
static bool  g_lspForceSkip  = false;

// Whether p[-offsetBack] is a genuine lead byte: counts consecutive bytes
// >= 0x81 ending at p[-offsetBack]; odd = lead, even = trail. Bounded by
// esiVal so it never reads before v6[0].
static bool ObCJKLineSplitPairIsGenuineLead(const BYTE* p, DWORD esiVal, DWORD offsetBack)
{
    DWORD run = 0;
    for (DWORD i = offsetBack; i <= esiVal; i++) {
        if (p[-(long)i] < 0x81) break;  // ASCII/control anchor: run ends here
        ++run;
    }
    return (run % 2) == 1;
}

static void __cdecl ObCJKLineSplitPairCheck(BYTE* ediVal, DWORD esiVal, DWORD espSnapshot)
{
    g_lspForceSkip = false;
    if (esiVal < 2) return;  // not enough written bytes to examine a pair

    BYTE* p = ediVal + esiVal;   // &v6[v22]
    BYTE trail = p[-1];
    BYTE lead  = p[-2];

    bool valueRangeOk = ObCJKIsLeadByte(lead, g_activeCodePage) && ObCJKIsTrailByte(trail, g_activeCodePage);
    bool genuineLead  = valueRangeOk && ObCJKLineSplitPairIsGenuineLead(p, esiVal, 2);
    bool isPair       = genuineLead;

    if (!isPair) return;

    p[-2] = ObCJKLineBreakSpaceEnabledPathA() ? 0x20 : 0x2D;  // '-' or ' ', per LineBreakSpaceEnablePathA
    p[-1] = 0x0A;  // '\n'
    p[0]  = lead;
    p[1]  = trail;
    g_lspForceSkip = true;
}

static __declspec(naked) void ObCJKLineSplitPairHook()
{
    __asm {
        mov  dword ptr [g_lspSavedEsp], esp
        pushad
        push dword ptr [g_lspSavedEsp]
        push esi
        push edi
        call ObCJKLineSplitPairCheck
        add  esp, 12
        popad
        cmp  byte ptr [g_lspForceSkip], 0
        jz   short lsp_native
        mov  edx, 0x0057617F     ; continuation past both native writes
        jmp  edx
lsp_native:
        jmp  dword ptr [g_lspTramp]
    }
}

static void ObCJKInstallLineSplitPairHook()
{
    if (g_lspTramp) return;
    ObCJKInstallHook((BYTE*)kVA_LineSplitPair, (void*)ObCJKLineSplitPairHook, &g_lspTramp, "LineSplitPair");
}

// Eighth hook: sub_575B40 VA 0x575FAE — per-byte pre-measurement width used
// for the CENTER/RIGHT-align starting pen.x (LEFT-align unaffected). Fix:
// native uses single-byte font-table widths, not the substituted CJK/ASCII
// widths, so menu button text wasn't centered (also pure-ASCII labels with
// AsciiRenderEnable).
//
// 5-byte native `movzx ecx,[esp+0x12]` replayed via the standard trampoline.
// Also SmartQuoteGuard's skip target. Reads srcPtr directly (var_4D2 may be
// rewritten by SmartQuoteGuard), no state carried between firings. Cases,
// in order:
//   1. Trail byte of a genuine pair -> 0 (counted on the lead firing). Not
//      gated on current's value range (BIG5/GBK trail bytes go down to 0x40).
//   2. Genuine lead byte with valid trail -> merged-pair glyph advance.
//   3. ASCII/half-width with AsciiRenderEnable -> substituted glyph advance.
// Anything else falls through to native.
static const DWORD kVA_PreMeasure           = 0x00575FAE;
static const DWORD kVA_PreMeasureAccumulate = 0x00575FDF;  // native "add ebx,eax", reused unmodified

static BYTE* g_pmTramp       = nullptr;
static DWORD g_pmSavedEsp    = 0;
static bool  g_pmForceWidth  = false;
static int   g_pmWidth       = 0;

// Whether srcPtr[idx] is a genuine lead byte: counts consecutive bytes
// >= 0x81 ending at idx; odd = lead, even = trail. Bounded at buffer start.
static bool ObCJKIsGenuineLeadAtIdx(const BYTE* srcPtr, DWORD idx)
{
    DWORD run = 0;
    DWORD i = idx;
    for (;;) {
        if (srcPtr[i] < 0x81) break;
        ++run;
        if (i == 0) break;
        --i;
    }
    return (run % 2) == 1;
}

static void __cdecl ObCJKPreMeasureCheck(DWORD espSnapshot)
{
    g_pmForceWidth = false;
    g_pmWidth = 0;

    DWORD idx         = *(DWORD*)(espSnapshot + 0x44);  // var_4A0
    BYTE* srcPtr       = *(BYTE**)(espSnapshot + 0x54);  // var_490
    void* fontInfoPtr  = *(void**)(espSnapshot + 0x38);  // var_4AC

    BYTE current = srcPtr[idx];
    if (current == 0) return;  // terminator — native handles it

    // Fix (crash): native stores current_byte*56 into var_494 (esp+0x50)
    // after the replaced movzx, and 0x57620D (long strings) re-reads it.
    // Forced-width branches skip that store, so replicate it unconditionally.
    *(DWORD*)(espSnapshot + 0x50) = (DWORD)current * 56;  // var_494

    // --- Case 1: trail byte of a genuine lead at idx-1.
    // Fix: checked first and not gated on current >= 0x81, otherwise BIG5/GBK
    // trail bytes 0x40-0x80 were measured again as ASCII (text shifted left).
    if (idx > 0) {
        BYTE prev = srcPtr[idx - 1];
        if (ObCJKIsLeadByte(prev, g_activeCodePage) && ObCJKIsTrailByte(current, g_activeCodePage) &&
            ObCJKIsGenuineLeadAtIdx(srcPtr, idx - 1)) {
            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            if (fontID >= 0 && !ObCJKIsFontIDExcluded(fontID)) {
                WORD code = (WORD)((prev << 8) | current);
                ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
                if (glyph && glyph->valid) {
                    g_pmWidth = 0;  // already counted on the lead firing
                    g_pmForceWidth = true;
                }
            }
            return;
        }
    }

    // --- Case 2: genuine CJK lead byte ---
    if (ObCJKIsLeadByte(current, g_activeCodePage) && ObCJKIsGenuineLeadAtIdx(srcPtr, idx)) {
        BYTE trail = srcPtr[idx + 1];  // safe: NUL-terminated buffer, worst case reads terminator
        if (trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage)) {
            int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
            if (fontID >= 0 && !ObCJKIsFontIDExcluded(fontID)) {
                WORD code = (WORD)((current << 8) | trail);
                ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
                if (glyph && glyph->valid) {
                    g_pmWidth = (int)glyph->gm.gmCellIncX;
                    g_pmForceWidth = true;
                }
            }
        }
        return;
    }

    // --- Case 3: plain ASCII/half-width byte ---
    if (!ObCJKIsAsciiCandidate(current)) return;
    int fontID = ObCJKFontIDFromInfoPtr(fontInfoPtr);
    if (fontID < 0 || ObCJKIsFontIDExcluded(fontID)) return;
    if (!ObCJKAsciiRenderEnabledForFont(fontID)) return;
    ObCJKGlyphEntry* glyph = ObCJKGlyphAtlas_GetGlyph(fontID, (WORD)current);
    if (!glyph || !glyph->valid) return;

    g_pmWidth = (int)glyph->gm.gmCellIncX;
    g_pmForceWidth = true;
}

static __declspec(naked) void ObCJKPreMeasureHook()
{
    __asm {
        mov  dword ptr [g_pmSavedEsp], esp
        pushad
        push dword ptr [g_pmSavedEsp]
        call ObCJKPreMeasureCheck
        add  esp, 4
        popad
        cmp  byte ptr [g_pmForceWidth], 0
        jz   short pm_native
        mov  eax, dword ptr [g_pmWidth]
        mov  edx, kVA_PreMeasureAccumulate
        jmp  edx
pm_native:
        jmp  dword ptr [g_pmTramp]
    }
}

static void ObCJKInstallPreMeasureHook()
{
    if (g_pmTramp) return;
    ObCJKInstallHook((BYTE*)kVA_PreMeasure, (void*)ObCJKPreMeasureHook, &g_pmTramp, "PreMeasure");
}
