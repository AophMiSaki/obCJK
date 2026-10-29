// main.cpp — obCJK OBSE Plugin
//
// Plugin entry point. OBSEPlugin_Load:
//   - creates Data\OBSE\Plugins\obCJK\obCJK.ini with defaults if missing,
//     reads DebugLogEnable / ActiveCodePage / hotkeys / IMEMode
//   - builds the kinsoku line-break table for the active codepage
//   - installs codepage-independent hooks (TexSwap, line-break marker,
//     Path A line height, Path C line floor)
//   - installs either the UTF-8 hook set (obCJK_*_UTF8.h) or the DBCS hook
//     set (TrailByteGuard/SmartQuoteGuard/TrailByteGuard2/LineSplitPair/
//     PreMeasure/WordWrap/NodeCopy/SecondLoop/LineBreak/Glyph)
//   - installs LootMenu and save/save-list/delete-save fixes unconditionally
// kMessage_GameInitialized installs the hooks that depend on other plugins
// being loaded (MenuQue delimiter hook, NorthernUI font bridge), registers
// PerFrameTask and subclasses the game window for cursor restore.
// PerFrameTask polls the IME and editor hotkeys (keyboard / gamepad hold /
// gamepad combo), launches the ini editor, and after it closes reloads
// settings and resets the glyph atlas if a font parameter changed.

#include <windows.h>
#include <shellapi.h>
#include "common/IDebugLog.h"
#include "obse/PluginAPI.h"
#include "include/obCJK_Path.h"
#include "include/obCJK_Gamepad.h"
#include "include/obCJK_Encoding.h"
#include "include/obCJK_LineBreakRule.h"
#include "include/obCJK_IME_Out.h"
#include "include/obCJK_IME_In.h"
#include "include/obCJK_WordWrapHook.h"
#include "include/obCJK_LineBreakHook.h"
#include "include/obCJK_LineFloorHook.h"
#include "include/obCJK_GlyphHook.h"
#include "include/obCJK_NorthernUICompat.h"  // NorthernUI xxnFontPath bridge
#include "include/obCJK_WordWrapHook_UTF8.h"
#include "include/obCJK_LineBreakRule_UTF8.h"
#include "include/obCJK_LineBreakHook_UTF8.h"
#include "include/obCJK_GlyphHook_UTF8.h"
#include "include/obCJK_TexSwap.h"
#include "include/obCJK_TexUpload.h"  // glyph placement into the TexSwap-enlarged native font texture
#include "include/obCJK_LootMenuTrailByteFixHook.h"  // LootMenu-only 0x7C trail-byte repair
#include "include/obCJK_MenuQueDelimHook.h"  // MenuQue tokenizer CJK-pair-aware delimiter scan (DBCS-only)
#include "include/obCJK_SaveNameTruncateHook.h"  // manual-save MAX_PATH silent-clear fix
#include "include/obCJK_CreateFileWShim.h"  // manual-save CJK-name encoding-mismatch fix at the CRT's CreateFileA call site
#include "include/obCJK_SaveListFindShim.h"  // save-list scanner (sub_45D450) FindFirstFileA/FindNextFileA CJK-name corruption fix
#include "include/obCJK_SaveReturnFixHook.h"  // manual-save "false success" fix (sub_465130 no longer unconditionally returns 1)
#include "include/obCJK_DeleteFileWShim.h"  // delete-save CJK-name false-success fix at sub_453480's entry

#define OBTCADD_VERSION 1

