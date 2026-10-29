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

// GamepadHold: 判斷 controlIndex 是否已連續按住超過 seconds 秒。
// pressStartTick 由呼叫端跨幀保存，放開時歸零、按下第一幀記錄起始 tick。
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

// 讀 [obCJK] GamepadHotkeyEnable（預設 0）；關閉時熱鍵不查手把狀態。
// 只在第一次呼叫時讀 ini，不隨存檔熱重載。
static bool ObCJKGamepadHotkeyEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetPrivateProfileIntA("obCJK", "GamepadHotkeyEnable", 0, k_iniMain) != 0) ? 1 : 0;
    return cached != 0;
}

// controlIndex → Xbox 按鈕名稱，只供 debug log 顯示。
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

// Debug log：讀 [obCJK] GamepadInputDiagEnable（預設 0），與 GamepadHotkeyEnable 無關。
// 開啟後每幀掃描 joystick 0 的 36 個 controlIndex（0-31 按鈕／32-35 十字鍵），
// 記錄按下事件；首次先印裝置名稱與 VID:PID，上限 300 行。
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
