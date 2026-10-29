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

static ObCJKCodePage ObCJKCodePageFromString(const char* s, ObCJKCodePage def = kCP_UTF8)
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

// UTF-8 sequence length from its lead byte (1-4); 0 = continuation or invalid lead.
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

// UTF-8 version of ObCJKIsAsciiCandidate: non-zero byte <= 0x7F.
// (ObCJKIsAsciiCandidate is DBCS-only; under UTF-8 it would accept every byte.)
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

// Forbidden single-byte char for both IME paths: control bytes except
// Backspace, plus the 9 characters Windows forbids in file names. Callers
// must only pass bytes that are not part of a multi-byte sequence.
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
