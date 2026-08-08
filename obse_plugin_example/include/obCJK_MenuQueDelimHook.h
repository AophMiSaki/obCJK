#pragma once
// Fixes obCJK's confirmed root cause for LootMenu (and any other MenuQue
// tile_SetString caller) mangling CJK item/value text — see
// obcjk_bug_garbled_lootmenu_slot78 memory (根因已定案，2026-07-14).
//
// MenuQue's KYO::TileExtractor::Extract (sub_1000A800 in Submodule.Game.dll)
// splits a "path|value"-style string via sub_10004BD0, whose *second*
// internal scan (sub_10004610 — find_first_of over the current delimiter
// charset, "|"=0x7C or "@"=0x40, selected by sub_10004550) walks the
// ALREADY-EXPANDED value text (e.g. an OBSE %n-expanded item name) one byte
// at a time. Big5's legal trail-byte range (0x40~0xFE) fully covers both
// delimiter values, so any CJK character whose trail byte happens to equal
// 0x7C or 0x40 gets misread as a delimiter and the token is truncated right
// there — confirmed byte-exact: "甜蛋糕" (Big5
// B2 A2 B3 4A BF 7C) arrives 1 byte short (missing trailing 0x7C); "疊起的
// 布料" (C5 7C ...) arrives as just "疊" because its OWN trail byte trips
// the false match at the very first character. Both shapes are the same
// bug, both fixed by the same scan-time skip below.
//
// sub_10004610's signature (disasm+decompile confirmed, 2026-07-14):
//   int __thiscall(this=text being scanned, Buf=delimiter-charset ptr,
//                   pos=start offset, MaxCount=delimiter-charset length)
// It scans `this`'s own buffer starting at `pos`, returns the offset of the
// first byte found in Buf (a delimiter match), or -1 if none before the end.
// `this` here is the RAW "path|value" text itself — KYO::TileExtractor keeps
// it at object offset +0x28 (confirmed via sub_10004F00, which assigns the
// Extract() input string there through sub_10002D60 — NOT at the object's
// own +0x00, which instead holds the small delimiter-charset string used as
// `Buf`/MaxCount for this call).
//
// Hook point: this retargets only the ONE call site inside sub_10004BD0 (VA
// 0x10004C3D) that performs this "find where the current token ends" scan —
// NOT sub_10004610 itself, which has a second, unrelated caller
// (sub_10004850, confirmed via xrefs_to) that must not be touched. Every
// caller of sub_10004BD0 (both the path- and the value-extraction calls
// inside sub_1000A800, plus sub_10003DF0/sub_10003F20/sub_10004E20) gets a
// byte-safe scan as a result; that's intentional and safe — no legitimate
// caller ever wants a CJK character's trail byte treated as a delimiter.
//
// Call-site instruction verified via get_bytes (2026-07-14): bytes at
// 0x10004C37..0x10004C41 are `push ecx / push edi / push eax /
// lea ecx,[esi+28h] / call sub_10004610` (E8 CE F9 FF FF, target confirmed
// = 0x10004610), with the E8 landing exactly on an instruction boundary —
// previous push ends at 0x10004C3D, next instruction (`cmp eax,0FFFFFFFFh`)
// begins at 0x10004C42. Patching just the 4-byte rel32 displacement in
// place needs no trampoline and cannot split an instruction.
//
// Module: MenuQue's actual plugin DLL is named Submodule.Game.dll (verified
// filename — main.cpp already does GetModuleHandleA("Submodule.Game.dll")
// for an unrelated editor-launch check; MenuQue.dll itself is a thin OBSE
// stub that loads Submodule.Game.dll — or Submodule.CS.dll in the
// Construction Set — through its own private "Submodule" loader, not the
// standard OBSE plugin-load path). This is
// obCJK's first hook INSIDE a DLL other than Oblivion.exe, so addresses are
// resolved at runtime via GetModuleHandleA's actual base plus an RVA
// (ObCJKModuleRvaToAddr, obCJK_HookUtil.h) instead of being used as fixed
// absolute pointers the way every Oblivion.exe hook in this project does.
// The RVA is computed against kObCJKMenuQuePreferredBase, a constant
// verified independently from the deployed file's own PE header — NOT
// re-derived from the live module's in-memory header, because MenuQue's
// custom Submodule loader was found (2026-07-14, see kObCJKMenuQuePreferredBase's
// own comment) to overwrite that field with the actual load address rather
// than leaving the file's original declared base in place. Do not copy this
// file's VA-resolution pattern into an Oblivion.exe hook, or assume a live
// header read is safe for another custom-loaded module, without re-verifying.
//
// Installed lazily (like obCJK_LootMenuTrailByteFixHook.h's per-call module
// check) rather than from OBSEPlugin_Load, since OBSE's plugin load order
// doesn't guarantee Submodule.Game.dll is already mapped at that point —
// call ObCJKInstallMenuQueDelimHook() from a point after all plugins are
// confirmed loaded (main.cpp calls it on kMessage_GameInitialized).
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_Path.h"  // k_iniMain

