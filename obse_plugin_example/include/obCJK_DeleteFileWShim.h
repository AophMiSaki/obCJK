#pragma once
// Fix: deleting a CJK/non-ASCII-named save reported success but left the
// file on disk. sub_453480's DeleteFileA call (VA 0x004534A6) ignores its
// result and always removes the list entry. That call site is already
// patched by xOBSE (Hooks_SaveLoad.cpp), so this hooks sub_453480's entry
// instead and deletes the .ess + derived .obse co-save itself before
// xOBSE's or the game's own attempt runs.
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_SaveDiag.h"  // SaveDiagEnable + ObCJKShortErrorReason

// Deletes one file (the .ess itself, or its derived .obse co-save), trying
// under g_activeCodePage first (MB_ERR_INVALID_CHARS), falling back to
// system-ANSI for legacy saves written under a different code page.
// SaveDiagEnable=1 logs full path/wLen detail; =0 (default) logs one
// concise ok/FAILED line, with a short reason on failure.
static void ObCJKFollowUpDeleteOne(const char* path, const char* kind)
{
    wchar_t wpath[MAX_PATH];
    int wLen = MultiByteToWideChar((UINT)g_activeCodePage, MB_ERR_INVALID_CHARS,
                                    path, -1, wpath, MAX_PATH);
    bool ok;
    DWORD err = 0;
    if (wLen > 0) {
        ok = (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES);
        if (!ok) {
            ok = DeleteFileW(wpath) != 0;
            if (!ok) err = GetLastError();
        }
    } else {
        ok = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
        if (!ok) {
            ok = DeleteFileA(path) != 0;
            if (!ok) err = GetLastError();
        }
    }

    if (ObCJKSaveDiagEnabled()) {
        _MESSAGE("obCJK:DeleteFollowUp:[%s] %s path=[%s] wLen=%d err=%lu",
                 kind, ok ? "ok" : "FAILED", path, wLen, err);
    } else if (ok) {
        _MESSAGE("obCJK:Delete: ok [%s] name=[%s]", kind, path);
    } else {
        char reasonBuf[32];
        _MESSAGE("obCJK:Delete: FAILED [%s] reason=%s name=[%s]",
                 kind, ObCJKShortErrorReason(err, reasonBuf, sizeof(reasonBuf)), path);
    }
}

// Mirrors xOBSE's Serialization.cpp ConvertSaveFileName() byte-for-byte
// (strip a trailing ".bak"-onward suffix if present, replace the remaining
// extension with ".obse", then re-append that suffix) so the path checked/
// deleted here is exactly what xOBSE's own HandleDeleteGame() derived.
static void ObCJKDeriveCosavePath(const char* essPath, char* outObsePath, size_t outSize)
{
    char buf[MAX_PATH];
    (void)lstrcpynA(buf, essPath, MAX_PATH);  // C6031: truncation is intended

    char bakSuffix[MAX_PATH] = "";
    char* bakPos = strstr(buf, ".bak");
    if (bakPos) {
        (void)lstrcpynA(bakSuffix, bakPos, MAX_PATH);
        *bakPos = '\0';
    }

    char* lastDot = strrchr(buf, '.');
    if (lastDot) *lastDot = '\0';

    // Fix: wsprintfA has no length limit; writing straight into outObsePath
    // could overflow. Build into a worst-case-sized temp, then truncate.
    char tmp[MAX_PATH * 2 + 16];
    wsprintfA(tmp, "%s.obse%s", buf, bakSuffix);
    (void)lstrcpynA(outObsePath, tmp, (int)outSize);
}

// Runs at sub_453480's entry: reads the save entry's filename (a2+0x3C)
// before the entry is destructed and deletes the .ess and .obse
// synchronously, before the game's post-delete save-list rescan.
static void __cdecl ObCJKDeleteSaveFix(int a2, int a3)
{
    const char* fname = a2 ? (const char*)(a2 + 0x3C) : nullptr;
    if (!fname) return;

    ObCJKFollowUpDeleteOne(fname, "ess");

    char obsePath[MAX_PATH];
    ObCJKDeriveCosavePath(fname, obsePath, MAX_PATH);
    ObCJKFollowUpDeleteOne(obsePath, "obse");
}

static BYTE* g_deleteSaveFixTramp = nullptr;

// thiscall entry: ecx=this (unused), [esp+4]=a2 (save-list entry to
// delete), [esp+8]=a3 (char*, unused). Read-only; arguments pass through
// unchanged.
static __declspec(naked) void ObCJKDeleteSaveFixHook()
{
    __asm {
        pushad
        push dword ptr [esp+0x28]  // a3
        push dword ptr [esp+0x28]  // a2 (offset stays +0x28: esp already shifted by the a3 push)
        call ObCJKDeleteSaveFix
        add  esp, 8
        popad
        jmp  dword ptr [g_deleteSaveFixTramp]
    }
}

static void ObCJKInstallDeleteSaveFixHook()
{
    static const DWORD kVA_DeleteEntry = 0x00453480;  // sub_453480, delete-save entry point
    if (g_deleteSaveFixTramp) return;  // already installed

    ObCJKInstallHook((BYTE*)kVA_DeleteEntry, (void*)ObCJKDeleteSaveFixHook, &g_deleteSaveFixTramp, "DeleteSaveFix");
}
