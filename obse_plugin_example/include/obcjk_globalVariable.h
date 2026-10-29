#pragma once

#include "obCJK_Path.h"  // HotkeySettings type + DIK_F11 constant used below

static ObCJKCodePage g_activeCodePage         = kCP_UTF8;
// IME熱鍵預設值(ini缺key時用)：鍵盤F11啟用；手把長按/組合鍵停用，組合鍵修飾鍵4(LB)。
static HotkeySettings g_imeHotkeySettings     = { DIK_F11, 0, false, 0, 1.0f, true, 0, 0, 4, true, false };
static bool          g_cursorReleased         = false;
static int           g_lineBreakCount         = -1;
