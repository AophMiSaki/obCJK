#pragma once
// DBCS kinsoku line-break hook. Applies obCJK_LineBreakRule.h's table to the
// native word-wrap overflow decision, so book/scroll text doesn't start a
// line with closing punctuation or end one with opening punctuation.
//
// Hook target: sub_577840 VA 0x577946 (the wrapwidth-overflow test itself).
// The overwritten bytes include the jle, so the hook jumps directly to the
// native continue/new-line targets instead of the trampoline.
//   esi = A-node (+0x00 font id, +0x04 lead, +0x05 trail, +0x1C IMG-override)
//   eax = candidate width, edi = block (+0x1C wrapwidth, +0x20 blank-line spacing)
// Also sets block+0x20 to the font's line height.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_LineBreakRule.h"
#include "obCJK_GlyphAtlas.h"

static const DWORD kVA_LineBreakCheck     = 0x00577946;
static const DWORD kVA_LineBreak_NewLine  = 0x0057794B;  // fallthrough target (overflow)
static const DWORD kVA_LineBreak_Continue = 0x0057799C;  // jle-taken target (fits)

static BYTE* g_lineBreakTramp = nullptr;  // allocated but never jumped to

// ForbidLineEnd of the previous character, applied one character late.
// Never reset between text blocks (may cause one extra line-continue at the
// start of the next block).
static bool  g_lbPrevForbidsEnd  = false;
static bool  g_lbContinueBranch  = true;  // true = jump to kVA_LineBreak_Continue

static void __cdecl ObCJKLineBreakCheck(BYTE* aNode, DWORD candidateWidth, BYTE* lineNode)
{
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

        // Fix: native writes block+0x20 from the western .fnt table (a CJK
        // lead byte indexes the wrong entry). block+0x20 is applied as
        // block+0x20 x (newline count - 1), i.e. blank-line / paragraph
        // spacing. Set it to the font's line height (cellHeight + LineGap) so
        // blank lines match text line height.
        int fontID = *(int*)(aNode + 0x00);
        if (!ObCJKIsFontIDExcluded(fontID)) {
            int lh = ObCJKLineHeightForFont(fontID);
            if (lh > 0) *(int*)(lineNode + 0x20) = lh;
        }
    }

    bool forceContinue = nativeOverflow && (forbidStart || g_lbPrevForbidsEnd);
    bool finalOverflow  = nativeOverflow && !forceContinue;

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