static const char* const kObCJKMenuQueModuleName    = "Submodule.Game.dll";
// Verified independently by parsing the deployed file's own PE header
// (IMAGE_OPTIONAL_HEADER.ImageBase field, 2026-07-14) — NOT re-derived from
// the loaded module at runtime, since MenuQue's custom "Submodule" loader
// rewrites that field in memory to match wherever it actually placed the
// module (see ObCJKModuleRvaToAddr's comment in obCJK_HookUtil.h). This is
// the file's original declared base, always 0x10000000 for this build.
static const DWORD       kObCJKMenuQuePreferredBase = 0x10000000;
static const DWORD       kObCJKVA_FindDelim         = 0x10004610;  // sub_10004610, find_first_of(delimset)
static const DWORD       kObCJKVA_ExtractCallSite   = 0x10004C3D;  // call site inside sub_10004BD0

typedef int (__thiscall *ObCJKFnFindDelim)(void* this_, void* Buf, int pos, size_t MaxCount);
static ObCJKFnFindDelim g_obCJKOrigFindDelim   = nullptr;
static DWORD            g_obCJKMenuQueDelimHits = 0;

// Returns `this`'s (the scanned text object's) own char data pointer,
// replicating the same capacity-vs-SSO-threshold branch seen throughout this
// object's disassembly: capacity field at +0x14; heap pointer at +0x00 when
// capacity>=0x10, otherwise the object itself IS the inline 16-byte buffer.
static inline const char* ObCJKMQStrData(const void* strObj)
{
    UInt32 capacity = *(const UInt32*)((const BYTE*)strObj + 0x14);
    return (capacity < 0x10) ? (const char*)strObj : *(const char* const*)strObj;
}

// Walks `text` from `start` to `matchPos` consuming whole Big5 lead+trail
// pairs (trusting well-formed input, same assumption every other byte
// classifier in this project makes — see ObCJKIsLeadByte/ObCJKIsTrailByte in
// obCJK_Encoding.h). Returns true if that walk overshoots `matchPos` by
// exactly one byte, i.e. matchPos is a trail byte whose lead byte sits at
// matchPos-1: a real CJK character was split, not a genuine delimiter.
static bool ObCJKMQSplitsPair(const char* text, int start, int matchPos)
{
    int i = start;
    while (i < matchPos)
        i += ObCJKIsLeadByte((BYTE)text[i], g_activeCodePage) ? 2 : 1;
    return i > matchPos;
}

