#pragma once
// Fix for the manual-save "shows success but writes no file" bug (see memory
// obcjk_bug_save_maxpath). This is the SECOND hook point tried — the first
// (hooking sub_465130's entry to truncate its name argument) was installed
// and confirmed harmless but ineffective: its diagnostic hit-log never fired
// even though the bug still reproduced in-game. Root cause traced further
// via IDA on 2026-07-19: the pause-menu Save dialog's "use/overwrite an
// existing save slot" branch (confirmed via sub_5D3390, one of sub_465130's
// 6 callers) passes name=NULL into sub_465130 and instead makes
// sub_45F2E0 reuse a path already cached at some other object's +0x60
// field — completely bypassing the name argument sub_465130's hook could
// see. Patching every possible upstream branch individually is fragile;
// this hook instead targets the one point they all provably converge on.
//
// sub_430970 (VA 0x00430970) is a generic file-stream-open constructor
// (14 call sites across the exe — textures, BSAs, generic file I/O, not
// save-specific) that every Save path — console, quicksave, autosave, and
// both sub_45F2E0 branches — ultimately calls with the fully-built path.
// Its own disassembly (confirmed via get_bytes/disasm) computes strlen(path)
// and silently clears the destination buffer to an empty string whenever
// that length reaches MAX_PATH (260) — no error, no failure flag; see
// memory's Bug點1 for the full trace. Because sub_430970 is shared by many
// unrelated file operations, this hook only acts when the incoming path
// ends in ".ess" (case-insensitive) — the same extension check sub_45F2E0
// itself uses to recognize a save file — so no other path type is ever
// touched.
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_SaveDiag.h"  // SaveDiagEnable

// Walks `s` (length `len`, not assumed NUL-terminated at `len`) from the
// start as a sequence of single-byte / DBCS-pair (or UTF-8 multi-byte, under
// kCP_UTF8) characters — same well-formed-input assumption every other byte
// classifier in this project makes (e.g. ObCJKLootMenuEndsInOrphanLead).
// Returns the largest prefix length <= maxLen that lands on a character
// boundary, so a multi-byte character is never split mid-sequence. Safe to
// run over the folder-path prefix too: drive letters/colons/backslashes/
// ASCII directory names are never DBCS lead bytes and are always <=0x7F
// under UTF-8, so they're always treated as 1-byte characters here.
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

// Diagnostic-only cap: this branch (path ends in ".ess") should only ever be
// reached once per actual save attempt, so a generous cap is just a safety
// net against something unexpectedly calling this in a loop — not a normal
// operating limit like the 300-call diagnostic caps elsewhere in this
// project. Gated behind SaveDiagEnable (obCJK_SaveDiag.h) — this confirmed
// the hook point is reached during a real manual save (see memory
// obcjk_bug_save_maxpath's "第二版" section), so it no longer needs to be
// unconditional; it only prints when SaveDiagEnable=1.
static const DWORD kObCJKSavePathDiagCap = 200;

// __cdecl, called from the naked hook below with (path) pushed. Returns the
// pointer sub_430970's replayed prologue should treat as its path argument —
// unchanged unless the path both ends in ".ess" AND is over the cap.
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

// thiscall entry: ecx=this (the file-stream object being constructed,
// unused here), [esp+4]=arg_0(path, char*), [esp+8]=arg_4(mode, int),
// [esp+0xC]=arg_8(flags, int) — exactly the CPU state at sub_430970's real
// first instruction, since this hook patches the function's own entry VA
// (nothing has touched the stack yet). Overwrites the path slot in place so
// the trampoline's replayed original prologue — and the strlen/MAX_PATH
// check right after it — reads the (possibly truncated) pointer.
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
// ★2026-07-19 temporary diagnostic-only hook, sub_465130 entry — purely
// observational (never modifies anything), added alongside the sub_430970
// hook above to empirically settle whether sub_465130 (the "common Save
// entry point" from the earlier, reverted attempt) is even reached at all
// during the user's actual manual-save action. Two rounds of pure static
// call-graph reasoning have each turned out wrong in a different way (see
// memory obcjk_bug_save_maxpath's "第二版" section), so this logs every
// call (capped, gated behind SaveDiagEnable — obCJK_SaveDiag.h) instead of
// guessing further from decompile output alone.
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
// [esp+0xC]=a4(int) — same layout established for the sub_465130 hook in
// the previous (reverted) attempt. Read-only: nothing is written back to
// the stack, original arguments flow through untouched.
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