IDebugLog    gLog("obCJK.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;

static OBSEConsoleInterface* g_console    = nullptr;
static OBSEInputInterface*   g_input      = nullptr;
static OBSETasks2Interface*  g_tasks2     = nullptr;
// 保底預設值：只有鍵盤binding啟用，長按/組合鍵停用，comboModifierButton=4(LB)。
static HotkeySettings        g_hotkeySettings = { DIK_F12, 1, false, 0, 1.0f, true, 0, 0, 4, true, false };
static bool                  g_keyWasDown = false;

static HWND    g_gameHWND       = NULL;
static WNDPROC g_origWndProc    = NULL;
static HANDLE  g_editorProcess  = NULL;

// Stashed at Load for the NorthernUI listener registration in MessageHandler.
static OBSEMessagingInterface* g_msgIntfc = nullptr;

// Which IME implementation reacts to ImeHotkey — selected via ini [obCJK] IMEMode.
enum ObCJKImeMode { kImeModeOut = 0, kImeModeIn = 1 };
static ObCJKImeMode g_imeMode         = kImeModeOut;
static bool         g_imeInKeyWasDown = false;


static void ObCJKEnsureMainIni()
{
    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    _MESSAGE("obCJK:main:ObCJKEnsureMainIni: cwd=[%s] iniPath=[%s]", cwd, k_iniMain);

    BOOL dirOk = CreateDirectoryA("Data\\OBSE\\Plugins\\obCJK", NULL);
    _MESSAGE("obCJK:main:ObCJKEnsureMainIni: CreateDirectory result=%d err=%lu", dirOk, GetLastError());

    if (GetFileAttributesA(k_iniMain) != INVALID_FILE_ATTRIBUTES) {
        _MESSAGE("obCJK:main:ObCJKEnsureMainIni: ini already exists, skip");
        return;
    }

    // Create with defaults (hotkey codes as hex strings): EditorHotkey=Ctrl+F12,
    // ImeHotkey=F11, keyboard bindings enabled, hold/combo bindings disabled.
    BOOL w1  = WritePrivateProfileStringA("obCJK", "ActiveCodePage", "UTF8", k_iniMain);
    BOOL w2  = WritePrivateProfileStringA("obCJK", "IMEMode",        "Out",  k_iniMain);
    BOOL w3  = WritePrivateProfileStringA("obCJK", "EditorHotkeyKbdCode",     "0x58",  k_iniMain);
    BOOL w4  = WritePrivateProfileStringA("obCJK", "EditorHotkeyKbdModifier", "Ctrl",  k_iniMain);
    BOOL w5  = WritePrivateProfileStringA("obCJK", "EditorHotkeyKbdDisabled", "0",     k_iniMain);
    BOOL w6  = WritePrivateProfileStringA("obCJK", "EditorHotkeyHoldDisabled",  "1",   k_iniMain);
    BOOL w7  = WritePrivateProfileStringA("obCJK", "EditorHotkeyComboDisabled", "1",   k_iniMain);
    BOOL w8  = WritePrivateProfileStringA("obCJK", "EditorHotkeyAllDisabled",   "0",   k_iniMain);
    BOOL w9  = WritePrivateProfileStringA("obCJK", "ImeHotkeyKbdCode",     "0x57",  k_iniMain);
    BOOL w10 = WritePrivateProfileStringA("obCJK", "ImeHotkeyKbdModifier", "None",  k_iniMain);
    BOOL w11 = WritePrivateProfileStringA("obCJK", "ImeHotkeyKbdDisabled", "0",     k_iniMain);
    BOOL w12 = WritePrivateProfileStringA("obCJK", "ImeHotkeyHoldDisabled",  "1",   k_iniMain);
    BOOL w13 = WritePrivateProfileStringA("obCJK", "ImeHotkeyComboDisabled", "1",   k_iniMain);
    BOOL w14 = WritePrivateProfileStringA("obCJK", "ImeHotkeyAllDisabled",   "0",   k_iniMain);
    _MESSAGE("obCJK:main:ObCJKEnsureMainIni: write results=%d%d%d%d%d%d%d%d%d%d%d%d%d%d err=%lu",
             w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, GetLastError());
}

static LRESULT CALLBACK ObCJKWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_SETFOCUS && g_cursorReleased) {
        ShowCursor(FALSE);
        g_cursorReleased = false;
    }
    return CallWindowProcA(g_origWndProc, hWnd, msg, wParam, lParam);
}

