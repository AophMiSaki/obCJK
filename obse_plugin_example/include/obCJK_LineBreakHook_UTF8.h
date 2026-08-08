#pragma once
// UTF-8 counterpart of obCJK_LineBreakHook.h — same native hook point
// (sub_577840 VA 0x577946, the wrapwidth-overflow test itself; full register
// semantics / why this can't use ObCJKInstallHook's trampoline-replay path /
// the kinsoku decision strategy: see that file's header comment and
// PathC_書籍HTML.md). Per the 2026-07-18 architecture decision
// (memory obcjk-utf8-plan), this file shares
// no logic with the DBCS version.
//
// Node layout re-verified for THIS function specifically (not just assumed
// shared from obCJK_WordWrapHook_UTF8.h/obCJK_GlyphHook_UTF8.h) — IDA
// decompile+disasm of sub_577840 (the hook's host function, connected to
// Oblivion.exe, confirmed via lookup_funcs on 0x573F10/0x577840/0x577120)
// shows the hook's `esi` (A-node) is the same 56-byte char node: it's passed
// unmodified into the already-verified sub_5772A0, and sub_577840 itself only
// reads esi+0x00 (font id) / esi+0x04 (charByte) / esi+0x1C (IMG-override) —
// same fields obCJK_WordWrapHook_UTF8.h uses. The function's two other
// callees (sub_576A30, a global font-table singleton getter that never
// touches the char node; sub_577710, a DIFFERENT 52-byte B-type line-container
// constructor that only writes its own freshly-malloc'd object) were checked
// too — neither writes esi+0x05~0x07. So the node[5..7] continuation-byte
// stash written by obCJK_WordWrapHook_UTF8.h's ObCJKWordWrapCheckUtf8/
// ObCJKSecondLoopCheckUtf8 (and carried by ObCJKNodeCopyUtf8) survives
// untouched into this hook, same as it does for GlyphHook_UTF8.h's Path C.
// memory obcjk-utf8-plan「續5」.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_LineBreakRule_UTF8.h"
// Needed only for ObCJKGlyphAtlas_GetGlyph()/ObCJKUtf8CodeForGlyph() reuse —
// same "encoding-agnostic utility, not disambiguation logic" exception
// obCJK_GlyphHook_UTF8.h already relies on when it includes this same file.
#include "obCJK_WordWrapHook_UTF8.h"

static const DWORD kVA_LineBreakCheckUtf8     = 0x00577946;
static const DWORD kVA_LineBreak_NewLineUtf8  = 0x0057794B;  // fallthrough target (overflow)
static const DWORD kVA_LineBreak_ContinueUtf8 = 0x0057799C;  // jle-taken target (fits)

static BYTE* g_lineBreakTrampUtf8 = nullptr;  // allocated but never jumped to, same rationale as the DBCS version

// Same one-character-late ForbidLineEnd tracking as the DBCS version, and the
// same known limitation (no safe "new text block started" hook point to
// reset it — worst case is one unnecessary line-continue at the start of an
// unrelated text block, cosmetic, not a crash).
static bool  g_lbPrevForbidsEndUtf8  = false;
static bool  g_lbContinueBranchUtf8  = true;  // true = jump to kVA_LineBreak_ContinueUtf8
static DWORD g_lbHitsUtf8            = 0;
static DWORD g_lbInterestingHitsUtf8 = 0;     // diagnostic cap, only overridden decisions

// Reconstructs the Unicode codepoint of the character this A-node describes.
// node[4] is the raw first byte (charByte, set natively regardless of
// encoding — see this file's derivation comment above for why that field is
// encoding-agnostic). If it's a validly-stashed multi-byte lead (continuation
// bytes present in node[5..], written by obCJK_WordWrapHook_UTF8.h before
// this hook ever fires), decode the full sequence; otherwise treat the raw
// byte itself as the codepoint (plain ASCII, or a malformed/un-stashed
// sequence — same "fall back to the raw byte, never worse than not
// recognizing kinsoku for it" philosophy as every other unmapped case in the
// UTF-8 files).
// outIsMultiByte (optional): set true only when a full, validly-stashed
// multi-byte sequence was actually decoded — lets callers (the line+0x20
// height override below) gate CJK-only logic without re-deriving seqLen.
static DWORD ObCJKLineBreakCodepointUtf8(BYTE* aNode, bool* outIsMultiByte = nullptr)
{
    if (outIsMultiByte) *outIsMultiByte = false;
    BYTE lead = aNode[4];
    int seqLen = ObCJKUtf8SeqLen(lead);
    if (seqLen < 2) return (DWORD)lead;

    for (int i = 1; i < seqLen; i++) {
        if (!ObCJKIsUtf8Continuation(aNode[4 + i])) return (DWORD)lead;
    }
    static const BYTE kLeadMask[5] = { 0, 0x7F, 0x1F, 0x0F, 0x07 };  // indexed by seqLen, mirrors ObCJKUtf8Decode
    DWORD cp = lead & kLeadMask[seqLen];
    for (int i = 1; i < seqLen; i++) cp = (cp << 6) | (aNode[4 + i] & 0x3F);
    if (outIsMultiByte) *outIsMultiByte = true;
    return cp;
}

