#pragma once
#include "obse/GameOSDepend.h"
#include "obCJK_Path.h"

static OSGlobals** const g_osGlobalsPtr = reinterpret_cast<OSGlobals**>(0x00B33398);

static bool IsPovDirectionActive(DWORD pov, int direction)
{
    if (pov == 0xFFFFFFFF) return false;
    int diff = ((int)pov - direction * 9000) % 36000;
    if (diff < 0) diff += 36000;
    if (diff > 18000) diff -= 36000;
    return diff >= -4500 && diff <= 4500;
}

static bool IsGamepadButtonDown(UInt8 whichJoystick, UInt8 controlIndex)
{
    OSGlobals* osg = g_osGlobalsPtr ? *g_osGlobalsPtr : nullptr;
    if (!osg || !osg->input) return false;

    OSInputGlobals* input = osg->input;
    if (!(input->flags & OSInputGlobals::kFlag_HasJoysticks)) return false;
    if (whichJoystick >= input->numJoysticks) return false;

    const DIJOYSTATE& state = input->joystickDeviceState[whichJoystick].allStateThisFrame;

    if (controlIndex < 32)
        return (state.rgbButtons[controlIndex] & 0x80) != 0;
    if (controlIndex <= 35)
        return IsPovDirectionActive(state.rgdwPOV[0], controlIndex - 32);
    return false;
}

// GamepadHold專用: 判斷 controlIndex 是否已經連續按住超過 seconds 秒。
// pressStartTick 由呼叫端持有、跨幀保存(HotkeySettings::holdStartTick，見
// obCJK_Path.h)——放開時歸零，按下的第一幀記錄起始tick，之後每幀比對經過
// 時間。GetTickCount() 溢位(約49.7天)會讓那一瞬間的差值算錯，但影響僅
// 一次判斷結果，下一幀就恢復正常，不處理。
static bool IsGamepadButtonHeldFor(UInt8 whichJoystick, UInt8 controlIndex, float seconds, DWORD& pressStartTick)
{
    if (!IsGamepadButtonDown(whichJoystick, controlIndex)) {
        pressStartTick = 0;
        return false;
    }
    DWORD now = GetTickCount();
    if (pressStartTick == 0) pressStartTick = now;
    return (now - pressStartTick) >= (DWORD)(seconds * 1000.0f);
}

// ini key GamepadHotkeyEnable ([obCJK] 節，obCJK_iniEdit.py 熱鍵設定區的
// 開關) — 預設關閉（0）。main.cpp 的 IsHotkeyDown() 只在這個回傳 true 時才
// 真的去查手把狀態；關閉時即使某個熱鍵的 Device 選了 Gamepad 也不會生效，
// 純鍵盤玩家可以完全跳過每幀查手把狀態的開銷。沿用 obCJK_GlyphHook.h
// ObCJKPathDiagEnabled() 同款 lazy-static 快取（見該檔案註解：只在第一次
// 呼叫時讀 ini，之後不會隨編輯器存檔即時重載）。
static bool ObCJKGamepadHotkeyEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "GamepadHotkeyEnable", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// obCJK_iniEdit.py _XBOX_BUTTON_NAMES 的 C++ 端對照，只用於下面的 DEBUG log
// 訊息好讀，不影響任何判斷邏輯。
static const char* GamepadControlIndexName(UInt8 idx)
{
    switch (idx) {
        case 0:  return "A";    case 1:  return "B";    case 2:  return "X";    case 3:  return "Y";
        case 4:  return "LB";   case 5:  return "RB";   case 6:  return "Back"; case 7:  return "Start";
        case 8:  return "LS";   case 9:  return "RS";
        case 32: return "DPadUp"; case 33: return "DPadRight";
        case 34: return "DPadDown"; case 35: return "DPadLeft";
        default: return nullptr;
    }
}

// ini key GamepadInputDiagEnable ([obCJK] 節，選項頁 DEBUG 區的開關) —
// 預設關閉（0）。開啟後每幀掃描 joystick 0 的全部 36 個 controlIndex
// （0-31按鈕/32-35十字鍵，跟 IsGamepadButtonDown() 同一套編號），
// 用來驗證按鈕/十字鍵等偵測邏輯有沒有正確對到玩家實際按下的按鍵，跟
// GamepadHotkeyEnable 是否開啟無關(這樣才能在還沒開熱鍵前就先確認手把
// 能不能被正確辨識)。第一次偵測到有手把時，log 開頭先印一行裝置識別資訊
// （名稱/VID:PID），方便對照使用者回報的log時知道當下測試的是哪支手把。
// log 行數上限固定300行，跟其他 DiagCap 系列的預設同一個量級，避免長時間
// 開著洗版obCJK.log。
static bool ObCJKGamepadInputDiagEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "GamepadInputDiagEnable", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

static void ObCJKGamepadInputDiagTick()
{
    if (!ObCJKGamepadInputDiagEnabled()) return;

    OSGlobals* osg = g_osGlobalsPtr ? *g_osGlobalsPtr : nullptr;
    if (!osg || !osg->input) return;
    OSInputGlobals* input = osg->input;
    if (!(input->flags & OSInputGlobals::kFlag_HasJoysticks) || input->numJoysticks == 0) return;

    static bool s_identified = false;
    static bool s_prevState[36] = {};
    static DWORD s_hits = 0;
    static const DWORD kDiagCap = 300;

    if (!s_identified) {
        s_identified = true;
        const DIDEVICEINSTANCE& dev = input->joystickDevices[0];
        UInt16 vendor  = (UInt16)(dev.guidProduct.Data1 & 0xFFFF);
        UInt16 product = (UInt16)((dev.guidProduct.Data1 >> 16) & 0xFFFF);
        _MESSAGE("obCJK:Gamepad:Diag: device=\"%s\" VID=0x%04X PID=0x%04X",
                 dev.tszProductName, vendor, product);
    }

    for (UInt8 i = 0; i < 36 && s_hits <= kDiagCap; i++) {
        bool now = IsGamepadButtonDown(0, i);
        if (now && !s_prevState[i]) {
            s_hits++;
            const char* name = GamepadControlIndexName(i);
            if (name) _MESSAGE("obCJK:Gamepad:Diag: pressed index=%u (%s)", i, name);
            else      _MESSAGE("obCJK:Gamepad:Diag: pressed index=%u", i);
        }
        s_prevState[i] = now;
    }
}
