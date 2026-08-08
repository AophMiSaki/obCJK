#pragma once
// Second half of the "manual save shows success but writes no file" fix —
// see obCJK_CreateFileWShim.h for the encoding-mismatch root cause and the
// hook that makes the underlying open actually succeed for CJK names. This
// hook is defense-in-depth: even with that fix, sub_465130 (the save writer)
// unconditionally returns 1 (success) regardless of whether the FileStream
// it built ever actually opened. Confirmed via a full, fresh IDA decompile of
// sub_465130 (2026-07-19): every one of its vtable write calls through `v7`
// (the FileStream, register `esi` at this point) discards its return value,
// and the function's tail is:
//
//   465836  mov ecx, dword_B33A10
//   46583c  call sub_432890
//   465841  mov al, 1          <-- unconditional; esi (v7) still valid here
//   465843  mov ecx, [esp+5Ch]
//   ...
//
// The FileStream object itself does carry a real, meaningful success flag at
// byte offset +36 (`*((BYTE*)v7+36)`) — set by sub_42FE80 when its open call
// fails, and independently confirmed to be checked elsewhere in the engine
// (sub_464060, the save-list-info reader, does
// `*(_BYTE*)(v8+36) && (v10 = sub_45DBC0(v8,0)) != 0`). sub_465130 alone never
// reads it. This hook patches exactly the `mov al,1` at VA 0x465841 to read
// that flag instead of assuming success.
//
// v7 can legitimately be NULL: when sub_465130's own `this+6` bit 0x200 is
// set (a "size accounting only, no real write" mode used elsewhere in the
// function — see its `*(this+36) += N` accumulator branches), v7 is never
// assigned at all and every write is skipped. That is not a failure, so this
// hook must special-case esi==0 back to the original "return 1" behavior
// rather than reading a NULL+36 flag.
#include <windows.h>
#include "obCJK_HookUtil.h"

static BYTE* g_saveReturnFixTramp = nullptr;

// Naked, no C wrapper needed — this is a pure register/flag check, not
// something that benefits from a C helper. Entered via JMP planted at
// 0x465841 (mid-function, not sub_465130's entry), so register state is
// exactly what the original `mov al,1` saw: esi = v7 (FileStream ptr or
// NULL), and nothing else in this hook's own path depends on incoming flags.
//
// ObCJKInstallHook's auto-computed copyLen for this address is 6 bytes
// (`mov al,1` = 2 bytes `B0 01`, plus `mov ecx,[esp+5Ch]` = 4 bytes
// `8B 4C 24 5C`, verified via get_bytes) — a 5-byte JMP needs at least one
// extra instruction's worth of room, and 6 is the smallest boundary-aligned
// cut. Rather than let the trampoline replay both original instructions
// (which would silently re-set al=1 via the copied `mov al,1`, undoing this
// hook's own decision), this hook computes al itself and then jumps into the
// trampoline at +2 — skipping past its copy of `mov al,1` and landing
// exactly on its copy of `mov ecx,[esp+5Ch]`, which falls straight into
// ObCJKInstallHook's own appended jmp back to 0x465847.
static __declspec(naked) void ObCJKSaveReturnFixHook()
{
    __asm {
        test esi, esi
        jz   ok            // v7==NULL: size-accounting-only branch, never opened a real file — not a failure
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
