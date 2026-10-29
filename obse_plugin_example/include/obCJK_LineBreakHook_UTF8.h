#pragma once
// UTF-8 version of the kinsoku line-break hook. Hooks the wrapwidth-overflow
// test in sub_577840 (VA 0x577946) and decides whether to break or continue
// the line:
//   - decodes the A-node's codepoint from node[4] (lead byte) + node[5..7]
//     (continuation bytes stashed by the UTF-8 WordWrap/SecondLoop/NodeCopy
//     hooks)
//   - suppresses a native overflow break when the current character is
//     forbidden at line start, or the previous one is forbidden at line end
//   - sets block+0x20 (blank-line spacing) to the font's line height
// Jumps directly to the native continue/new-line targets; the trampoline
// is never used. Shares no logic with the DBCS version.
//
// A-node (esi): +0x00 font id, +0x04 charByte, +0x05..0x07 continuation
// bytes, +0x1C IMG-override.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_LineBreakRule_UTF8.h"
// Needed only for its encoding-agnostic utilities.
#include "obCJK_WordWrapHook_UTF8.h"

static const DWORD kVA_LineBreakCheckUtf8     = 0x00577946;
static const DWORD kVA_LineBreak_NewLineUtf8  = 0x0057794B;  // fallthrough target (overflow)
static const DWORD kVA_LineBreak_ContinueUtf8 = 0x0057799C;  // jle-taken target (fits)

static BYTE* g_lineBreakTrampUtf8 = nullptr;  // allocated but never jumped to

// ForbidLineEnd of the previous character, applied one character late.
// Never reset between text blocks (may cause one extra line-continue at the
// start of the next block).
static bool  g_lbPrevForbidsEndUtf8  = false;
static bool  g_lbContinueBranchUtf8  = true;  // true = jump to kVA_LineBreak_ContinueUtf8

// Decodes the A-node's Unicode codepoint from node[4] + stashed continuation
// bytes in node[5..]. ASCII or an invalid/unstashed sequence returns the raw
// lead byte.
static DWORD ObCJKLineBreakCodepointUtf8(BYTE* aNode)
{
    BYTE lead = aNode[4];
    int seqLen = ObCJKUtf8SeqLen(lead);
    if (seqLen < 2) return (DWORD)lead;

    for (int i = 1; i < seqLen; i++) {
        if (!ObCJKIsUtf8Continuation(aNode[4 + i])) return (DWORD)lead;
    }
    static const BYTE kLeadMask[5] = { 0, 0x7F, 0x1F, 0x0F, 0x07 };  // indexed by seqLen
    DWORD cp = lead & kLeadMask[seqLen];
    for (int i = 1; i < seqLen; i++) cp = (cp << 6) | (aNode[4 + i] & 0x3F);
    return cp;
}

static void __cdecl ObCJKLineBreakCheckUtf8(BYTE* aNode, DWORD candidateWidth, BYTE* lineNode)
{
    DWORD wrapWidth = *(DWORD*)(lineNode + 0x1C);
    bool nativeOverflow = ((int)candidateWidth > (int)wrapWidth);

    DWORD imgField = *(DWORD*)(aNode + 0x1C);  // A-node's own IMG-override field
    bool forbidStart = false, forbidEnd = false;
    if (imgField == 0) {
        DWORD codepoint = ObCJKLineBreakCodepointUtf8(aNode);
        forbidStart = ObCJKForbidLineStartUtf8(codepoint);
        forbidEnd   = ObCJKForbidLineEndUtf8(codepoint);

        // Fix: block+0x20 (blank-line / paragraph spacing) is set to the
        // font's line height instead of the glyph's ink height.
        int fontID = *(int*)(aNode + 0x00);
        if (!ObCJKIsFontIDExcluded(fontID)) {
            int lh = ObCJKLineHeightForFont(fontID);
            if (lh > 0) *(int*)(lineNode + 0x20) = lh;
        }
    }

    bool forceContinue = nativeOverflow && (forbidStart || g_lbPrevForbidsEndUtf8);
    bool finalOverflow  = nativeOverflow && !forceContinue;

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
