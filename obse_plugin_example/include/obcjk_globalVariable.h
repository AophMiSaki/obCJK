#pragma once

#include "obCJK_Path.h"  // HotkeySettings type + DIK_F11 constant used below

static ObCJKCodePage g_activeCodePage         = kCP_BIG5;
// comboModifierButton 預設4(LB)只在ini完全缺這個key時才用得到，見
// g_hotkeySettings(main.cpp)同款保底值。長按/組合鍵預設停用(disabled=true)，
// 只有鍵盤binding預設啟用，跟obCJK_iniEdit.py _OBCJK_SECTION_DEFAULTS一致。
static HotkeySettings g_imeHotkeySettings     = { DIK_F11, 0, false, 0, 1.0f, true, 0, 0, 4, true, false };
static bool          g_cursorReleased         = false;
static int           g_lineBreakCount         = -1;