static bool IsKeyDown(UInt16 dxScanCode)
{
    if (g_input && g_input->IsKeyPressedReal(dxScanCode))
        return true;
    // Fallback: IsKeyPressedReal relies on InputPollFakeHandle which is skipped in some
    // game states (e.g. main menu). Use GetAsyncKeyState so hotkeys work everywhere.
    UINT vk = MapVirtualKeyA(dxScanCode, MAPVK_VSC_TO_VK);
    return vk && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
}

// modifier: 0=None (no requirement), 1=Ctrl, 2=Shift. VK_CONTROL/VK_SHIFT
// match either the left or right physical key, so no L/R split needed.
static bool IsModifierDown(UInt8 modifier)
{
    switch (modifier) {
        case 1:  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        case 2:  return (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
        default: return true;
    }
}

// 鍵盤/手把長按/手把組合鍵 3 組binding任一組「未停用且條件成立」就觸發。
// allDisabled最優先，開啟時整組熱鍵不觸發。
// 手把只查joystick index 0，且受 [obCJK] GamepadHotkeyEnable 總開關控制；
// 鍵盤binding不受此開關影響。
// GamepadCombo: 修飾鍵+主鍵同時按住。
// GamepadHold: 單一按鍵連續按住超過hk.holdSeconds秒。
static bool IsHotkeyDown(HotkeySettings& hk)
{
    if (hk.allDisabled)
        return false;

    if (!hk.kbdDisabled && IsModifierDown(hk.kbdModifier) && IsKeyDown(hk.kbdCode))
        return true;

    if (ObCJKGamepadHotkeyEnabled()) {
        if (!hk.comboDisabled
            && IsGamepadButtonDown(0, hk.comboModifierButton)
            && IsGamepadButtonDown(0, hk.comboButton))
            return true;
        if (!hk.holdDisabled
            && IsGamepadButtonHeldFor(0, hk.holdButton, hk.holdSeconds, hk.holdStartTick))
            return true;
    }

    return false;
}

static bool LaunchProcess(const char* exePath, const char* pyPath,
                          HANDLE* outProcess, const char* params = nullptr)
{
    SHELLEXECUTEINFOA sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = "open";
    sei.nShow        = SW_SHOW;
    sei.lpParameters = params;

    if (GetFileAttributesA(exePath) != INVALID_FILE_ATTRIBUTES)
        sei.lpFile = exePath;
    else if (GetFileAttributesA(pyPath) != INVALID_FILE_ATTRIBUTES)
        sei.lpFile = pyPath;
    else
        return false;

    if (!ShellExecuteExA(&sei))
        return false;

    if (*outProcess) CloseHandle(*outProcess);
    *outProcess = sei.hProcess;
    return true;
}

// Reads+deletes the editor's changed-fields file (font parameters only) and
// logs each line. Returns true iff any font parameter changed.
static bool ObCJKLogChanges()
{
    HANDLE hFile = CreateFileA(k_changesPath, GENERIC_READ, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    char buf[4096] = {};
    DWORD bytesRead = 0;
    ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, NULL);
    CloseHandle(hFile);
    DeleteFileA(k_changesPath);
    if (bytesRead == 0) return false;
    buf[bytesRead] = '\0';
    char* p = buf;
    while (*p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p) _MESSAGE("obCJK:main:ObCJKLogChanges:   changed: %s", p);
        if (!nl) break;
        p = nl + 1;
    }
    return true;
}

// [obCJK] DebugLogEnable sets obCJK.log's log level: 0 = kLevel_Message
// (hook install results, warnings), 1 = kLevel_VerboseMessage (also writes
// _VMESSAGE diagnostic dumps). Called at load and after the editor saves.
static void ObCJKLoadDebugLogSetting()
{
    int enabled = GetPrivateProfileIntA("obCJK", "DebugLogEnable", 0, k_iniMain);
    IDebugLog::LogLevel level = enabled ? IDebugLog::kLevel_VerboseMessage : IDebugLog::kLevel_Message;
    gLog.SetLogLevel(level);
    gLog.SetPrintLevel(level);
}

