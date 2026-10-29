#pragma once
// Fix: manual save reports success even when no file was written.
// sub_465130 (save writer) ends with an unconditional `mov al,1` at 0x465841,
// ignoring whether its FileStream (v7, in esi) actually opened.
// This hook replaces that with the FileStream open flag at byte +36
// (set by sub_42FE80; 0 = open failed).
// esi==NULL is the size-accounting-only mode (this+6 bit 0x200, no real
// write) and keeps the original return 1.
// Companion to obCJK_CreateFileWShim.h, which fixes the CJK filename open itself.
#include <windows.h>
#include "obCJK_HookUtil.h"

static BYTE* g_saveReturnFixTramp = nullptr;

// Entered at 0x465841 with esi = FileStream ptr or NULL; sets al.
// Trampoline copies 6 bytes: `mov al,1` (B0 01) + `mov ecx,[esp+5Ch]`
// (8B 4C 24 5C). Jump to tramp+2 to skip the copied `mov al,1`, which would
// overwrite al back to 1.
static __declspec(naked) void ObCJKSaveReturnFixHook()
{
    __asm {
        test esi, esi
        jz   ok            // NULL = size-accounting-only mode, not a failure
        cmp  byte ptr [esi+36], 0
        jne  ok
        mov  al, 0
        jmp  next
    ok:
        mov  al, 1
    next:
        mov  edx, dword ptr [g_saveReturnFixTramp]
        add  edx, 2
        jmp  edx
    }
}

static void ObCJKInstallSaveReturnFixHook()
{
    static const DWORD kVA_SaveReturnPoint = 0x00465841;  // sub_465130 tail, `mov al,1` right before its final return
    if (g_saveReturnFixTramp) return;  // already installed

    ObCJKInstallHook((BYTE*)kVA_SaveReturnPoint, (void*)ObCJKSaveReturnFixHook, &g_saveReturnFixTramp, "SaveReturnFix");
}
