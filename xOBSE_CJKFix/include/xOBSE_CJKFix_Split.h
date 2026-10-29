// xOBSE_CJKFix_Split.h
// 切字串類：修補 2（對話框按鈕 _mbsbtype／_mbclen）、3（選單 _mbstok_s）、5（sv_Split）、7（SetStringIniSetting 的 strtok_s）。
#pragma once
#include "xOBSE_CJKFix_Common.h"

namespace {

// ---------- 修補 2：_mbsbtype / _mbclen ----------
// 問題：MessageBoxEX／文字輸入對話框切按鈕時，用 CRT 目前碼頁判斷雙位元組字元，與實際編碼不同時判錯。
// 做法：換匯入槽，白名單呼叫點改用 ini 編碼的字元長度表判斷。
typedef int (__cdecl* MbsbtypeFn)(const unsigned char*, size_t);
typedef size_t (__cdecl* MbclenFn)(const unsigned char*);
MbsbtypeFn g_origMbsbtype = nullptr;
MbclenFn   g_origMbclen = nullptr;
const DWORD kMbsbtypeRets[] = { 0x25F18, 0x5E175 };
const DWORD kMbclenRets[] = { 0x25F27, 0x5E188 };

int MbsbtypeImpl(const unsigned char* s, size_t count) {
    size_t i = 0;
    while (s[i]) {
        const int n = CharLen(s + i);
        if (count < i + n) {
            if (count == i) return n > 1 ? 1 : 0;
            return 2;
        }
        i += n;
    }
    return -1;
}

int __cdecl Hook_Mbsbtype(const unsigned char* s, size_t count) {
    if (!InList((ULONG_PTR)_ReturnAddress(), kMbsbtypeRets, _countof(kMbsbtypeRets))) return g_origMbsbtype(s, count);
    Hit(2, "_mbsbtype");
    return MbsbtypeImpl(s, count);
}

size_t __cdecl Hook_Mbclen(const unsigned char* s) {
    if (!InList((ULONG_PTR)_ReturnAddress(), kMbclenRets, _countof(kMbclenRets))) return g_origMbclen(s);
    Hit(2, "_mbclen");
    return (size_t)CharLen(s);
}

bool InstallFix2(HMODULE m) {
    ULONG_PTR* bt = ExpectSlot(m, "_mbsbtype", 0xAA23C);
    ULONG_PTR* cl = ExpectSlot(m, "_mbclen", 0xAA244);
    if (!bt || !cl) return false;
    for (DWORD r : kMbsbtypeRets)
        if (At(r)[-6] != 0xFF || At(r)[-5] != 0x15 || *(ULONG_PTR*)(At(r) - 4) != (ULONG_PTR)bt) {
            Log("FAIL Fix2 _mbsbtype call site rva=0x%X", r); DumpBytes("  actual-site", r - 6, 6); return false;
        }
    for (DWORD r : kMbclenRets)
        if (At(r)[-6] != 0xFF || At(r)[-5] != 0x15 || *(ULONG_PTR*)(At(r) - 4) != (ULONG_PTR)cl) {
            Log("FAIL Fix2 _mbclen call site rva=0x%X", r); DumpBytes("  actual-site", r - 6, 6); return false;
        }
    g_origMbsbtype = (MbsbtypeFn)*bt;
    g_origMbclen = (MbclenFn)*cl;
    return WriteSlot(bt, (void*)&Hook_Mbsbtype) && WriteSlot(cl, (void*)&Hook_Mbclen);
}

// ---------- 逐字元切字串（修補 3、修補 7 共用） ----------
// 依 ini 編碼逐字元前進，只有單位元組字元可當分隔符，不會切到雙位元組字元的尾位元組。
char* TokenizeCharAware(char* text, const char* delims, char** ctx) {
    char* p = text ? text : *ctx;
    if (!p) return nullptr;
    auto isDelim = [&](unsigned char c) { return c && strchr(delims, c) != nullptr; };
    while (*p) {
        if (CharLen((const unsigned char*)p) == 1 && isDelim((unsigned char)*p)) ++p; else break;
    }
    if (!*p) { *ctx = p; return nullptr; }
    char* token = p;
    while (*p) {
        const int n = CharLen((const unsigned char*)p);
        if (n == 1 && isDelim((unsigned char)*p)) break;
        p += n;
    }
    if (*p) *p++ = 0;
    *ctx = p;
    return token;
}

bool AllAscii(const char* s) {
    for (; *s; ++s) if ((unsigned char)*s >= 0x80) return false;
    return true;
}

// ---------- 修補 3：_mbstok_s（SetMenuStringValue） ----------
// 問題：選單文字用 _mbstok_s 切割，依 CRT 目前碼頁判斷字元，與實際編碼不同時切錯。
// 做法：換匯入槽，白名單呼叫點且分隔符全為 ASCII 時改用逐字元切割。
typedef unsigned char* (__cdecl* MbstokFn)(unsigned char*, const unsigned char*, unsigned char**);
MbstokFn g_origMbstok = nullptr;
const DWORD kMbstokRets[] = { 0x499DE, 0x499EC };

unsigned char* __cdecl Hook_Mbstok(unsigned char* text, const unsigned char* delims, unsigned char** ctx) {
    if (!delims || !ctx || !AllAscii((const char*)delims) ||
        !InList((ULONG_PTR)_ReturnAddress(), kMbstokRets, _countof(kMbstokRets)))
        return g_origMbstok(text, delims, ctx);
    Hit(3, "_mbstok_s");
    return (unsigned char*)TokenizeCharAware((char*)text, (const char*)delims, (char**)ctx);
}

bool InstallFix3(HMODULE m) {
    ULONG_PTR* slot = ExpectSlot(m, "_mbstok_s", 0xAA240);
    if (!slot) return false;
    for (DWORD r : kMbstokRets)
        if (At(r)[-2] != 0xFF || At(r)[-1] != 0xD6) {
            Log("FAIL Fix3 call site rva=0x%X", r); DumpBytes("  actual-site", r - 6, 6); return false;
        }
    g_origMbstok = (MbstokFn)*slot;
    return WriteSlot(slot, (void*)&Hook_Mbstok);
}

// ---------- 修補 5：sv_Split 多位元組分隔符 ----------
// 問題：Tokenizer 把分隔符拆成位元組集合逐位元組查表，資料字元只要含其中一個位元組就被切開。
// 做法：Tokenizer 建構後改寫內部兩個字串：資料中的多位元組分隔符換成一個未使用的控制字元，
//       分隔符字串改成 ASCII 分隔符加該控制字元。
typedef void (__fastcall* TokCtorFn)(void* self, void* edx, const char* src, const char* delims);
TokCtorFn g_origTokCtor = nullptr;

struct RawStr {
    union { char buf[16]; char* ptr; } u;
    size_t size;
    size_t cap;
};

char* RawData(RawStr* s) { return s->cap >= 16 ? s->u.ptr : s->u.buf; }

void RewriteTokenizer(void* self) {
    RawStr* d = (RawStr*)self;
    RawStr* s = (RawStr*)((char*)self + 0x1C);
    char* dp = RawData(d);
    char* sp = RawData(s);
    const size_t dn = d->size, sn = s->size;

    struct Mc { size_t pos; int len; };
    Mc multi[64] = {};
    int multiCount = 0;
    std::string ascii;
    for (size_t i = 0; i < dn;) {
        const unsigned char c = (unsigned char)dp[i];
        int n = CharLen((const unsigned char*)dp + i);
        if (n == 1 && c < 0x80) { ascii.push_back((char)c); ++i; continue; }
        if (n == 1 && !IsValidSingleHigh(c)) { Log("Fix5 skip: invalid delimiter bytes"); return; }
        if (multiCount >= 64) return;
        multi[multiCount++] = { i, n };
        i += n;
    }
    if (!multiCount) return;

    int marker = 0;
    for (int m = 1; m < 32 && !marker; ++m) {
        if (m == 9 || m == 10 || m == 13) continue;
        bool used = false;
        for (size_t i = 0; i < dn; ++i) if ((unsigned char)dp[i] == m) used = true;
        for (size_t i = 0; i < sn; ++i) if ((unsigned char)sp[i] == m) used = true;
        if (!used) marker = m;
    }
    if (!marker) { Log("Fix5 skip: no free marker byte"); return; }

    size_t out = 0;
    for (size_t i = 0; i < sn;) {
        const int n = CharLen((const unsigned char*)sp + i);
        bool isDelim = false;
        if (n > 1 || (unsigned char)sp[i] >= 0x80) {
            for (int k = 0; k < multiCount && !isDelim; ++k)
                if (multi[k].len == n && memcmp(sp + i, dp + multi[k].pos, n) == 0) isDelim = true;
        }
        if (isDelim) sp[out++] = (char)marker;
        else { for (int k = 0; k < n; ++k) sp[out++] = sp[i + k]; }
        i += n;
    }
    sp[out] = 0;
    s->size = out;

    ascii.push_back((char)marker);
    memcpy(dp, ascii.c_str(), ascii.size() + 1);
    d->size = ascii.size();
    Hit(5, "Tokenizer rewrite");
}

void __fastcall Hook_TokCtor(void* self, void*, const char* src, const char* delims) {
    g_origTokCtor(self, nullptr, src, delims);
    RewriteTokenizer(self);
}

bool InstallFix5() {
    g_origTokCtor = (TokCtorFn)(g_base + 0x9D1C0);
    return RetargetCall("Fix5", 0x5CB19, 0x9D1C0, (void*)&Hook_TokCtor);
}

// ---------- 修補 7：strtok_s（SetStringIniSetting） ----------
// 問題：「設定名|設定值」用 strtok_s 逐位元組切，DBCS 尾位元組等於分隔符時字元被切斷。
// 做法：換匯入槽，白名單呼叫點且分隔符全為 ASCII 時改用逐字元切割。
typedef char* (__cdecl* StrtokFn)(char*, const char*, char**);
StrtokFn g_origStrtok = nullptr;
const DWORD kStrtokRets[] = { 0x257B8, 0x257E6 };

char* __cdecl Hook_Strtok(char* text, const char* delims, char** ctx) {
    if (!delims || !ctx || !AllAscii(delims) ||
        !InList((ULONG_PTR)_ReturnAddress(), kStrtokRets, _countof(kStrtokRets)))
        return g_origStrtok(text, delims, ctx);
    Hit(7, "strtok_s");
    return TokenizeCharAware(text, delims, ctx);
}

bool InstallFix7(HMODULE m) {
    ULONG_PTR* slot = ExpectSlot(m, "strtok_s", 0xAA2EC);
    if (!slot) return false;
    for (DWORD r : kStrtokRets)
        if (At(r)[-2] != 0xFF || At(r)[-1] != 0xD3) {
            Log("FAIL Fix7 call site rva=0x%X", r); DumpBytes("  actual-site", r - 6, 6); return false;
        }
    g_origStrtok = (StrtokFn)*slot;
    return WriteSlot(slot, (void*)&Hook_Strtok);
}

} // namespace
