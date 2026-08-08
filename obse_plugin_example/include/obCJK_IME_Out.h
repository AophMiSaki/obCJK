#pragma once
// External-window IME path: hotkey launches an external Python/exe window for
// text entry; result is read back from a temp file and injected once the
// process exits. Counterpart to obCJK_IME_In.h (native Windows IME, no popup).
#include <windows.h>
#include <shellapi.h>
#include "common/IDebugLog.h"
#include "obCJK_Path.h"
#include "obCJK_Encoding.h"

// NiTArray header needed to walk g_TileMenuArray
struct ObCJKNiTArray {
    void*  vtbl;
    void** data;
    WORD   capacity;
    WORD   firstFree;
    WORD   numObjs;
};

// ── IME state ────────────────────────────────────────────────────────────────
static bool    g_imeKeyWasDown  = false;
static HANDLE  g_imeProcess     = NULL;
static char    g_pendingInsert[512] = {};
static DWORD   g_injectCountdown = 0;
static DWORD   g_injectByteCount = 0;

// Inject encoded text into the active name-input tile via HandleKeyboardInput (vtable 12).
//   Path A — TextEdit    (0x41B): HandleKeyboardInput per byte,
//             fallback to UpdateField on TextEditMenu::text tile (vtable 5, Menu+0x28)
//   Path B — RaceSex     (0x40C): HandleKeyboardInput per byte
//   Path C — Alchemy     (0x410): HandleKeyboardInput per byte
//   Path D — Enchantment (0x412): HandleKeyboardInput per byte
//   Path E — Spellmaking (0x411): HandleKeyboardInput per byte
//   Path F — SigilStone  (0x418): HandleKeyboardInput per byte
// g_TileMenuArray = NiTArray<TileMenu*> at 0x00B13970; TileMenu::menu at TileMenu+0x44
static void InjectIntoTextEdit(const char* text, DWORD byteCount)
{
    static const DWORD kTileMenuArrayAddr = 0x00B13970;
    static const DWORD kMenuType_Message  = 0x3E9;
    static const DWORD kMenuType_TextEdit = 0x41B;
    static const DWORD kTileValue_string  = 0x0FDE;

    typedef DWORD (__thiscall *UpdateFieldFn)(void*, DWORD, float, const char*);
    typedef bool  (__thiscall *HandleKeyFn)(void*, char);

    ObCJKNiTArray* arr = (ObCJKNiTArray*)kTileMenuArrayAddr;
    if (!arr->data) return;

    static const DWORD kMenuTypes[] = { 0x41B, 0x40C, 0x410, 0x412, 0x411, 0x418 };
    for (DWORD m = 0; m < 6; m++) {
        DWORD idx = kMenuTypes[m] - kMenuType_Message;
        if (idx >= (DWORD)arr->capacity || !arr->data[idx]) continue;

        BYTE* tileMenu = (BYTE*)arr->data[idx];
        BYTE* menu     = *(BYTE**)(tileMenu + 0x44);
        if (!menu) return;

        HandleKeyFn hkFn = (HandleKeyFn)(*(void***)menu)[12];
        DWORD handled = 0;
        for (DWORD i = 0; i < byteCount; i++)
            if (hkFn(menu, (char)(unsigned char)text[i])) handled++;

        // Path A fallback: if HandleKeyboardInput rejected all bytes, write via UpdateField
        if (handled == 0 && kMenuTypes[m] == kMenuType_TextEdit) {
            BYTE* tile = *(BYTE**)(menu + 0x28);  // TextEditMenu::text
            if (tile)
                ((UpdateFieldFn)(*(void***)tile)[5])(tile, kTileValue_string, 0.0f, text);
        }
        return;
    }
}

