// xOBSE_CJKFix_Case.h
// 大小寫與比較類：修補 4（sv_Replace）、6（sv_ToUpper／sv_ToLower）、8（_stricmp）、9（BSStringT Includes／Replace／Compare）。
#pragma once
#include "xOBSE_CJKFix_Common.h"

namespace {

// ---------- 修補 4：sv_Replace 的 ci_equal 寬字元 ----------
// 問題：不分大小寫搜尋時把 wchar_t 截成 char 比較，中文字元高位元組被丟掉。
// 做法：兩個 movzx byte 改 word（B6→B7），ci_equal 呼叫改指向寬字元比較。
bool __cdecl WideEq(int a, int b) {
    unsigned x = (unsigned)a & 0xFFFF, y = (unsigned)b & 0xFFFF;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    Hit(4, "WideEq");
    return x == y;
}

bool InstallFix4() {
    static const unsigned char expect[] = { 0x0F,0xB6,0x07,0x50,0x0F,0xB6,0x04,0x3B,0x50 };
    if (!ExpectBytes("Fix4 sv_Replace loop", 0x9B7C6, expect, sizeof(expect))) return false;
    if (!CallTargetIs(0x9B7CF, 0xB310)) {
        Log("FAIL Fix4: 0x9B7CF is not call to ci_equal(0xB310)");
        DumpBytes("  actual-site", 0x9B7CF, 5);
        return false;
    }
    const unsigned char b7 = 0xB7;
    return WriteCode(0x9B7C7, &b7, 1) && WriteCode(0x9B7CB, &b7, 1) &&
           RetargetCall("Fix4", 0x9B7CF, 0xB310, (void*)&WideEq);
}

// ---------- 修補 6：sv_ToUpper / sv_ToLower ----------
// 問題：逐位元組 toupper／tolower，DBCS 尾位元組落在字母範圍時被改掉。
// 做法：整段迴圈換成呼叫逐字元轉換函式，只轉單位元組 ASCII 字母。
void __cdecl Cjk_ToUpperRange(char* b, char* e) {
    Hit(6, "ToUpper");
    for (char* p = b; p < e;) {
        const int n = CharLen((const unsigned char*)p);
        if (n == 1) *p = (char)AsciiUpper((unsigned char)*p);
        p += n;
    }
}

void __cdecl Cjk_ToLowerRange(char* b, char* e) {
    Hit(6, "ToLower");
    for (char* p = b; p < e;) {
        const int n = CharLen((const unsigned char*)p);
        if (n == 1) *p = (char)AsciiLower((unsigned char)*p);
        p += n;
    }
}

// 迴圈起點 rva 的原始位元組：movsx eax,[esi]; push eax; call [slot]; mov [esi+edi],al; add esp,4; inc esi; cmp esi,ebx; jne
bool PatchCaseLoop(const char* tag, DWORD rva, bool upper, size_t loopLen, bool hasJmp, void* fn) {
    unsigned char expect[23] = { 0x0F,0xBE,0x06,0x50,0xFF,0x15,0,0,0,0,0x88,0x04,0x3E,0x83,0xC4,0x04,0x46,0x3B,0xF3,0x75,0xEB,0xEB,0x3E };
    const DWORD slotRva = upper ? 0xAA2D4 : 0xAA2E8;
    *(DWORD*)(expect + 6) = (DWORD)(g_base + slotRva);
    if (!ExpectBytes(tag, rva, expect, loopLen)) return false;
    unsigned char patch[23];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0x53;
    patch[1] = 0x56;
    patch[2] = 0xE8;
    *(int*)(patch + 3) = (int)((ULONG_PTR)fn - (g_base + rva + 7));
    patch[7] = 0x83; patch[8] = 0xC4; patch[9] = 0x08;
    if (hasJmp) {
        patch[10] = 0xEB;
        patch[11] = (unsigned char)(0x5D085 - (rva + 12));
    }
    return WriteCode(rva, patch, loopLen);
}

bool InstallFix6() {
    return PatchCaseLoop("Fix6 sv_ToUpper loop", 0x5D030, true, 23, true, (void*)&Cjk_ToUpperRange) &&
           PatchCaseLoop("Fix6 sv_ToLower loop", 0x5D070, false, 21, false, (void*)&Cjk_ToLowerRange);
}

// ---------- 修補 8：_stricmp ----------
// 問題：逐位元組折疊大小寫，DBCS 尾位元組只差大小寫的不同字被判成相同。
// 做法：換整個模組的匯入槽，雙位元組字元整字逐位元組比對，只折疊單位元組 ASCII 字母。
typedef int (__cdecl* StricmpFn)(const char*, const char*);
StricmpFn g_origStricmp = nullptr;

int __cdecl Hook_Stricmp(const char* a, const char* b) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (;;) {
        const int n1 = CharLen(p), n2 = CharLen(q);
        if (n1 > 1 || n2 > 1) {
            const int n = n1 < n2 ? n1 : n2;
            for (int k = 0; k < n; ++k)
                if (p[k] != q[k]) return p[k] < q[k] ? -1 : 1;
            if (n1 != n2) return n1 < n2 ? -1 : 1;
            p += n1; q += n2;
            continue;
        }
        const unsigned char x = AsciiLower(*p), y = AsciiLower(*q);
        if (x != y) return x < y ? -1 : 1;
        if (!x) { Hit(8, "_stricmp"); return 0; }
        ++p; ++q;
    }
}

