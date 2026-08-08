// main.cpp — obCJK OBSE Plugin

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
#include "include/obCJK_GlyphHook.h"
#include "include/obCJK_NorthernUICompat.h"  // NorthernUI xxnFontPath bridge, see its top comment
#include "include/obCJK_WordWrapHook_UTF8.h"
#include "include/obCJK_LineBreakRule_UTF8.h"
#include "include/obCJK_LineBreakHook_UTF8.h"
#include "include/obCJK_GlyphHook_UTF8.h"
#include "include/obCJK_TexSwap.h"
#include "include/obCJK_TexUpload.h"  // Plan B primitive only — not called yet, see its top comment
#include "include/obCJK_LootMenuTrailByteFixHook.h"  // LootMenu-only 0x7C trail-byte repair, see its top comment
#include "include/obCJK_MenuQueDelimHook.h"  // MenuQue tokenizer CJK-pair-aware delimiter scan, see its top comment; DBCS-only, see kCP_UTF8 gate at its call site
#include "include/obCJK_SaveNameTruncateHook.h"  // manual-save MAX_PATH silent-clear fix, see its top comment
#include "include/obCJK_CreateFileWShim.h"  // manual-save CJK-name encoding-mismatch fix, patched at the CRT's own CreateFileA call site (supersedes deleted obCJK_SaveWideOpenHook.h), see its top comment
#include "include/obCJK_SaveListFindShim.h"  // save-list scanner (sub_45D450) FindFirstFileA/FindNextFileA CJK-name corruption fix, see its top comment
#include "include/obCJK_SaveReturnFixHook.h"  // manual-save "false success" fix (sub_465130 no longer unconditionally returns 1), see its top comment
#include "include/obCJK_DeleteFileWShim.h"  // delete-save CJK-name false-success fix, patched at sub_453480's own DeleteFileA call site, see its top comment

#define OBTCADD_VERSION 1

