#pragma once
// Retargets the game's own CreateFileA call site inside its CRT _fsopen
// chain (sub_99D763, VA 0x0099D997) to CreateFileW, decoding the path under
// g_activeCodePage instead of the system ANSI code page — fixes "Game
// Saved" reporting success while no .ess file was actually written for
// CJK/non-ASCII save names. Falls back to the original CreateFileA when the
// path isn't valid under g_activeCodePage (a legacy save written under a
// different code page). Full root-cause derivation, abandoned prior
// designs (cross-CRT FILE* crash, wrong hook point), and blast-radius
// check: 存檔命名與刪除支援非ASCII.md
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_SaveDiag.h"  // SaveDiagEnable + ObCJKShortErrorReason

// Same signature as CreateFileA — this directly replaces the `call dword
// ptr [CreateFileA]` instruction at the patched site, so the calling
// convention (stdcall, callee pops its own 7 args) must match exactly.
static HANDLE WINAPI ObCJKCreateFileAShim(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
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

    // Scoped to .ess (save file) paths only — this call site is the CRT's
    // shared _fsopen chain, so it also sees read-only opens the game makes
    // to fetch existing saves' metadata (description/playtime) whenever the
    // load/save menu refreshes, not just real save writes. isWrite tells
    // those apart via GENERIC_WRITE. SaveDiagEnable=1 logs every touch
    // (read or write) with full decode detail; =0 (default) logs only real
    // writes (an actual new/overwritten save), one concise ok/FAILED line.
    size_t len = lpFileName ? strlen(lpFileName) : 0;
    if (len >= 4 && _stricmp(lpFileName + len - 4, ".ess") == 0) {
        bool ok = (h != INVALID_HANDLE_VALUE);
        bool isWrite = (dwDesiredAccess & GENERIC_WRITE) != 0;
        DWORD err = ok ? 0 : GetLastError();
        // The game's own save-file wrapper (sub_42FE80) always probes the
        // target path with CRT mode "r+b" (dwCreationDisposition=
        // OPEN_EXISTING) first, and only when that fails falls back to "wb"
        // (create) + reopen. Right after the backup-rotation step
        // (sub_45F2E0) renames the previous save out of the way, this probe
        // is *expected* to miss with ERROR_FILE_NOT_FOUND on every single
        // save — the CRT's own fallback then creates + reopens the file
        // successfully in the same call chain. IDA-verified via
        // sub_45F2E0/sub_430970/sub_42FE80 decompile + mode-string check
        // (0xA36340="r+b", 0xA36338="wb"). Not a real failure, so it must
        // not be logged as one — a genuine problem (permission denied, disk
        // full, path too long, ...) still surfaces as FAILED below.
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

// Retargets the 6-byte `FF 15 xx xx xx xx` (call dword ptr [CreateFileA])
// at sub_99D763's one CreateFileA call site to a direct 5-byte `E8 rel32`
// call into this shim, padded with one trailing NOP so the following
// instruction (`mov edi, eax` at site+6, confirmed via get_bytes) is left
// completely untouched. Not ObCJKPatchCallTarget: that helper only
// retargets an existing `E8` call, and this site is an IAT-indirect `FF
// 15`, not a direct call — a different opcode shape needing its own patch.
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
