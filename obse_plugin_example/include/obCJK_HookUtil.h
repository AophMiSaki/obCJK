#pragma once
// Generic x86 hotpatch/trampoline utilities for hooking Oblivion.exe directly.
// See Hook內容與規範.md.
#include <windows.h>
#include "common/IDebugLog.h"

// Returns extra bytes beyond opcode+ModRM due to SIB byte and displacement.
// Called only for instructions that have a ModRM byte.
static int ObCJKModRMExtra(const BYTE* modrm_ptr)
{
    BYTE modrm = *modrm_ptr;
    BYTE mod = (modrm >> 6) & 3;
    BYTE rm  = modrm & 7;
    if (mod == 3) return 0;
    int extra = 0;
    if (rm == 4) {
        extra += 1;   // SIB byte
        // SIB base==5 with mod==0: disp32 appended (e.g. mov reg,[eax*4+addr32])
        if (mod == 0 && (*(modrm_ptr + 1) & 7) == 5)
            extra += 4;
    }
    if      (mod == 1)   extra += 1;    // disp8
    else if (mod == 2)   extra += 4;    // disp32
    else if (rm  == 5)   extra += 4;    // mod==0, rm==5: disp32 only (no base)
    return extra;
}

// Returns byte length of a single x86 instruction. Uses proper ModRM decoding
// to avoid splitting multi-byte memory-access instructions mid-way — a fixed
// byte-count guess corrupts the trampoline if the target uses a memory-operand
// form (e.g. lea, mov r,[mem], movzx r,[mem]) instead of reg-to-reg.
static int ObCJKGetInstrLen(const BYTE* p)
{
    BYTE op = p[0];
    if (op >= 0x50 && op <= 0x5F) return 1;              // push/pop r32
    if (op == 0xE8 || op == 0xE9) return 5;               // call/jmp rel32
    if (op == 0x68) return 5;                              // push imm32
    if (op == 0x6A) return 2;                              // push imm8
    if (op >= 0xB8 && op <= 0xBF) return 5;               // mov r32, imm32
    if (op >= 0xB0 && op <= 0xB7) return 2;               // mov r8, imm8 (no ModRM) —
    // needed by obCJK_SaveReturnFixHook.h's hook at sub_465130 VA 0x465841
    // (`mov al, 1`); without this case it fell through to the default
    // `return 1` below and split the instruction (the stray immediate byte
    // was then misparsed as a second opcode).
    if (op == 0xFF) return 2 + ObCJKModRMExtra(p + 1);    // Group 5 (INC/DEC/CALL/JMP/PUSH
    // r/m32) — every reg-field variant is ModRM-only, no immediate. Needed by
    // obCJK_SaveWideOpenHook.h's hook at sub_982440's entry (`ff 74 24 0C` =
    // push [esp+0Ch]); without this case it fell through to the default
    // `return 1` below and split the instruction.
    if (op == 0x0F && p[1] >= 0xB6 && p[1] <= 0xB7)       // movzx r32, r/m8|r/m16
        return 3 + ObCJKModRMExtra(p + 2);
    if (op >= 0xD8 && op <= 0xDF) {                        // x87 FPU (fld/fild/fadd/
    // fsubp/fstp/etc.) — needed by obCJK_GlyphHook.h's PathA line-height
    // hook target (VA 0x5768c4, sub_576670's per-line Y-cursor step is
    // pure FPU arithmetic). Register-to-register form (2nd byte >= 0xC0,
    // e.g. DE E9 = fsubp st(1),st) is always 2 bytes total; memory-operand
    // form reuses the same ModRM/SIB/disp decoding as every other case
    // here. See PathA主要文字顯示.md 第25節.
        BYTE modrm2 = p[1];
        if (modrm2 >= 0xC0) return 2;
        return 2 + ObCJKModRMExtra(p + 1);
    }
    if (op == 0x8B || op == 0x89 || op == 0x8D)           // mov/lea r32, r/m32
        return 2 + ObCJKModRMExtra(p + 1);
    if (op == 0x8A || op == 0x88)                          // mov r8, r/m8 / r/m8, r8
        return 2 + ObCJKModRMExtra(p + 1);
    if (op == 0x84 || op == 0x85)                          // test r/m8,r8 / r/m32,r32 —
    // needed by obCJK_DeleteFileWShim.h's diagnostic hook at sub_453480+0x1A
    // (VA 0x45349A, `85 f6` = `test esi,esi`); without this case it fell
    // through to the default `return 1` below and split the instruction,
    // same failure mode documented for the 0x3C/0xB0-B7 cases above.
        return 2 + ObCJKModRMExtra(p + 1);
    if (op == 0x80)                                        // alu r/m8,  imm8
        return 3 + ObCJKModRMExtra(p + 1);
    if (op == 0x83)                                        // alu r/m32, imm8
        return 3 + ObCJKModRMExtra(p + 1);
    if (op == 0x81)                                        // alu r/m32, imm32
        return 6 + ObCJKModRMExtra(p + 1);
    if (op == 0x3C) return 2;                              // CMP AL, imm8 (accumulator
    // form, no ModRM) — needed by obCJK_WordWrapHook.h's TrailByteGuard hook
    // (VA 0x576226 sits right on top of `cmp al,7Eh`); without this case it
    // fell through to the default `return 1` below and split the
    // instruction, see that hook's header comment for the corrupted-copyLen
    // trace this caused.
    // ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m32,r32 and r32,r/m32 — same ModRM
    // encoding as mov/lea above.
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
        return 2 + ObCJKModRMExtra(p + 1);
    }
    if (op == 0xEB) return 2;                // jmp rel8 — same reasoning as above,
    if (op >= 0x70 && op <= 0x7F) return 2;  // as is Jcc rel8.
    if (op == 0xF6 || op == 0xF7) {          // Grp3: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m
        // reg field (ModRM bits 3-5) selects the sub-opcode: only TEST (reg 0/1)
        // carries an immediate — NOT/NEG/MUL/IMUL/DIV/IDIV (reg 2-7) do not.
        BYTE reg = (p[1] >> 3) & 7;
        int immLen = (reg <= 1) ? (op == 0xF6 ? 1 : 4) : 0;
        return 2 + ObCJKModRMExtra(p + 1) + immLen;
    }
    return 1;
}

