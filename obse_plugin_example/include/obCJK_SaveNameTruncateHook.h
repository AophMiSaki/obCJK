#pragma once
// Fix: manual save "shows success but writes no file" for long (CJK) save
// names. sub_430970 (VA 0x00430970), the generic file-stream-open
// constructor every save path (console, quicksave, autosave, save dialog)
// ends up calling with the full path, silently clears the path to an empty
// string when strlen reaches MAX_PATH (260).
//
// Hooked here rather than at sub_465130 because the save dialog's overwrite
// branch passes name=NULL and reuses a cached path. sub_430970 is shared by
// unrelated file I/O, so only paths ending in ".ess" are touched: they are
// truncated on a character boundary to kObCJKSavePathCap.
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_SaveDiag.h"  // SaveDiagEnable

// Largest prefix length <= maxLen of `s` (length `len`) that ends on a
// character boundary (DBCS pair or UTF-8 sequence), walking from the start.
static size_t ObCJKSafeTruncateLenExplicit(const char* s, size_t len, size_t maxLen)
{
    if (len <= maxLen) return len;
    size_t i = 0, lastSafe = 0;
    while (i < len) {
        BYTE b = (BYTE)s[i];
        int charLen;
        if (g_activeCodePage == kCP_UTF8) {
            charLen = ObCJKUtf8SeqLen(b);
            if (charLen <= 0) charLen = 1;
        } else {
            charLen = ObCJKIsLeadByte(b, g_activeCodePage) ? 2 : 1;
        }
        if (i + (size_t)charLen > maxLen) break;
        i += charLen;
        lastSafe = i;
    }
    return lastSafe;
}

static BYTE*  g_savePathFixTramp = nullptr;
static char   g_savePathFixBuf[MAX_PATH];
static DWORD  g_savePathFixHits  = 0;
static DWORD  g_savePathDiagHits = 0;

// Safety margin under sub_430970's own MAX_PATH(260) cliff.
static const size_t kObCJKSavePathCap = 255;

// Debug log line cap (SaveDiagEnable=1 only).
static const DWORD kObCJKSavePathDiagCap = 200;

// Returns the path sub_430970 should use: unchanged unless it ends in ".ess"
// and is longer than the cap.
static const char* __cdecl ObCJKSavePathFixCheck(const char* path)
{
    if (!path) return path;
    size_t len = strlen(path);
    if (len < 4 || _stricmp(path + len - 4, ".ess") != 0) return path;  // not a save file — never touch

    g_savePathDiagHits++;
    if (ObCJKSaveDiagEnabled() && g_savePathDiagHits <= kObCJKSavePathDiagCap) {
        _MESSAGE("obCJK:SavePathFix:diag#%lu len=%u path=[%s]",
                 g_savePathDiagHits, (unsigned)len, path);
    }

    if (len <= kObCJKSavePathCap) return path;

    size_t bodyLen = len - 4;                       // strip the ".ess" we just matched
    size_t targetBodyLen = kObCJKSavePathCap - 4;
    if (bodyLen <= targetBodyLen) return path;

    size_t cut = ObCJKSafeTruncateLenExplicit(path, bodyLen, targetBodyLen);
    if (cut + 4 >= sizeof(g_savePathFixBuf)) cut = sizeof(g_savePathFixBuf) - 1 - 4;

    memcpy(g_savePathFixBuf, path, cut);
    memcpy(g_savePathFixBuf + cut, ".ess", 4);
    g_savePathFixBuf[cut + 4] = '\0';

    g_savePathFixHits++;
    if (ObCJKSaveDiagEnabled()) {
        _MESSAGE("obCJK:SavePathFix:hit#%lu len %u->%u path=[%s]",
                 g_savePathFixHits, (unsigned)len, (unsigned)(cut + 4), g_savePathFixBuf);
    }

    return g_savePathFixBuf;
}

// thiscall entry: ecx=this (unused), [esp+4]=path, [esp+8]=mode,
// [esp+0xC]=flags. Overwrites the path slot so the original function reads
// the (possibly truncated) pointer.
static __declspec(naked) void ObCJKSavePathFixHook()
{
    __asm {
        pushad
        push dword ptr [esp+0x24]  // path: was [esp+4] before pushad's 0x20 bytes
        call ObCJKSavePathFixCheck
        add  esp, 4
        mov  [esp+0x24], eax
        popad
        jmp  dword ptr [g_savePathFixTramp]
    }
}

static void ObCJKInstallSavePathFixHook()
{
    static const DWORD kVA_SavePathEntry = 0x00430970;  // sub_430970, file-stream-open constructor
    if (g_savePathFixTramp) return;  // already installed

    ObCJKInstallHook((BYTE*)kVA_SavePathEntry, (void*)ObCJKSavePathFixHook, &g_savePathFixTramp, "SavePathFix");
}

// ---------------------------------------------------------------------
// Diagnostic hook at sub_465130 entry: logs a2/name/a4 of each call
// (SaveDiagEnable=1, max 200 lines). Read-only.
static BYTE*  g_saveEntryDiagTramp = nullptr;
static DWORD  g_saveEntryDiagHits  = 0;
static const DWORD kObCJKSaveEntryDiagCap = 200;

static void __cdecl ObCJKSaveEntryDiagLog(int a2, const char* name, int a4)
{
    g_saveEntryDiagHits++;
    if (!ObCJKSaveDiagEnabled() || g_saveEntryDiagHits > kObCJKSaveEntryDiagCap) return;
    _MESSAGE("obCJK:SaveEntryDiag:#%lu a2=%d name=%s a4=%d",
             g_saveEntryDiagHits, a2, name ? name : "(null)", a4);
}

// thiscall entry: ecx=this (unused), [esp+4]=a2(int), [esp+8]=name(char*),
// [esp+0xC]=a4(int). Original arguments flow through untouched.
static __declspec(naked) void ObCJKSaveEntryDiagHook()
{
    __asm {
        pushad
        push dword ptr [esp+0x2C]  // a4
        push dword ptr [esp+0x2C]  // name (offset stays +0x2C: esp already shifted by the a4 push)
        push dword ptr [esp+0x2C]  // a2   (offset stays +0x2C: esp already shifted by two prior pushes)
        call ObCJKSaveEntryDiagLog
        add  esp, 0Ch
        popad
        jmp  dword ptr [g_saveEntryDiagTramp]
    }
}

static void ObCJKInstallSaveEntryDiagHook()
{
    static const DWORD kVA_SaveEntry = 0x00465130;  // sub_465130, common Save entry point
    if (g_saveEntryDiagTramp) return;  // already installed

    ObCJKInstallHook((BYTE*)kVA_SaveEntry, (void*)ObCJKSaveEntryDiagHook, &g_saveEntryDiagTramp, "SaveEntryDiag");
}