bool InstallFix8(HMODULE m) {
    ULONG_PTR* slot = ExpectSlot(m, "_stricmp", 0xAA2D8);
    if (!slot) return false;
    g_origStricmp = (StricmpFn)*slot;
    return WriteSlot(slot, (void*)&Hook_Stricmp);
}

// ---------- 修補 9：BSStringT::Includes / Replace / Compare ----------
// 問題：逐位元組 tolower 比較且不認字元邊界，DBCS 下會誤判相同或從字元中間匹配、切開。
// 做法：CompareName／ModName／魔法效果名稱／CompareNames 四個呼叫點改指向逐字元版本；PathFunc 的呼叫不動。
struct BSStr {
    char*          m_data;
    unsigned short m_dataLen;
    unsigned short m_bufLen;
};
typedef bool (__fastcall* BSSetFn)(BSStr*, void*, const char*);
BSSetFn g_bsSet = nullptr;

bool MatchAt(const unsigned char* h, size_t hn, const unsigned char* n, size_t nn) {
    size_t a = 0, b = 0;
    while (b < nn) {
        if (a >= hn) return false;
        const int n1 = CharLen(h + a), n2 = CharLen(n + b);
        if (n1 != n2) return false;
        if (n1 > 1) { if (memcmp(h + a, n + b, n1) != 0) return false; }
        else if (AsciiLower(h[a]) != AsciiLower(n[b])) return false;
        a += n1; b += n2;
    }
    return true;
}

int FindCI(const unsigned char* h, size_t hn, const unsigned char* n, size_t nn) {
    if (nn == 0) return hn > 0 ? 0 : -1;
    for (size_t i = 0; i < hn;) {
        if (MatchAt(h + i, hn - i, n, nn)) return (int)i;
        i += CharLen(h + i);
    }
    return -1;
}

bool __fastcall Hook_Includes(BSStr* self, void*, const char* toFind) {
    if (!self->m_data || !toFind) return false;
    Hit(9, "Includes");
    return FindCI((const unsigned char*)self->m_data, self->m_dataLen, (const unsigned char*)toFind, strlen(toFind)) >= 0;
}

bool __fastcall Hook_Replace(BSStr* self, void*, const char* toReplace, const char* replaceWith) {
    if (!self->m_data || !toReplace) return false;
    std::string curr(self->m_data, self->m_dataLen);
    const size_t nn = strlen(toReplace);
    const int pos = FindCI((const unsigned char*)curr.c_str(), curr.size(), (const unsigned char*)toReplace, nn);
    if (pos < 0) return false;
    Hit(9, "Replace");
    curr.replace((size_t)pos, nn, replaceWith ? replaceWith : "");
    g_bsSet(self, nullptr, curr.c_str());
    return true;
}

std::string FoldAscii(const BSStr* s) {
    std::string r(s->m_data ? s->m_data : "", s->m_data ? s->m_dataLen : 0);
    for (size_t i = 0; i < r.size();) {
        const int n = CharLen((const unsigned char*)r.c_str() + i);
        if (n == 1) r[i] = (char)AsciiLower((unsigned char)r[i]);
        i += n;
    }
    return r;
}

double __cdecl CompareImpl(const BSStr* a, const BSStr* b) {
    if (!a->m_data) return -2;
    Hit(9, "Compare");
    const std::string x = FoldAscii(a), y = FoldAscii(b);
    const size_t n = x.size() < y.size() ? x.size() : y.size();
    const int c = memcmp(x.data(), y.data(), n);
    if (c < 0) return -1;
    if (c > 0) return 1;
    if (x.size() < y.size()) return -1;
    if (x.size() > y.size()) return 1;
    return 0;
}

// thiscall(ecx=this, [esp+4]=compareTo, [esp+8]=bool)、ret 8、double 回傳於 xmm0 的呼叫慣例包裝。
__declspec(naked) void CompareThunk() {
    __asm {
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        push ecx
        call CompareImpl
        add esp, 12
        sub esp, 8
        fstp qword ptr [esp]
        movsd xmm0, qword ptr [esp]
        add esp, 8
        ret 8
    }
}

bool InstallFix9() {
    g_bsSet = (BSSetFn)(g_base + 0xBBC0);
    return RetargetCall("Fix9 Includes@CompareName", 0x30633, 0xBC80, (void*)&Hook_Includes) &&
           RetargetCall("Fix9 Replace@ModName", 0x308EB, 0xBE10, (void*)&Hook_Replace) &&
           RetargetCall("Fix9 Replace@EffectName", 0x37FC6, 0xBE10, (void*)&Hook_Replace) &&
           RetargetCall("Fix9 Compare@CompareNames", 0x32917, 0xC060, (void*)&CompareThunk);
}

} // namespace
