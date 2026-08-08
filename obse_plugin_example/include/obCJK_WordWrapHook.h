#pragma once
// Hooks Oblivion.exe's string-layout main loop (sub_577C10) right after its
// per-byte metrics call (sub_577120) returns, to correct the advance width
// for CJK 2-byte pairs during word-wrap. Node field layout, hook-point
// derivation: see Hook內容與規範.md「二、」節. node+0x24
// Hooks 4-8 (sub_575B40's five independent alignment/garbling fixes) are
// documented in full in PathA主要文字顯示.md.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obse/GameAPI.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_GlyphAtlas.h"
// Eighth hook (below) needs ObCJKFontIDFromInfoPtr() to resolve sub_575B40's
// raw FontInfo* ("this") into a fontID — same reverse lookup obCJK_GlyphHook.h's
// Path A/B already use for the identical FontInfo*-shaped pointer. Pulled in
// here (rather than relying on main.cpp's separate include of this file to
// come first) so this header compiles standalone regardless of include order.
#include "obCJK_GlyphHook.h"

static const DWORD kVA_WordWrapLoop = 0x00577F05;

static BYTE* g_wordWrapTramp = nullptr;
static DWORD g_wwSavedEsp    = 0;
static bool  g_wwIsCJK       = false;

// [1-a] ASCII/half-width single-byte advance override — mirrors the CJK
// branch below but with no lead/trail merge (single byte, esi/ebp already
// advances by exactly 1 natively, no extra skip needed). Overwrites this
// node's advance with our GDI atlas font's measurement so word-wrap layout
// width matches what obCJK_GlyphHook.h actually draws once it substitutes
// this byte too — see D3D替換文字.md「三、」. Codes GDI can't
// rasterize (space, control chars — entry->valid stays false, see
// ObCJKGlyphAtlas_GetGlyph) are silently left on native advance, which is
// correct because the draw side leaves those on native rendering too.
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
        // [2026-07-12] HEIGHT override — see PathA主要文字顯示.md 第15節:
        // node+0x28 is fed by sub_577120's per-font-slot FIXED height constant
        // (decompiled as this[10] there), never computed per-character like
        // WIDTH (this[9]/node+0x24) is, so CJK glyphs taller than that
        // constant were silently under-measured. Same int32-not-float
        // caution as the WIDTH field above. CJK-only (this branch already
        // requires a valid lead/trail pair) — never applied to the ASCII
        // override function, which must leave native western line-height
        // constants untouched.
        *(int*)(node + 0x28) = (int)glyph->gm.gmBlackBoxY;
        // +0x05 padding stash, carried onward by ObCJKNodeCopyHook below —
        // see Hook內容與規範.md「四、」節.
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
// carrying the +0x05 trail byte onward so obCJK_GlyphHook.h's draw-side
// hooks can see it. VA derivation + register-safety proof: see
// Hook內容與規範.md「一、」.
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
// 0x578AD4-0x578B4E), fixes an index-desync bug on CJK pairs (confirmed by
// the 5-call-site sub_577120 sweep that only this loop needs a hook).
// VA/register derivation: Hook內容與規範.md「四、」.
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
        // [1-a] same ASCII override as ObCJKWordWrapCheck above, this loop's
        // own node/field layout (node+0x00/0x1C/0x24) is identical.
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
        // [2026-07-12] HEIGHT override — same fix/rationale as
        // ObCJKWordWrapCheck above, see its comment + PathA主要文字顯示.md 第15節.
        *(int*)(node + 0x28) = (int)glyph->gm.gmBlackBoxY;
        *(BYTE*)(node + 0x05) = trail;  // carried onward by NodeCopyHook, same as above
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
// `cmp al,7Eh`/`jz`, the instruction that strips '~' hotkey markers. A CJK
// trail byte that happens to equal 0x7E (e.g. Big5 "繼"=C4 7E) gets
// misidentified as the marker and stripped, orphaning the lead byte. Full
// technical writeup: PathA主要文字顯示.md「一、」.
//
// NOT installed via ObCJKInstallHook's trampoline-replay path: the
// intercepted range ends with `jz rel8` whose displacement is relative to
// its own address, so this hook always recomputes the branch in C and jumps
// to one of the two real native VAs directly (trampoline still allocated
// but unused).
//
// espSnapshot: hooked instruction is `mov al,[esp+idx-derived]` itself, so
// AL isn't loaded yet — must recompute from idx/srcPtr. sub_575B40 has a
// flat esp frame (single `sub esp,4D4h` prologue): var_4A0(idx)->esp+0x44,
// var_490(srcPtr)->esp+0x54 (same offsets reused by every guard hook below).
static const DWORD kVA_TrailByteGuard = 0x00576226;

