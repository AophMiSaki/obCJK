// xOBSE_CJKFix_Common.h
// 共用：全域狀態、log、各編碼字元長度、編碼名稱對照、記憶體／匯入槽修補工具。
#pragma once

namespace {

enum Encoding { kEncUtf8, kEncBig5, kEncGbk, kEncSjis, kEncKorean };
Encoding  g_enc = kEncUtf8;
UINT      g_cp = CP_UTF8;
ULONG_PTR g_base = 0;
LONG      g_hits[10] = {};

// ---------- log ----------
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gLog.Log(IDebugLog::kLevel_Message, fmt, ap);
    va_end(ap);
}

void Hit(int fix, const char* what) {
    LONG n = InterlockedIncrement(&g_hits[fix]);
    if (n <= 8) Log("hit Fix%d %s count=%ld", fix, what, n);
}

// ---------- 字元長度 ----------
bool InRange(unsigned c, unsigned lo, unsigned hi) { return c >= lo && c <= hi; }

int CharLen(const unsigned char* p) {
    const unsigned c = p[0];
    if (c < 0x80) return 1;
    const unsigned t = p[1];
    switch (g_enc) {
    case kEncUtf8:
        if (InRange(c, 0xC2, 0xDF)) return InRange(t, 0x80, 0xBF) ? 2 : 1;
        if (InRange(c, 0xE0, 0xEF)) return (InRange(t, 0x80, 0xBF) && InRange(p[2], 0x80, 0xBF)) ? 3 : 1;
        if (InRange(c, 0xF0, 0xF4))
            return (InRange(t, 0x80, 0xBF) && InRange(p[2], 0x80, 0xBF) && InRange(p[3], 0x80, 0xBF)) ? 4 : 1;
        return 1;
    case kEncBig5:
        if (InRange(c, 0x81, 0xFE) && (InRange(t, 0x40, 0x7E) || InRange(t, 0xA1, 0xFE))) return 2;
        return 1;
    case kEncGbk:
        if (InRange(c, 0x81, 0xFE) && InRange(t, 0x40, 0xFE) && t != 0x7F) return 2;
        return 1;
    case kEncSjis:
        if ((InRange(c, 0x81, 0x9F) || InRange(c, 0xE0, 0xFC)) && (InRange(t, 0x40, 0x7E) || InRange(t, 0x80, 0xFC))) return 2;
        return 1;
    case kEncKorean:
        if (InRange(c, 0x81, 0xFE) && (InRange(t, 0x41, 0x5A) || InRange(t, 0x61, 0x7A) || InRange(t, 0x81, 0xFE))) return 2;
        return 1;
    }
    return 1;
}

bool IsValidSingleHigh(unsigned char b) { return g_enc == kEncSjis && InRange(b, 0xA1, 0xDF); }

unsigned char AsciiLower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }
unsigned char AsciiUpper(unsigned char c) { return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c; }

// ---------- 位址與修補工具 ----------
unsigned char* At(DWORD rva) { return (unsigned char*)(g_base + rva); }

bool InList(ULONG_PTR ret, const DWORD* list, size_t n) {
    if (ret < g_base) return false;
    const ULONG_PTR rva = ret - g_base;
    for (size_t i = 0; i < n; ++i) if (rva == list[i]) return true;
    return false;
}

void DumpBytes(const char* tag, DWORD rva, size_t n) {
    char buf[256] = {};
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < sizeof(buf); ++i) o += sprintf_s(buf + o, sizeof(buf) - o, "%02X ", At(rva)[i]);
    Log("%s rva=0x%X bytes=%s", tag, rva, buf);
}

bool WriteCode(DWORD rva, const unsigned char* data, size_t n) {
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(At(rva), n, PAGE_EXECUTE_READWRITE, &old)) {
        Log("FAIL VirtualProtect rva=0x%X err=%lu", rva, GetLastError());
        return false;
    }
    memcpy(At(rva), data, n);
    VirtualProtect(At(rva), n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), At(rva), n);
    return true;
}

bool ExpectBytes(const char* tag, DWORD rva, const unsigned char* expect, size_t n) {
    if (memcmp(At(rva), expect, n) == 0) return true;
    Log("FAIL %s: bytes mismatch", tag);
    DumpBytes("  expect-site", rva, n);
    return false;
}

bool CallTargetIs(DWORD rva, DWORD targetRva) {
    const unsigned char* p = At(rva);
    if (p[0] != 0xE8) return false;
    const DWORD target = (DWORD)(rva + 5 + *(const int*)(p + 1));
    return target == targetRva;
}

bool RetargetCall(const char* tag, DWORD rva, DWORD oldTargetRva, void* newTarget) {
    if (!CallTargetIs(rva, oldTargetRva)) {
        Log("FAIL %s: not E8 call to 0x%X", tag, oldTargetRva);
        DumpBytes("  actual-site", rva, 5);
        return false;
    }
    unsigned char patch[5] = {};
    patch[0] = 0xE8;
    *(int*)(patch + 1) = (int)((ULONG_PTR)newTarget - (g_base + rva + 5));
    return WriteCode(rva, patch, 5);
}

ULONG_PTR* ImportSlot(HMODULE module, const char* name) {
    BYTE* b = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + dos->e_lfanew);
    const DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return nullptr;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + rva); d->Name; ++d) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(b + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* slots = (IMAGE_THUNK_DATA32*)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* e = (IMAGE_IMPORT_BY_NAME*)(b + names->u1.AddressOfData);
            if (!strcmp((const char*)e->Name, name)) return (ULONG_PTR*)&slots->u1.Function;
        }
    }
    return nullptr;
}

// 取得匯入槽並確認位址與預期 RVA 相同。
ULONG_PTR* ExpectSlot(HMODULE module, const char* name, DWORD expectRva) {
    ULONG_PTR* slot = ImportSlot(module, name);
    if (!slot) { Log("FAIL import %s not found", name); return nullptr; }
    const DWORD rva = (DWORD)((ULONG_PTR)slot - g_base);
    if (rva != expectRva) { Log("FAIL import %s slot rva=0x%X expect=0x%X", name, rva, expectRva); return nullptr; }
    return slot;
}

bool WriteSlot(ULONG_PTR* slot, void* value) {
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    InterlockedExchange((volatile LONG*)slot, (LONG)(ULONG_PTR)value);
    VirtualProtect(slot, sizeof(*slot), old, &ignored);
    return true;
}

// ---------- 編碼名稱 ----------
// 名稱比照 obCJK 的 ObCJKCodePageFromString；無法辨識時用 UTF8。
Encoding ParseEncoding(const char* s) {
    if (!_stricmp(s, "big5") || !_stricmp(s, "950")) return kEncBig5;
    if (!_stricmp(s, "gbk") || !_stricmp(s, "936")) return kEncGbk;
    if (!_stricmp(s, "shift_jis") || !_stricmp(s, "sjis") || !_stricmp(s, "932")) return kEncSjis;
    if (!_stricmp(s, "cp949") || !_stricmp(s, "949")) return kEncKorean;
    return kEncUtf8;
}

const char* EncodingName(Encoding e) {
    switch (e) {
    case kEncBig5: return "BIG5";
    case kEncGbk: return "GBK";
    case kEncSjis: return "SJIS";
    case kEncKorean: return "KOREAN";
    default: return "UTF8";
    }
}

UINT CodePageOf(Encoding e) {
    switch (e) {
    case kEncBig5: return 950;
    case kEncGbk: return 936;
    case kEncSjis: return 932;
    case kEncKorean: return 949;
    default: return CP_UTF8;
    }
}

} // namespace