static void __cdecl ObCJKLineBreakCheckUtf8(BYTE* aNode, DWORD candidateWidth, BYTE* lineNode)
{
    g_lbHitsUtf8++;
    DWORD wrapWidth = *(DWORD*)(lineNode + 0x1C);
    bool nativeOverflow = ((int)candidateWidth > (int)wrapWidth);

    DWORD imgField = *(DWORD*)(aNode + 0x1C);  // A-node's own IMG-override field
    bool forbidStart = false, forbidEnd = false;
    if (imgField == 0) {
        bool isMultiByte = false;
        DWORD codepoint = ObCJKLineBreakCodepointUtf8(aNode, &isMultiByte);
        forbidStart = ObCJKForbidLineStartUtf8(codepoint);
        forbidEnd   = ObCJKForbidLineEndUtf8(codepoint);

        // [2026-07-30] line+0x20 height override — DBCS counterpart
        // (obCJK_LineBreakHook.h) has the full sub_576AB0 Path C render-time
        // root cause writeup (PathC_書籍HTML.md 第22節) and the
        // take-max-not-overwrite rationale (第13節「已知風險」: native's own
        // write is itself unconditional per character, so a mixed CJK+ASCII
        // line would otherwise let whichever character is processed LAST win
        // instead of the tallest).
        int fontID = *(int*)(aNode + 0x00);
        if (!ObCJKIsFontIDExcluded(fontID)) {
            ObCJKGlyphEntry* glyph = nullptr;
            if (isMultiByte) {
                WORD code = ObCJKUtf8CodeForGlyph(codepoint);
                glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
            } else {
                BYTE lead = aNode[4];
                if (ObCJKUtf8IsAsciiCandidate(lead) && ObCJKAsciiRenderEnabledForFont(fontID)) {
                    glyph = ObCJKGlyphAtlas_GetGlyph(fontID, (WORD)lead);
                }
            }
            if (glyph && glyph->valid) {
                int h = (int)glyph->gm.gmBlackBoxY;
                int* heightSlot = (int*)(lineNode + 0x20);
                if (h > *heightSlot) *heightSlot = h;
            }
        }
    }

    bool forceContinue = nativeOverflow && (forbidStart || g_lbPrevForbidsEndUtf8);
    bool finalOverflow  = nativeOverflow && !forceContinue;

    if (forceContinue && g_lbInterestingHitsUtf8 < 40) {
        g_lbInterestingHitsUtf8++;
    }

    g_lbPrevForbidsEndUtf8 = forbidEnd;
    g_lbContinueBranchUtf8 = !finalOverflow;
}

static __declspec(naked) void ObCJKLineBreakHookUtf8()
{
    __asm {
        pushad
        push edi                 ; arg2 (lineNode)
        push eax                 ; arg1 (candidateWidth)
        push esi                 ; arg0 (aNode)
        call ObCJKLineBreakCheckUtf8
        add  esp, 12
        popad
        cmp  byte ptr [g_lbContinueBranchUtf8], 0
        je   short lb_newline_utf8
        mov  edx, 0x57799C       ; kVA_LineBreak_ContinueUtf8
        jmp  edx
lb_newline_utf8:
        mov  edx, 0x57794B       ; kVA_LineBreak_NewLineUtf8
        jmp  edx
    }
}

static void ObCJKInstallLineBreakHookUtf8()
{
    if (g_lineBreakTrampUtf8) return;
    ObCJKInstallHook((BYTE*)kVA_LineBreakCheckUtf8, (void*)ObCJKLineBreakHookUtf8, &g_lineBreakTrampUtf8, "LineBreakUtf8");
}
