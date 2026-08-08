#pragma once
// Fix for the "delete save" false-success bug: sub_453480's DeleteFileA
// call (VA 0x004534A6) never checks its result and unconditionally removes
// the in-game save-list entry, so a CJK/non-ASCII save name silently fails
// to delete on disk. Can't patch that call site directly — xOBSE's own
// Hooks_SaveLoad.cpp always wins the race for those same 6 bytes — so this
// hooks sub_453480's entry instead and does the real delete itself
// (.ess + derived .obse co-save) before either xOBSE's or the game's own
// attempt runs. Full investigation, abandoned call-site-patch design, and
// the diag-hook trampoline gap it surfaced:
// 存檔命名與刪除支援非ASCII.md
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
    lstrcpynA(buf, essPath, MAX_PATH);

    char bakSuffix[MAX_PATH] = "";
    char* bakPos = strstr(buf, ".bak");
    if (bakPos) {
        lstrcpynA(bakSuffix, bakPos, MAX_PATH);
        *bakPos = '\0';
    }

    char* lastDot = strrchr(buf, '.');
    if (lastDot) *lastDot = '\0';

    // buf and bakSuffix are each capped at MAX_PATH by lstrcpynA above, so
    // "%s.obse%s" can't exceed ~525 chars — build into a temp buffer sized
    // for that worst case, then lstrcpynA-truncate into the caller's actual
    // outObsePath/outSize (wsprintfA itself has no length parameter, so
    // writing straight into outObsePath had no truncation protection).
    char tmp[MAX_PATH * 2 + 16];
    wsprintfA(tmp, "%s.obse%s", buf, bakSuffix);
    lstrcpynA(outObsePath, tmp, (int)outSize);
}

// Hooked at sub_453480's entry (see this file's top comment for why here
// and not the DeleteFileA call site further down). Reads a2+0x3C (the save
// entry's filename) before the entry gets destructed, then deletes both the
// .ess and its derived .obse companion right away — this must run
// synchronously here, not deferred to next frame, so it lands before the
// game's own post-delete save-list rescan.
static void __cdecl ObCJKDeleteEntryDiagLog(int a2, int a3)
{
    const char* fname = a2 ? (const char*)(a2 + 0x3C) : nullptr;
    if (!fname) return;

    ObCJKFollowUpDeleteOne(fname, "ess");

    char obsePath[MAX_PATH];
    ObCJKDeriveCosavePath(fname, obsePath, MAX_PATH);
    ObCJKFollowUpDeleteOne(obsePath, "obse");
}

static BYTE* g_deleteEntryDiagTramp = nullptr;

// thiscall entry: ecx=this (unused), [esp+4]=a2(int, the save-list entry
// to delete), [esp+8]=a3(char*, unused) — same CPU state as sub_453480's
// real first instruction, since this hook patches the function's own entry
// VA. Read-only: nothing is written back to the stack, original arguments
// flow through untouched.
static __declspec(naked) void ObCJKDeleteEntryDiagHook()
{
    __asm {
        pushad
        push dword ptr [esp+0x28]  // a3
        push dword ptr [esp+0x28]  // a2 (offset stays +0x28: esp already shifted by the a3 push)
        call ObCJKDeleteEntryDiagLog
        add  esp, 8
        popad
        jmp  dword ptr [g_deleteEntryDiagTramp]
    }
}

static void ObCJKInstallDeleteEntryDiagHook()
{
    static const DWORD kVA_DeleteEntry = 0x00453480;  // sub_453480, delete-save entry point
    if (g_deleteEntryDiagTramp) return;  // already installed

    ObCJKInstallHook((BYTE*)kVA_DeleteEntry, (void*)ObCJKDeleteEntryDiagHook, &g_deleteEntryDiagTramp, "DeleteEntryDiag");
}
