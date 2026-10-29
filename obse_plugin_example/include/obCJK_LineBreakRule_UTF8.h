#pragma once
// UTF-8 kinsoku (line-break) table, separate from obCJK_LineBreakRule.h
// (no shared logic). Keyed directly by Unicode codepoint (UTF-8 hooks decode
// to codepoints), so it is built once and never re-encoded per code page.
#include <windows.h>
#include <stdlib.h>

enum ObCJKLineBreakFlagUtf8 {
    kObCJKForbidLineStartUtf8 = 0x01,  // must not begin a line (closing punctuation)
    kObCJKForbidLineEndUtf8   = 0x02,  // must not end a line (opening punctuation)
};

struct ObCJKLineBreakEntryUtf8 {
    DWORD codepoint;
    BYTE  flags;  // ObCJKLineBreakFlagUtf8 bitmask
};

// Same rule set as obCJK_LineBreakRule.h (duplicated). All BMP, so
// wchar_t -> DWORD is the codepoint.
static const wchar_t kObCJKForbidStartCharsUtf8[] =
    L"）］｝〉》」』】〕｠"
    L"、。，．；：？！"
    L"…‥～’”"
    L"々〆・·";
static const wchar_t kObCJKForbidEndCharsUtf8[] =
    L"（［｛〈《「『【〔｟"
    L"‘“";

static ObCJKLineBreakEntryUtf8 g_lineBreakTableUtf8[128];
static int  g_lineBreakCountUtf8 = 0;
static bool g_lineBreakBuiltUtf8 = false;  // built once — codepoint keys don't depend on g_activeCodePage

static int __cdecl ObCJKLineBreakCompareUtf8(const void* a, const void* b)
{
    DWORD ca = ((const ObCJKLineBreakEntryUtf8*)a)->codepoint;
    DWORD cb = ((const ObCJKLineBreakEntryUtf8*)b)->codepoint;
    return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

static void ObCJKAddLineBreakFlagUtf8(DWORD codepoint, BYTE flag)
{
    if (codepoint == 0) return;
    int cap = (int)(sizeof(g_lineBreakTableUtf8) / sizeof(g_lineBreakTableUtf8[0]));
    for (int i = 0; i < g_lineBreakCountUtf8; i++) {
        if (g_lineBreakTableUtf8[i].codepoint == codepoint) {
            g_lineBreakTableUtf8[i].flags |= flag;
            return;
        }
    }
    if (g_lineBreakCountUtf8 >= cap) return;
    g_lineBreakTableUtf8[g_lineBreakCountUtf8].codepoint = codepoint;
    g_lineBreakTableUtf8[g_lineBreakCountUtf8].flags     = flag;
    g_lineBreakCountUtf8++;
}

static void ObCJKBuildLineBreakTableUtf8()
{
    g_lineBreakCountUtf8 = 0;
    for (const wchar_t* p = kObCJKForbidStartCharsUtf8; *p; p++)
        ObCJKAddLineBreakFlagUtf8((DWORD)*p, kObCJKForbidLineStartUtf8);
    for (const wchar_t* p = kObCJKForbidEndCharsUtf8; *p; p++)
        ObCJKAddLineBreakFlagUtf8((DWORD)*p, kObCJKForbidLineEndUtf8);
    qsort(g_lineBreakTableUtf8, g_lineBreakCountUtf8, sizeof(g_lineBreakTableUtf8[0]), ObCJKLineBreakCompareUtf8);
    g_lineBreakBuiltUtf8 = true;
}

static BYTE ObCJKLineBreakLookupUtf8(DWORD codepoint)
{
    if (!g_lineBreakBuiltUtf8) ObCJKBuildLineBreakTableUtf8();
    if (g_lineBreakCountUtf8 <= 0) return 0;
    int lo = 0, hi = g_lineBreakCountUtf8 - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        DWORD midCp = g_lineBreakTableUtf8[mid].codepoint;
        if (midCp == codepoint) return g_lineBreakTableUtf8[mid].flags;
        if (midCp < codepoint) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static bool ObCJKForbidLineStartUtf8(DWORD codepoint) { return (ObCJKLineBreakLookupUtf8(codepoint) & kObCJKForbidLineStartUtf8) != 0; }
static bool ObCJKForbidLineEndUtf8(DWORD codepoint)   { return (ObCJKLineBreakLookupUtf8(codepoint) & kObCJKForbidLineEndUtf8)   != 0; }