static BYTE* g_tbgTramp    = nullptr;  // allocated but never jumped to, see comment above
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
    // A trail byte that happens to equal 0x7E is only distinguishable from
    // a real '~' hotkey marker by what precedes it: a genuine hotkey marker
    // is never immediately preceded by a CJK lead byte expecting this
    // exact byte as its pair.
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

// Fifth hook: sub_575B40 (same function as TrailByteGuard above) VA
// 0x575F8A — feeds the native smart-quote jump table (VA 0x575F91) that
// unconditionally substitutes byte values 0x91-0x94 with ASCII '/". Those
// values fall inside every supported codepage's CJK lead-byte range, so a
// genuine CJK lead byte in 0x91-0x94 gets mangled into a quote + orphaned
// trail byte — a content-corruption bug (sub_576670's own draw loop has the
// identical substitution), distinct from the CENTER-align width bug. Full
// technical writeup: PathA主要文字顯示.md「二、」.
//
// 7-byte native span (575F8A `mov[esp+0x12],al` + 575F8E `movzx eax,al`),
// no short jump inside it, so unlike TrailByteGuard this uses
// ObCJKInstallHook's standard trampoline replay for the native path. Same
// esp+0x44/esp+0x54 offsets as TrailByteGuard above; var_4D2(current byte,
// pre-substitution)->esp+0x12. AL already holds the raw current byte at
// hook entry (loaded earlier in the loop) — no recompute needed here.
static const DWORD kVA_SmartQuoteGuard      = 0x00575F8A;
static const DWORD kVA_SmartQuoteSkipTarget = 0x00575FAE;  // def_575F9B, jump-table convergence point (also kVA_PreMeasure, the Eighth hook below)

static BYTE* g_sqTramp     = nullptr;
static DWORD g_sqSavedEsp  = 0;
static bool  g_sqTakeSkip  = false;

