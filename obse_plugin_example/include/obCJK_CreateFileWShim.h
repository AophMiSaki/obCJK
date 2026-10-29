#pragma once
// Fix: "Game Saved" reported success but no .ess was written for
// CJK/non-ASCII save names. Retargets the CreateFileA call inside the
// game's CRT _fsopen chain (sub_99D763, VA 0x0099D997) to CreateFileW,
// decoding the path under g_activeCodePage instead of the system ANSI code
// page. Falls back to CreateFileA when the path isn't valid under
// g_activeCodePage (legacy saves written under another code page).
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_SaveDiag.h"  // SaveDiagEnable + ObCJKShortErrorReason

// Same signature as CreateFileA (stdcall, 7 args) — called in place of
// `call dword ptr [CreateFileA]`.
static HANDLE WINAPI ObCJKCreateFileAShim(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    // C6387: null name can't be opened by either path; fail like CreateFileA would.
    if (!lpFileName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    wchar_t wpath[MAX_PATH];
    int wLen = lpFileName
        ? MultiByteToWideChar((UINT)g_activeCodePage, MB_ERR_INVALID_CHARS,
                               lpFileName, -1, wpath, MAX_PATH)
        : 0;

    HANDLE h = (wLen > 0)
        ? CreateFileW(wpath, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
        : CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    // Logging, .ess paths only. This shared _fsopen site also sees read-only
    // opens (save-list metadata); isWrite separates them. SaveDiagEnable=1
    // logs every open in detail; =0 logs only writes, one ok/FAILED line.
    size_t len = lpFileName ? strlen(lpFileName) : 0;
    if (len >= 4 && _stricmp(lpFileName + len - 4, ".ess") == 0) {
        bool ok = (h != INVALID_HANDLE_VALUE);
        bool isWrite = (dwDesiredAccess & GENERIC_WRITE) != 0;
        DWORD err = ok ? 0 : GetLastError();
        // Fix: not logged as FAILED — sub_42FE80 first opens with "r+b"
        // (OPEN_EXISTING), which misses with ERROR_FILE_NOT_FOUND on every
        // save after sub_45F2E0's backup rotation, then falls back to "wb".
        bool isBenignRetryProbe = !ok && isWrite && err == ERROR_FILE_NOT_FOUND
                                   && dwCreationDisposition == OPEN_EXISTING;
        if (ObCJKSaveDiagEnabled()) {
            if (isBenignRetryProbe) {
                _MESSAGE("obCJK:Save: probe(no-existing-file, create-fallback-follows) path=[%s]",
                         lpFileName);
            } else {
                _MESSAGE("obCJK:Save: %s op=%s path=[%s] wLen=%d err=%lu",
                         ok ? "ok" : "FAILED", isWrite ? "write" : "read", lpFileName, wLen, err);
            }
        } else if (isWrite && !isBenignRetryProbe) {
            if (ok) {
                _MESSAGE("obCJK:Save: ok name=[%s]", lpFileName);
            } else {
                char reasonBuf[32];
                _MESSAGE("obCJK:Save: FAILED reason=%s name=[%s]",
                         ObCJKShortErrorReason(err, reasonBuf, sizeof(reasonBuf)), lpFileName);
            }
        }
    }

    return h;
}

// Rewrites sub_99D763's 6-byte `FF 15 xx xx xx xx` (call dword ptr
// [CreateFileA]) into `E8 rel32` + NOP calling the shim; the next
// instruction is untouched. (ObCJKPatchCallTarget only handles `E8` calls.)
static bool ObCJKInstallCreateFileWShim()
{
    static const DWORD kVA_CreateFileACallSite = 0x0099D997;
    BYTE* site = (BYTE*)kVA_CreateFileACallSite;

    if (site[0] != 0xFF || site[1] != 0x15) {
        _MESSAGE("obCJK:CreateFileWShim: FAILED");
        return false;
    }

    DWORD oldProt = 0;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
    site[0] = 0xE8;
    *(INT32*)(site + 1) = (INT32)((DWORD)&ObCJKCreateFileAShim - (DWORD)(site + 5));
    site[5] = 0x90;  // NOP filler for the 6th original byte
    VirtualProtect(site, 6, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 6);

    _MESSAGE("obCJK:CreateFileWShim: ok");
    return true;
}
