#pragma once
// CJK kinsoku (line-break prohibition) rule table — obCJK's own design, not
// ported from obtc.dll (its data table is confirmed dead code for BIG5).
// Design/derivation: 01_文字繪製呼叫鏈.md「五、」節. Wired into the
// wrapwidth-overflow decision by obCJK_LineBreakHook.h (sub_577840 VA
// 0x577946) — see that header for the hook itself.
#include <windows.h>
#include <stdlib.h>
#include "obCJK_Encoding.h"

enum ObCJKLineBreakFlag {
    kObCJKForbidLineStart = 0x01,  // must not begin a line (closing punctuation)
    kObCJKForbidLineEnd   = 0x02,  // must not end a line (opening punctuation)
};

struct ObCJKLineBreakEntry {
    WORD code;   // (lead<<8)|trail for 2-byte chars, or the single byte itself
    BYTE flags;  // ObCJKLineBreakFlag bitmask
};

// Canonical rule set in Unicode, converted per-codepage at runtime via
// WideCharToMultiByte instead of hand-transcribing byte tables.
static const wchar_t kObCJKForbidStartChars[] =
    L"）］｝〉》」』】〕｠"  // ) ] } 〉 》 」 』 】 〕 ﹠
    L"、。，．；：？！"              // 、。，．；：？！
    L"…‥～’”"                                 // …‥～’”
    L"々〆・·";                                      // 々〆・·
static const wchar_t kObCJKForbidEndChars[] =
    L"（［｛〈《「『【〔｟"  // ( [ { 〈 《 「 『 【 〔 ﹝
    L"‘“";                                                  // ‘“

// Built lazily on first lookup, keyed to whatever g_activeCodePage currently
// is. main.cpp reads ActiveCodePage once at startup and never changes it at
// runtime today, so this only ever rebuilds once; ObCJKLineBreakRule_Invalidate()
// is here in case that stops being true later.
static ObCJKLineBreakEntry g_lineBreakTable[128];
static ObCJKCodePage  g_lineBreakBuiltFor = (ObCJKCodePage)-1;

static int __cdecl ObCJKLineBreakCompare(const void* a, const void* b)
{
    return (int)((const ObCJKLineBreakEntry*)a)->code - (int)((const ObCJKLineBreakEntry*)b)->code;
}

// Encodes one Unicode char into the target codepage; returns 0 if the
// codepage has no representation for it (entry is simply omitted).
static WORD ObCJKEncodeCharToCodePage(wchar_t wc, UINT winCodePage)
{
    BYTE buf[2] = {};
    BOOL usedDefault = FALSE;
    int len = WideCharToMultiByte(winCodePage, 0, &wc, 1, (char*)buf, sizeof(buf),
                                   NULL, &usedDefault);
    if (len <= 0 || usedDefault) return 0;
    if (len == 1) return buf[0];
    return (WORD)((buf[0] << 8) | buf[1]);
}

static void ObCJKAddLineBreakFlag(WORD code, BYTE flag)
{
    if (code == 0) return;
    int cap = (int)(sizeof(g_lineBreakTable) / sizeof(g_lineBreakTable[0]));
    for (int i = 0; i < g_lineBreakCount; i++) {
        if (g_lineBreakTable[i].code == code) {
            g_lineBreakTable[i].flags |= flag;
            return;
        }
    }
    if (g_lineBreakCount >= cap) return;  // table sized generously (128) for ~40 entries, shouldn't happen
    g_lineBreakTable[g_lineBreakCount].code  = code;
    g_lineBreakTable[g_lineBreakCount].flags = flag;
    g_lineBreakCount++;
}

static void ObCJKBuildLineBreakTable(ObCJKCodePage cp)
{
    g_lineBreakCount = 0;
    UINT winCP = (UINT)cp;  // kCP_BIG5=950 etc. (obCJK_Encoding.h) are real Windows codepage IDs
    for (const wchar_t* p = kObCJKForbidStartChars; *p; p++)
        ObCJKAddLineBreakFlag(ObCJKEncodeCharToCodePage(*p, winCP), kObCJKForbidLineStart);
    for (const wchar_t* p = kObCJKForbidEndChars; *p; p++)
        ObCJKAddLineBreakFlag(ObCJKEncodeCharToCodePage(*p, winCP), kObCJKForbidLineEnd);
    qsort(g_lineBreakTable, g_lineBreakCount, sizeof(g_lineBreakTable[0]), ObCJKLineBreakCompare);
    g_lineBreakBuiltFor = cp;
}

static BYTE ObCJKLineBreakLookup(WORD code)
{
    if (g_lineBreakBuiltFor != g_activeCodePage)
        ObCJKBuildLineBreakTable(g_activeCodePage);
    if (g_lineBreakCount <= 0) return 0;
    int lo = 0, hi = g_lineBreakCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_lineBreakTable[mid].code == code) return g_lineBreakTable[mid].flags;
        if (g_lineBreakTable[mid].code < code) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static bool ObCJKForbidLineStart(WORD code) { return (ObCJKLineBreakLookup(code) & kObCJKForbidLineStart) != 0; }
static bool ObCJKForbidLineEnd(WORD code)   { return (ObCJKLineBreakLookup(code) & kObCJKForbidLineEnd)   != 0; }

static void ObCJKLineBreakRule_Invalidate() { g_lineBreakBuiltFor = (ObCJKCodePage)-1; }
