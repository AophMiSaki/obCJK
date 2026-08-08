#pragma once
// Symptom-level fix for the LootMenu slot7/8 off-by-one garble bug (see
// obcjk_bug_garbled_lootmenu_slot78 memory, 續五/續六/續七). Root cause is
// still NOT located: something in MenuQue's tile_SetString/%n item-name
// expansion path (never found despite extensive static tracing — see that
// memory's 續四~續九) truncates the string it eventually passes to
// sub_58CA50 (Tile SetText+dedup) by exactly one byte whenever a Big5
// character's trail byte equals 0x7C ('|', the same byte LootMenu's own
// esp script uses as a path/format separator in "lootmenu\_stringN|%n" —
// suspected but unconfirmed to be a shared tokenizer coincidentally
// matching this value). Confirmed directly: "甜蛋糕"
// (Big5 B2 A2 B3 4A BF 7C, 6 bytes) arrived at sub_58CA50 as only 5 bytes
// (B2 A2 B3 4A BF) with the object's own oldSize field also recording 5 —
// the byte is gone before this function is ever called, not lost by any
// obCJK hook or by this function's own logic.
//
// This does NOT fix pathA's separate '~' (0x7E) hotkey-stripping bug
// (obCJK_WordWrapHook.h's TrailByteGuard) — confirmed to be a different
// function (sub_575B40) via full disassembly (2026-07-14), not the same
// mechanism, not fixed here.
//
// Scope-gated to LootMenu specifically per user instruction (2026-07-14):
// only repair when lootmenu.dll is loaded. Every other caller of
// sub_58CA50 (main menu, pause menu, any other Tile string trait) falls
// through completely unmodified — this hook must never touch text outside
// LootMenu's own code path, since the missing-trailing-0x7C mechanism has
// only ever been observed there and is NOT confirmed to generalize.
//
// Hook point (VA 0x58CA50) and its trampoline-copyLen safety were already
// verified via get_bytes against this exact address (2026-07-14, see the
// retired obCJK_DebugSlot78Hook.h that originally used this same VA — not
// re-derived here, same target, same verification stands): raw entry is
// `push -1` (2 bytes) + `push offset SEH_596020` (5 bytes) = 7-byte
// prologue, landing on a clean instruction boundary before the
// FS-prefixed/opcode-0xA1 bytes ObCJKGetInstrLen can't parse; the default
// minLen=5 copyLen never reaches those bytes. Do not reuse this reasoning
// for a different hook target without re-deriving it the same way.
//
// a2's type (plain null-terminated char*, not a compact-string struct) is
// inferred from the call site (sub_58CF40 @ 0x58d09a pushes a raw DWORD
// loaded via `mov reg,[x+8]`) and from sub_58CA50 itself passing it
// straight into sub_4028D0(dst, a2, 0) — the trailing 0 argument selects
// sub_4028D0's strlen-based length path, confirming sub_58CA50 never
// receives an explicit length for this argument and must compute it via
// strlen, i.e. a2 is a real C string. See obcjk_bug_garbled_lootmenu_slot78
// memory, 續五's sibling finding, for why this is disasm-derived rather than
// decompile-derived (Hex-Rays cache was unreliable in this IDB).
#include <windows.h>
#include <string.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_Encoding.h"
#include "obCJK_Path.h"  // k_iniMain

// ini key LootMenuDiagEnable ([obCJK] 節, obCJK_iniEdit.py 功能開關) — 預設
// 關閉（0）。開啟後下面slot7/8修正的repair命中訊息，以及含0x7C原始位元組
// dump才會印出；LootMenuDiagCap（同節，300/500/1000三選一下拉選單）是各自
// 的印出行數上限，避免掃過整個選單畫面時洗版obCJK.log。同款寫法沿用
// obCJK_GlyphHook.h ObCJKPathDiagEnabled()/ObCJKPathDiagCap()（Path A/B/C
// 診斷log）。
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
// call rather than at plugin Load() time, since OBSE plugin load order
// relative to other plugins' DLL mapping isn't a guarantee this file wants
// to depend on — by the time any Tile SetText actually fires (real gameplay
// UI activity), every plugin DLL that's going to load already has.
static int g_lootMenuDllState = -1;

