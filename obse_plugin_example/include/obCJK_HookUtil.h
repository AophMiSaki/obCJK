#pragma once
// Generic x86 hotpatch/trampoline utilities for hooking Oblivion.exe directly.
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

// Returns byte length of a single x86 instruction, with ModRM/SIB/disp
// decoding so memory-operand forms aren't split mid-instruction.
// Fix: each case below that was missing (mov r8,imm8; FF group; x87; test;
// cmp al,imm8) fell through to `return 1`, split a hooked instruction and
// corrupted that hook's trampoline.
static int ObCJKGetInstrLen(const BYTE* p)
{
    BYTE op = p[0];
    if (op >= 0x50 && op <= 0x5F) return 1;              // push/pop r32
    if (op == 0xE8 || op == 0xE9) return 5;               // call/jmp rel32
    if (op == 0x68) return 5;                              // push imm32
    if (op == 0x6A) return 2;                              // push imm8
    if (op >= 0xB8 && op <= 0xBF) return 5;               // mov r32, imm32
    if (op >= 0xB0 && op <= 0xB7) return 2;               // mov r8, imm8 (no ModRM)
    if (op == 0xFF) return 2 + ObCJKModRMExtra(p + 1);    // Group 5 INC/DEC/CALL/JMP/PUSH r/m32, no immediate
    if (op == 0x0F && p[1] >= 0xB6 && p[1] <= 0xB7)       // movzx r32, r/m8|r/m16
        return 3 + ObCJKModRMExtra(p + 2);
    if (op >= 0xD8 && op <= 0xDF) {                        // x87 FPU: reg form (2nd byte >= 0xC0) is 2 bytes
        BYTE modrm2 = p[1];
        if (modrm2 >= 0xC0) return 2;
        return 2 + ObCJKModRMExtra(p + 1);
    }
    if (op == 0x8B || op == 0x89 || op == 0x8D)           // mov/lea r32, r/m32
        return 2 + ObCJKModRMExtra(p + 1);
    if (op == 0x8A || op == 0x88)                          // mov r8, r/m8 / r/m8, r8
        return 2 + ObCJKModRMExtra(p + 1);
    if (op == 0x84 || op == 0x85)                          // test r/m8,r8 / r/m32,r32
        return 2 + ObCJKModRMExtra(p + 1);
    if (op == 0x80)                                        // alu r/m8,  imm8
        return 3 + ObCJKModRMExtra(p + 1);
    if (op == 0x83)                                        // alu r/m32, imm8
        return 3 + ObCJKModRMExtra(p + 1);
    if (op == 0x81)                                        // alu r/m32, imm32
        return 6 + ObCJKModRMExtra(p + 1);
    if (op == 0x3C) return 2;                              // cmp al, imm8 (no ModRM)
    // ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m,r and r,r/m — ModRM encoded.
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
    if (op == 0xEB) return 2;                // jmp rel8
    if (op >= 0x70 && op <= 0x7F) return 2;  // Jcc rel8
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

// Converts an IDA VA to the runtime address inside `mod`, using the caller's
// known preferred base (the file's ImageBase).
// Fix: don't read mod's in-memory ImageBase — MenuQue's Submodule loader
// rewrites it, which broke this for Submodule.Game.dll.
static BYTE* ObCJKModuleRvaToAddr(HMODULE mod, DWORD idaVA, DWORD knownPreferredBase)
{
    return (BYTE*)mod + (idaVA - knownPreferredBase);
}

// Retargets an existing 5-byte `call rel32` (E8) to `newTarget` in place;
// no trampoline. The caller must ensure `callSiteAddr` is the start of an E8
// call; only the opcode byte is re-checked here.
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
