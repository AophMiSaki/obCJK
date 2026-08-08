#pragma once
// UTF-8 counterpart of obCJK_LineBreakRule.h. Per the 2026-07-18 architecture
// decision (memory obcjk-utf8-plan), this file shares no logic with the DBCS version.
//
// Structurally simpler than the DBCS table: that file has to round-trip each
// Unicode rule character through WideCharToMultiByte into the active DBCS
// codepage (BYTE buf[2], (lead<<8)|trail) because native/hook code only ever
// sees raw codepage bytes. UTF-8 hook code decodes straight to a Unicode
// codepoint (obCJK_WordWrapHook_UTF8.h's ObCJKUtf8Decode), so this table can
// key directly on the codepoint the rule characters already are — no
// encode/decode round-trip, and no per-codepage rebuild (the table is the
// same regardless of which legacy codepage the game happens to also support).
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

// Same rule set as obCJK_LineBreakRule.h's kObCJKForbidStartChars/
// kObCJKForbidEndChars (duplicated, not shared, per the architecture
// decision above) — every character here is BMP, so a plain wchar_t->DWORD
// widen is a lossless codepoint value on Windows' UTF-16 wchar_t.
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
    if (g_lineBreakCountUtf8 >= cap) return;  // table sized generously (128) for ~40 entries, shouldn't happen
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