// Replacement logic for sub_10004610, called by the naked thunk below.
// __cdecl (not __thiscall — MSVC's E1447 forbids __thiscall on a free
// function; only a real non-static member function gets it implicitly), so
// the thunk passes `this_` as an explicit first argument instead of via ecx.
// Falls straight through to the original for every non-CJK-split case (no
// match, or a genuine delimiter at a real character boundary).
static int __cdecl ObCJKMenuQueDelimFindFixImpl(void* this_, void* Buf, int pos, size_t MaxCount)
{
    int result = g_obCJKOrigFindDelim(this_, Buf, pos, MaxCount);
    if (result < 0) return result;

    const char* text = ObCJKMQStrData(this_);
    int scanStart = pos;
    while (result >= 0 && ObCJKMQSplitsPair(text, scanStart, result)) {
        g_obCJKMenuQueDelimHits++;
        scanStart = result + 1;
        result = g_obCJKOrigFindDelim(this_, Buf, scanStart, MaxCount);
    }
    return result;
}

// Naked thunk installed at the patched call site (VA 0x10004C3D) in place of
// sub_10004610 — must reproduce sub_10004610's exact calling convention
// (thiscall: ecx=this, [esp+4]=Buf/[esp+8]=pos/[esp+0xC]=MaxCount pushed by
// the caller, callee cleans up via `retn 0Ch`) since the caller (sub_10004BD0)
// is unmodified and still sets up the call this way. Captures the 3 stack
// args and ecx into registers before pushing anything (so the later `push
// dword ptr [esp+...]` reads are never thrown off by our own pushes), calls
// the __cdecl impl above with (this_, Buf, pos, MaxCount) in the matching
// left-to-right order, cleans up its own 4 pushed args, then replays the
// original callee's `retn 0Ch` so the caller's stack balances identically
// whether it called the real sub_10004610 or this thunk.
static __declspec(naked) int ObCJKMenuQueDelimFindFixThunk()
{
    __asm {
        mov eax, [esp+4]              // Buf
        mov edx, [esp+8]              // pos
        push dword ptr [esp+0Ch]      // MaxCount (last stack arg, still valid — no pushes yet)
        push edx                      // pos
        push eax                      // Buf
        push ecx                      // this_
        call ObCJKMenuQueDelimFindFixImpl
        add  esp, 16
        retn 0Ch
    }
}

static void ObCJKInstallMenuQueDelimHook()
{
    if (g_obCJKOrigFindDelim) return;  // already installed

    // Gated by [obCJK] MenuQueEnable (default 1, same "<Feature>Enable"
    // convention as TexSwapEnable/AsciiRenderEnable) — see obCJK_iniEdit.py's
    // menuque slider.
    if (GetPrivateProfileIntA("obCJK", "MenuQueEnable", 1, k_iniMain) == 0) {
        _MESSAGE("obCJK:MenuQueDelimFix: disabled via ini (MenuQueEnable=0), hook not installed");
        return;
    }

    HMODULE mod = GetModuleHandleA(kObCJKMenuQueModuleName);
    if (!mod) return;  // MenuQue not loaded (yet) — caller may retry later

    // This module is NOT relocated the way the standard Windows loader
    // would be — MenuQue's own "Submodule"
    // loader rewrites the in-memory ImageBase field to match its actual load
    // address, so re-deriving the preferred base from the live module (as
    // ObCJKModuleRvaToAddr originally did) silently computed a no-op offset.
    // Use the independently file-verified constant instead — see
    // kObCJKMenuQuePreferredBase above.
    BYTE* findDelimAddr = ObCJKModuleRvaToAddr(mod, kObCJKVA_FindDelim, kObCJKMenuQuePreferredBase);
    BYTE* callSite = ObCJKModuleRvaToAddr(mod, kObCJKVA_ExtractCallSite, kObCJKMenuQuePreferredBase);
    _VMESSAGE("obCJK:MenuQueDelimFix:diag mod=%p findDelimAddr=%p callSite=%p "
             "callSiteBytes=[%02X %02X %02X %02X %02X %02X %02X %02X]",
             mod, findDelimAddr, callSite,
             callSite[0], callSite[1], callSite[2], callSite[3],
             callSite[4], callSite[5], callSite[6], callSite[7]);

    g_obCJKOrigFindDelim = (ObCJKFnFindDelim)findDelimAddr;
    ObCJKPatchCallTarget(callSite, (void*)ObCJKMenuQueDelimFindFixThunk, "MenuQueDelimFix");
}
