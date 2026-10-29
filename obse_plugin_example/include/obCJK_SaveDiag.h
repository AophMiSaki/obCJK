#pragma once
// Shared verbosity switch + short failure-reason helper for the save/delete
// log group (CreateFileWShim, SaveNameTruncateHook, SaveListFindShim,
// DeleteFileWShim). ini key [obCJK] SaveDiagEnable, default 0.
// 1 = full diagnostic log, including per-entry save-list scan lines.
// 0 = one success/failure line per save/delete action, with
//     ObCJKShortErrorReason on failure; no scan diagnostics.
// No line cap.
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
// SaveDiagEnable=0 failure line; unlisted codes print the number into buf.
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
