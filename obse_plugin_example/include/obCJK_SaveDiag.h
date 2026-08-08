#pragma once
// Shared verbosity switch + short failure-reason helper for the save/delete
// log group: obCJK_CreateFileWShim.h (save-file CreateFileW/A result),
// obCJK_SaveNameTruncateHook.h (SavePathFix/SaveEntryDiag), obCJK_
// SaveListFindShim.h (save-list scan), obCJK_DeleteFileWShim.h (delete
// follow-up). ini key SaveDiagEnable ([obCJK] 節, obCJK_iniEdit.py 功能開關)
// — 預設關閉(0)。開啟＝完整記錄（沿用各檔案原本的診斷輸出，含背景存檔清單
// 掃描的逐筆診斷行）；關閉＝每個檔案只印出該次存檔/刪檔動作本身的成功/
// 失敗一行，失敗時用 ObCJKShortErrorReason 附簡短原因，不印背景掃描診斷。
// 同款「開/關」讀取寫法沿用 ObCJKLootMenuDiagEnabled()（obCJK_
// LootMenuTrailByteFixHook.h）／ObCJKPathDiagEnabled()（obCJK_GlyphHook.h），
// 但不設印出行數上限（PathDiagCap 那種）：存檔/刪檔是使用者單次動作觸發，
// 天生就是低頻事件，不像逐字型/逐選單項目的診斷需要防洗版上限。
#include <windows.h>
#include "obCJK_Path.h"  // k_iniMain

static bool ObCJKSaveDiagEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "SaveDiagEnable", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// Maps a Win32 GetLastError() code to a short Chinese phrase for the
// SaveDiagEnable=0 concise failure line. buf/bufSize back the fallback for
// codes not explicitly covered — an unfamiliar number beats a silently wrong
// guess at the cause.
static const char* ObCJKShortErrorReason(DWORD err, char* buf, size_t bufSize)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:       return "找不到檔案";
    case ERROR_PATH_NOT_FOUND:       return "找不到路徑";
    case ERROR_ACCESS_DENIED:        return "存取被拒";
    case ERROR_SHARING_VIOLATION:    return "檔案被佔用";
    case ERROR_FILENAME_EXCED_RANGE: return "路徑過長";
    case ERROR_DISK_FULL:            return "磁碟空間不足";
    case ERROR_INVALID_NAME:         return "檔名不合法";
    case ERROR_WRITE_PROTECT:        return "磁碟為唯讀";
    default:
        wsprintfA(buf, "錯誤碼=%lu", err);
        return buf;
    }
}
