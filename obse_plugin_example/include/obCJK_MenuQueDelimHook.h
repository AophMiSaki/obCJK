#pragma once
// Fix (root cause): LootMenu (and any MenuQue tile_SetString caller) cut
// CJK item/value text short.
//
// MenuQue's KYO::TileExtractor::Extract (sub_1000A800 in Submodule.Game.dll)
// splits "path|value" strings via sub_10004BD0, whose scan sub_10004610
// (find_first_of over the delimiter set "|"=0x7C or "@"=0x40) walks the
// already-expanded text byte by byte. A CJK trail byte equal to 0x7C/0x40
// was taken as a delimiter and the token truncated there: "甜蛋糕"
// (B2 A2 B3 4A BF 7C) lost its last byte; "疊起的布料" (C5 7C ...) became "疊".
//
// sub_10004610: int __thiscall(this=scanned text, Buf=delimiter set,
// pos=start offset, MaxCount=delimiter set length); returns the offset of
// the first delimiter at/after pos, or -1. `this` is the raw "path|value"
// text at TileExtractor+0x28.
//
// Only the call site inside sub_10004BD0 (VA 0x10004C3D, `E8 rel32`) is
// retargeted, not sub_10004610 itself (it has another unrelated caller,
// sub_10004850). Matches that would split a CJK pair are skipped.
//
// Addresses are inside Submodule.Game.dll (loaded by MenuQue's own
// Submodule loader), resolved as module base + RVA against the file's
// ImageBase (kObCJKMenuQuePreferredBase). Installed from main.cpp on
// kMessage_GameInitialized, when the DLL is guaranteed to be mapped.
#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_Path.h"  // k_iniMain

static const char* const kObCJKMenuQueModuleName    = "Submodule.Game.dll";
// The file's ImageBase from its PE header (the in-memory field is rewritten
// by MenuQue's loader, see ObCJKModuleRvaToAddr).
static const DWORD       kObCJKMenuQuePreferredBase = 0x10000000;
static const DWORD       kObCJKVA_FindDelim         = 0x10004610;  // sub_10004610, find_first_of(delimset)
static const DWORD       kObCJKVA_ExtractCallSite   = 0x10004C3D;  // call site inside sub_10004BD0

typedef int (__thiscall *ObCJKFnFindDelim)(void* this_, void* Buf, int pos, size_t MaxCount);
static ObCJKFnFindDelim g_obCJKOrigFindDelim   = nullptr;
static DWORD            g_obCJKMenuQueDelimHits = 0;

// Char data of the scanned std::string: capacity at +0x14; heap pointer at
// +0x00 when capacity >= 0x10, otherwise the inline 16-byte buffer.
static inline const char* ObCJKMQStrData(const void* strObj)
{
    UInt32 capacity = *(const UInt32*)((const BYTE*)strObj + 0x14);
    return (capacity < 0x10) ? (const char*)strObj : *(const char* const*)strObj;
}

// Walks `text` from `start` to `matchPos` by whole lead+trail pairs; true if
// the walk overshoots matchPos, i.e. matchPos is a trail byte (false match).
static bool ObCJKMQSplitsPair(const char* text, int start, int matchPos)
{
    int i = start;
    while (i < matchPos)
        i += ObCJKIsLeadByte((BYTE)text[i], g_activeCodePage) ? 2 : 1;
    return i > matchPos;
}

// Replacement for sub_10004610: calls the original and, while the match
// splits a CJK pair, searches again after it. __cdecl (free functions can't
// be __thiscall); the thunk passes `this_` explicitly.
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

// Thunk at the patched call site with sub_10004610's convention (thiscall:
// ecx=this, [esp+4]=Buf, [esp+8]=pos, [esp+0xC]=MaxCount, `retn 0Ch`):
// forwards to the __cdecl impl and returns the same way.
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

    // ini [obCJK] MenuQueEnable (default 1).
    if (GetPrivateProfileIntA("obCJK", "MenuQueEnable", 1, k_iniMain) == 0) {
        _MESSAGE("obCJK:MenuQueDelimFix: disabled via ini (MenuQueEnable=0), hook not installed");
        return;
    }

    HMODULE mod = GetModuleHandleA(kObCJKMenuQueModuleName);
    if (!mod) return;  // MenuQue not loaded (yet) — caller may retry later

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
