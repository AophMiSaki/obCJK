// obCJK_LineFloorHook.h
// Path C line-height takeover (encoding-independent), two parts:
//
// 1. Line-height floor. sub_5772A0 seeds each line's height (line+0x18) from
//    the native constant table dword_B12E08[fontID] (35,35,30,60,60), which
//    keeps lines from shrinking when the CJK font is smaller. The table is
//    read at 5 sites, each a 7-byte `mov reg, [idx*4 + 0x00B12E08]`; every
//    site is replaced by `call stub` + 2 nops. The stub returns
//    ObCJKLineHeightForFont(fontID) when obCJK takes over that font,
//    otherwise the original table value (out-of-table IDs and slot4 behave
//    exactly as native).
//
// 2. Node height. sub_577120 sets every non-image node's height (node+0x28)
//    to round(fnt.fontSize), so ASCII letters and spaces (not rewritten by
//    any glyph hook) still feed that constant into sub_5772A0's per-line max.
//    The store at 0x57718F is replaced by a call that writes the font's line
//    height instead when obCJK takes the font over.
//
// All sites are byte-verified before any is patched.
#pragma once

#include <windows.h>
#include "common/IDebugLog.h"
#include "obCJK_HookUtil.h"
#include "obCJK_GlyphAtlas.h"

static const DWORD kVA_NativeLineFloorTable = 0x00B12E08;

// Value the patched `mov` should produce for fontID.
static int __cdecl ObCJKLineFloorFor(int fontID)
{
    if (fontID >= 0 && fontID < kObCJKMaxFontID && !ObCJKIsFontIDExcluded(fontID)) {
        int lh = ObCJKLineHeightForFont(fontID);
        if (lh > 0) return lh;
    }
    return *(int*)(kVA_NativeLineFloorTable + (DWORD)fontID * 4);
}

// Each stub: index in <idx>, result written to <tgt>, every other register and
// the flags preserved. Offsets are into the pushad frame:
// edi 0, esi 4, ebp 8, esp 12, ebx 16, edx 20, ecx 24, eax 28.
#define OBCJK_FLOOR_STUB(name, idxOff, tgtOff)          \
static __declspec(naked) void name()                    \
{                                                       \
    __asm { pushfd }                                    \
    __asm { pushad }                                    \
    __asm { push dword ptr [esp + idxOff] }             \
    __asm { call ObCJKLineFloorFor }                    \
    __asm { add esp, 4 }                                \
    __asm { mov dword ptr [esp + tgtOff], eax }         \
    __asm { popad }                                     \
    __asm { popfd }                                     \
    __asm { ret }                                       \
}

OBCJK_FLOOR_STUB(ObCJKLineFloorStub_eax_from_edx, 20, 28)  // 0x577362
OBCJK_FLOOR_STUB(ObCJKLineFloorStub_ecx_from_eax, 28, 24)  // 0x577401
OBCJK_FLOOR_STUB(ObCJKLineFloorStub_ecx_from_edx, 20, 24)  // 0x577534, 0x577545
OBCJK_FLOOR_STUB(ObCJKLineFloorStub_ebx_from_ebx, 16, 16)  // 0x577572

// Node height (node+0x28) takeover. The 5 bytes at 0x57718F
// (`mov edi,[esi]` + `mov [esi+28h],eax`) become a call to the stub below:
// same two effects, but eax is replaced by the font's line height when obCJK
// takes the font over. nativeH is eax at that point.
static const DWORD kVA_NodeHeightStore = 0x0057718F;

static int __cdecl ObCJKNodeBaseHeightFor(int fontID, int nativeH)
{
    if (fontID >= 0 && fontID < kObCJKMaxFontID && !ObCJKIsFontIDExcluded(fontID)) {
        int lh = ObCJKLineHeightForFont(fontID);
        if (lh > 0) return lh;
    }
    return nativeH;
}

// In: esi = node, eax = native height. Out: edi = [esi], [esi+28h] = adjusted
// height, eax = adjusted height; ecx/edx/flags preserved.
static __declspec(naked) void ObCJKNodeHeightStub()
{
    __asm {
        pushfd
        push ecx
        push edx
        mov  edi, dword ptr [esi]
        push eax
        push edi
        call ObCJKNodeBaseHeightFor
        add  esp, 8
        pop  edx
        pop  ecx
        popfd
        mov  dword ptr [esi + 0x28], eax
        ret
    }
}

struct ObCJKLineFloorSite {
    DWORD va;
    BYTE  expect[7];   // original `mov reg, [idx*4+0xB12E08]`
    void* stub;
};

static void ObCJKInstallLineFloorHooks()
{
    static bool installed = false;
    if (installed) return;

    const ObCJKLineFloorSite sites[] = {
        { 0x00577362, { 0x8B, 0x04, 0x95, 0x08, 0x2E, 0xB1, 0x00 }, (void*)ObCJKLineFloorStub_eax_from_edx },
        { 0x00577401, { 0x8B, 0x0C, 0x85, 0x08, 0x2E, 0xB1, 0x00 }, (void*)ObCJKLineFloorStub_ecx_from_eax },
        { 0x00577534, { 0x8B, 0x0C, 0x95, 0x08, 0x2E, 0xB1, 0x00 }, (void*)ObCJKLineFloorStub_ecx_from_edx },
        { 0x00577545, { 0x8B, 0x0C, 0x95, 0x08, 0x2E, 0xB1, 0x00 }, (void*)ObCJKLineFloorStub_ecx_from_edx },
        { 0x00577572, { 0x8B, 0x1C, 0x9D, 0x08, 0x2E, 0xB1, 0x00 }, (void*)ObCJKLineFloorStub_ebx_from_ebx },
    };
    const int n = (int)(sizeof(sites) / sizeof(sites[0]));

    // Verify all sites first so nothing is half-patched.
    static const BYTE kNodeHeightExpect[5] = { 0x8B, 0x3E, 0x89, 0x46, 0x28 };
    for (int i = 0; i < n; i++) {
        if (memcmp((void*)sites[i].va, sites[i].expect, 7) != 0) {
            _WARNING("obCJK:LineFloor: bytes at 0x%08X do not match the expected mov — not installing any site", sites[i].va);
            return;
        }
    }
    if (memcmp((void*)kVA_NodeHeightStore, kNodeHeightExpect, 5) != 0) {
        _WARNING("obCJK:LineFloor: bytes at 0x%08X do not match the expected node-height store — not installing any site", kVA_NodeHeightStore);
        return;
    }

    for (int i = 0; i < n; i++) {
        BYTE* p = (BYTE*)sites[i].va;
        DWORD oldProt = 0;
        VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE8;
        *(INT32*)(p + 1) = (INT32)((DWORD)sites[i].stub - (DWORD)(p + 5));
        p[5] = 0x90;
        p[6] = 0x90;
        VirtualProtect(p, 7, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 7);
    }
    {
        BYTE* p = (BYTE*)kVA_NodeHeightStore;
        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE8;
        *(INT32*)(p + 1) = (INT32)((DWORD)ObCJKNodeHeightStub - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    installed = true;
    _MESSAGE("obCJK:LineFloor: %d floor sites + node-height store patched (via ObCJKLineHeightForFont)", n);
}
