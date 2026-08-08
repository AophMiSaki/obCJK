#pragma once
#include <windows.h>

enum ObCJKCodePage {
    kCP_BIG5   = 950,
    kCP_GBK    = 936,
    kCP_SJIS   = 932,
    kCP_KOREAN = 949,
    kCP_UTF8   = 65001,
};

#include "obcjk_globalVariable.h"

static ObCJKCodePage ObCJKCodePageFromString(const char* s, ObCJKCodePage def = kCP_BIG5)
{
    if (!s || !s[0]) return def;
    if (_stricmp(s, "big5")      == 0 || _stricmp(s, "950") == 0) return kCP_BIG5;
    if (_stricmp(s, "gbk")       == 0 || _stricmp(s, "936") == 0) return kCP_GBK;
    if (_stricmp(s, "shift_jis") == 0 || _stricmp(s, "sjis") == 0 || _stricmp(s, "932") == 0) return kCP_SJIS;
    if (_stricmp(s, "cp949")     == 0 || _stricmp(s, "949") == 0) return kCP_KOREAN;
    if (_stricmp(s, "utf8")      == 0 || _stricmp(s, "utf-8") == 0 || _stricmp(s, "65001") == 0) return kCP_UTF8;
    return def;
}

static bool ObCJKIsLeadByte(BYTE b, ObCJKCodePage cp)
{
    switch (cp) {
    case kCP_BIG5:
    case kCP_GBK:    return b >= 0x81 && b <= 0xFE;
    case kCP_SJIS:   return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
    case kCP_KOREAN: return b >= 0x81 && b <= 0xFE;
    }
    return false;
}

static bool ObCJKIsTrailByte(BYTE b, ObCJKCodePage cp)
{
    if (b == 0x7F) return false;
    switch (cp) {
    case kCP_BIG5:
    case kCP_GBK:    return b >= 0x40 && b <= 0xFE;
    case kCP_SJIS:   return b >= 0x40 && b <= 0xFC;
    case kCP_KOREAN: return b >= 0x41 && b <= 0xFE;
    }
    return false;
}

// UTF-8為變長編碼，介面語意與DBCS的lead/trail true/false判斷不同：
// 回答「這個序列共幾個byte」而非「這個byte是不是lead/trail」。continuation byte範圍(0x80-0xBF)
// 與各長度lead byte範圍完全不重疊，故不需要DBCS奇偶游程消歧義法。
static int ObCJKUtf8SeqLen(BYTE b)
{
    if (b <= 0x7F) return 1;
    if (b >= 0xC2 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 0;
}

static inline bool ObCJKIsUtf8Continuation(BYTE b)
{
    return b >= 0x80 && b <= 0xBF;
}

// UTF-8 counterpart of ObCJKIsAsciiCandidate (below) — that function checks
// "not a lead byte under g_activeCodePage's DBCS table", which is the wrong
// question for UTF-8 (ObCJKIsLeadByte has no UTF-8 case and would return
// false for every byte, wrongly admitting lead/continuation bytes as ASCII
// candidates). UTF-8's ASCII range is exactly 0x00-0x7F by construction —
// no table lookup needed. 
static inline bool ObCJKUtf8IsAsciiCandidate(BYTE b)
{
    return b != 0 && b <= 0x7F;
}

static const char* ObCJKCodePageName(ObCJKCodePage cp)
{
    switch (cp) {
    case kCP_GBK:    return "GBK";
    case kCP_SJIS:   return "SJIS";
    case kCP_KOREAN: return "KOREAN";
    case kCP_UTF8:   return "UTF8";
    default:         return "BIG5";
    }
}

static const char* ObCJKCodePagePyEnc(ObCJKCodePage cp)
{
    switch (cp) {
    case kCP_GBK:    return "gbk";
    case kCP_SJIS:   return "shift_jis";
    case kCP_KOREAN: return "cp949";
    case kCP_UTF8:   return "utf-8";
    default:         return "big5";
    }
}

static inline bool ObCJKIsAsciiCandidate(BYTE b)
{
    return b != 0 && !ObCJKIsLeadByte(b, g_activeCodePage);
}

// Single-byte forbidden-char predicate shared by both IME paths (native
// obCJK_IME_In.h edit control and external obCJK_IME_Out.h Python window):
// Tab / control bytes (Backspace excluded so editing still works) plus the
// 9 characters Windows forbids in file names. Only meaningful when called on
// a byte that is NOT part of a multi-byte lead/trail or UTF-8 sequence —
// callers must establish that first (see ObCJKIsLeadByte/ObCJKUtf8SeqLen)
// so a DBCS trail byte or UTF-8 continuation byte is never tested here.
static inline bool ObCJKIsForbiddenFilenameChar(BYTE ch)
{
    if (ch == 0x08) return false;  // Backspace must stay typable
    if (ch < 0x20) return true;    // Tab (0x09), CR/LF, other control bytes
    switch (ch) {
    case '<': case '>': case ':': case '"':
    case '/': case '\\': case '|': case '?': case '*':
        return true;
    default:
        return false;
    }
}
