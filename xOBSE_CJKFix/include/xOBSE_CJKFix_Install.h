// xOBSE_CJKFix_Install.h
// 依序安裝修補 1～9，各項位元組或匯入槽不符時該項不安裝。
#pragma once
#include "xOBSE_CJKFix_Common.h"
#include "xOBSE_CJKFix_Convert.h"
#include "xOBSE_CJKFix_Split.h"
#include "xOBSE_CJKFix_Case.h"

namespace {

const char k_obCJKIni[] = "Data\\OBSE\\Plugins\\obCJK\\obCJK.ini";

void InstallAll() {
    // 編碼取自 obCJK.ini 的 [obCJK] ActiveCodePage，缺值或無法辨識時用 UTF8。
    char cpBuf[16] = {};
    GetPrivateProfileStringA("obCJK", "ActiveCodePage", "UTF8", cpBuf, sizeof(cpBuf), k_obCJKIni);
    g_enc = ParseEncoding(cpBuf);
    g_cp = CodePageOf(g_enc);
    Log("config ini=%s ActiveCodePage=\"%s\" -> Encoding=%s codepage=%u systemACP=%u",
        k_obCJKIni, cpBuf, EncodingName(g_enc), g_cp, GetACP());

    HMODULE m = GetModuleHandleA("obse_1_2_416.dll");
    if (!m) { Log("FAIL obse_1_2_416.dll not loaded"); return; }
    g_base = (ULONG_PTR)m;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(g_base + ((const IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    Log("xOBSE module base=0x%p TimeDateStamp=0x%08X SizeOfImage=0x%X", (void*)g_base, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);

    for (int fix = 1; fix <= 9; ++fix) {
        bool ok = false;
        switch (fix) {
        case 1: ok = InstallFix1(m); break;
        case 2: ok = InstallFix2(m); break;
        case 3: ok = InstallFix3(m); break;
        case 4: ok = InstallFix4(); break;
        case 5: ok = InstallFix5(); break;
        case 6: ok = InstallFix6(); break;
        case 7: ok = InstallFix7(m); break;
        case 8: ok = InstallFix8(m); break;
        case 9: ok = InstallFix9(); break;
        }
        Log("Fix%d %s", fix, ok ? "installed" : "NOT installed");
    }
}

} // namespace