// 讀單一hk(Editor或Ime)的3組binding+總開關。keyPrefix如"EditorHotkey"/
// "ImeHotkey"；defaultKbdCode/defaultKbdModifier是鍵盤binding缺key時的預設值；
// 手把長按/組合鍵/總開關的預設值兩組共用，寫在函式內。
static void ObCJKLoadHotkeyBinding(HotkeySettings& hk, const char* keyPrefix,
                                    UInt16 defaultKbdCode, UInt8 defaultKbdModifier)
{
    char iniKey[48];
    char buf[32];

    wsprintfA(iniKey, "%sKbdCode", keyPrefix);
    GetPrivateProfileStringA("obCJK", iniKey, "", buf, sizeof(buf), k_iniMain);
    hk.kbdCode = ParseHotkey(buf, defaultKbdCode);
    wsprintfA(iniKey, "%sKbdModifier", keyPrefix);
    GetPrivateProfileStringA("obCJK", iniKey, "", buf, sizeof(buf), k_iniMain);
    hk.kbdModifier = ParseHotkeyModifier(buf, defaultKbdModifier);
    wsprintfA(iniKey, "%sKbdDisabled", keyPrefix);
    hk.kbdDisabled = GetPrivateProfileIntA("obCJK", iniKey, 0, k_iniMain) != 0;

    wsprintfA(iniKey, "%sHoldButton", keyPrefix);
    GetPrivateProfileStringA("obCJK", iniKey, "0", buf, sizeof(buf), k_iniMain);
    hk.holdButton = ParseGamepadButton(buf, 0);
    wsprintfA(iniKey, "%sHoldSeconds", keyPrefix);
    GetPrivateProfileStringA("obCJK", iniKey, "1.0", buf, sizeof(buf), k_iniMain);
    hk.holdSeconds = ParseHotkeyHoldSeconds(buf, 1.0f);
    wsprintfA(iniKey, "%sHoldDisabled", keyPrefix);
    hk.holdDisabled = GetPrivateProfileIntA("obCJK", iniKey, 1, k_iniMain) != 0;

    wsprintfA(iniKey, "%sComboButton", keyPrefix);
    GetPrivateProfileStringA("obCJK", iniKey, "0", buf, sizeof(buf), k_iniMain);
    hk.comboButton = ParseGamepadButton(buf, 0);
    wsprintfA(iniKey, "%sComboModifierButton", keyPrefix);
    GetPrivateProfileStringA("obCJK", iniKey, "4", buf, sizeof(buf), k_iniMain);
    hk.comboModifierButton = ParseGamepadButton(buf, 4);
    wsprintfA(iniKey, "%sComboDisabled", keyPrefix);
    hk.comboDisabled = GetPrivateProfileIntA("obCJK", iniKey, 1, k_iniMain) != 0;

    wsprintfA(iniKey, "%sAllDisabled", keyPrefix);
    hk.allDisabled = GetPrivateProfileIntA("obCJK", iniKey, 0, k_iniMain) != 0;
}

// Reads EditorHotkey*/ImeHotkey* and IMEMode from ini into the matching
// globals. Called at load and after the editor saves.
static void ObCJKLoadHotkeySettings()
{
    ObCJKLoadHotkeyBinding(g_hotkeySettings,    "EditorHotkey", DIK_F12, 1);
    ObCJKLoadHotkeyBinding(g_imeHotkeySettings, "ImeHotkey",    DIK_F11, 0);

    char modeBuf[16] = "Out";
    GetPrivateProfileStringA("obCJK", "IMEMode", "Out", modeBuf, sizeof(modeBuf), k_iniMain);
    g_imeMode = (_stricmp(modeBuf, "In") == 0) ? kImeModeIn : kImeModeOut;

    // Reset key-was-down latches so a stale latch from the previous binding
    // doesn't swallow the first press of the new one.
    g_keyWasDown      = false;
    g_imeInKeyWasDown = false;
}

