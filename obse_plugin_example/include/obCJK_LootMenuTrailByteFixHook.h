#pragma once
// Fix (symptom-level fallback): LootMenu slot7/8 item names garbled when a
// Big5 character's trail byte is 0x7C ('|'). The string reaches sub_58CA50
// (Tile SetText+dedup) already missing that last byte, e.g. "甜蛋糕"
// B2 A2 B3 4A BF 7C arrives as B2 A2 B3 4A BF. The root cause (MenuQue's
// delimiter search splitting CJK pairs) is fixed in obCJK_MenuQueDelimHook.h;
// this hook is kept as a backup and no longer fires once that fix works.
//
// This hooks sub_58CA50's entry and, when the string ends in an orphan lead
// byte, appends the missing 0x7C. Only active when lootmenu.dll is loaded;
// all other callers pass through unmodified. a2 is a plain C string.
//
// Entry prologue is `push -1` + `push offset SEH_596020` (7 bytes), so the
// default copyLen stops before the FS-prefixed bytes ObCJKGetInstrLen can't
// parse.
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_Path.h"  // k_iniMain

// ini [obCJK] LootMenuDiagEnable（預設0）：開啟才印修正命中與含0x7C的原始
// 位元組；LootMenuDiagCap（300/500/1000）為各自的印出行數上限。
static bool ObCJKLootMenuDiagEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "LootMenuDiagEnable", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

static DWORD ObCJKLootMenuDiagCap()
{
    static int cached = -1;
    if (cached < 0) {
        cached = GetPrivateProfileIntA("obCJK", "LootMenuDiagCap", 300, k_iniMain);
        if (cached != 300 && cached != 500 && cached != 1000) cached = 300;
    }
    return (DWORD)cached;
}

static BYTE* g_lootMenuFixTramp = nullptr;
static DWORD g_lootMenuFixHits  = 0;
static DWORD g_lootMenuRawHits  = 0;

// -1 = not yet checked, 0 = absent, 1 = present. Checked lazily on first
// call (all plugin DLLs are loaded by then).
static int g_lootMenuDllState = -1;

static inline bool ObCJKLootMenuDllPresent()
{
    if (g_lootMenuDllState < 0)
        g_lootMenuDllState = GetModuleHandleA("lootmenu.dll") ? 1 : 0;
    return g_lootMenuDllState == 1;
}

// Reused every call: sub_58CA50 copies the string before returning.
static char g_lootMenuFixBuf[512];

// Walks the string from the start as single bytes / lead+trail pairs and
// returns true if it ends on a lead byte with nothing after it.
// Fix: checking only the last byte misfired on complete strings, since
// trail bytes (0x40-0xFE) overlap lead bytes (0x81-0xFE), e.g. "不" A4 A3,
// and appended a spurious 0x7C to correct text.
static bool ObCJKLootMenuEndsInOrphanLead(const char* s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        BYTE b = (BYTE)s[i];
        if (ObCJKIsLeadByte(b, g_activeCodePage)) {
            if (i + 1 >= len) return true;  // lead byte with nothing after it
            i += 2;  // consume the lead+trail pair, trust it's well-formed
        } else {
            i += 1;
        }
    }
    return false;
}

static void ObCJKLootMenuFormatHex(const BYTE* p, size_t len, char* out, size_t outSize)
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < outSize; i++) {
        out[pos++] = kHex[(p[i] >> 4) & 0xF];
        out[pos++] = kHex[p[i] & 0xF];
        out[pos++] = ' ';
    }
    out[pos] = '\0';
}

static const char* __cdecl ObCJKLootMenuFixCheck(void* thisPtr, const char* a2)
{
    if (!ObCJKLootMenuDllPresent()) return a2;
    if (!a2) return a2;

    size_t len = strlen(a2);
    if (len == 0 || len + 2 > sizeof(g_lootMenuFixBuf)) return a2;

    // Debug log: raw bytes of any string containing 0x7C. Truncation right
    // after a complete pair ending in 0x7C (e.g. "疊起的布料" -> "疊" C5 7C)
    // leaves no orphan lead and is not repaired below.
    if (memchr(a2, 0x7C, len)) {
        g_lootMenuRawHits++;
        if (ObCJKLootMenuDiagEnabled() && g_lootMenuRawHits <= ObCJKLootMenuDiagCap()) {
            char rawHex[400] = {};
            ObCJKLootMenuFormatHex((const BYTE*)a2, len, rawHex, sizeof(rawHex));
            _MESSAGE("obCJK:LootMenuFix:raw this=%p len=%u bytes=[%s]", thisPtr, (unsigned)len, rawHex);
        }
    }

    if (!ObCJKLootMenuEndsInOrphanLead(a2, len)) return a2;

    memcpy(g_lootMenuFixBuf, a2, len);
    g_lootMenuFixBuf[len]     = 0x7C;  // restore the confirmed-missing trail byte
    g_lootMenuFixBuf[len + 1] = '\0';

    g_lootMenuFixHits++;
    if (ObCJKLootMenuDiagEnabled() && g_lootMenuFixHits <= ObCJKLootMenuDiagCap()) {
        _MESSAGE("obCJK:LootMenuFix:hit#%lu this=%p len %u->%u (appended missing 0x7C trail byte)",
                 g_lootMenuFixHits, thisPtr, (unsigned)len, (unsigned)(len + 1));
    }

    return g_lootMenuFixBuf;
}

// thiscall entry: ecx=this, [esp+4]=a2. Overwrites the a2 stack slot so the
// original function reads the (possibly repaired) pointer.
static __declspec(naked) void ObCJKLootMenuFixHook()
{
    __asm {
        pushad
        push dword ptr [esp+0x24]  // a2: was [esp+4] before pushad's 8 saved regs (0x20) + 4
        push ecx                    // this
        call ObCJKLootMenuFixCheck
        add  esp, 8
        mov  [esp+0x24], eax        // overwrite a2 slot with (possibly repaired) pointer
        popad
        jmp  dword ptr [g_lootMenuFixTramp]
    }
}

static void ObCJKInstallLootMenuTrailByteFixHook()
{
    static const DWORD kVA_LootMenuFix = 0x0058CA50;  // sub_58CA50, Tile SetText+dedup
    if (g_lootMenuFixTramp) return;  // already installed

    // Requires both [obCJK] MenuQueEnable and LootMenuEnable (default 1).
    if (GetPrivateProfileIntA("obCJK", "MenuQueEnable", 1, k_iniMain) == 0 ||
        GetPrivateProfileIntA("obCJK", "LootMenuEnable", 1, k_iniMain) == 0) {
        _MESSAGE("obCJK:LootMenuFix: disabled via ini (MenuQueEnable/LootMenuEnable=0), hook not installed");
        return;
    }
    ObCJKInstallHook((BYTE*)kVA_LootMenuFix, (void*)ObCJKLootMenuFixHook, &g_lootMenuFixTramp, "LootMenuFix");
}