static void __cdecl ObCJKSmartQuoteGuardCheck(DWORD espSnapshot)
{
    DWORD idx    = *(DWORD*)(espSnapshot + 0x44);
    BYTE* srcPtr = *(BYTE**)(espSnapshot + 0x54);
    BYTE  lead   = srcPtr[idx];

    // A byte in 0x91-0x94 is only distinguishable from a real smart-quote
    // marker by whether it's actually pairing with a valid trail byte —
    // same disambiguation strategy as ObCJKTrailByteGuardCheck above.
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
        mov  byte ptr [esp+0x12], al   ; replay the native store the trampoline would have done — CJK path skips the jump table entirely, so nothing else will write var_4D2 back
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

// Sixth hook: sub_575B40 (same function as the two guards above) VA
// 0x575FFB — a second, independent '~' hotkey-marker check (distinct from
// TrailByteGuardHook's VA 0x576226), gating v56/v74/v77 (line-break/hyphen
// insertion candidate position) rather than the output-copy decision. Same
// 0x7E-trail-byte misidentification as the fourth hook, but worse here: it
// marks the position as a legal hyphenation point, so a wrap-width overflow
// can slice a CJK character in half and insert a `-` mid-character. Full
// technical writeup: PathA主要文字顯示.md「三、」.
//
// 5-byte native `cmp` (opcode 0x80, ObCJKGetInstrLen handles it) stops
// exactly at the following untouched `jnz short 576015` — unlike
// TrailByteGuardHook, the native comparison can be replayed via the
// standard trampoline and only the forced-skip (genuine CJK pair) case
// needs a manual jump to the jnz's taken target. Same esp+0x44/esp+0x54
// offsets as the fourth hook (same function, same flat frame).
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

    // Same disambiguation as ObCJKTrailByteGuardCheck above: a trail byte
    // that happens to equal 0x7E is only distinguishable from a real '~'
    // hotkey marker by whether the preceding byte is a CJK lead byte
    // expecting this exact byte as its pair.
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

// Seventh hook: sub_575B40 (same function as the three guards above) VA
// 0x57616E — the "no candidate break point, force-break at current
// position" branch (v58==0 in the decompile). When width overflow hits with
// no prior space/hotkey candidate recorded, native code splices a "-\n"
// break in by blindly treating v6[v22-1] as one arbitrary byte to carry past
// the split; if that byte is actually a CJK pair's trail half (lead at
// v6[v22-2]), the split lands inside the pair and the trail byte renders as
// a stray ASCII glyph. The sibling branches at VA 0x576039 (real memmove of
// the whole tail) are NOT affected. Full technical writeup:
// PathA主要文字顯示.md「四、」.
//
// 8-byte native span (57616E/576172, two `mov [esi+edi+k],reg`) ends
// exactly on the 576176 instruction boundary; native's own '\n'/'-' writes
// there are left untouched and reached via standard trampoline replay on
// the non-pair path. esi=v22 (write-cursor index), edi=reallocated v6
// pointer — live registers, not stack locals, so (like
// ObCJKWordWrapCheck/ObCJKSecondLoopCheck) passed as explicit call args.
//
// Fix: peek v6[v22-2]/v6[v22-1] directly (don't trust AL/DL). If they form
// a valid CJK pair, write the break one byte earlier so the whole pair
// moves past the split as a unit: v6[v22-2]='-', v6[v22-1]='\n',
// v6[v22]=lead, v6[v22+1]=trail — same v22+2 endpoint native's own carry
// produces, so nothing downstream needs to change.
//
// A pure value-range check on v6[v22-2]/v6[v22-1] is NOT sufficient: Big5's
// lead range (0x81-0xFE) and the upper half of its trail range (0xA1-0xFE)
// fully overlap, so a byte that is structurally the trailing half of the
// character before it can itself pass ObCJKIsLeadByte — confirmed false
// positive via diagnostic log (full byte trace: PathA主要文字顯示.md
// 「四、」false-positive段落; 級/鈍
// adjacent-but-unrelated characters misread as one pair).
// Fix: walk backward from v6[v22-2] counting the run of consecutive high
// bytes (>=0x81); odd run length = genuinely un-paired lead byte, even =
// trailing half of an earlier pair (false positive — defer to native,
// which self-heals this case since LABEL_91 writes its trail right after).
static const DWORD kVA_LineSplitPair           = 0x0057616E;
static const DWORD kVA_LineSplitPairSkipTarget = 0x00576176;  // native's own '\n'/'-' writes, reached only on the non-pair path via trampoline replay — not used directly, kept for documentation

// The two OTHER native '-' (0x2D) writes in sub_575B40, both outside any
// installed hook's range and unrelated to the CJK-pair guard above — the
// "有候選斷點" branch (mov byte ptr [ebp+0],2Dh, memmove-based, real bytes
// C6 45 00 2D at VA 0x576079) and the "無候選斷點強制斷行" branch's own
// unmodified fallthrough (mov byte ptr [esi+edi-1],2Dh, real bytes
// C6 44 3E FF 2D at VA 0x57617A). Both are 4/5-byte `mov r/m8,imm8`
// instructions where the immediate operand is the very last byte — flipping
// just that byte (0x2D <-> 0x20) changes the marker character with zero
// control-flow changes, so it doesn't need a hook at all. IDA-confirmed live
// (2026-07-24, exe matches PathA主要文字顯示.md「四、」
// byte-for-byte, no drift). DBCS and UTF8 builds share the same .exe, so
// this only needs to run once regardless of active codepage. sub_575B40's
// only caller is sub_576670 (Path A), so this pair is Path-A-only — see the
// PathBC pair below for the separate, shared Path B/C tree.
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

// Path B/C's own forced-split hyphen marker — a completely different native
// function (sub_5772A0, the shared word-wrap tree used by the general
// string-layout pipeline, NOT sub_575B40) from the Path A pair above.
// IDA-confirmed (2026-07-29): when sub_5772A0's "no space candidate to break
// at" branch force-splits mid-word, it builds a one-character hyphen node via
// `push 2Dh` (VA 0x577470) followed by `call sub_576F30` (the tree-node
// constructor — the pushed byte becomes node+0x04, the char the node
// renders). `push imm8` encodes as two bytes (6A 2D) with the immediate as
// the second byte, so flipping just that byte (0x2D <-> 0x20) swaps the
// marker with zero control-flow changes, same technique as the Path A pair
// — no hook needed. sub_5772A0 has no notion of Path B vs Path C (both flow
// through the same tree before draw dispatch splits them), so one switch
// necessarily covers both.
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

// Determines whether p[-offsetBack] is genuinely acting as a CJK lead byte
// (as opposed to the trailing half of an earlier pair with a
// lead-range-compatible value too — see the false-positive note above).
// Walks backward counting the run of consecutive high bytes (>=0x81) ending
// at (and including) p[-offsetBack]; an odd run length means that position
// is a genuine, currently-unpaired lead byte, even means it's a trail byte.
// Bounded by esiVal so it never reads before v6[0].
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
        mov  edx, 0x0057617F     ; shared continuation past both native writes (font/width calc, esi+=2 happens later, unaffected)
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

// Eighth hook: sub_575B40 (same function as the guards/LineSplitPair above)
// VA 0x575FAE — entry point of the per-byte pre-measurement width formula
// that CENTER/RIGHT-align pen.x offsets are computed from, before the
// glyph-draw loop (sub_576670) even starts (LEFT-align never reads this
// total and is unaffected). This native formula does a single-byte
// font-table lookup completely independent of what the draw side actually
// substitutes for the same byte (merged CJK pair width, or — when
// AsciiRenderEnable is on — GDI-measured ASCII advance), so the CENTER/
// RIGHT starting pen.x doesn't land where the glyph loop's own per-char
// pen.x ends up: the "menu button text not centered" bug, including
// pure-ASCII labels like "Mods" (ASCII-substitution mismatch, no CJK pair
// involved). Full technical writeup: PathA主要文字顯示.md
// 「五、」.
//
// 5-byte native `movzx ecx,[esp+0x12]` — standard trampoline replays it
// unchanged on the native-fallback path. This VA is also SmartQuoteGuard's
// skip-target, so a CJK pair it already disambiguated flows straight into
// this hook too, no separate wiring needed. Same esp+0x44/esp+0x54 frame as
// every other guard, plus fontInfoPtr(var_4AC)->esp+0x38 (the `this`
// ObCJKFontIDFromInfoPtr() already resolves for Path A).
//
// Design: peek srcPtr[idx]/[idx-1]/[idx+1] directly — never trust var_4D2
// (may already be rewritten by SmartQuoteGuard), never carry state between
// per-byte firings. Same genuine-lead-vs-trailing-half parity walk as
// LineSplitPair's ObCJKLineSplitPairIsGenuineLead, applied forward across
// the source string. Three mutually exclusive cases, checked in this order
// (order/gating matters — see the fix note on ObCJKPreMeasureCheck below):
//   1. Structurally the trailing half of a real pair -> contribute 0
//      (already counted on the lead firing). Checked BEFORE case 2/3 and
//      NOT gated on current's own value range, since BIG5/GBK trail bytes
//      span 0x40-0xFE, well below the lead-byte floor 0x81.
//   2. Genuine, currently-unpaired CJK lead byte with valid trailing byte
//      -> override with the same merged-pair glyph width the draw side
//      uses. Never also treated as an ASCII candidate.
//   3. Neither of the above (including SJIS's half-width-katakana gap
//      0xA0-0xDF) -> mirrors ObCJKWordWrapAsciiOverride: override with this
//      byte's own GDI-measured advance when AsciiRenderEnable is on.
// Anything not covered falls through to the native formula unchanged via
// the standard trampoline — never worse than native.
static const DWORD kVA_PreMeasure           = 0x00575FAE;
static const DWORD kVA_PreMeasureAccumulate = 0x00575FDF;  // native "add ebx,eax", reused unmodified

static BYTE* g_pmTramp       = nullptr;
static DWORD g_pmSavedEsp    = 0;
static bool  g_pmForceWidth  = false;
static int   g_pmWidth       = 0;

static DWORD g_pmHits         = 0;  // diagnostic cap, same convention as kPADiagCap etc.
static const DWORD kPMDiagCap = 60;

// Same technique as ObCJKLineSplitPairIsGenuineLead above — walks backward
// from idx counting the run of consecutive high bytes (>=0x81) ending at
// (and including) idx. Odd run = genuinely unpaired lead byte at idx; even
// run = trailing half of an earlier pair. Bounded at the buffer start.
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

    // Native stores current_byte*56 into var_494 (esp+0x50) right after the
    // movzx this hook replaces, and a downstream wrap-overflow block
    // (~VA 0x57620D, long strings only) re-reads that same slot for a
    // second font-table lookup. Every forced-width branch below jumps
    // straight to native's accumulate instruction, skipping that store —
    // left unfixed, var_494 stays stale and 0x57620D dereferences garbage
    // (crash root cause: PathA主要文字顯示.md「五、」
    // Regression 2). Always replicate the store unconditionally: harmless on the
    // native-fallback path too, since the trampoline's own replay
    // overwrites it again before anything downstream reads it.
    *(DWORD*)(espSnapshot + 0x50) = (DWORD)current * 56;  // var_494

    // --- Case 1 (checked FIRST, unconditionally on current's own value):
    // structurally the trailing half of the byte immediately before us.
    // Gating this on `current >= 0x81` (an earlier version's regression,
    // see PathA主要文字顯示.md「五、」Regression 1)
    // wrongly assumed every codepage's trail-byte floor matches its lead-byte floor —
    // BIG5/GBK's real trail-byte floor is 0x40, so trail bytes in 0x40-0x80
    // fell through to the ASCII branch and got measured a second time on
    // top of the lead firing's merged width, inflating the CENTER/RIGHT
    // premeasure total (text shifted left of true center). Fix: check
    // "trailing half of a genuine lead at idx-1" first, regardless of
    // current's own value — same disambiguation TrailByteGuard/
    // TrailByteGuard2/SmartQuoteGuard above already use.
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
                    g_pmHits++;
                    /*
                    if (g_pmHits <= kPMDiagCap) {
                        _MESSAGE("obCJK:PreMeasure:trail hit#%lu idx=%u code=0x%04X slot=%d width=0 (already counted)",
                                  g_pmHits, idx, code, ObCJKSlotFromFontID(fontID));
                    }
                    */
                }
            }
            return;  // structurally a real pair's trailing half -> never
                      // also an ASCII candidate, regardless of whether we
                      // forced a width above
        }
    }

    // --- Case 2: genuine, currently-unpaired CJK lead byte ---
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
                    g_pmHits++;
                    /*
                    if (g_pmHits <= kPMDiagCap) {
                        _MESSAGE("obCJK:PreMeasure:lead hit#%lu idx=%u code=0x%04X slot=%d width=%d",
                                  g_pmHits, idx, code, ObCJKSlotFromFontID(fontID), g_pmWidth);
                    }
                    */
                }
            }
        }
        return;  // lead byte -> never also an ASCII candidate, regardless of
                  // whether we forced a width above
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
    g_pmHits++;
    /*
    if (g_pmHits <= kPMDiagCap) {
        _MESSAGE("obCJK:PreMeasure:ascii hit#%lu idx=%u byte=0x%02X slot=%d width=%d",
                  g_pmHits, idx, current, ObCJKSlotFromFontID(fontID), g_pmWidth);
    }
    */
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