// Minimum bytes (>= minLen, default 5 for a JMP rel32 patch) whose instructions
// end on a boundary — avoids splitting an instruction across the patch, which
// would corrupt the trampoline.
static int ObCJKCalcCopyLen(const BYTE* fn, int minLen = 5)
{
    int n = 0;
    while (n < minLen) n += ObCJKGetInstrLen(fn + n);
    return n;
}

// Installs a 5-byte JMP hook at `target`, building a RWX trampoline that runs
// the overwritten original bytes then jumps back to target+copyLen. copyLen is
// always computed via ObCJKCalcCopyLen — callers never guess a byte count.
// `tramp` receives the trampoline entry point (jmp/call through it to run the
// original code path). Safe to call any time after `target`'s module is mapped.
static bool ObCJKInstallHook(BYTE* target, void* hookFn, BYTE** tramp, const char* tag)
{
    int copyLen = ObCJKCalcCopyLen(target);

    *tramp = (BYTE*)VirtualAlloc(NULL, copyLen + 5, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (!*tramp) {
        _WARNING("obCJK:HookUtil:%s: VirtualAlloc failed (err=%lu)", tag, GetLastError());
        return false;
    }
    memcpy(*tramp, target, copyLen);
    BYTE* jmpBack = *tramp + copyLen;
    jmpBack[0] = 0xE9;
    *(INT32*)(jmpBack + 1) = (INT32)((DWORD)(target + copyLen) - (DWORD)(jmpBack + 5));

    DWORD oldProt = 0;
    VirtualProtect(target, copyLen, PAGE_EXECUTE_READWRITE, &oldProt);
    target[0] = 0xE9;
    *(INT32*)(target + 1) = (INT32)((DWORD)hookFn - (DWORD)(target + 5));
    for (int i = 5; i < copyLen; i++) target[i] = 0x90;
    VirtualProtect(target, copyLen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, copyLen);

    _MESSAGE("obCJK:HookUtil:%s: ok (target=%p copyLen=%d)", tag, target, copyLen);
    return true;
}

// Resolves an IDA-derived VA to the actual runtime address inside `mod`.
// `knownPreferredBase` must be supplied by the caller (verified independently,
// e.g. from the on-disk file's own IMAGE_OPTIONAL_HEADER.ImageBase) — do not
// re-derive it by reading mod's live in-memory ImageBase field. See
// Hook內容與規範.md「二、」for why (MenuQue's custom Submodule loader
// rewrites that field, which silently broke this for Submodule.Game.dll).
static BYTE* ObCJKModuleRvaToAddr(HMODULE mod, DWORD idaVA, DWORD knownPreferredBase)
{
    return (BYTE*)mod + (idaVA - knownPreferredBase);
}

// Retargets an existing 5-byte `call rel32` (E8 xx xx xx xx) instruction to
// `newTarget` in place, without touching any other byte — no trampoline
// needed since we're not altering the callee's own prologue, only redirecting
// one specific call site to a different function with a matching signature.
// Caller must have already verified via get_bytes that `callSiteAddr` really
// is the start of a genuine E8 call (this only re-checks the opcode byte as
// a last-chance guard, it does not re-derive instruction boundaries).
static bool ObCJKPatchCallTarget(BYTE* callSiteAddr, void* newTarget, const char* tag)
{
    if (callSiteAddr[0] != 0xE8) {
        _WARNING("obCJK:HookUtil:%s: byte at %p is not 0xE8 (call rel32), refusing to patch", tag, callSiteAddr);
        return false;
    }
    DWORD oldProt = 0;
    VirtualProtect(callSiteAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    *(INT32*)(callSiteAddr + 1) = (INT32)((DWORD)newTarget - (DWORD)(callSiteAddr + 5));
    VirtualProtect(callSiteAddr, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), callSiteAddr, 5);

    _MESSAGE("obCJK:HookUtil:%s: call-site patched (site=%p newTarget=%p)", tag, callSiteAddr, newTarget);
    return true;
}
