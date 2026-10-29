#pragma once
// Fix: CJK-named saves showed as corrupted in the load/save menu.
//
// sub_45D450 is the save-list scanner; on every list refresh it builds
// "<SavesFolder>*.ess" and enumerates it:
//   FindFirstFileA(pattern, &FindFileData)  // VA 0x0045D4F9
//   ... sub_98208B(v8, "%s%s%s", folder, subdir, FindFileData.cFileName) ...
//   FindNextFileA(handle, &FindFileData)    // VA 0x0045D5A6
// The A APIs convert the on-disk UTF-16 name to the system ANSI code page,
// replacing unrepresentable CJK chars with `?`, so every later operation
// (list display, load) used a wrong name.
//
// Only these two call sites inside sub_45D450 are patched (not the IAT
// slots, which have many unrelated callers). They go through
// FindFirstFileW/FindNextFileW and convert the wide name to
// g_activeCodePage — the code page obCJK creates save files with — so the
// round-trip is lossless. Other WIN32_FIND_DATA fields are copied verbatim.
//
// If the search pattern can't be decoded under g_activeCodePage, the
// FindFirst shim falls back to the original FindFirstFileA so the list is
// still scanned. Find handles are shared between A and W, so the FindNext
// shim always uses FindNextFileW.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_SaveDiag.h"  // SaveDiagEnable

static DWORD g_saveListFindShimHits = 0;

// Copies the fixed-layout fields verbatim and re-encodes cFileName /
// cAlternateFileName from UTF-16 to g_activeCodePage bytes (not CP_ACP).
static void ObCJKFindDataWToA(const WIN32_FIND_DATAW* wfd, WIN32_FIND_DATAA* afd)
{
    afd->dwFileAttributes = wfd->dwFileAttributes;
    afd->ftCreationTime   = wfd->ftCreationTime;
    afd->ftLastAccessTime = wfd->ftLastAccessTime;
    afd->ftLastWriteTime  = wfd->ftLastWriteTime;
    afd->nFileSizeHigh    = wfd->nFileSizeHigh;
    afd->nFileSizeLow     = wfd->nFileSizeLow;
    afd->dwReserved0      = wfd->dwReserved0;
    afd->dwReserved1      = wfd->dwReserved1;

    int n = WideCharToMultiByte((UINT)g_activeCodePage, 0, wfd->cFileName, -1,
                                 afd->cFileName, MAX_PATH, nullptr, nullptr);
    if (n <= 0) afd->cFileName[0] = '\0';

    n = WideCharToMultiByte((UINT)g_activeCodePage, 0, wfd->cAlternateFileName, -1,
                             afd->cAlternateFileName, 14, nullptr, nullptr);
    if (n <= 0) afd->cAlternateFileName[0] = '\0';
}

static HANDLE WINAPI ObCJKFindFirstFileAShim(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    // C6387: null pattern fails like FindFirstFileA would.
    if (!lpFileName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    wchar_t wpath[MAX_PATH];
    int wLen = MultiByteToWideChar((UINT)g_activeCodePage, MB_ERR_INVALID_CHARS,
                                    lpFileName, -1, wpath, MAX_PATH);
    if (wLen > 0) {
        WIN32_FIND_DATAW wfd;
        HANDLE h = FindFirstFileW(wpath, &wfd);
        if (h != INVALID_HANDLE_VALUE) {
            ObCJKFindDataWToA(&wfd, lpFindFileData);
            g_saveListFindShimHits++;
            if (ObCJKSaveDiagEnabled()) {
                _MESSAGE("obCJK:SaveListFindShim:#%lu pattern=[%s] name=[%s]",
                          g_saveListFindShimHits, lpFileName, lpFindFileData->cFileName);
            }
        }
        return h;  // INVALID_HANDLE_VALUE here is a legitimate "no match", not a decode failure
    }
    if (ObCJKSaveDiagEnabled()) {
        _MESSAGE("obCJK:SaveListFindShim: pattern=[%s] not valid under active code page %d — falling back to system-ANSI FindFirstFileA",
                  lpFileName, (int)g_activeCodePage);
    }
    return FindFirstFileA(lpFileName, lpFindFileData);
}

static BOOL WINAPI ObCJKFindNextFileAShim(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    WIN32_FIND_DATAW wfd;
    if (!FindNextFileW(hFindFile, &wfd)) return FALSE;
    ObCJKFindDataWToA(&wfd, lpFindFileData);
    return TRUE;
}

// Rewrites a 6-byte `FF 15 xx xx xx xx` (call dword ptr [IAT]) into
// `E8 rel32` + NOP calling the shim; the following instruction is untouched.
static bool ObCJKPatchFindCallSite(BYTE* site, void* shim, const char* tag)
{
    if (site[0] != 0xFF || site[1] != 0x15) {
        _WARNING("obCJK:HookUtil:%s: bytes at %p are not FF 15 (call dword ptr [mem32]), refusing to patch", tag, site);
        return false;
    }
    DWORD oldProt = 0;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
    site[0] = 0xE8;
    *(INT32*)(site + 1) = (INT32)((DWORD)shim - (DWORD)(site + 5));
    site[5] = 0x90;
    VirtualProtect(site, 6, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    _MESSAGE("obCJK:HookUtil:%s: call-site patched (site=%p shim=%p)", tag, site, shim);
    return true;
}

static void ObCJKInstallSaveListFindShim()
{
    static const DWORD kVA_FindFirstFileACallSite = 0x0045D4F9;  // inside sub_45D450
    static const DWORD kVA_FindNextFileACallSite  = 0x0045D5A6;  // inside sub_45D450

    ObCJKPatchFindCallSite((BYTE*)kVA_FindFirstFileACallSite, (void*)ObCJKFindFirstFileAShim, "SaveListFindFirst");
    ObCJKPatchFindCallSite((BYTE*)kVA_FindNextFileACallSite,  (void*)ObCJKFindNextFileAShim,  "SaveListFindNext");
}
