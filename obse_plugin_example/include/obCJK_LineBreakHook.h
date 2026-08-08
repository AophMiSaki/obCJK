#pragma once
// Applies obCJK_LineBreakRule.h's kinsoku (line-break prohibition) table to
// the native word-wrap overflow decision, so book/scroll text doesn't start
// a line with closing punctuation or end one with opening punctuation.
//
// Hook target: sub_577840 VA 0x577946 (the wrapwidth-overflow test itself,
// not a call site). Full register semantics at the hook point, why this
// can't use ObCJKInstallHook's trampoline-replay path (overwritten bytes
// include the jle itself), and the kinsoku decision strategy (forceContinue
// / g_lbPrevForbidsEnd one-character-late ForbidLineEnd tracking): see
// PathC_書籍HTML.md.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_LineBreakRule.h"
#include "obCJK_GlyphAtlas.h"

static const DWORD kVA_LineBreakCheck     = 0x00577946;
static const DWORD kVA_LineBreak_NewLine  = 0x0057794B;  // fallthrough target (overflow)
static const DWORD kVA_LineBreak_Continue = 0x0057799C;  // jle-taken target (fits)

static BYTE* g_lineBreakTramp = nullptr;  // allocated but never jumped to, see top comment

// Whether the most recently placed character (on whatever line it ended up
// on) forbids ending a line — carried across calls so the next overflow can
// be overridden into a continue. No known safe "new text block started"
// hook point to reset this at yet; a stale value at worst forces one
// unnecessary line-continue at the start of an unrelated text block
// (cosmetic, not a crash).
static bool  g_lbPrevForbidsEnd  = false;
static bool  g_lbContinueBranch  = true;  // true = jump to kVA_LineBreak_Continue
static DWORD g_lbHits            = 0;
static DWORD g_lbInterestingHits = 0;     // diagnostic cap, only overridden decisions

static void __cdecl ObCJKLineBreakCheck(BYTE* aNode, DWORD candidateWidth, BYTE* lineNode)
{
    g_lbHits++;
    DWORD wrapWidth = *(DWORD*)(lineNode + 0x1C);
    bool nativeOverflow = ((int)candidateWidth > (int)wrapWidth);

    DWORD imgField = *(DWORD*)(aNode + 0x1C);  // A-node's own IMG-override field
    bool forbidStart = false, forbidEnd = false;
    WORD code = 0;
    if (imgField == 0) {
        BYTE lead = aNode[4];
        bool isCJKPair = false;
        if (ObCJKIsLeadByte(lead, g_activeCodePage)) {
            BYTE trail = aNode[5];
            isCJKPair = (trail != 0 && ObCJKIsTrailByte(trail, g_activeCodePage));
            code = isCJKPair ? (WORD)((lead << 8) | trail) : (WORD)lead;
        } else {
            code = (WORD)lead;
        }
        forbidStart = ObCJKForbidLineStart(code);
        forbidEnd   = ObCJKForbidLineEnd(code);

        // [2026-07-30] line+0x20 height override — see
        // PathC_書籍HTML.md 第22節: sub_576AB0 (Path C draw loop, the
        // wraptree renderer NorthernUI's wrapwidth text actually uses) steps
        // its per-line Y-cursor by `line[0x20] + line[0x18]`. line[0x20] is
        // exactly this function's own host (sub_577840, this+8) — for a CJK
        // lead byte it queries a western .fnt table using that single byte as
        // a raw index (wrong entry); for a genuine ASCII byte rendered
        // through the ORIGINAL native font it's a correct per-glyph read, but
        // when AsciiRenderEnable substitutes a different (possibly taller)
        // GDI face for that byte, this native value no longer matches what's
        // actually drawn either. Either way it's already been computed (and
        // unconditionally written) by the time this hook fires.
        //
        // Take-max, not overwrite: native's own write above is itself
        // unconditional per character — see PathC_書籍HTML.md 第13節「已知風險」— so
        // a mixed CJK+ASCII line would have whichever character is processed
        // LAST win, not the tallest. Comparing against whatever's already in
        // the field (native's untouched value, or an earlier override of
        // ours from a previous character on this same line object) instead
        // accumulates the true per-line max regardless of processing order,
        // and self-resets the moment sub_5765B0 allocates a fresh line
        // object for an actual line break.
        int fontID = *(int*)(aNode + 0x00);
        if (!ObCJKIsFontIDExcluded(fontID)) {
            ObCJKGlyphEntry* glyph = nullptr;
            if (isCJKPair) {
                glyph = ObCJKGlyphAtlas_GetGlyph(fontID, code);
            } else if (ObCJKIsAsciiCandidate(lead) && ObCJKAsciiRenderEnabledForFont(fontID)) {
                glyph = ObCJKGlyphAtlas_GetGlyph(fontID, (WORD)lead);
            }
            if (glyph && glyph->valid) {
                int h = (int)glyph->gm.gmBlackBoxY;
                int* heightSlot = (int*)(lineNode + 0x20);
                if (h > *heightSlot) *heightSlot = h;
            }
        }
    }

    bool forceContinue = nativeOverflow && (forbidStart || g_lbPrevForbidsEnd);
    bool finalOverflow  = nativeOverflow && !forceContinue;

    if (forceContinue && g_lbInterestingHits < 40) {
        g_lbInterestingHits++;
    }

    g_lbPrevForbidsEnd = forbidEnd;
    g_lbContinueBranch = !finalOverflow;
}

static __declspec(naked) void ObCJKLineBreakHook()
{
    __asm {
        pushad
        push edi                 ; arg2 (lineNode)
        push eax                 ; arg1 (candidateWidth)
        push esi                 ; arg0 (aNode)
        call ObCJKLineBreakCheck
        add  esp, 12
        popad
        cmp  byte ptr [g_lbContinueBranch], 0
        je   short lb_newline
        mov  edx, 0x57799C       ; kVA_LineBreak_Continue
        jmp  edx
lb_newline:
        mov  edx, 0x57794B       ; kVA_LineBreak_NewLine
        jmp  edx
    }
}

static void ObCJKInstallLineBreakHook()
{
    if (g_lineBreakTramp) return;
    ObCJKInstallHook((BYTE*)kVA_LineBreakCheck, (void*)ObCJKLineBreakHook, &g_lineBreakTramp, "LineBreak");
}