static void PerFrameTask()
{
    static bool s_firstRun = true;
    if (s_firstRun) {
        _MESSAGE("obCJK:main:PerFrameTask: first run, editorHotkey=0x%02X(mod=%u,kbdDis=%u,holdDis=%u,comboDis=%u,allDis=%u) "
                 "imeHotkey=0x%02X(mod=%u,kbdDis=%u,holdDis=%u,comboDis=%u,allDis=%u) codePage=%s imeMode=%s",
                 g_hotkeySettings.kbdCode, g_hotkeySettings.kbdModifier,
                 g_hotkeySettings.kbdDisabled, g_hotkeySettings.holdDisabled,
                 g_hotkeySettings.comboDisabled, g_hotkeySettings.allDisabled,
                 g_imeHotkeySettings.kbdCode, g_imeHotkeySettings.kbdModifier,
                 g_imeHotkeySettings.kbdDisabled, g_imeHotkeySettings.holdDisabled,
                 g_imeHotkeySettings.comboDisabled, g_imeHotkeySettings.allDisabled,
                 ObCJKCodePageName(g_activeCodePage), g_imeMode == kImeModeIn ? "In" : "Out");
        s_firstRun = false;
    }

    // [obCJK] GamepadInputDiagEnable 手把輸入診斷，不受 GamepadHotkeyEnable 影響。
    ObCJKGamepadInputDiagTick();

    // IME: mode-gated so only one of the two systems reacts to a given hotkey press.
    bool imeKeyDown = IsHotkeyDown(g_imeHotkeySettings);
    if (g_imeMode == kImeModeIn)
        ObCJKImeInPerFrame(g_gameHWND, g_activeCodePage, imeKeyDown, g_imeInKeyWasDown);
    else
        ObCJKImePerFrame(g_activeCodePage, imeKeyDown, g_cursorReleased);

    // Editor closed with a save: reload settings, and reset the glyph atlas
    // if a font parameter changed.
    if (g_editorProcess != NULL &&
        WaitForSingleObject(g_editorProcess, 0) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(g_editorProcess, &exitCode);
        CloseHandle(g_editorProcess);
        g_editorProcess = NULL;

        if (exitCode == 1 || exitCode == 2) {
            ObCJKLoadHotkeySettings();
            ObCJKLoadDebugLogSetting();
            ObCJKTexSwapReloadEnableFlag();
            if (ObCJKLogChanges()) {
                // Fix: don't rewind the TexSwap shelf-pack cursor. Text that
                // isn't redrawn every frame keeps its old UV, and a new glyph
                // packed into that rect made it show the wrong character.
                ObCJKGlyphAtlas_Reset();
                _MESSAGE("obCJK:main:PerFrameTask: font parameter change detected, atlas reset for hot-reload");
            }
        }
    }

    bool editorDown = IsHotkeyDown(g_hotkeySettings);
    if (editorDown && !g_keyWasDown) {
        ClipCursor(NULL);
        while (ShowCursor(TRUE) < 0) {}
        g_cursorReleased = true;
        // --ingame: launched from the game; --extrafonts: MenuQue
        // (Submodule.Game.dll) is loaded, unlocks slot7/8 editing.
        const char* editorParams = GetModuleHandleA("Submodule.Game.dll")
            ? "--ingame --extrafonts" : "--ingame";
        if (!LaunchProcess(k_editorExe, k_editorPy, &g_editorProcess, editorParams))
            _WARNING("editor not found (%s, %s)", k_editorExe, k_editorPy);
    }
    g_keyWasDown = editorDown;
}