IDebugLog    gLog("obCJK.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;

static OBSEConsoleInterface* g_console    = nullptr;
static OBSEInputInterface*   g_input      = nullptr;
static OBSETasks2Interface*  g_tasks2     = nullptr;
// comboModifierButton 預設4(LB)只在ini完全缺這個key時才用得到(理論上
// ensure_obcjk_section_defaults()一定會補寫，這裡只是保底)。長按/組合鍵
// 預設停用(disabled=true)，只有鍵盤binding預設啟用，跟obCJK_iniEdit.py
// _OBCJK_SECTION_DEFAULTS一致。
static HotkeySettings        g_hotkeySettings = { DIK_F12, 1, false, 0, 1.0f, true, 0, 0, 4, true, false };
static bool                  g_keyWasDown = false;

static HWND    g_gameHWND       = NULL;
static WNDPROC g_origWndProc    = NULL;
static HANDLE  g_editorProcess  = NULL;

// Stashed at Load so MessageHandler's kMessage_GameInitialized case can
// register the NorthernUI font-bridge listener (obCJK_NorthernUICompat.h) —
// OBSEPlugin_Load's own msgIntfc local goes out of scope before then.
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

    // Create with defaults — hotkey values as hex strings (strtoul-parseable).
    // Keys/defaults mirror obCJK_iniEdit.py _OBCJK_SECTION_DEFAULTS: kbd
    // binding enabled by default (EditorHotkey=Ctrl+F12 — bare F12 is Steam's
    // built-in screenshot key; ImeHotkey=bare F11), hold/combo bindings
    // disabled by default until the user opts in via the ini editor.
    BOOL w1  = WritePrivateProfileStringA("obCJK", "ActiveCodePage", "BIG5", k_iniMain);
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

// 鍵盤/手把長按/手把組合鍵 3 組binding互不排斥，可以同時生效——只要其中
// 任一組「未停用且條件成立」就算觸發，不再是舊版「三選一」的device switch。
// allDisabled(對應iniEdit「全部停用」勾選框)最優先短路，一旦開啟這組熱鍵
// (Editor或Ime)不論哪個裝置都不會觸發。
// 兩種手把裝置都只查(唯一支援的)joystick index 0，且都被 [obCJK]
// GamepadHotkeyEnable 總開關閘門(預設關閉，obCJK_iniEdit.py 熱鍵設定區) —
// 見 ObCJKGamepadHotkeyEnabled() 註解；這個開關關閉時，即使某組手把binding
// 個別勾了啟用也不會生效，鍵盤binding不受這個開關影響。
// GamepadCombo: 修飾鍵(左)+主鍵(右)都要同時held，跟鍵盤Ctrl/Shift+Key同概念。
// GamepadHold: 單一按鍵要連續held超過hk.holdSeconds秒才算觸發。
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

// Reads+deletes the Python editor's changed-fields diff file. That file is
// only ever written when a _SLOT_TABS font parameter (FontParam<N>_1/_2 or
// its _InterLinear variant) actually changed value — hotkey/IME-mode/
// codepage-only saves, or closing without saving, never populate it. Returns
// true iff a change was logged, which is exactly the signal the caller needs
// to decide whether a font hot-reload is warranted.
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

// [obCJK] DebugLogEnable gates IDebugLog's LOG level (what actually gets
// written to obCJK.log via IDebugLog::Message() — see IDebugLog.cpp Log(),
// `log = level <= logLevel`). PrintLevel is a separate, unrelated threshold
// that only gates printf() to a console obCJK's DLL never has, so it is not
// the relevant knob here despite the similar name. Default (0) keeps
// logLevel at kLevel_Message — HookUtil's hook-install ok/fail lines,
// _WARNING failures everywhere — matching the iniEdit "顯示debug log" hint.
// Enabled (1) raises it to kLevel_VerboseMessage so the diagnostic-dump
// _VMESSAGE calls in obCJK_GlyphAtlas.h/obCJK_MenuQueDelimHook.h also get
// written. Called once at plugin load, and again after the in-game ini
// editor closes with a save, same convention as ObCJKLoadHotkeySettings()
// below.
static void ObCJKLoadDebugLogSetting()
{
    int enabled = GetPrivateProfileIntA("obCJK", "DebugLogEnable", 0, k_iniMain);
    IDebugLog::LogLevel level = enabled ? IDebugLog::kLevel_VerboseMessage : IDebugLog::kLevel_Message;
    gLog.SetLogLevel(level);
    gLog.SetPrintLevel(level);
}

// 讀單一hk(Editor或Ime)的3組binding+總開關。keyPrefix如"EditorHotkey"/
// "ImeHotkey"；defaultKbdCode/defaultKbdModifier是鍵盤binding找不到key時的
// 保底值(Editor=Ctrl+F12,Ime=F11無修飾鍵，跟舊版預設一致)；其餘欄位(手把
// 長按/組合鍵/總開關)兩組hk預設值相同，直接寫死在函式內。
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

// Reads EditorHotkey*/ImeHotkey* (3 bindings each, see ObCJKLoadHotkeyBinding())
// and IMEMode from ini into the matching globals. Called once at plugin load,
// and again after the in-game ini editor closes with a save — those keys
// aren't part of the _SLOT_TABS diff ObCJKLogChanges() checks, so they need
// their own reload path to take effect without restarting the game.
static void ObCJKLoadHotkeySettings()
{
    ObCJKLoadHotkeyBinding(g_hotkeySettings,    "EditorHotkey", DIK_F12, 1);
    ObCJKLoadHotkeyBinding(g_imeHotkeySettings, "ImeHotkey",    DIK_F11, 0);

    char modeBuf[16] = "Out";
    GetPrivateProfileStringA("obCJK", "IMEMode", "Out", modeBuf, sizeof(modeBuf), k_iniMain);
    g_imeMode = (_stricmp(modeBuf, "In") == 0) ? kImeModeIn : kImeModeOut;

    // Reset key-was-down latches: if a hotkey just changed to a scancode/
    // button that happens to be physically held right now, stale latch
    // state would otherwise read as "already down" and swallow the first
    // real press, or (for the previous binding) leave a phantom "was down"
    // that never triggered a release edge.
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

    // [obCJK] GamepadInputDiagEnable(DEBUG，選項頁) — 跟 GamepadHotkeyEnable
    // 是否開啟無關，見 ObCJKGamepadInputDiagTick() 註解。
    ObCJKGamepadInputDiagTick();

    // IME: mode-gated so only one of the two systems reacts to a given hotkey press.
    bool imeKeyDown = IsHotkeyDown(g_imeHotkeySettings);
    if (g_imeMode == kImeModeIn)
        ObCJKImeInPerFrame(g_gameHWND, g_activeCodePage, imeKeyDown, g_imeInKeyWasDown);
    else
        ObCJKImePerFrame(g_activeCodePage, imeKeyDown, g_cursorReleased);

    // Editor: process close → reload hotkeys/IME mode unconditionally (any
    // save writes those [obCJK] keys, and they aren't part of the
    // _SLOT_TABS diff below), plus hot-reload the font atlas cache iff an
    // actual font parameter changed. ObCJKLogChanges() only returns true
    // when the diff file had content, which the Python editor only ever
    // writes for _SLOT_TABS keys — a hotkey/IME/codepage-only save, or
    // closing without saving, leaves it false and no font reload happens.
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
                // [2026-07-17] Deliberately NOT rewinding the TexSwap shelf-pack
                // cursor here (that was ObCJKTexSwapResetRegions(), now removed).
                // Some already-visible UI text (e.g. the main menu's static
                // bottom hotkey row) isn't redrawn every frame, so its baked UV
                // still points at its old rect in the shared native font
                // texture. Rewinding the cursor let a freshly-placed (but
                // unrelated) glyph get shelf-packed into that same rect,
                // making the stale, never-redrawn text flip to a different —
                // still valid-looking, but wrong — CJK glyph (see
                // obcjk_refresh_bug_001.png). Letting the cursor only ever
                // advance forward means an old rect is never overwritten by a
                // new placement, at the cost of monotonically consuming the
                // fixed-size reserved region across repeated in-session
                // reloads — already handled gracefully by TexUpload's
                // overflow fuse (obCJK_TexUpload.h's overflowWarned path).
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
        // Tell obCJK_iniEdit.py this is a live in-game launch, and whether
        // MenuQue (Submodule.Game.dll) is loaded — slot7/8 editing is only
        // unlocked in-game when this is present; a standalone launch (no
        // "--ingame" arg at all) is always unlocked. See obCJK_iniEdit.py
        // SLOT78_ENABLED.
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
        // Installed here (not from OBSEPlugin_Load) because OBSE's plugin
        // load order doesn't guarantee Submodule.Game.dll (MenuQue) is
        // already mapped by then — every other plugin is confirmed loaded
        // by kMessage_GameInitialized. See obCJK_MenuQueDelimHook.h top
        // comment.
        //
        // DBCS-only: gated out under kCP_UTF8. Proven structurally harmless
        // if it did install under UTF-8 (ObCJKIsLeadByte returns false for
        // kCP_UTF8, so ObCJKMQSplitsPair degrades to i+=1 and never
        // overshoots matchPos), but the
        // delimiter-value-can't-collide-with-a-continuation-byte reasoning
        // that makes it safe also means it has nothing to do in UTF-8 mode,
        // so keep the gate explicit here instead of relying on that
        // incidental no-op.
        if (g_activeCodePage != kCP_UTF8)
            ObCJKInstallMenuQueDelimHook();

        // Same "every plugin confirmed loaded by now" reasoning as the
        // MenuQue hook above — see obCJK_NorthernUICompat.h top comment.
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
    char cpBuf[16] = "BIG5";
    GetPrivateProfileStringA("obCJK", "ActiveCodePage", "BIG5",
                             cpBuf, sizeof(cpBuf), k_iniMain);
    g_activeCodePage = ObCJKCodePageFromString(cpBuf);
    _MESSAGE("obCJK:main:Load: ActiveCodePage=%s", ObCJKCodePageName(g_activeCodePage));

    // Pure file I/O (datastore.xml parse), no dependency on other plugins —
    // safe to run immediately, unlike the listener registration below.
    ObCJKNorthernUICompat_Init();

    // Build the kinsoku (line-break prohibition) classification table for the
    // active codepage now, so the log confirms how many punctuation entries
    // actually round-tripped through WideCharToMultiByte for this codepage
    // (some Unicode punctuation in the shared list has no representation in
    // every codepage and is silently skipped — see obCJK_LineBreakRule.h).
    // Wired into the wrapwidth-overflow decision below via
    // ObCJKInstallLineBreakHook() (obCJK_LineBreakHook.h).
    //
    // UTF-8's table (obCJK_LineBreakRule_UTF8.h) keys directly on Unicode
    // codepoint instead of re-encoded DBCS bytes, and doesn't depend on
    // g_activeCodePage at all (same rule set regardless of which legacy
    // codepage the game also supports) — call it explicitly here anyway
    // (instead of relying on its lazy first-lookup build) purely so the log
    // line below reports an accurate count at load time, matching the DBCS
    // branch's diagnostic.
    if (g_activeCodePage == kCP_UTF8) {
        ObCJKBuildLineBreakTableUtf8();
        _MESSAGE("obCJK:main:Load: LineBreakRule table (UTF8) built, %d entries",
                 g_lineBreakCountUtf8);
    } else {
        ObCJKBuildLineBreakTable(g_activeCodePage);
        _MESSAGE("obCJK:main:Load: LineBreakRule table built for %s, %d entries",
                 ObCJKCodePageName(g_activeCodePage), g_lineBreakCount);
    }

    // obCJK_WordWrapHook.h corrects word-wrap advance for CJK pairs and
    // stashes the trail byte for the draw side (+0x05 node field).
    // ObCJKInstallNodeCopyHook() carries that byte from the scratch node to
    // the permanent tree node. ObCJKInstallSecondLoopHook() applies the same
    // fix to sub_578960's independent second per-character loop, which
    // desyncs from the main loop on CJK text (real issue, but not the
    // book-open crash's root cause — see obCJK_WordWrapHook.h's top comment
    // and 」for the actual root cause, an int32 field written as float).
    //
    // obCJK_TexSwap.h must install BEFORE ObCJKInstallGlyphHooks() runs:
    // Path A/B/C now call obCJK_TexUpload.h's ObCJKTexUpload_GetOrPlaceGlyph(),
    // which only has somewhere to place a glyph once TexSwap has enlarged
    // that font's native .tex at load time.
    // Installing GlyphHooks first would still be safe (glyph placement just
    // keeps returning "not ready" until TexSwap runs), but this order matches
    // the actual load-time dependency.
    ObCJKInstallTexSwapHook();

    // Flips sub_575B40's (Path A) forced-line-break marker byte ('-' <-> ' ')
    // via direct immediate-byte patch — same two native VAs regardless of
    // active codepage, so this runs once here rather than inside either
    // branch below. See obCJK_WordWrapHook.h's kVA_LineBreakMarkerImm_*
    // comment for the exact bytes/addresses.
    ObCJKApplyLineBreakSpaceSettingPathA();
    // Same idea for the shared Path B/C word-wrap tree's (sub_5772A0) own,
    // separate forced-split hyphen marker — also codepage-independent, see
    // obCJK_WordWrapHook.h's kVA_LineBreakMarkerImm_PathBC comment.
    ObCJKApplyLineBreakSpaceSettingPathBC();

    // Path A per-line height fix (sub_576670 VA 0x5768c4) — also
    // codepage-independent (same native line-step regardless of DBCS/
    // UTF-8 decode mode; only the CJK-ready branch that FEEDS its tracked
    // max height differs, ObCJKPathACheck vs ObCJKPathACheckUtf8, both
    // already installed below). See obCJK_GlyphHook.h's
    // ObCJKInstallPathALineHeightHook comment + .
    ObCJKInstallPathALineHeightHook();

    if (g_activeCodePage == kCP_UTF8) {
        // UTF-8 branch (obCJK_*_UTF8.h). 6 of the 8 DBCS WordWrapHook hooks
        // have a UTF-8 counterpart (WordWrapCheck/NodeCopy/SecondLoop/
        // SmartQuoteGuard/LineSplitPair/PreMeasure below); TrailByteGuard
        // and TrailByteGuard2 have none — those two fix native's ambiguous-
        // byte-range misjudgment at 0x7E (a legal DBCS trail-byte value
        // that native's hotkey/hyphenation logic mistakes for the literal
        // '~' ASCII byte), which only exists because DBCS lead/trail ranges
        // overlap that ASCII value. UTF-8's continuation-byte range
        // (0x80-0xBF) never overlaps 0x7E, so the bug they patch is
        // structurally absent (see obCJK_WordWrapHook_UTF8.h top comment
        // and memory obcjk-utf8-plan for the full reasoning).
        ObCJKInstallSmartQuoteGuardHookUtf8();
        ObCJKInstallPreMeasureHookUtf8();
        ObCJKInstallLineSplitPairHookUtf8();
        ObCJKInstallWordWrapHookUtf8();
        ObCJKInstallNodeCopyHookUtf8();
        ObCJKInstallSecondLoopHookUtf8();
        ObCJKInstallLineBreakHookUtf8();
        ObCJKInstallGlyphHooksUtf8();
    } else {
        // TrailByteGuard fixes sub_575B40's buffer-copy step (VA 0x576226),
        // which runs before word-wrap measurement/draw ever see the byte
        // stream — install it alongside WordWrapHook so a CJK trail byte that
        // equals 0x7E isn't already stripped by the time the hooks below read
        // it. See obCJK_WordWrapHook.h's TrailByteGuard section for the bug.
        ObCJKInstallTrailByteGuardHook();
        // SmartQuoteGuard fixes the other independent single-byte misjudgment in
        // sub_575B40 (VA 0x575F8A): bytes 0x91-0x94 fall inside every supported
        // codepage's CJK lead-byte range but the native smart-quote jump table
        // unconditionally substitutes them with ASCII '/" regardless. Same
        // function, same buffer-copy step as TrailByteGuard above, so install it
        // alongside it. See obCJK_WordWrapHook.h's SmartQuoteGuard section.
        ObCJKInstallSmartQuoteGuardHook();
        // TrailByteGuard2 fixes a second, independent '~' hotkey misjudgment in
        // sub_575B40 (VA 0x575FFB), distinct from TrailByteGuard's VA 0x576226 —
        // same false-positive-on-0x7E-trail-byte class of bug, but this one
        // mismarks a legal hyphenation point instead of corrupting width. See
        // obCJK_WordWrapHook.h's TrailByteGuard2 section.
        ObCJKInstallTrailByteGuard2Hook();
        // LineSplitPair fixes sub_575B40's "no candidate break point, force
        // break at current position" branch (VA 0x57616E): native blindly
        // carries a single arbitrary byte past the inserted "-\n", which splits
        // a CJK lead/trail pair in half when that byte happens to be a trail
        // byte. Independent of, and unaffected by, the three guards above (they
        // gate different branches of the same function). See
        // obCJK_WordWrapHook.h's LineSplitPair section.
        ObCJKInstallLineSplitPairHook();
        // PreMeasure fixes sub_575B40's own independent per-byte width
        // accumulation (VA 0x575FAE), which feeds the CENTER/RIGHT-align
        // starting pen.x offset in sub_576670 — the actual "menu button text
        // not centered" bug.
        // Independent of the four guards/LineSplitPair above (they gate other
        // branches of the same function); order relative to them doesn't matter
        // at runtime. See obCJK_WordWrapHook.h's PreMeasure section.
        ObCJKInstallPreMeasureHook();
        ObCJKInstallWordWrapHook();
        ObCJKInstallNodeCopyHook();
        ObCJKInstallSecondLoopHook();
        // obCJK_LineBreakHook.h applies the kinsoku table above to the
        // wrapwidth-overflow decision (sub_577840 VA 0x577946), so it must
        // install after the node+0x05 trail byte is reliably populated by the
        // three hooks above (same per-character pipeline, install order here
        // doesn't matter at runtime — it matters that they all install).
        ObCJKInstallLineBreakHook();
        // obCJK_GlyphHook.h Path A/B/C: all three now replace the native
        // single-byte glyph pointer with a Plan B CJK glyph placed inside the
        // real (TexSwap-enlarged) native font texture via
        // ObCJKTexUpload_GetOrPlaceGlyph() — no SetTexture/D3D device calls of
        // their own (the earlier per-glyph SetTexture design was architecturally
        // inert). This rewiring is confirmed working — Path A/B/C all display 
        // CJK on realhardware.
        ObCJKInstallGlyphHooks();
    }

    // LootMenu slot7/8 off-by-one garble fix — gated to only repair when
    // lootmenu.dll is loaded, see obCJK_LootMenuTrailByteFixHook.h top
    // comment. Supersedes the retired obCJK_DebugSlot78Hook.h diagnostic,
    // which used this same hook point (sub_58CA50) for pure logging.
    // Installed unconditionally (not gated on g_activeCodePage like the
    // block above): the fix itself is already self-gating via
    // ObCJKIsLeadByte(b, g_activeCodePage), which returns false for every
    // byte under kCP_UTF8 (no switch case — see obCJK_Encoding.h), so it's
    // a structural no-op under UTF-8 rather than needing an explicit branch
    // here.
    ObCJKInstallLootMenuTrailByteFixHook();

    // Manual-save "success but no file written" fix — see
    // obCJK_SaveNameTruncateHook.h top comment and memory
    // obcjk_bug_save_maxpath. Installed unconditionally: it's a correctness
    // fix for vanilla game code, not a user-facing feature toggle.
    ObCJKInstallSavePathFixHook();
    // ★2026-07-19 temporary diagnostic-only hook (see its own comment in
    // obCJK_SaveNameTruncateHook.h) — remove once the real bottleneck for
    // the manual-save bug is confirmed from actual obCJK.log output.
    ObCJKInstallSaveEntryDiagHook();

    // Manual-save CJK-name encoding-mismatch fix + "false success" fix — see
    // obCJK_CreateFileWShim.h / obCJK_SaveReturnFixHook.h top comments and
    // memory obcjk_bug_save_maxpath. Installed unconditionally, same
    // reasoning as ObCJKInstallSavePathFixHook() above: these are
    // correctness fixes for vanilla game code, not user-facing toggles.
    ObCJKInstallCreateFileWShim();
    ObCJKInstallSaveReturnFixHook();
    // Save-list scanner (sub_45D450) FindFirstFileA/FindNextFileA CJK-name
    // corruption fix — see obCJK_SaveListFindShim.h top comment. Separate
    // bug from the CreateFileA encoding mismatch above: this one corrupts
    // the *name itself* (silently, via lossy ANSI best-fit substitution),
    // not just the open call, so it must be installed alongside it rather
    // than superseding it.
    ObCJKInstallSaveListFindShim();

    // Delete-save CJK-name false-success fix (.ess + .obse follow-up
    // delete) — see obCJK_DeleteFileWShim.h top comment and memory
    // obcjk_bug_delete_falsesuccess.
    ObCJKInstallDeleteEntryDiagHook();

    ObCJKLoadHotkeySettings();

    OBSEMessagingInterface* msgIntfc =
        (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging);
    if (msgIntfc)
        msgIntfc->RegisterListener(g_pluginHandle, "OBSE", MessageHandler);
    g_msgIntfc = msgIntfc;  // stashed for ObCJKNorthernUICompat_RegisterListener, see MessageHandler

    return true;
}

} // extern "C"
