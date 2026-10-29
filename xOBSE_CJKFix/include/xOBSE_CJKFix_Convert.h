// xOBSE_CJKFix_Convert.h
// 編碼轉換類：修補 1（字串變數 MultiByteToWideChar／WideCharToMultiByte 的 CP_ACP）。
#pragma once
#include "xOBSE_CJKFix_Common.h"

namespace {

// ---------- 修補 1：MultiByteToWideChar / WideCharToMultiByte 的 CP_ACP ----------
// 問題：sv_ 字串變數窄寬轉換寫死 CP_ACP，系統碼頁與遊戲文字編碼不同時轉壞。
// 做法：換匯入槽，只在白名單呼叫點把 CP_ACP 改成 ini 指定碼頁；UTF-8 下帶預設字元參數的呼叫不改（API 不允許）。
typedef int (WINAPI* MbFn)(UINT, DWORD, LPCCH, int, LPWSTR, int);
typedef int (WINAPI* WideFn)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
MbFn   g_origMb = nullptr;
WideFn g_origWide = nullptr;
const DWORD kMbRets[] = { 0x5AA37,0x5AA68,0x5AFDE,0x5B00D,0x9AFAE,0x9AFD9,0x9B249,0x9B274,
                          0x9B340,0x9B368,0x9B45C,0x9B485,0x9B5DC,0x9B605,0x9B615,0x9B63E };
const DWORD kWideRets[] = { 0x9B0CD,0x9B0EA,0x9B193,0x9B1B9,0x9B9BE,0x9B9DB };

int WINAPI Hook_Mb(UINT cp, DWORD flags, LPCCH src, int n, LPWSTR dst, int cap) {
    UINT use = cp;
    if (cp == CP_ACP && InList((ULONG_PTR)_ReturnAddress(), kMbRets, _countof(kMbRets))) {
        use = g_cp;
        Hit(1, "MultiByteToWideChar");
    }
    return g_origMb(use, flags, src, n, dst, cap);
}

int WINAPI Hook_Wide(UINT cp, DWORD flags, LPCWCH src, int n, LPSTR dst, int cap, LPCCH def, LPBOOL used) {
    UINT use = cp;
    const bool utf8NeedsNullDef = (g_cp == CP_UTF8) && (def || used);
    if (cp == CP_ACP && !utf8NeedsNullDef && InList((ULONG_PTR)_ReturnAddress(), kWideRets, _countof(kWideRets))) {
        use = g_cp;
        Hit(1, "WideCharToMultiByte");
    }
    return g_origWide(use, flags, src, n, dst, cap, def, used);
}

bool InstallFix1(HMODULE m) {
    ULONG_PTR* mb = ExpectSlot(m, "MultiByteToWideChar", 0xAA034);
    ULONG_PTR* wide = ExpectSlot(m, "WideCharToMultiByte", 0xAA038);
    if (!mb || !wide) return false;
    for (DWORD r : kMbRets)
        if (At(r)[-2] != 0xFF || (At(r)[-1] != 0xD7 && At(r)[-1] != 0xD3)) {
            Log("FAIL Fix1 mb call site rva=0x%X", r); DumpBytes("  actual-site", r - 6, 6); return false;
        }
    for (DWORD r : kWideRets) {
        const bool indirect = At(r)[-2] == 0xFF && At(r)[-1] == 0xD7;
        const bool iat = At(r)[-6] == 0xFF && At(r)[-5] == 0x15 && *(ULONG_PTR*)(At(r) - 4) == (ULONG_PTR)wide;
        if (!indirect && !iat) { Log("FAIL Fix1 wide call site rva=0x%X", r); DumpBytes("  actual-site", r - 6, 6); return false; }
    }
    g_origMb = (MbFn)*mb;
    g_origWide = (WideFn)*wide;
    return WriteSlot(mb, (void*)&Hook_Mb) && WriteSlot(wide, (void*)&Hook_Wide);
}

} // namespace