static inline bool ObCJKLootMenuDllPresent()
{
    if (g_lootMenuDllState < 0)
        g_lootMenuDllState = GetModuleHandleA("lootmenu.dll") ? 1 : 0;
    return g_lootMenuDllState == 1;
}

// Reused every call — safe because sub_58CA50 copies this string's content
// into its own internal storage (via sub_4028D0) before it returns, so the
// repaired buffer only needs to survive for the duration of this one call,
// not beyond it. Sized generously for item/list display names.
static char g_lootMenuFixBuf[512];

// __cdecl, called from the naked hook below with (this, a2) pushed in that
// order — returns the pointer sub_58CA50's replayed prologue should use in
// place of the original a2: unchanged unless a repair was actually applied.
// Walks the string as a sequence of single-byte / Big5-pair characters from
// the start — exactly the same well-formed-content assumption every other
// byte classifier in this project makes (ObCJKIsLeadByte/ObCJKIsTrailByte).
// Returns true only if the walk itself lands on a lead byte with nothing
// after it. Checking only the *last* byte (this function's original
// version) is wrong: ObCJKIsTrailByte's legal range (0x40~0xFE) entirely
// overlaps ObCJKIsLeadByte's range (0x81~0xFE), so any complete, correct
// string whose final character's trail byte happens to land in 0x81~0xFE
// (extremely common — e.g. "不" = Big5 A4 A3) was being misdetected as
// truncated and got a spurious 0x7C appended, corrupting otherwise-correct
// text on menus that were never actually bugged. The last-byte-only check
// fired on nearly every single call (hit rate close to 100%), which is only
// possible if it was matching complete strings, not real truncations — see
// obcjk_bug_garbled_lootmenu_slot78 memory, 續九.
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

    // "疊起的布料" arrived
    // as just "疊" (Big5 C5 7C — a complete, well-formed pair whose own
    // trail byte happens to BE 0x7C) with everything after it already gone
    // before this function was ever called. A complete pair has no orphan
    // lead byte for ObCJKLootMenuEndsInOrphanLead to catch, so this class of
    // loss is invisible to the hit-log below — log the raw bytes of any
    // string containing 0x7C anywhere (not just trailing) to see what
    // sub_58CA50 actually receives for these. Low volume: most strings
    // never contain 0x7C at all.
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

// thiscall entry: ecx=this, [esp+4]=a2, exactly as the CPU state is at the
// real function's first instruction (this hook patches VA 0x58CA50 itself,
// not a mid-function point, so nothing has touched these yet). Overwrites
// the a2 slot on the stack in place so the trampoline's replayed original
// prologue reads the repaired pointer when it eventually loads arg_4 — no
// different than if the caller had passed that pointer to begin with.
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

    // Gated by [obCJK] LootMenuEnable (default 1, same "<Feature>Enable"
    // convention as TexSwapEnable/AsciiRenderEnable), AND by MenuQueEnable —
    // obCJK_iniEdit.py only lets the user turn LootMenuEnable's slider on
    // when MenuQueEnable=1, so this hook checks both instead of trusting the
    // ini to always agree if hand-edited.
    if (GetPrivateProfileIntA("obCJK", "MenuQueEnable", 1, k_iniMain) == 0 ||
        GetPrivateProfileIntA("obCJK", "LootMenuEnable", 1, k_iniMain) == 0) {
        _MESSAGE("obCJK:LootMenuFix: disabled via ini (MenuQueEnable/LootMenuEnable=0), hook not installed");
        return;
    }
    ObCJKInstallHook((BYTE*)kVA_LootMenuFix, (void*)ObCJKLootMenuFixHook, &g_lootMenuFixTramp, "LootMenuFix");
}
