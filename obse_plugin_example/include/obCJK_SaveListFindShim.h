#pragma once
// Fixes CJK-named saves showing as "corrupted" in the load/save menu even
// after obCJK_CreateFileWShim.h made file creation itself succeed
// (CreateFileWShim:#380/381/382 all report `ok` for a brand
// new "Save 127 - 雫（しずく..." save, but the very next time the game
// rebuilds its save list, SavePathFix:diag#12 shows the exact same path
// with the CJK segment replaced by `?` — a separate, independent bug from
// the CreateFileA encoding mismatch this file's sibling already fixed).
//
// Root cause: sub_45D450 is the save-list scanner (confirmed via IDA
// decompile) — on every list refresh it does:
//   lstrcpyA/lstrcatA to build "<SavesFolder>*.ess"
//   FindFirstFileA(pattern, &FindFileData)  // VA 0x0045D4F9
//   ... sub_98208B(v8, "%s%s%s", folder, subdir, FindFileData.cFileName) ...
//   FindNextFileA(handle, &FindFileData)    // VA 0x0045D5A6
// FindFirstFileA/FindNextFileA always marshal the found filename into
// their WIN32_FIND_DATAA::cFileName field by converting the filesystem's
// real UTF-16 name to the *system* ANSI code page (same class of bug as
// CreateFileA, but this is a completely different Win32 entry point/call
// site — patching CreateFileA does nothing for this path). Whenever the
// on-disk name contains a byte sequence with no representation in the
// system code page (any CJK char under Big5/950, as in this project),
// the conversion is lossy and substitutes `?` — and unlike CreateFileA's
// silent-fail behavior, this doesn't fail, it silently corrupts the name
// baked into `v8` above and every downstream operation (list display,
// re-open for load, etc.) then permanently addresses the file by that
// wrong, unrecoverable name.
//
// Fix: patch both call sites *inside sub_45D450 specifically* (not the
// FindFirstFileA/FindNextFileA IAT slots, which have 14 and 9 other call
// sites respectively across the exe per `xrefs_to` — completely unrelated
// subsystems that must stay untouched) to go through FindFirstFileW /
// FindNextFileW instead, then convert the returned wide filename to
// g_activeCodePage (not CP_ACP) ourselves — the same code page obCJK used
// to create the file in the first place, so the round-trip is lossless
// for any name obCJK itself wrote. All other WIN32_FIND_DATA fields
// (attributes, timestamps, size) are identical layout between the A and
// W struct variants and are copied verbatim.
//
// Search-pattern fallback: the pattern string itself ("<folder>*.ess") is
// always plain ASCII in practice, but if MultiByteToWideChar somehow
// fails to decode it under g_activeCodePage, ObCJKFindFirstFileAShim
// falls through to the original ANSI FindFirstFileA so the scan still
// happens (with the original lossy-name behavior) instead of the save
// list silently going empty. Win32 find handles are not format-specific
// (FindNextFileW works on a handle obtained via FindFirstFileA and vice
// versa — both are thin wrappers over the same underlying directory
// search object), so ObCJKFindNextFileAShim can unconditionally use
// FindNextFileW regardless of which path produced the handle.
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
    if (lpFileName) {
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

// Retargets sub_45D450's two `FF 15 xx xx xx xx` (call dword ptr [IAT])
// call sites to direct 5-byte `E8 rel32` calls into the shims above, each
// padded with one trailing NOP — same 6-byte-in/6-byte-out shape as
// obCJK_CreateFileWShim.h's patch, confirmed via get_bytes: both sites
// are followed by a 2-byte instruction (`mov ebp,eax` / `test eax,eax`)
// that must stay untouched.
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