static void MessageHandler(OBSEMessagingInterface::Message* msg)
{
    if (msg->type == OBSEMessagingInterface::kMessage_GameInitialized) {
        // Installed here, not in OBSEPlugin_Load: MenuQue (Submodule.Game.dll)
        // is only guaranteed loaded by kMessage_GameInitialized. DBCS-only.
        if (g_activeCodePage != kCP_UTF8)
            ObCJKInstallMenuQueDelimHook();

        // Also needs other plugins loaded first.
        ObCJKNorthernUICompat_RegisterListener(g_msgIntfc, g_pluginHandle);

        if (g_tasks2)
            g_tasks2->EnqueueTask(PerFrameTask);

        g_gameHWND = FindWindowA("Oblivion", NULL);
        if (g_gameHWND) {
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(
                g_gameHWND, GWLP_WNDPROC, (LONG_PTR)ObCJKWndProc);
        } else {
            _WARNING("FindWindowA(Oblivion) failed, cursor restore disabled");
        }
    }
}

extern "C" {

bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
{
    _MESSAGE("obCJK:main:OBSEPlugin_Query: query");
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name        = "obCJK";
    info->version     = OBTCADD_VERSION;

    if (!obse->isEditor) {
        if (obse->obseVersion < OBSE_VERSION_INTEGER) {
            _ERROR("OBSE version too old (got %u expected at least %u)",
                   obse->obseVersion, OBSE_VERSION_INTEGER);
            return false;
        }
        if (obse->oblivionVersion != OBLIVION_VERSION) {
            _ERROR("incorrect Oblivion version (got %08X need %08X)",
                   obse->oblivionVersion, OBLIVION_VERSION);
        }
    }
    return true;
}

bool OBSEPlugin_Load(const OBSEInterface* obse)
{
    _MESSAGE("obCJK:main:OBSEPlugin_Load: load");
    ObCJKEnsureMainIni();
    ObCJKLoadDebugLogSetting();
    g_pluginHandle = obse->GetPluginHandle();

    g_console = (OBSEConsoleInterface*)obse->QueryInterface(kInterface_Console);
    g_input   = (OBSEInputInterface*)obse->QueryInterface(kInterface_Input);
    g_tasks2  = (OBSETasks2Interface*)obse->QueryInterface(kInterface_Tasks2);

    if (!g_console) _WARNING("OBSEConsoleInterface unavailable");
    if (!g_input)   _WARNING("OBSEInputInterface unavailable");
    if (!g_tasks2)  _WARNING("OBSETasks2Interface unavailable");

    // Read ActiveCodePage from ini to select which lead/trail-byte table obCJK uses.
    char cpBuf[16] = "UTF8";
    GetPrivateProfileStringA("obCJK", "ActiveCodePage", "UTF8",
                             cpBuf, sizeof(cpBuf), k_iniMain);
    g_activeCodePage = ObCJKCodePageFromString(cpBuf);
    _MESSAGE("obCJK:main:Load: ActiveCodePage=%s", ObCJKCodePageName(g_activeCodePage));

    // Parses NorthernUI's datastore.xml; no dependency on other plugins.
    ObCJKNorthernUICompat_Init();

    // Build the kinsoku (line-break prohibition) table for the active
    // codepage and log its entry count.
    if (g_activeCodePage == kCP_UTF8) {
        ObCJKBuildLineBreakTableUtf8();
        _MESSAGE("obCJK:main:Load: LineBreakRule table (UTF8) built, %d entries",
                 g_lineBreakCountUtf8);
    } else {
        ObCJKBuildLineBreakTable(g_activeCodePage);
        _MESSAGE("obCJK:main:Load: LineBreakRule table built for %s, %d entries",
                 ObCJKCodePageName(g_activeCodePage), g_lineBreakCount);
    }

    // TexSwap enlarges each font's native .tex at load; must install before
    // the glyph hooks, which place CJK glyphs into that enlarged texture.
    ObCJKInstallTexSwapHook();

    // Codepage-independent hooks/patches:
    // Path A (sub_575B40) forced-line-break marker byte ('-' <-> ' ').
    ObCJKApplyLineBreakSpaceSettingPathA();
    // Path B/C (sub_5772A0) forced-split hyphen marker byte.
    ObCJKApplyLineBreakSpaceSettingPathBC();
    // Path A per-line height fix (sub_576670 VA 0x5768C4).
    ObCJKInstallPathALineHeightHook();
    // Path C line-height floor (sub_5772A0's dword_B12E08 reads).
    ObCJKInstallLineFloorHooks();

    if (g_activeCodePage == kCP_UTF8) {
        // UTF-8 branch. No TrailByteGuard/TrailByteGuard2 counterpart: UTF-8
        // continuation bytes (0x80-0xBF) never equal 0x7E ('~').
        ObCJKInstallSmartQuoteGuardHookUtf8();
        ObCJKInstallPreMeasureHookUtf8();
        ObCJKInstallLineSplitPairHookUtf8();
        ObCJKInstallWordWrapHookUtf8();
        ObCJKInstallNodeCopyHookUtf8();
        ObCJKInstallSecondLoopHookUtf8();
        ObCJKInstallLineBreakHookUtf8();
        ObCJKInstallGlyphHooksUtf8();
    } else {
        // DBCS branch.
        // TrailByteGuard: sub_575B40 VA 0x576226 strips a 0x7E trail byte
        // as the '~' hotkey marker.
        ObCJKInstallTrailByteGuardHook();
        // SmartQuoteGuard: sub_575B40 VA 0x575F8A replaces lead bytes
        // 0x91-0x94 with ASCII '/".
        ObCJKInstallSmartQuoteGuardHook();
        // TrailByteGuard2: sub_575B40 VA 0x575FFB marks a 0x7E trail byte
        // as a hyphenation point.
        ObCJKInstallTrailByteGuard2Hook();
        // LineSplitPair: sub_575B40 VA 0x57616E forced break splits a CJK
        // lead/trail pair.
        ObCJKInstallLineSplitPairHook();
        // PreMeasure: sub_575B40 VA 0x575FAE per-byte width feeding
        // CENTER/RIGHT alignment (menu button text not centered).
        ObCJKInstallPreMeasureHook();
        // WordWrap: CJK pair advance + trail byte stash in node+0x05;
        // NodeCopy: carries it to the permanent tree node;
        // SecondLoop: same fix for sub_578960's second per-character loop.
        ObCJKInstallWordWrapHook();
        ObCJKInstallNodeCopyHook();
        ObCJKInstallSecondLoopHook();
        // Applies the kinsoku table to the wrapwidth-overflow decision
        // (sub_577840 VA 0x577946).
        ObCJKInstallLineBreakHook();
        // Path A/B/C: replace the native glyph pointer with a CJK glyph
        // placed in the TexSwap-enlarged native font texture.
        ObCJKInstallGlyphHooks();
    }

    // LootMenu slot7/8 off-by-one garble fix (only when lootmenu.dll is
    // loaded). Installed for both codepages; no-op under UTF-8.
    ObCJKInstallLootMenuTrailByteFixHook();

    // Manual-save "success but no file written" (MAX_PATH) fix.
    ObCJKInstallSavePathFixHook();
    // Diagnostic: logs sub_465130 call arguments (SaveDiagEnable=1).
    ObCJKInstallSaveEntryDiagHook();

    // Manual-save CJK-name encoding-mismatch fix + "false success" fix.
    ObCJKInstallCreateFileWShim();
    ObCJKInstallSaveReturnFixHook();
    // Save-list scanner (sub_45D450) FindFirstFileA/FindNextFileA CJK-name
    // corruption fix.
    ObCJKInstallSaveListFindShim();

    // Delete-save CJK-name false-success fix (.ess + .obse).
    ObCJKInstallDeleteSaveFixHook();

    ObCJKLoadHotkeySettings();

    OBSEMessagingInterface* msgIntfc =
        (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging);
    if (msgIntfc)
        msgIntfc->RegisterListener(g_pluginHandle, "OBSE", MessageHandler);
    g_msgIntfc = msgIntfc;

    return true;
}

} // extern "C"