// Strips Tab / Windows-illegal filename symbols / control bytes from an
// already-encoded buffer (Big5/GBK/SJIS/Korean or UTF-8) without splitting a
// multi-byte sequence apart — a DBCS trail byte or UTF-8 continuation byte
// can land on the same numeric value as one of these ASCII symbols (e.g.
// Big5/GBK trail bytes span 0x40-0xFE, which covers '\' 0x5C and '|' 0x7C),
// so a raw per-byte strip would tear a CJK character in half and corrupt
// everything after it. Defense-in-depth for the paste-into-game step: keeps
// bad bytes out even if something upstream (clipboard paste into the Python
// entry, a hand-edited result file, etc.) let them through.
static DWORD ObCJKImeOutStripForbidden(char* buf, DWORD len, ObCJKCodePage cp)
{
    DWORD outLen = 0;
    DWORD i = 0;
    while (i < len) {
        BYTE b = (BYTE)buf[i];
        DWORD seqLen = 1;
        if (cp == kCP_UTF8) {
            int sl = ObCJKUtf8SeqLen(b);
            if (sl >= 2 && i + (DWORD)sl <= len) seqLen = (DWORD)sl;
        } else if (ObCJKIsLeadByte(b, cp) && i + 1 < len) {
            seqLen = 2;
        }
        if (seqLen == 1 && ObCJKIsForbiddenFilenameChar(b)) {
            i += 1;
            continue;  // drop this single byte, never a multi-byte sequence
        }
        for (DWORD k = 0; k < seqLen; k++)
            buf[outLen++] = buf[i + k];
        i += seqLen;
    }
    return outLen;
}

// Per-frame IME handler: inject countdown, process result reading, hotkey launch.
// Call from PerFrameTask as: ObCJKImePerFrame(g_activeCodePage, IsKeyDown(g_imeHotkey), g_cursorReleased)
static void ObCJKImePerFrame(ObCJKCodePage cp, bool imeKeyDown, bool& cursorReleased)
{
    // ── inject countdown ─────────────────────────────────────────────────────
    if (g_injectCountdown > 0) {
        g_injectCountdown--;
        if (g_injectCountdown == 0 && g_injectByteCount > 0) {
            g_pendingInsert[g_injectByteCount] = '\0';
            _MESSAGE("obCJK:IME_Out:PerFrame: injecting \"%s\" (%lu bytes)", g_pendingInsert, g_injectByteCount);
            InjectIntoTextEdit(g_pendingInsert, g_injectByteCount);
            g_injectByteCount = 0;
        }
    }

    // ── read result after Python IME closes ──────────────────────────────────
    if (g_imeProcess != NULL && WaitForSingleObject(g_imeProcess, 0) == WAIT_OBJECT_0) {
        DWORD bytesRead = 0;
        HANDLE hFile = CreateFileA(k_imeResultPath, GENERIC_READ, 0, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        bool fileOpened = (hFile != INVALID_HANDLE_VALUE);
        if (fileOpened) {
            ReadFile(hFile, g_pendingInsert, sizeof(g_pendingInsert) - 1, &bytesRead, NULL);
            bytesRead = ObCJKImeOutStripForbidden(g_pendingInsert, bytesRead, cp);
            g_pendingInsert[bytesRead] = '\0';
            CloseHandle(hFile);
            DeleteFileA(k_imeResultPath);
        }
        _MESSAGE("obCJK:IME_Out:PerFrame: external IME process exited, resultFile=%s bytesRead=%lu",
                  fileOpened ? "opened" : "missing", bytesRead);
        if (bytesRead > 0) {
            g_injectByteCount = bytesRead;
            g_injectCountdown = 10;
        }
        CloseHandle(g_imeProcess);
        g_imeProcess = NULL;
    }

    // ── hotkey ───────────────────────────────────────────────────────────────
    if (imeKeyDown && !g_imeKeyWasDown) {
        ClipCursor(NULL);
        while (ShowCursor(TRUE) < 0) {}
        cursorReleased = true;

        const char* imeFile = nullptr;
        if (GetFileAttributesA(k_imeInputExe) != INVALID_FILE_ATTRIBUTES)
            imeFile = k_imeInputExe;
        else if (GetFileAttributesA(k_imeInputPy) != INVALID_FILE_ATTRIBUTES)
            imeFile = k_imeInputPy;

        if (!imeFile) {
            _WARNING("IME input not found (%s, %s)", k_imeInputExe, k_imeInputPy);
        } else {
            SHELLEXECUTEINFOA sei = {};
            sei.cbSize       = sizeof(sei);
            sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb       = "open";
            sei.lpFile       = imeFile;
            sei.lpParameters = ObCJKCodePagePyEnc(cp);
            sei.nShow        = SW_SHOW;
            if (ShellExecuteExA(&sei)) {
                if (g_imeProcess) CloseHandle(g_imeProcess);
                g_imeProcess = sei.hProcess;
                _MESSAGE("obCJK:IME_Out:PerFrame: launched %s (codepage=%s)", imeFile, sei.lpParameters);
            } else {
                _WARNING("obCJK:IME_Out:PerFrame: ShellExecuteExA failed file=%s err=%lu", imeFile, GetLastError());
            }
        }
    }
    g_imeKeyWasDown = imeKeyDown;
}
