r"""
obCJK_iniEdit.py — obCJK 字體/外掛設定編輯程式（BIG5 / GBK / SJIS / KOREAN / UTF8）
- 支援 BIG5 / GBK / SJIS / KOREAN / UTF8 五種編碼切換，共用同一份 obCJK.dll、
  同一份 ini（Data\OBSE\Plugins\obCJK\obCJK.ini），各編碼各自一個 section
  （[BIG5]/[GBK]/[SJIS]/[KOREAN]/[UTF8]），不再是各自獨立 DLL + 獨立 ini 檔
- 切換前確認 Data\OBSE\Plugins\ 下有 obCJK.dll
- EnumFontFamiliesExW 依 Charset 列舉系統字體
- 每個 SLOT 設定半角/全角字體參數（[BIG5]/[GBK]/[SJIS]/[KOREAN]/[UTF8] 節，
  「遊戲內實際顯示」schema）
    p0 字寬、p1 高、p2 字距、p3 Y偏移
    p4 濃度（-8..+8，下拉選單；0 對應舊版公式約 +3 的濃淡，見
    obCJK_GlyphAtlas.h ObCJKApplyDensityContrast() 的 +3 基準位移）
    p5 粗細（300/400/500/700）、p6 對比（弱/普通/強烈）
    p7 斜體（0/1，checkbox）
- SLOT 7/8（MenuQue 額外字型）：僅在遊戲內啟動（obCJK.dll 傳入 --ingame）且
  偵測到 MenuQue 已載入額外字型（--extrafonts）時開放編輯；非遊戲內啟動
  （手動執行）則不受限制，見 SLOT78_ENABLED
- 該ini檔以utf-8編寫與讀取（細節見 write_ini() 說明）
- 本工具自己的操作介面顯示語言（按鈕/標籤文字）獨立於上面的編碼/字型設定，
  翻譯內容放在同層的 language\ 資料夾（一個語言一個 .json），透過介面上的
  「顯示語言」下拉選單切換並記住在 [obCJK] UILang（見 load_languages()/tr()）
- [obCJK] IMEMode（Out=外部視窗 obCJK_IME_Out.h／In=內部輸入 obCJK_IME_In.h）
  可在介面上切換
- [obCJK] NorthernUIEnable（0=原版/其他UI／1=原DLL／2=新DLL，下拉選單跟顯示
  語言同一列）非「原版/其他UI」時，字型分頁多開 SLOT9/10 佔位分頁（保留，
  分頁本身停用點不到）+ 5 個 NorthernUI 角色分頁（Normal/Large/
  MediumLargeUpper/Shadowed/Small，對應 FontParam33~37）
"""

import configparser
import ctypes
import ctypes.wintypes
import faulthandler
import io
import json
import re
import shutil
import sys
import textwrap
import winreg
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox, ttk
from pathlib import Path

# ── 常數 ─────────────────────────────────────────────────────────────────────

# obCJK 只有一個 DLL、一份 ini（Data\OBSE\Plugins\obCJK\obCJK.ini），
# 四種編碼各自對應 ini 裡的一個 section，不再各自獨立一份 ini 檔。
_OBCJK_DLL = "obCJK.dll"

# GDI DEFAULT_CHARSET——UTF8 用（Windows 沒有"UTF8_CHARSET"這個常數）。定義在
# _CP_ROWS 之前，因為下面的 tuple 字面值在模組載入時就會立刻求值。
DEFAULT_CHARSET = 0x01

# (名稱, CodePage, GDI charset, ini節名, UI語言標籤)
# UTF8 的 GDI charset 用 DEFAULT_CHARSET（Windows 沒有"UTF8_CHARSET"這個常數，
# 與 C++ 端 obCJK_GlyphAtlas.h ObCJKCharsetForActiveCodePage() 的選擇一致）。
_CP_ROWS = [
    ("BIG5",   950,   0x88, "BIG5",   "繁體中文"),
    ("GBK",    936,   0x86, "GBK",    "簡體中文"),
    ("SJIS",   932,   0x80, "SJIS",   "日文"),
    ("KOREAN", 949,   0x81, "KOREAN", "韓文"),
    ("UTF8",   65001, DEFAULT_CHARSET, "UTF8",   "UTF-8"),
]

CODEPAGE_CONFIG: dict[str, dict] = {
    name: {"code": code, "charset": cs, "section": sec, "lang": lang}
    for name, code, cs, sec, lang in _CP_ROWS
}

SELECTABLE_CP = ["BIG5", "GBK", "SJIS", "KOREAN", "UTF8"]

# GlyphXAlign規則C的預設標點清單——跟obCJK_GlyphAtlas.h的
# kObCJKGlyphXAlignCDefaultChars必須是同一份6個字，改一邊要記得改另一邊。
_GLYPH_X_ALIGN_C_DEFAULT_CHARS = "。，、；：·"

# ── UI 多語系翻譯（外部化到 language\ 資料夾，見 load_languages()）───────────────

LANGUAGE_DIR = Path(__file__).resolve().parent / "language"
DEFAULT_UI_LANG = "zh-TW"

_CAMEL_RE = re.compile(r"(?<!^)(?=[A-Z])")


def _camel_to_snake(s: str) -> str:
    """'slot1Desc' → 'slot1_desc'；language\\*.json 的 uiText_xxxYyy key 還原成
    現有全檔 tr("xxx_yyy") 呼叫端沿用的原始 snake_case key，呼叫端完全不用改。"""
    return _CAMEL_RE.sub("_", s).lower()


_THICKNESS_KEYS = [("300", 300), ("400", 400), ("500", 500), ("700", 700)]
_CONTRAST_KEYS  = [("weak", -1), ("normal", 0), ("strong", 1)]

# 選項頁每列右側灰色說明文字的自動換行寬度（像素）。窗口大小是
# App.__init__ 最後用 update_idletasks()+winfo_reqwidth() 依內容自動算出來
# 的（見 App._build_content 呼叫端附近的 self.geometry(...)），所以調大這個
# 值只會讓視窗一起變寬（上限是螢幕寬度-80），不會被裁切。
_HINT_WRAPLENGTH = 480

# App._build_content() 幫Font/Options頁的捲動canvas設定高度上限時，用來
# 保留給「編碼選單列+顯示語言列+分頁籤列+底部儲存ini列」這圈固定外殼的
# 粗估總高度（像素）——見該處呼叫端註解，避免內容過高時把底部儲存列擠出
# 視窗看不到。
_CHROME_RESERVE_H = 170

# 內嵌救援預設值（language\ 資料夾遺失/讀取失敗時使用，內容=原本寫死的 BIG5 版本，
# 確保就算漏帶資料夾也不會直接崩潰，只是退化成單一語言）。
_FALLBACK_UI_TEXT: dict[str, str] = {
    "half": "半角", "full": "全角",
    "apply": "套用選取", "reset": "初始化",
    "width": "字寬", "height": "高", "spacing": "字距", "ypos": "Y偏移",
    "density": "濃度", "weight": "粗細", "contrast": "對比", "italic": "斜體",
    "hotkey_set": "設定", "hotkey_dis": "停用",
    "hotkey_dlg_title": "熱鍵捕捉",
    "hotkey_dlg_prompt": "請按下目標按鍵",
    "hotkey_dlg_cancel": "（按 Esc 取消）",
    "hotkey_dlg_fail": "⚠ 無法識別此鍵，請重試",
    "dik_disabled": "(停用)",
    "hk_editor": "開啟編輯器：", "hk_editor_hint": "預設 Ctrl+F12（單獨 F12 是 Steam 內建截圖鍵）",
    "hk_ime_lbl": "開啟 IME：",  "hk_ime_hint": "預設 F11 = DIK_F11 = 0x57",
    "hk_editor_frame_title": "編輯器", "hk_ime_frame_title": "IME",
    "hk_all_dis": "全部停用",
    "hk_device_kbd": "鍵盤", "hk_device_pad": "手把",
    "hk_device_hold": "手把長按鍵", "hk_device_combo": "手把組合鍵",
    "hotkey_dlg_prompt_pad": "請按下手把上的按鈕",
    "hotkey_dlg_pad_fail": "⚠ 找不到手把裝置，請確認已連接",
    "hotkey_dlg_pad_detected": "已偵測到裝置：{name}",
    "hotkey_dlg_pad_disallowed": "此按鍵不可用於這個模式，請按其他按鍵",
    "gamepad_hotkey_enable_lbl": "手把熱鍵偵測：",
    "gamepad_hotkey_enable_hint": "有勾選才會對手把熱鍵做偵測，預設關閉；關閉時即使下面「手把長按鍵」「手把組合鍵」有勾選啟用也不會生效。"
        "「手把長按鍵」＝單一按鍵按住超過自訂秒數才觸發；「手把組合鍵」＝左邊修飾鍵"
        "(肩鍵/Start/Menu)按住+右邊主鍵(不含左右搖桿按鍵)一起按下觸發。"
        "扳機鍵不支援直接指定為熱鍵（不同手把的類比軸差異太大，偵測不穩定）；"
        "若要用扳機觸發熱鍵，請改用 JoyToKey 等工具先把扳機映射成手把按鍵或鍵盤按鍵。",
    "hk_mod_none": "無", "hk_mod_ctrl": "Ctrl", "hk_mod_shift": "Shift",
    "hotkey_conflict_title": "熱鍵衝突",
    "hotkey_conflict_msg": "這個按鍵組合已被以下功能佔用：\n{desc}\n\n仍可以繼續使用，但可能無法正常觸發，或跟該功能互相干擾。",
    "conflict_desc_f12": "F12：Steam 內建的螢幕截圖快捷鍵",
    "conflict_desc_shift_tab": "Shift+Tab：Steam Overlay（介面）開啟熱鍵",
    "conflict_desc_ctrl_c": "Ctrl+C：系統「複製」",
    "conflict_desc_ctrl_v": "Ctrl+V：系統「貼上」",
    "conflict_desc_ctrl_x": "Ctrl+X：系統「剪下」",
    "conflict_desc_ctrl_z": "Ctrl+Z：系統「復原」",
    "conflict_desc_ctrl_y": "Ctrl+Y：系統「重做」",
    "conflict_desc_ctrl_a": "Ctrl+A：系統「全選」",
    "conflict_desc_ctrl_s": "Ctrl+S：系統「儲存」",
    "conflict_desc_ctrl_p": "Ctrl+P：系統「列印」",
    "conflict_desc_ctrl_f": "Ctrl+F：系統「尋找」",
    "conflict_desc_ctrl_n": "Ctrl+N：系統「開新檔案」",
    "conflict_desc_ctrl_o": "Ctrl+O：系統「開啟舊檔」",
    "conflict_desc_ctrl_w": "Ctrl+W：系統「關閉視窗/分頁」",
    "conflict_desc_ctrl_tab": "Ctrl+Tab：系統「切換分頁」",
    "conflict_desc_ctrl_esc": "Ctrl+Esc：等同 Windows 鍵，開啟「開始」選單",
    "done": "完成", "error": "錯誤",
    "cp_frame": "編碼 / 語言選擇", "korean_note": "",
    "font_frame": "系統{lang}字體",
    "font_frame_ascii": "系統ASCII字體",
    "font_mode_cjk": "CJK字體",
    "font_mode_ascii": "ASCII字體（半角）",
    "hk_main_frame": "熱鍵設定（點「設定」後依裝置按下按鍵）",
    "save_ini": "儲存 ini",
    "preview": "預覽",
    "hint_title": "提示", "hint_msg": "請在左側列表點選字體後自動套用。",
    "saved_utf8": "已儲存：\n{path}",
    "slot1_desc": "標題、大部分UI、書籍",
    "slot2_desc": "HUD、對話字幕",
    "slot3_desc": "地圖地名、彈出文本、剩餘UI…",
    "slot5_desc": "書籍、信件(手寫)",
    "slot7_desc": "MenuQue 額外字型 1",
    "slot8_desc": "MenuQue 額外字型 2",
    "ui_lang_lbl": "顯示語言：",
    "ime_mode_lbl": "IME 模式：",
    "ime_mode_out": "外部視窗（原有）",
    "ime_mode_in": "內部輸入",
    "glyph_x_align_lbl": "標點/窄字水平對齊：",
    "glyph_x_align_rule_a": "規則A（依字型bearing）",
    "glyph_x_align_rule_b": "規則B（置中）",
    "glyph_x_align_hint": "只影響黑盒比cell窄的字（標點、小假名等），一般漢字不受影響",
    "glyph_x_align_rule_c": "規則C（指定標點強制置中）",
    "glyph_x_align_c_chars_lbl": "規則C標點清單：",
    "glyph_x_align_c_chars_hint": "C選項給那些覺得某些符號不夠置中的人使用",
    "glyph_x_align_c_chars_edit_btn": "編輯清單...",
    "glyph_x_align_c_chars_dlg_title": "規則C標點清單",
    "glyph_x_align_c_chars_col_idx": "序號",
    "glyph_x_align_c_chars_col_char": "符號",
    "glyph_x_align_c_chars_col_code": "UTF8碼",
    "glyph_x_align_c_chars_new_lbl": "新字元：",
    "glyph_x_align_c_chars_add_btn": "新增",
    "glyph_x_align_c_chars_del_btn": "刪除所選",
    "glyph_x_align_c_chars_ok_btn": "確定",
    "glyph_x_align_c_chars_cancel_btn": "取消",
    "font_not_found_title": "字型可能不存在",
    "font_not_found_msg": "以下欄位的字型名稱在系統字型清單中找不到：\n{items}\n仍要繼續儲存嗎？",
    "page_font": "字型",
    "page_options": "選項",
    "features_frame": "功能開關",
    "ascii_render_lbl": "ASCII 應用自選字型：",
    "ascii_render_hint": "半形英數字元套用自訂字型，關閉則使用遊戲原生字型",
    "menuque_lbl": "啟用 MenuQue Hook：",
    "menuque_hint": "CJK分隔字元誤判修正（戰利品清單等 MenuQue 文字）(只有非UTF8需要)",
    "lootmenu_lbl": "啟用 LootMenu 修正：",
    "lootmenu_hint": "戰利品清單 slot7/8 亂碼修正，需先啟用上方 MenuQue(只有非UTF8需要)",
    "texswap_lbl": "取消CJK全部顯示(DEBUG)：",
    "texswap_hint": "除錯用，開啟後CJK與ASCII自選字型全部停用，退回遊戲原生字型",
    "debuglog_lbl": "顯示debug log(DEBUG)：",
    "debuglog_hint": "關閉時log只會顯示hook是否成功套用，開啟後才會印出詳細診斷內容",
    "gamepad_input_diag_lbl": "手把輸入檢測(DEBUG)：",
    "gamepad_input_diag_hint": "開啟後檢測遊戲中按下的所有手把按鍵並記錄到log，用來確認手把熱鍵有沒有偵測成功；開啟時log第一行會先記錄目前是哪支手把",
    "path_diag_lbl": "PATH A/B/C 診斷log(DEBUG)：",
    "path_diag_hint": "開啟後印出CJK字型替換的catch/miss訊息，右側為每條路徑各自的印出行數上限，除錯用",
    "path_diag_slot78_lbl": "只印 slot7/8",
    "lootmenu_diag_lbl": "slot7/8 修正診斷log(DEBUG)：",
    "lootmenu_diag_hint": "開啟後印出戰利品清單slot7/8亂碼修正的命中訊息，右側為印出行數上限，除錯用",
    "hang_watchdog_lbl": "凍結監控(DEBUG)：",
    "hang_watchdog_hint": "介面卡住超過5秒會自動把當下所有執行緒的呼叫堆疊寫入 obCJK_iniEdit_hang.log，用於排查難重現的卡死",
    "linebreak_space_patha_lbl": "PATH A 斷行\"-\"改成空白：",
    "linebreak_space_patha_hint": "文字強制斷行時用空白(0x20)取代插入\"-\"(0x2D)",
    "linebreak_space_pathbc_lbl": "PATH B、C 斷行\"-\"改成空白：",
    "linebreak_space_pathbc_hint": "對PATH B(控制台)/C(書籍)也應用空白取代\"-\"",
    "save_diag_lbl": "存檔/刪檔完整記錄log(DEBUG)：",
    "save_diag_hint": "關閉時只印出每次存檔/刪檔的成功或失敗（失敗附簡短原因），開啟後印出完整診斷內容，除錯用",
    "ascii_native": "遊戲原生字型",
    "northernui_lbl": "NorthernUI 相容：",
    "northernui_opt_vanilla": "原版/其他UI",
    "northernui_opt_official": "原NorthernUI",
    "northernui_opt_patched": "新NorthernUI",
    "slot_reserved_desc": "保留（尚未開放）",
}
_FALLBACK_THICKNESS_OPTS = [("300 細", 300), ("400 標準", 400), ("500 適中", 500), ("700 粗體", 700)]
_FALLBACK_CONTRAST_OPTS  = [("弱", -1), ("普通", 0), ("強烈", 1)]
_FALLBACK_LANGUAGES: dict[str, dict] = {
    DEFAULT_UI_LANG: {
        "display_name": "繁體中文",
        "ui_text": _FALLBACK_UI_TEXT,
        "thickness_opts": _FALLBACK_THICKNESS_OPTS,
        "contrast_opts": _FALLBACK_CONTRAST_OPTS,
    }
}


def _load_one_language(path: Path) -> "tuple[str, dict] | None":
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        print(f"[警告] 語言檔讀取失敗，略過: {path} ({e})")
        return None
    code = raw.get("meta_code")
    if not code:
        print(f"[警告] 語言檔缺少 meta_code，略過: {path}")
        return None
    ui_text = {
        _camel_to_snake(k[len("uiText_"):]): v
        for k, v in raw.items() if k.startswith("uiText_")
    }
    thickness_opts = [
        (raw[f"thicknessOpts_{k}"], v)
        for k, v in _THICKNESS_KEYS if f"thicknessOpts_{k}" in raw
    ]
    contrast_opts = [
        (raw[f"contrastOpts_{k}"], v)
        for k, v in _CONTRAST_KEYS if f"contrastOpts_{k}" in raw
    ]
    return code, {
        "display_name": raw.get("meta_displayName", code),
        "ui_text": ui_text,
        "thickness_opts": thickness_opts or _FALLBACK_THICKNESS_OPTS,
        "contrast_opts": contrast_opts or _FALLBACK_CONTRAST_OPTS,
    }


def load_languages() -> dict[str, dict]:
    """掃描 language\\*.json，回傳 {語言代碼: {display_name, ui_text, thickness_opts,
    contrast_opts}}。資料夾不存在/掃描不到任何檔案時退回內嵌救援預設值。"""
    if not LANGUAGE_DIR.is_dir():
        print(f"[警告] 找不到語言資料夾 {LANGUAGE_DIR}，使用內嵌預設語言")
        return dict(_FALLBACK_LANGUAGES)
    result: dict[str, dict] = {}
    for path in sorted(LANGUAGE_DIR.glob("*.json")):
        loaded = _load_one_language(path)
        if loaded is not None:
            result[loaded[0]] = loaded[1]
    if not result:
        print(f"[警告] {LANGUAGE_DIR} 內沒有可用的語言檔，使用內嵌預設語言")
        return dict(_FALLBACK_LANGUAGES)
    return result


LANGUAGES: dict[str, dict] = load_languages()
THICKNESS_OPTS_LANG: dict[str, list] = {c: d["thickness_opts"] for c, d in LANGUAGES.items()}
CONTRAST_OPTS_LANG: dict[str, list] = {c: d["contrast_opts"] for c, d in LANGUAGES.items()}

_UI_LANG: str = DEFAULT_UI_LANG if DEFAULT_UI_LANG in LANGUAGES else next(iter(LANGUAGES))


def tr(key: str) -> str:
    lang = LANGUAGES.get(_UI_LANG) or LANGUAGES.get(DEFAULT_UI_LANG) or next(iter(LANGUAGES.values()))
    return lang["ui_text"].get(key, key)


def _combo_width(labels) -> int:
    """依目前使用中字型的實際像素寬度換算成 ttk Combobox 'width=' 用的字元
    單位——ttk 的 width 是以字型「平均字元寬度」（約等於'0'的寬度）換算，
    日韓文全形字元實際渲染寬度通常是這個平均寬度的~2倍，若直接用
    len(s) 當字元數，全形字串（如日文「外部ウィンドウ（既存）」、
    「ルールA（フォントのbearingに従う）」）還是會裝不下被裁掉。這裡改
    用 tkinter.font 直接量測目前選項清單裡最寬字串的實際像素寬度再換算，
    對任何語言都準確，不用逐語言調整。呼叫時機須在 Tk root 建立之後。"""
    fnt = tkfont.nametofont("TkDefaultFont")
    avg_px = fnt.measure("0")
    widest_px = max(fnt.measure(s) for s in labels)
    return -(-widest_px // avg_px) + 1


def _find_oblivion_dir() -> "Path | None":
    """嘗試從登錄機碼找 Oblivion 安裝目錄。"""
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        for sub in (
            r"SOFTWARE\Bethesda Softworks\Oblivion",
            r"SOFTWARE\WOW6432Node\Bethesda Softworks\Oblivion",
        ):
            try:
                with winreg.OpenKey(hive, sub) as k:
                    val, _ = winreg.QueryValueEx(k, "Installed Path")
                    p = Path(val)
                    if p.exists():
                        return p
            except OSError:
                pass
    return None


_OBL_DIR = _find_oblivion_dir()


def check_obcjk_dll() -> tuple[bool, str]:
    """回傳 (存在, 錯誤訊息)。存在時 msg 為空字串。單一 obCJK.dll，跟編碼無關。"""
    if _OBL_DIR is None:
        return False, "無法從登錄機碼找到 Oblivion 安裝目錄，無法確認 .dll 位置"
    dll_path = _OBL_DIR / "Data" / "OBSE" / "Plugins" / _OBCJK_DLL
    if not dll_path.exists():
        return False, (
            f"obCJK.dll 不存在！\n"
            f"請確認有在 Oblivion\\Data\\OBSE\\Plugins\\ 放入 obCJK.dll！"
        )
    return True, ""


# ── in-game 訊號（obCJK.dll 啟動編輯器時傳入的命令列參數）──────────────────
# 判斷 SLOT 7/8（MenuQue 額外字型）是否開放編輯：
#   非遊戲內啟動（手動雙擊執行，沒有 --ingame）→ 一律開放，方便預先設定
#   遊戲內啟動（obCJK.dll 熱鍵喚出，有 --ingame）→ 只有 obCJK.dll 同時偵測到
#   MenuQue 已載入額外字型（--extrafonts）才開放，避免對不會生效的設定瞎改
_ARGV_SET           = {a.lower() for a in sys.argv[1:]}
_LAUNCHED_INGAME    = "--ingame" in _ARGV_SET
_EXTRA_FONTS_LOADED = "--extrafonts" in _ARGV_SET
SLOT78_ENABLED      = (not _LAUNCHED_INGAME) or _EXTRA_FONTS_LOADED

# NorthernUI 相容模式（[obCJK] NorthernUIEnable）：0=原版/其他UI（不啟用橋接，
# 預設）、1=原DLL（官方未修改的NorthernUI.dll，obCJK.dll執行期直接讀取
# FontInfo+0x08的rawID）、2=新DLL（使用者自行重編譯的patch版，透過OBSE
# Messaging廣播）。跟C++端obCJK_NorthernUICompat.h的
# kObCJKNuiMode_Off/OfficialDLL/PatchedDLL一致，見obcjk_northernui_font_compat
# 記憶2026-07-30「兩路徑UI統一」。
NORTHERNUI_MODES = ["0", "1", "2"]

# NorthernUI 5 個字型角色 → ini engineID 33~37（FontParam33~FontParam37_1/_2），
# 對應 obCJK_NorthernUICompat.h 的 roleIndex 0~4（Normal/Large/
# MediumLargeUpper/Shadowed/Small）。顯示名稱刻意寫死不走翻譯系統，見
# obcjk_northernui_font_compat 記憶「iniEdit UI具體設計」第3點。
_NORTHERNUI_ROLES = [
    (33, "Normal"),
    (34, "Large"),
    (35, "MediumLargeUpper"),
    (36, "Shadowed"),
    (37, "Small"),
]


# p5 粗細 / p6 對比 — per-顯示語言標籤，見上方 load_languages() 產生的
# THICKNESS_OPTS_LANG / CONTRAST_OPTS_LANG（key 現在是語言代碼如 "zh-TW"，
# 不再是編碼名稱）。

# 字型預覽用的實際 CJK 樣本文字——用「正在編輯哪個編碼」(self._current_cp) 查表，
# 跟操作介面顯示語言（_UI_LANG）是兩件不同的事，不搬進 language\ 資料夾。
PREVIEW_SAMPLE: dict[str, str] = {
    "BIG5":   "繁體中文字體預覽\n「尤利爾·賽普汀」文字測試……AaBbCc 123 !?\n。，、；：「」『』（）？！──……《》〈〉．—～",
    "GBK":    "简体中文字体预览\n「尤利爾·賽普汀」文字測試……AaBbCc 123 !?",
    "SJIS":   "日本語フォントプレビュー\nAaBbCc 123 !?",
    "KOREAN": "한국어 글꼴 미리보기\n「유리엘 셉팀」글꼴 테스트……AaBbCc 123 !?",
    # UTF8 賣點是可在同一份文字混用多語系（單一 DBCS 編碼做不到），樣本刻意混
    # 繁中/日文/韓文於一行，驗證字型對這些混合 codepoint 是否都有字形可畫。
    "UTF8":   "UTF-8萬國碼預覽　尤利爾·セプティム　유리엘 셉팀\nขอบคุณ-Việt-अ आ इ ई उ ऊ ऋ ऌ ऍ ऎ ए-العربية\nAaBbCc 123 !?　。，「」（）",
}

# p4 濃度 — [2026-07-16] 縮小成 -8..+8（原 -15..+15），基準位移 +3 見
# ObCJKApplyDensityContrast()，讓「0」看起來接近舊版的 +3。
DENSITY_VALUES = [str(v) for v in range(-8, 9)]

PARAM_FIELDS = [
    ("width",   0, -999, 999),
    ("height",  1, -999, 999),
    ("spacing", 2, -999, 999),
    ("ypos",    3, -999, 999),
]

# DirectInput 掃描碼表（INI 以無前綴十六進位字串儲存，如 "43" = 0x43 = DIK_F9）
SCAN_CODES: dict[str, int] = {
    "DIK_ESCAPE":       0x01, "DIK_1":            0x02, "DIK_2":         0x03,
    "DIK_3":            0x04, "DIK_4":            0x05, "DIK_5":         0x06,
    "DIK_6":            0x07, "DIK_7":            0x08, "DIK_8":         0x09,
    "DIK_9":            0x0A, "DIK_0":            0x0B, "DIK_MINUS":     0x0C,
    "DIK_EQUALS":       0x0D, "DIK_BACK":         0x0E, "DIK_TAB":       0x0F,
    "DIK_Q":            0x10, "DIK_W":            0x11, "DIK_E":         0x12,
    "DIK_R":            0x13, "DIK_T":            0x14, "DIK_Y":         0x15,
    "DIK_U":            0x16, "DIK_I":            0x17, "DIK_O":         0x18,
    "DIK_P":            0x19, "DIK_LBRACKET":     0x1A, "DIK_RBRACKET":  0x1B,
    "DIK_RETURN":       0x1C, "DIK_LCONTROL":     0x1D, "DIK_A":         0x1E,
    "DIK_S":            0x1F, "DIK_D":            0x20, "DIK_F":         0x21,
    "DIK_G":            0x22, "DIK_H":            0x23, "DIK_J":         0x24,
    "DIK_K":            0x25, "DIK_L":            0x26, "DIK_SEMICOLON": 0x27,
    "DIK_APOSTROPHE":   0x28, "DIK_GRAVE":        0x29, "DIK_LSHIFT":    0x2A,
    "DIK_BACKSLASH":    0x2B, "DIK_Z":            0x2C, "DIK_X":         0x2D,
    "DIK_C":            0x2E, "DIK_V":            0x2F, "DIK_B":         0x30,
    "DIK_N":            0x31, "DIK_M":            0x32, "DIK_COMMA":     0x33,
    "DIK_PERIOD":       0x34, "DIK_SLASH":        0x35, "DIK_RSHIFT":    0x36,
    "DIK_MULTIPLY":     0x37, "DIK_LMENU":        0x38, "DIK_SPACE":     0x39,
    "DIK_CAPITAL":      0x3A, "DIK_F1":           0x3B, "DIK_F2":        0x3C,
    "DIK_F3":           0x3D, "DIK_F4":           0x3E, "DIK_F5":        0x3F,
    "DIK_F6":           0x40, "DIK_F7":           0x41, "DIK_F8":        0x42,
    "DIK_F9":           0x43, "DIK_F10":          0x44, "DIK_NUMLOCK":   0x45,
    "DIK_SCROLL":       0x46, "DIK_NUMPAD7":      0x47, "DIK_NUMPAD8":   0x48,
    "DIK_NUMPAD9":      0x49, "DIK_SUBTRACT":     0x4A, "DIK_NUMPAD4":   0x4B,
    "DIK_NUMPAD5":      0x4C, "DIK_NUMPAD6":      0x4D, "DIK_ADD":       0x4E,
    "DIK_NUMPAD1":      0x4F, "DIK_NUMPAD2":      0x50, "DIK_NUMPAD3":   0x51,
    "DIK_NUMPAD0":      0x52, "DIK_DECIMAL":      0x53, "DIK_OEM_102":   0x56,
    "DIK_F11":          0x57, "DIK_F12":          0x58, "DIK_F13":       0x64,
    "DIK_F14":          0x65, "DIK_F15":          0x66, "DIK_KANA":      0x70,
    "DIK_CONVERT":      0x79, "DIK_NOCONVERT":    0x7B, "DIK_YEN":       0x7D,
    "DIK_NUMPADEQUALS": 0x8D, "DIK_PREVTRACK":    0x90, "DIK_NEXTTRACK": 0x99,
    "DIK_NUMPADENTER":  0x9C, "DIK_RCONTROL":     0x9D, "DIK_MUTE":      0xA0,
    "DIK_CALCULATOR":   0xA1, "DIK_PLAYPAUSE":    0xA2, "DIK_MEDIASTOP": 0xA4,
    "DIK_VOLUMEDOWN":   0xAE, "DIK_VOLUMEUP":     0xB0, "DIK_WEBHOME":   0xB2,
    "DIK_DIVIDE":       0xB5, "DIK_SYSRQ":        0xB7, "DIK_RMENU":     0xB8,
    "DIK_PAUSE":        0xC5, "DIK_HOME":         0xC7, "DIK_UP":        0xC8,
    "DIK_PRIOR":        0xC9, "DIK_LEFT":         0xCB, "DIK_RIGHT":     0xCD,
    "DIK_END":          0xCF, "DIK_DOWN":         0xD0, "DIK_NEXT":      0xD1,
    "DIK_INSERT":       0xD2, "DIK_DELETE":       0xD3, "DIK_LWIN":      0xDB,
    "DIK_RWIN":         0xDC, "DIK_APPS":         0xDD, "DIK_POWER":     0xDE,
    "DIK_SLEEP":        0xDF, "DIK_WAKE":         0xE3,
}

_DIK_BY_VAL: dict[int, str] = {v: k for k, v in SCAN_CODES.items()}


def ini_to_dik(ini_val: str) -> str:
    """'43' → 'DIK_F9 (0x43)'；0 或找不到則回傳 '(停用)'。"""
    s = ini_val.strip()
    try:
        code = int(s, 0) if s.lower().startswith("0x") else int(s, 16)
    except ValueError:
        return ini_val
    if code == 0:
        return tr("dik_disabled")
    name = _DIK_BY_VAL.get(code, "")
    return f"{name} (0x{code:02X})" if name else ini_val


def dik_to_ini(display: str) -> str:
    """'DIK_F9 (0x43)' → '43'；解析括號內的十六進位值。"""
    if "(" in display and ")" in display:
        hex_part = display.split("(")[-1].rstrip(")").strip()
        try:
            val = int(hex_part, 0)
            return f"{val:02X}"
        except ValueError:
            pass
    return display.strip()


def _slot_label(n: int, desc_key: str) -> str:
    """組出分頁標籤：SLOT 編號另起一行，說明文字依字數自動換行（避免英文等
    較長語言的說明把分頁、進而把整個視窗撐得過寬）。"""
    desc = textwrap.fill(tr(desc_key), width=18)
    return f"SLOT {n}\n{desc}"


def slot_tabs() -> list[tuple[str, str]]:
    tabs = [
        (_slot_label(1, "slot1_desc"), "FontParam1"),
        (_slot_label(2, "slot2_desc"), "FontParam2"),
        (_slot_label(3, "slot3_desc"), "FontParam3"),
        (_slot_label(5, "slot5_desc"), "FontParam5"),
    ]
    # SLOT 7/8 = MenuQue 額外字型（engine ID 7/8）。只有遊戲內偵測到 MenuQue
    # 實際載入額外字型時才開放編輯，見 SLOT78_ENABLED 的規則說明。
    if SLOT78_ENABLED:
        tabs += [
            (_slot_label(7, "slot7_desc"), "FontParam7"),
            (_slot_label(8, "slot8_desc"), "FontParam8"),
        ]
    return tabs

# 每個條目：(half_def, full_def)，順序對應 slot_tabs()。
# 2026-07-30 依使用者指示，所有 codepage / 所有 SLOT 的預設字距（p2，index 2）
# 統一改為 0（含NorthernUI——見下方MODE_DEFAULTS["UTF8"] SLOT7/8註解，其餘
# NorthernUI 5個角色分頁沿用SLOT1預設值，見_build_font_page() defs[0]）。
_BIG5_DEFS = [
    (("Tahoma",     [0, 32, 0, 0, 34, 400, 0, 0]),
     ("微軟正黑體", [0, 34, 0, 0, 34, 400, 0, 0])),
    (("Tahoma",     [0, 40, 0, 0, 34, 400, 0, 0]),
     ("微軟正黑體", [0, 40, 0, 0, 34, 400, 0, 0])),
    (("Tahoma",     [0, 26, 0, 0, 34, 400, 0, 0]),
     ("微軟正黑體", [0, 26, 0, 0, 34, 400, 0, 0])),
    (("Tahoma",     [0, 34, 0, 0, 34, 400, 0, 0]),
     ("微軟正黑體", [0, 34, 0, 0, 34, 400, 0, 0])),
    # SLOT 7/8 (MenuQue) — 沿用 SLOT 1 預設值當起點，高度改為 16；
    # Y偏移沿用實測調校值 -3/-4（唯一保留非0 Y偏移預設的例外）
    (("Tahoma",     [0, 16, 0, -3, 34, 400, 0, 0]),
     ("微軟正黑體", [0, 16, 0, -3, 34, 400, 0, 0])),
    (("Tahoma",     [0, 16, 0, -4, 34, 400, 0, 0]),
     ("微軟正黑體", [0, 16, 0, -4, 34, 400, 0, 0])),
]

# UTF8 模式：沿用 _BIG5_DEFS 同一組數值參數（高度/偏移等未經 Noto Sans Mono
# CJK 實機調校，僅字型名稱換成 Noto Sans Mono CJK），半形/CJK 兩欄都用同一
# 字型——見下方 MODE_DEFAULTS["UTF8"] 註解。
_UTF8_DEFS = [
    (("Noto Sans Mono CJK", [0, 32, 0, 0, 34, 400, 0, 0]),
     ("Noto Sans Mono CJK", [0, 34, 0, 0, 34, 400, 0, 0])),
    (("Noto Sans Mono CJK", [0, 40, 0, 0, 34, 400, 0, 0]),
     ("Noto Sans Mono CJK", [0, 40, 0, 0, 34, 400, 0, 0])),
    (("Noto Sans Mono CJK", [0, 26, 0, 0, 34, 400, 0, 0]),
     ("Noto Sans Mono CJK", [0, 26, 0, 0, 34, 400, 0, 0])),
    (("Noto Sans Mono CJK", [0, 34, 0, 0, 34, 400, 0, 0]),
     ("Noto Sans Mono CJK", [0, 34, 0, 0, 34, 400, 0, 0])),
    # SLOT 7/8 (MenuQue) — 2026-07-19 改用 Tahoma 半/全形同一字型，
    # 字高24/Y偏移0/濃度7（使用者實測值，取代原本沿用SLOT1的
    # Noto Sans Mono CJK種子值，修正ShortcutLabel被icon遮擋問題）；字距原
    # 實測值為2，2026-07-30依使用者指示改為全域預設0（含NorthernUI）。
    (("Tahoma", [0, 24, 0, 0, 7, 400, 0, 0]),
     ("Tahoma", [0, 24, 0, 0, 7, 400, 0, 0])),
    (("Tahoma", [0, 24, 0, 0, 7, 400, 0, 0]),
     ("Tahoma", [0, 24, 0, 0, 7, 400, 0, 0])),
]

MODE_DEFAULTS: dict[str, list] = {
    "BIG5": _BIG5_DEFS,
    "GBK": [
        (("Tahoma",         [0, 32, 0, 0, 34, 400, 0, 0]),
         ("LXGW WenKai GB", [0, 34, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",         [0, 40, 0, 0, 34, 400, 0, 0]),
         ("LXGW WenKai GB", [0, 40, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",         [0, 26, 0, 0, 34, 400, 0, 0]),
         ("LXGW WenKai GB", [0, 26, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",         [0, 34, 0, 0, 34, 400, 0, 0]),
         ("LXGW WenKai GB", [0, 34, 0, 0, 34, 400, 0, 0])),
        # SLOT 7/8 (MenuQue) — 沿用 SLOT 1 預設值當起點，高度改為 16
        (("Tahoma",         [0, 16, 0, 0, 34, 400, 0, 0]),
         ("LXGW WenKai GB", [0, 16, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",         [0, 16, 0, 0, 34, 400, 0, 0]),
         ("LXGW WenKai GB", [0, 16, 0, 0, 34, 400, 0, 0])),
    ],
    "SJIS": [
        # SLOT 1
        (("Kingthings Petrock Light", [0, 34, 0, 0, 36, 400, 0, 0]),
         ("Tkaisho-GT01",             [0, 34, 0, 0, 36, 400, 0, 0])),
        # SLOT 2
        (("SimSun",              [0, 37, 0, 0, 38, 400, 0, 0]),
         ("BIZ-UDMincho-Medium", [0, 37, 0, 0, 38, 400, 0, 0])),
        # SLOT 3
        (("SimSun",              [0, 28, 0, 0, 34, 400, 0, 0]),
         ("BIZ-UDMincho-Medium", [0, 28, 0, 0, 34, 400, 0, 0])),
        # SLOT 5
        (("SAIMOJIフォント極", [0, 30, 0, 0, 34, 400, 1, 0]),
         ("SAIMOJIフォント極", [0, 30, 0, 0, 34, 400, 1, 0])),
        # SLOT 7/8 (MenuQue) — 沿用 SLOT 1 預設值當起點，高度改為 16
        (("Kingthings Petrock Light", [0, 16, 0, 0, 36, 400, 0, 0]),
         ("Tkaisho-GT01",             [0, 16, 0, 0, 36, 400, 0, 0])),
        (("Kingthings Petrock Light", [0, 16, 0, 0, 36, 400, 0, 0]),
         ("Tkaisho-GT01",             [0, 16, 0, 0, 36, 400, 0, 0])),
    ],
    "KOREAN": [
        (("Tahoma",    [0, 32, 0, 0, 34, 400, 0, 0]),
         ("맑은 고딕", [0, 34, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",    [0, 40, 0, 0, 34, 400, 0, 0]),
         ("맑은 고딕", [0, 40, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",    [0, 26, 0, 0, 34, 400, 0, 0]),
         ("맑은 고딕", [0, 26, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",    [0, 34, 0, 0, 34, 400, 0, 0]),
         ("맑은 고딕", [0, 34, 0, 0, 34, 400, 0, 0])),
        # SLOT 7/8 (MenuQue) — 沿用 SLOT 1 預設值當起點，高度改為 16
        (("Tahoma",    [0, 16, 0, 0, 34, 400, 0, 0]),
         ("맑은 고딕", [0, 16, 0, 0, 34, 400, 0, 0])),
        (("Tahoma",    [0, 16, 0, 0, 34, 400, 0, 0]),
         ("맑은 고딕", [0, 16, 0, 0, 34, 400, 0, 0])),
    ],
    # UTF8 是通用 Unicode 模式，沒有單一對應語言；半形/CJK 兩欄都用
    # Noto Sans Mono CJK（不帶地區後綴，讓系統依 codepoint 自動 fallback
    # 選字形，同時涵蓋繁中/簡中/日/韓混排——UTF-8 模式的賣點正是這種多語系
    # 混排，不適合再沿用單一地區字型如微軟正黑體）。這裡只是種子值，使用者
    # 仍可在字型分頁自由改選任何系統字型。
    "UTF8": _UTF8_DEFS,
}

# ── Windows 字體列舉 ──────────────────────────────────────────────────────────

class LOGFONTW(ctypes.Structure):
    _fields_ = [
        ("lfHeight",         ctypes.wintypes.LONG),
        ("lfWidth",          ctypes.wintypes.LONG),
        ("lfEscapement",     ctypes.wintypes.LONG),
        ("lfOrientation",    ctypes.wintypes.LONG),
        ("lfWeight",         ctypes.wintypes.LONG),
        ("lfItalic",         ctypes.c_ubyte),
        ("lfUnderline",      ctypes.c_ubyte),
        ("lfStrikeOut",      ctypes.c_ubyte),
        ("lfCharSet",        ctypes.c_ubyte),
        ("lfOutPrecision",   ctypes.c_ubyte),
        ("lfClipPrecision",  ctypes.c_ubyte),
        ("lfQuality",        ctypes.c_ubyte),
        ("lfPitchAndFamily", ctypes.c_ubyte),
        ("lfFaceName",       ctypes.c_wchar * 32),
    ]


FONTENUMPROC = ctypes.WINFUNCTYPE(
    ctypes.c_int,
    ctypes.POINTER(LOGFONTW),
    ctypes.c_void_p,
    ctypes.wintypes.DWORD,
    ctypes.wintypes.LPARAM,
)


# GDI ANSI_CHARSET——半角/ASCII 西文字型用（如 Tahoma、Arial、Times New Roman）。
# 跟上面 _CP_ROWS 的 CJK charset 分開列舉：多數西文字型不會同時宣告支援 CJK
# charset，若只用目前編碼的 charset 列舉，「半角」欄位常見西文字型會完全
# 列不出來，因此字型清單改成可切換 CJK / ASCII 兩種 charset 顯示。
ANSI_CHARSET = 0x00


def enum_cjk_fonts(charset: int) -> list[str]:
    """列舉系統中支援指定 GDI charset 的字體。"""
    gdi32  = ctypes.windll.gdi32
    user32 = ctypes.windll.user32
    hdc    = user32.GetDC(None)
    found: list[str] = []

    def _cb(lplf, _tm, _ft, _lp):
        name = lplf.contents.lfFaceName
        if not name.startswith("@") and name not in found:
            found.append(name)
        return 1

    cb = FONTENUMPROC(_cb)
    lf = LOGFONTW()
    lf.lfCharSet  = charset
    lf.lfFaceName = ""
    gdi32.EnumFontFamiliesExW(hdc, ctypes.byref(lf), cb, 0, 0)
    user32.ReleaseDC(None, hdc)
    return sorted(found)


# ── INI 解析 ─────────────────────────────────────────────────────────────────

def parse_font_param(raw: str) -> tuple[str, list[int]]:
    """'Tahoma,0,32,-1,24,34,400,0,0' → ('Tahoma', [0,32,-1,24,34,400,0,0])"""
    parts = raw.split(",")
    name  = parts[0].strip()
    nums: list[int] = []
    for p in parts[1:]:
        try:
            nums.append(int(p.strip()))
        except ValueError:
            nums.append(0)
    while len(nums) < 8:
        nums.append(0)
    return name, nums[:8]


def build_font_param(name: str, nums: list[int]) -> str:
    return name + "," + ",".join(str(n) for n in nums)


def _split_ini_sections(raw: bytes) -> tuple[bytes, bytes]:
    """位元組安全切開 [obCJK] 節跟其餘所有節（legacy FontParam*）。

    用 latin-1 當「位元組↔字串」無損中介編碼——它對任何位元組值都是雙射，
    不會像 utf-8/mbcs 那樣對任意位元組序列拋 UnicodeDecodeError，所以能先
    用普通字串比對切出 [obCJK] 節的範圍，再各自用正確編碼解碼。安全性成立
    的理由：'['、']'、CR、LF 在 Big5/GBK/Shift-JIS/UTF-8 這些多位元組編碼裡
    都不可能是 lead/trail byte 的一部分，所以在位元組層級切節不會弄壞任何
    一種編碼的內容。
    """
    text = raw.decode("latin-1")
    obcjk_lines: list[str] = []
    rest_lines: list[str] = []
    in_obcjk = False
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_obcjk = (stripped == "[obCJK]")
        (obcjk_lines if in_obcjk else rest_lines).append(line)
    return ("".join(obcjk_lines).encode("latin-1"),
            "".join(rest_lines).encode("latin-1"))


def read_ini(path: Path) -> configparser.ConfigParser:
    cfg = configparser.ConfigParser()
    cfg.optionxform = str
    if not path.exists():
        return cfg
    obcjk_bytes, rest_bytes = _split_ini_sections(path.read_bytes())

    if rest_bytes.strip():
        for enc in ("utf-8-sig", "utf-8", "mbcs"):
            try:
                cfg.read_string(rest_bytes.decode(enc))
                print(f"[讀取] legacy FontParam* 節: {enc} → {path}")
                break
            except (UnicodeDecodeError, configparser.Error):
                continue
        else:
            raise RuntimeError(f"無法解讀 {path}（legacy FontParam* 節）")

    if obcjk_bytes.strip():
        for enc in ("utf-8-sig", "utf-8", "mbcs"):
            try:
                cfg.read_string(obcjk_bytes.decode(enc))
                print(f"[讀取] [obCJK] 節: {enc} → {path}")
                break
            except (UnicodeDecodeError, configparser.Error):
                continue
        else:
            cfg.read_string(obcjk_bytes.decode("mbcs", errors="replace"))
            print("[警告] [obCJK] 節有無法解碼的位元組，已用系統ANSI codepage+替代字元頂替")
    return cfg


def write_ini(cfg: configparser.ConfigParser, path: Path) -> None:
    """整份 ini（[obCJK] 節 + FontParam* 節）統一寫 UTF-8（無BOM）。

    C++ 端讀 FontParam<N>_1/_2 字型名稱時，需自行以
    MultiByteToWideChar(CP_UTF8,...) 轉寬字元後呼叫 CreateFontIndirectW，
    才能正確處理非ASCII字型名稱（見 obCJK_GlyphAtlas.h），這步驟與本檔案
    編碼寫入方式無關，是 C++ 端獨立要做的事。
    """
    if path.exists():
        bak = path.with_suffix(".ini.bak")
        shutil.copy2(path, bak)
        print(f"[備份] {bak}")
    path.parent.mkdir(parents=True, exist_ok=True)

    obcjk_cfg = configparser.ConfigParser()
    obcjk_cfg.optionxform = str
    rest_cfg = configparser.ConfigParser()
    rest_cfg.optionxform = str
    for sec in cfg.sections():
        target = obcjk_cfg if sec == "obCJK" else rest_cfg
        target.add_section(sec)
        for k, v in cfg.items(sec):
            target.set(sec, k, v)

    obcjk_buf = io.StringIO()
    obcjk_cfg.write(obcjk_buf)
    rest_buf = io.StringIO()
    rest_cfg.write(rest_buf)

    obcjk_bytes = obcjk_buf.getvalue().replace("\n", "\r\n").encode("utf-8")
    rest_bytes  = rest_buf.getvalue().replace("\n", "\r\n").encode("utf-8")

    with path.open("wb") as f:
        f.write(obcjk_bytes)
        f.write(rest_bytes)
    print(f"[寫入] 全部=UTF-8 → {path}")


# ── ModeTab：一個 Mode 的頁籤 ────────────────────────────────────────────────

class ModeTab(ttk.Frame):
    """
    半角/全角的字體名稱列在各自的 sub-frame（以便 Entry 伸展），
    參數列直接放在 ModeTab 自己的 grid（row 1 / row 3），
    同一 grid 確保兩列欄位垂直對齊。
    """

    def __init__(self, parent, prefix: str, cfg: configparser.ConfigParser,
                 on_apply_request,
                 half_default: tuple, full_default: tuple,
                 sec: str = "BIG5",
                 on_param_change=None):
        super().__init__(parent, padding=6)
        self._prefix           = prefix
        self._on_apply_request = on_apply_request

        # per-顯示語言 粗細 / 對比 lookup
        th_opts          = THICKNESS_OPTS_LANG.get(_UI_LANG, THICKNESS_OPTS_LANG[DEFAULT_UI_LANG])
        self._th_labels  = [lbl for lbl, _ in th_opts]
        self._th_by_val  = {v: lbl for lbl, v in th_opts}
        self._th_by_lbl  = {lbl: v  for lbl, v in th_opts}
        co_opts          = CONTRAST_OPTS_LANG.get(_UI_LANG, CONTRAST_OPTS_LANG[DEFAULT_UI_LANG])
        self._co_labels  = [lbl for lbl, _ in co_opts]
        self._co_by_val  = {v: lbl for lbl, v in co_opts}
        self._co_by_lbl  = {lbl: v  for lbl, v in co_opts}

        def _get(key, default):
            return cfg.get(sec, key, fallback=default) if cfg.has_section(sec) else default

        half_str = f"{half_default[0]}," + ",".join(str(n) for n in half_default[1])
        full_str = f"{full_default[0]}," + ",".join(str(n) for n in full_default[1])

        n1, p1  = parse_font_param(_get(f"{prefix}_1", half_str))
        n2, p2  = parse_font_param(_get(f"{prefix}_2", full_str))

        self._name_vars  = [tk.StringVar(value=n1), tk.StringVar(value=n2)]
        self._pvars: list[dict[int, tk.StringVar]] = [{}, {}]

        # 半形專用「使用遊戲原生字型」開關 — 對應 obCJK_GlyphAtlas.h 新增的
        # FontParam<engineID>_1_Native ini key（跟 FontParam<N>_1 的字型名稱
        # 字串分開存放，避免用魔法字串污染字型名稱欄位）。勾選後半形這一列的
        # 欄位全部停用，C++ 端會讓這個 SLOT 的半形字元完全跳過obCJK自訂渲染、
        # 回退到 Oblivion 原生 bitmap 字型。全形沒有這個概念，只做在 idx=0。
        self._native_var = tk.BooleanVar(value=(_get(f"{prefix}_1_Native", "0") == "1"))
        self._on_param_change = on_param_change
        # normal-state widgets (Entry/Spinbox/Button/Checkbutton) vs
        # readonly-state widgets (Combobox — must return to "readonly", not
        # "normal", or unchecking 原生 would leave them freely typable.
        self._half_widgets: list[tk.Widget] = []
        self._half_readonly_widgets: list[tk.Widget] = []

        row_labels    = [tr("half"), tr("full")]
        ini_nums_list = [p1, p2]
        def_names     = [half_default[0], full_default[0]]
        def_nums_list = [half_default[1], full_default[1]]

        for i in range(2):
            ini_nums  = ini_nums_list[i]
            def_name  = def_names[i]
            def_nums  = def_nums_list[i]
            pvars     = self._pvars[i]
            name_row   = i * 3
            param_row  = i * 3 + 1
            param_row2 = i * 3 + 2

            nf = ttk.Frame(self)
            nf.grid(row=name_row, column=0, columnspan=20, sticky="ew",
                    pady=(0 if i == 0 else 6, 2))
            nf.columnconfigure(1, weight=1)

            ttk.Label(nf, text=row_labels[i], anchor="e").grid(
                row=0, column=0, padx=(0, 4))
            name_entry = ttk.Entry(nf, textvariable=self._name_vars[i])
            name_entry.grid(row=0, column=1, sticky="ew", padx=(0, 4))
            apply_btn = ttk.Button(nf, text=tr("apply"),
                       command=lambda idx=i: self._on_apply_request(self, idx))
            apply_btn.grid(row=0, column=2, padx=(0, 2))
            ttk.Button(nf, text=tr("reset"),
                       command=lambda idx=i, dn=def_name, dnums=def_nums:
                           self._reset(idx, dn, dnums)
                       ).grid(row=0, column=3, padx=(2, 0))
            if i == 0:
                self._half_widgets += [name_entry, apply_btn]
                ttk.Checkbutton(nf, text=tr("ascii_native"), variable=self._native_var,
                                command=self._on_native_toggle
                                ).grid(row=0, column=4, padx=(4, 0))

            col = 0
            for trkey, pidx, lo, hi in PARAM_FIELDS:
                var = tk.StringVar(value=str(ini_nums[pidx]))
                pvars[pidx] = var
                ttk.Label(self, text=tr(trkey), anchor="e").grid(
                    row=param_row, column=col, padx=(4, 0))
                spin = ttk.Spinbox(self, textvariable=var, from_=lo, to=hi, width=6)
                spin.grid(row=param_row, column=col + 1, padx=(0, 2))
                if i == 0:
                    self._half_widgets.append(spin)
                col += 2

            col = 0
            d_var = tk.StringVar(value=str(ini_nums[4] // 2 - 8))
            pvars[4] = d_var
            ttk.Label(self, text=tr("density"), anchor="e").grid(
                row=param_row2, column=col, padx=(4, 0))
            d_box = ttk.Combobox(self, textvariable=d_var, values=DENSITY_VALUES,
                         state="readonly", width=5)
            d_box.grid(row=param_row2, column=col + 1, padx=(0, 2))
            if i == 0:
                self._half_readonly_widgets.append(d_box)
            col += 2

            t_var = tk.StringVar(value=self._th_by_val.get(ini_nums[5], self._th_labels[1]))
            pvars[5] = t_var
            ttk.Label(self, text=tr("weight"), anchor="e").grid(
                row=param_row2, column=col, padx=(4, 0))
            t_box = ttk.Combobox(self, textvariable=t_var, values=self._th_labels,
                         state="readonly", width=_combo_width(self._th_labels))
            t_box.grid(row=param_row2, column=col + 1, padx=(0, 2))
            if i == 0:
                self._half_readonly_widgets.append(t_box)
            col += 2

            c_var = tk.StringVar(value=self._co_by_val.get(ini_nums[6], self._co_labels[1]))
            pvars[6] = c_var
            ttk.Label(self, text=tr("contrast"), anchor="e").grid(
                row=param_row2, column=col, padx=(4, 0))
            c_box = ttk.Combobox(self, textvariable=c_var, values=self._co_labels,
                         state="readonly", width=_combo_width(self._co_labels))
            c_box.grid(row=param_row2, column=col + 1, padx=(0, 2))
            if i == 0:
                self._half_readonly_widgets.append(c_box)
            col += 2

            # p7 = italic — [2026-07-11] repurposed from the legacy (never
            # exposed/used) "unknown" field; semantics are obCJK's own, see
            # obCJK_GlyphAtlas.h.
            it_var = tk.BooleanVar(value=bool(ini_nums[7]))
            pvars[7] = it_var
            it_box = ttk.Checkbutton(self, text=tr("italic"), variable=it_var)
            it_box.grid(row=param_row2, column=col, columnspan=2, padx=(4, 0))
            if i == 0:
                self._half_widgets.append(it_box)

        self.columnconfigure(14, weight=1)
        self._on_native_toggle()

        if on_param_change is not None:
            def _fire(*_):
                on_param_change()
            for nv in self._name_vars:
                nv.trace_add("write", _fire)
            for pvs in self._pvars:
                for sv in pvs.values():
                    sv.trace_add("write", _fire)

    def _on_native_toggle(self) -> None:
        """勾選「遊戲原生字型」時，把半形這一列所有欄位灰掉停用——這個SLOT的
        半形設定已經不會被obCJK_GlyphAtlas.h讀取（FontParam<N>_1_Native=1時，
        C++端的ObCJKAsciiRenderEnabledForFont()對這個fontID一律回傳false，
        直接回退到Oblivion原生bitmap字型），停用UI避免使用者誤以為欄位還有效。"""
        native = bool(self._native_var.get())
        normal_state   = "disabled" if native else "normal"
        readonly_state = "disabled" if native else "readonly"
        for w in self._half_widgets:
            w.configure(state=normal_state)
        for w in self._half_readonly_widgets:
            w.configure(state=readonly_state)
        if self._on_param_change is not None:
            self._on_param_change()

    def _reset(self, idx: int, default_name: str, default_nums: list[int]) -> None:
        self._name_vars[idx].set(default_name)
        pvars = self._pvars[idx]
        for _, pidx, _, _ in PARAM_FIELDS:
            pvars[pidx].set(str(default_nums[pidx]))
        pvars[4].set(str(default_nums[4] // 2 - 8))
        pvars[5].set(self._th_by_val.get(default_nums[5], self._th_labels[1]))
        pvars[6].set(self._co_by_val.get(default_nums[6], self._co_labels[1]))
        pvars[7].set(bool(default_nums[7]))
        if idx == 0:
            self._native_var.set(False)
            self._on_native_toggle()

    def set_name(self, idx: int, name: str) -> None:
        self._name_vars[idx].set(name)

    def get_preview_params(self, idx: int = 1) -> dict:
        """Returns preview parameters for the given side (0=half, 1=full)."""
        pvars = self._pvars[idx]
        def _int(key, default):
            try:
                return int(pvars[key].get())
            except (ValueError, tk.TclError, KeyError):
                return default
        return {
            "name":     self._name_vars[idx].get().strip(),
            "size":     max(8, min(_int(1, 24), 72)),
            "width":    _int(0, 0),
            "spacing":  _int(2, 0),
            "density":  _int(4, 0),
            "weight":   self._th_by_lbl.get(pvars[5].get(), 400) if 5 in pvars else 400,
            "contrast": self._co_by_lbl.get(pvars[6].get(), 0) if 6 in pvars else 0,
            "italic":   bool(pvars[7].get()) if 7 in pvars else False,
        }

    def _get_nums(self, idx: int) -> list[int]:
        pvars = self._pvars[idx]
        nums  = [0] * 8
        for _, pidx, _, _ in PARAM_FIELDS:
            try:
                nums[pidx] = int(pvars[pidx].get())
            except (tk.TclError, ValueError):
                nums[pidx] = 0
        try:
            display = int(pvars[4].get())
        except (tk.TclError, ValueError):
            display = 2
        nums[4] = (display + 8) * 2
        nums[5] = self._th_by_lbl.get(pvars[5].get(), 400)
        nums[6] = self._co_by_lbl.get(pvars[6].get(), 0)
        try:
            nums[7] = 1 if pvars[7].get() else 0
        except (tk.TclError, KeyError):
            nums[7] = 0
        return nums

    def collect(self) -> dict[str, str]:
        return {
            f"{self._prefix}_1": build_font_param(
                self._name_vars[0].get().strip(), self._get_nums(0)),
            f"{self._prefix}_2": build_font_param(
                self._name_vars[1].get().strip(), self._get_nums(1)),
            f"{self._prefix}_1_Native": "1" if self._native_var.get() else "0",
        }


# ── 熱鍵捕捉元件 ─────────────────────────────────────────────────────────────

_MODIFIER_SYMS = frozenset({
    "Shift_L", "Shift_R", "Control_L", "Control_R",
    "Alt_L", "Alt_R", "Super_L", "Super_R",
    "Caps_Lock", "Num_Lock", "Scroll_Lock",
})

# 已知會跟 Windows / Steam 系統快捷鍵衝突的組合 —— (modifier token, DIK 名稱) ->
# 對應 tr() 說明文字的 key（見 language/*.json 的 uiText_conflictDesc*）。
# modifier 只有 "none"/"ctrl"/"shift" 三種（obCJK 熱鍵目前不支援 Alt 組合）。
_RESERVED_COMBOS: dict[tuple[str, str], str] = {
    ("none",  "DIK_F12"):    "conflict_desc_f12",       # Steam 內建截圖鍵
    ("shift", "DIK_TAB"):    "conflict_desc_shift_tab", # Steam Overlay 熱鍵
    ("ctrl",  "DIK_C"):      "conflict_desc_ctrl_c",
    ("ctrl",  "DIK_V"):      "conflict_desc_ctrl_v",
    ("ctrl",  "DIK_X"):      "conflict_desc_ctrl_x",
    ("ctrl",  "DIK_Z"):      "conflict_desc_ctrl_z",
    ("ctrl",  "DIK_Y"):      "conflict_desc_ctrl_y",
    ("ctrl",  "DIK_A"):      "conflict_desc_ctrl_a",
    ("ctrl",  "DIK_S"):      "conflict_desc_ctrl_s",
    ("ctrl",  "DIK_P"):      "conflict_desc_ctrl_p",
    ("ctrl",  "DIK_F"):      "conflict_desc_ctrl_f",
    ("ctrl",  "DIK_N"):      "conflict_desc_ctrl_n",
    ("ctrl",  "DIK_O"):      "conflict_desc_ctrl_o",
    ("ctrl",  "DIK_W"):      "conflict_desc_ctrl_w",
    ("ctrl",  "DIK_TAB"):    "conflict_desc_ctrl_tab",
    ("ctrl",  "DIK_ESCAPE"): "conflict_desc_ctrl_esc",
}


def _check_hotkey_conflict(modifier: str, dik_name: str) -> "str | None":
    """回傳已知衝突的說明文字；沒有已知衝突則回傳 None。只提示，不阻擋設定。"""
    key = _RESERVED_COMBOS.get((modifier, dik_name))
    return tr(key) if key else None


# Xbox 相容手把透過舊版 DirectInput 相容層時，rgbButtons[] 常見的社群公認
# 對應（0~9）。這組表沒有反組譯過本遊戲引擎驗證，只是 XInput-over-DirectInput
# 的通用慣例，純粹用來讓顯示好讀；實際存進 ini 的仍是原始 index，不影響
# main.cpp 端行為，也不保證涵蓋所有廠牌/驅動。10 以後或無法辨識時退回「手把N」。
_XBOX_BUTTON_NAMES = {
    0: "A", 1: "B", 2: "X", 3: "Y",
    4: "LB", 5: "RB", 6: "Back", 7: "Start",
    8: "LS", 9: "RS",
    # 32-35 不是實體按鈕，是 obCJK_iniEdit_dinput.py 的 DinputJoystick.poll()
    # 從 POV hat 換算出來的十字鍵方向，跟 obCJK_Gamepad.h 的 IsPovDirectionActive
    # 用同一套編號，兩邊要保持一致。
    32: "十字鍵上", 33: "十字鍵右", 34: "十字鍵下", 35: "十字鍵左",
}


def _gamepad_button_label(index: int) -> str:
    name = _XBOX_BUTTON_NAMES.get(index)
    return f"{name} ({index})" if name else f"{tr('hk_device_pad')} {index}"


# 手把組合鍵的修飾鍵(左)只開放這幾個 — 肩鍵/START/MENU(Back)，避免
# 選到A/B/X/Y這種原本就常被單獨使用的按鍵當「一直按著」的修飾鍵。跟
# _gamepad_button_label()共用同一份index語意(見obCJK_Gamepad.h)。
_GAMEPAD_COMBO_MODIFIER_CHOICES = [4, 5, 6, 7]  # LB,RB,Back(Menu),Start
# 組合鍵的主鍵(右)不開放左右搖桿按鍵(8/9)。
_GAMEPAD_COMBO_MAIN_DISALLOWED = frozenset({8, 9})


class HotkeyCapture(ttk.Frame):
    """單一裝置（鍵盤 / 手把長按 / 手把組合鍵，由建構時的 device 參數固定，
    生命週期內不可切換）的唯讀熱鍵顯示 + 「設定」按鈕；點擊後依裝置彈出
    對話框等待鍵盤按鍵或手把按鈕。是否啟用交由外層 MultiHotkeyCapture 的
    停用勾選框控制（見 set_row_enabled()），這裡不再有裝置切換或停用按鈕。"""

    def __init__(self, parent, device: str, ini_raw: str = "00", gamepad_button: int = 0,
                 modifier: str = "none", gamepad_modifier_button: int = 4,
                 hold_seconds: float = 1.0):
        super().__init__(parent)
        self._device                  = device if device in ("kbd", "hold", "combo") else "kbd"
        self._raw                     = self._normalize(ini_raw)
        self._gamepad_button          = gamepad_button
        self._gamepad_modifier_button = (gamepad_modifier_button
                                          if gamepad_modifier_button in _GAMEPAD_COMBO_MODIFIER_CHOICES
                                          else _GAMEPAD_COMBO_MODIFIER_CHOICES[0])
        self._hold_seconds            = max(0.1, min(10.0, hold_seconds))
        self._modifier                = modifier if modifier in ("none", "ctrl", "shift") else "none"

        # 修飾鍵（無/Ctrl/Shift）—— 只在 device=="kbd" 時建立/有意義。
        self._modifier_labels   = {
            "none": tr("hk_mod_none"), "ctrl": tr("hk_mod_ctrl"), "shift": tr("hk_mod_shift"),
        }
        self._modifier_by_label = {v: k for k, v in self._modifier_labels.items()}
        self._modifier_var      = tk.StringVar(value=self._modifier_labels[self._modifier])

        # 組合鍵的修飾鍵(左)——只在 device=="combo" 時建立/有意義。
        self._combo_mod_labels   = {i: _gamepad_button_label(i) for i in _GAMEPAD_COMBO_MODIFIER_CHOICES}
        self._combo_mod_by_label = {v: k for k, v in self._combo_mod_labels.items()}
        self._combo_mod_var      = tk.StringVar(value=self._combo_mod_labels[self._gamepad_modifier_button])

        # 長按秒數——只在 device=="hold" 時建立/有意義。
        self._hold_seconds_var = tk.DoubleVar(value=self._hold_seconds)

        self._display = tk.StringVar(value=self._current_display())

        # device 這輩子不會變，不再需要舊版那套 pack_forget()/pack() 動態
        # 切換的 _variable_slot，依固定 device 只建立對應那一個控制項即可。
        if self._device == "kbd":
            self._modifier_combo = ttk.Combobox(
                self, textvariable=self._modifier_var,
                values=list(self._modifier_labels.values()),
                state="readonly",
                width=_combo_width(self._modifier_labels.values()),
            )
            self._modifier_combo.pack(side="left", padx=(0, 3))
            self._modifier_var.trace_add("write", self._on_modifier_change)
        elif self._device == "combo":
            self._combo_mod_combo = ttk.Combobox(
                self, textvariable=self._combo_mod_var,
                values=list(self._combo_mod_labels.values()),
                state="readonly",
                width=_combo_width(self._combo_mod_labels.values()),
            )
            self._combo_mod_combo.pack(side="left", padx=(0, 3))
            self._combo_mod_var.trace_add("write", self._on_combo_mod_change)
        elif self._device == "hold":
            self._hold_seconds_spin = ttk.Spinbox(
                self, textvariable=self._hold_seconds_var,
                from_=0.1, to=10.0, increment=0.1, width=5, format="%.1f",
            )
            self._hold_seconds_spin.pack(side="left", padx=(0, 3))

        ttk.Entry(self, textvariable=self._display,
                  state="readonly", width=24).pack(side="left")
        self._set_button = ttk.Button(self, text=tr("hotkey_set"), command=self._open_capture)
        self._set_button.pack(side="left", padx=(3, 0))

    @staticmethod
    def _normalize(raw: str) -> str:
        return dik_to_ini(raw)

    def _current_display(self) -> str:
        if self._device == "hold":
            return _gamepad_button_label(self._gamepad_button)
        if self._device == "combo":
            return f"{_gamepad_button_label(self._gamepad_modifier_button)} + {_gamepad_button_label(self._gamepad_button)}"
        return ini_to_dik(self._raw)

    def _on_modifier_change(self, *_args):
        self._modifier = self._modifier_by_label.get(self._modifier_var.get(), "none")
        self._warn_if_conflict()

    def _on_combo_mod_change(self, *_args):
        self._gamepad_modifier_button = self._combo_mod_by_label.get(
            self._combo_mod_var.get(), _GAMEPAD_COMBO_MODIFIER_CHOICES[0])
        self._display.set(self._current_display())

    def _warn_if_conflict(self):
        if self._device != "kbd":
            return
        try:
            code = int(self._raw, 16)
        except ValueError:
            return
        name = _DIK_BY_VAL.get(code, "")
        if not name:
            return
        desc = _check_hotkey_conflict(self._modifier, name)
        if desc:
            messagebox.showwarning(
                tr("hotkey_conflict_title"), tr("hotkey_conflict_msg").format(desc=desc))

    def _open_capture(self):
        if self._device == "hold":
            self._open_capture_gamepad()
        elif self._device == "combo":
            self._open_capture_gamepad(disallowed=_GAMEPAD_COMBO_MAIN_DISALLOWED)
        else:
            self._open_capture_keyboard()

    def _open_capture_keyboard(self):
        dlg = tk.Toplevel(self)
        dlg.title(tr("hotkey_dlg_title"))
        dlg.resizable(False, False)
        dlg.transient(self.winfo_toplevel())
        dlg.grab_set()

        ttk.Label(dlg, text=tr("hotkey_dlg_prompt"),
                  font=("", 13), padding=(28, 18)).pack()
        status = tk.StringVar(value=tr("hotkey_dlg_cancel"))
        ttk.Label(dlg, textvariable=status,
                  foreground="gray", padding=(0, 0, 0, 14)).pack()

        def on_key(event):
            if event.keysym in _MODIFIER_SYMS:
                return
            if event.keysym == "Escape":
                dlg.destroy()
                return
            vk   = event.keycode
            scan = ctypes.windll.user32.MapVirtualKeyW(vk, 0)
            if scan == 0:
                status.set(tr("hotkey_dlg_fail"))
                return
            name    = _DIK_BY_VAL.get(scan, "")
            display = f"{name} (0x{scan:02X})" if name else f"(0x{scan:02X})"
            self._display.set(display)
            self._raw = f"{scan:02X}"
            dlg.destroy()
            self._warn_if_conflict()

        dlg.bind("<KeyPress>", on_key)
        dlg.update_idletasks()
        pw = self.winfo_toplevel()
        x  = pw.winfo_x() + (pw.winfo_width()  - dlg.winfo_width())  // 2
        y  = pw.winfo_y() + (pw.winfo_height() - dlg.winfo_height()) // 2
        dlg.geometry(f"+{x}+{y}")
        dlg.focus_set()

    def _open_capture_gamepad(self, disallowed: frozenset = frozenset()):
        # dlg 一定要先建立、先顯示，才做任何有可能失敗的手把初始化——不管
        # 是lazy import本身失敗、DirectInput初始化失敗、還是單純沒接手把，
        # 使用者都要看到「找不到手把裝置」這個對話框，而不是按下「設定」
        # 卻什麼事都沒發生(Tkinter會把按鈕callback裡漏接的例外整個吞掉，
        # 使用者看不到任何錯誤訊息或console，體感上就是「沒反應」)。
        dlg = tk.Toplevel(self)
        dlg.title(tr("hotkey_dlg_title"))
        dlg.resizable(False, False)
        dlg.transient(self.winfo_toplevel())
        dlg.grab_set()

        ttk.Label(dlg, text=tr("hotkey_dlg_prompt_pad"),
                  font=("", 13), padding=(28, 18)).pack()
        status = tk.StringVar(value=tr("hotkey_dlg_cancel"))
        # 綁到dlg上保活：status只是這個function的區域變數，「沒偵測到手把」
        # 提早return那條分支沒有任何closure捕捉它(poll()才會捕捉，但那條
        # 分支根本不會建立poll())，function一return，Python就會馬上GC掉
        # 這個StringVar，連帶底層Tcl變數被unset，Label顯示會變空白——
        # 這正是使用者回報「沒接手把時看不到任何錯誤訊息」的根因，用這行
        # 讓它跟dlg同生命週期即可避免。
        dlg._status_var = status
        ttk.Label(dlg, textvariable=status,
                  foreground="gray", padding=(0, 0, 0, 14)).pack()

        dlg.update_idletasks()
        pw = self.winfo_toplevel()
        x  = pw.winfo_x() + (pw.winfo_width()  - dlg.winfo_width())  // 2
        y  = pw.winfo_y() + (pw.winfo_height() - dlg.winfo_height()) // 2
        dlg.geometry(f"+{x}+{y}")
        dlg.lift()
        dlg.focus_force()

        poll_job = [None]
        joy = None

        def cleanup():
            if poll_job[0] is not None:
                dlg.after_cancel(poll_job[0])
            if joy is not None:
                joy.close()

        def on_escape(_event=None):
            cleanup()
            dlg.destroy()

        dlg.bind("<Escape>", on_escape)
        dlg.protocol("WM_DELETE_WINDOW", on_escape)

        try:
            # Lazy import: keyboard-only users never pay the DirectInput ctypes cost,
            # and a binding issue on some system can't break the rest of the tool.
            import obCJK_iniEdit_dinput as dinput
            joy = dinput.DinputJoystick()
            devices = joy.enumerate()
            if not devices:
                raise OSError("no attached joystick")
            joy.open(devices[0][0], dlg.winfo_id())
        except Exception:
            status.set(tr("hotkey_dlg_pad_fail"))
            dlg.focus_set()
            return

        # 顯示DirectInput實際列舉到的裝置名稱——如果使用者確定沒接手把卻看到
        # 這裡有名稱，代表是系統/其他軟體註冊的虛擬裝置(如Steam虛擬手把)
        # 讓DirectInput一直回報「已連接」，不是iniEdit判斷錯誤；有名稱可以
        # 對照排查是哪個裝置。
        status.set(tr("hotkey_dlg_pad_detected").format(name=devices[0][2] or "?"))

        prev_state = None

        def poll():
            nonlocal prev_state
            try:
                state = joy.poll()
            except Exception:
                status.set(tr("hotkey_dlg_pad_fail"))
                poll_job[0] = dlg.after(50, poll)
                return
            if prev_state is not None:
                for i, (was, now) in enumerate(zip(prev_state, state)):
                    if now and not was:
                        if i in disallowed:
                            status.set(tr("hotkey_dlg_pad_disallowed"))
                            continue
                        self._gamepad_button = i
                        self._display.set(self._current_display())
                        cleanup()
                        dlg.destroy()
                        return
            prev_state = state
            poll_job[0] = dlg.after(50, poll)

        poll_job[0] = dlg.after(50, poll)
        dlg.focus_set()

    def get_ini_value(self) -> dict:
        hold_seconds = self._hold_seconds
        if self._device == "hold":
            try:
                hold_seconds = max(0.1, min(10.0, float(self._hold_seconds_var.get())))
            except (ValueError, tk.TclError):
                pass
        return {
            "code": self._raw,
            "gamepad_button": self._gamepad_button,
            "gamepad_modifier_button": self._gamepad_modifier_button,
            "hold_seconds": hold_seconds,
            "modifier": {"ctrl": "Ctrl", "shift": "Shift"}.get(self._modifier, "None"),
        }

    def set_row_enabled(self, enabled: bool) -> None:
        """外層 MultiHotkeyCapture 依這一列的停用勾選框(或全部停用)狀態呼叫，
        只灰化跟這個固定 device 有關的控制項，不影響已存的設定值本身。"""
        widget_state = "normal" if enabled else "disabled"
        if self._device == "kbd":
            self._modifier_combo.configure(state=("readonly" if enabled else "disabled"))
        elif self._device == "combo":
            self._combo_mod_combo.configure(state=("readonly" if enabled else "disabled"))
        elif self._device == "hold":
            self._hold_seconds_spin.configure(state=widget_state)
        self._set_button.configure(state=widget_state)


class MultiHotkeyCapture(ttk.Frame):
    """一個熱鍵動作（開啟編輯器 / 開啟IME）同時管理 3 組互不排斥的獨立
    綁定：鍵盤、手把長按、手把組合鍵，各自一列(HotkeyCapture固定device)
    +各自的「停用」勾選框；底部另有「全部停用」總勾選框。全部停用勾選時，
    其餘 3 個勾選框變成不可選取（灰階唯讀），但保留原本的勾選狀態；個別
    勾選其中一列的停用，也只灰化那一列自己的控制項，互不影響。"""

    def __init__(self, parent, *,
                 kbd_code: str, kbd_modifier: str, kbd_disabled: bool,
                 hold_button: int, hold_seconds: float, hold_disabled: bool,
                 combo_button: int, combo_modifier_button: int, combo_disabled: bool,
                 all_disabled: bool):
        super().__init__(parent)

        self._all_disabled_var = tk.IntVar(value=1 if all_disabled else 0)

        self._kbd_cap = HotkeyCapture(
            self, device="kbd", ini_raw=kbd_code, modifier=kbd_modifier)
        self._hold_cap = HotkeyCapture(
            self, device="hold", gamepad_button=hold_button, hold_seconds=hold_seconds)
        self._combo_cap = HotkeyCapture(
            self, device="combo", gamepad_button=combo_button,
            gamepad_modifier_button=combo_modifier_button)

        self._rows = [
            (tk.IntVar(value=1 if kbd_disabled else 0), tr("hk_device_kbd"), self._kbd_cap),
            (tk.IntVar(value=1 if hold_disabled else 0), tr("hk_device_hold"), self._hold_cap),
            (tk.IntVar(value=1 if combo_disabled else 0), tr("hk_device_combo"), self._combo_cap),
        ]
        self._row_checkbuttons: list[ttk.Checkbutton] = []
        for r, (var, label, cap) in enumerate(self._rows):
            cb = ttk.Checkbutton(self, text=f"{tr('hotkey_dis')} {label}",
                                  variable=var, command=self._sync_row_states)
            cb.grid(row=r, column=0, sticky="w", padx=(0, 6), pady=1)
            cap.grid(row=r, column=1, sticky="w", pady=1)
            self._row_checkbuttons.append(cb)

        ttk.Checkbutton(
            self, text=tr("hk_all_dis"), variable=self._all_disabled_var,
            command=self._sync_row_states,
        ).grid(row=len(self._rows), column=0, columnspan=2, sticky="w", pady=(2, 0))

        self._sync_row_states()

    def _sync_row_states(self):
        all_off = bool(self._all_disabled_var.get())
        for cb, (var, _label, cap) in zip(self._row_checkbuttons, self._rows):
            cb.configure(state="disabled" if all_off else "normal")
            cap.set_row_enabled(not (all_off or bool(var.get())))

    def get_ini_value(self) -> dict:
        kbd   = self._kbd_cap.get_ini_value()
        hold  = self._hold_cap.get_ini_value()
        combo = self._combo_cap.get_ini_value()
        return {
            "kbd_code": kbd["code"],
            "kbd_modifier": kbd["modifier"],
            "kbd_disabled": bool(self._rows[0][0].get()),
            "hold_button": hold["gamepad_button"],
            "hold_seconds": hold["hold_seconds"],
            "hold_disabled": bool(self._rows[1][0].get()),
            "combo_button": combo["gamepad_button"],
            "combo_modifier_button": combo["gamepad_modifier_button"],
            "combo_disabled": bool(self._rows[2][0].get()),
            "all_disabled": bool(self._all_disabled_var.get()),
        }


# ── obCJK plugin ini [obCJK] 節的預設值 — 併入主視窗單一畫面，不再獨立分頁 ──

_OBCJK_SECTION_DEFAULTS = {
    "ActiveCodePage":    "BIG5",
    # 熱鍵：鍵盤/手把長按/手把組合鍵 3 組同時獨立生效，各自可停用，
    # 另有 AllDisabled 總開關（見 MultiHotkeyCapture）。
    "EditorHotkeyKbdCode":     "0x58",
    "EditorHotkeyKbdModifier": "Ctrl",
    "EditorHotkeyKbdDisabled": "0",
    "EditorHotkeyHoldButton":  "0",
    "EditorHotkeyHoldSeconds": "1.0",
    "EditorHotkeyHoldDisabled": "1",
    "EditorHotkeyComboButton": "0",
    "EditorHotkeyComboModifierButton": "4",
    "EditorHotkeyComboDisabled": "1",
    "EditorHotkeyAllDisabled": "0",
    "ImeHotkeyKbdCode":     "0x57",
    "ImeHotkeyKbdModifier": "None",
    "ImeHotkeyKbdDisabled": "0",
    "ImeHotkeyHoldButton":  "0",
    "ImeHotkeyHoldSeconds": "1.0",
    "ImeHotkeyHoldDisabled": "1",
    "ImeHotkeyComboButton": "0",
    "ImeHotkeyComboModifierButton": "4",
    "ImeHotkeyComboDisabled": "1",
    "ImeHotkeyAllDisabled": "0",
    "GamepadHotkeyEnable":      "0",
    "IMEMode":           "Out",
    "UILang":            DEFAULT_UI_LANG,
    "BackgroundOpacity": "0",
    "GlyphXAlign":       "0",
    "GlyphXAlignCChars": _GLYPH_X_ALIGN_C_DEFAULT_CHARS,
    "MenuQueEnable":     "1",
    "LootMenuEnable":    "1",
    "AsciiRenderEnable": "1",
    "TexSwapEnable":     "1",
    "LineBreakSpaceEnablePathA": "1",
    "LineBreakSpaceEnablePathBC": "1",
    "DebugLogEnable":    "0",
    "PathDiagEnable":    "0",
    "PathDiagCap":       "300",
    "PathDiagSlot78Only": "0",
    "LootMenuDiagEnable": "0",
    "LootMenuDiagCap":    "300",
    "HangWatchdogEnable": "0",
    "SaveDiagEnable":     "0",
    "NorthernUIEnable":   "0",
    "GamepadInputDiagEnable": "0",
}


def ensure_obcjk_section_defaults(cfg: configparser.ConfigParser,
                                   ini_path: "Path | None") -> None:
    """若 ini 不存在，或存在但缺少 [obCJK] 必要 key，立刻補寫預設值。"""
    if not ini_path:
        return
    if not cfg.has_section("obCJK"):
        cfg.add_section("obCJK")
    if ini_path.exists() and all(
        cfg.has_option("obCJK", k) for k in _OBCJK_SECTION_DEFAULTS
    ):
        return
    try:
        for k, v in _OBCJK_SECTION_DEFAULTS.items():
            if not cfg.has_option("obCJK", k):
                cfg.set("obCJK", k, v)
        write_ini(cfg, ini_path)
    except Exception:
        pass


def parse_obcjk_hotkey_raw(cfg: configparser.ConfigParser, key: str, default: str) -> str:
    """讀 [obCJK] 熱鍵 key（可能是 "0x58" 或十進位字串），轉成 HotkeyCapture 用的 2 位16進位字串。"""
    raw = cfg.get("obCJK", key, fallback=default).strip()
    try:
        if raw.lower().startswith("0x"):
            raw = f"{int(raw, 0):02X}"   # "0x58" → "58"
        else:
            raw = f"{int(raw):02X}"       # "88" decimal → "58"
    except ValueError:
        pass
    return raw


def parse_obcjk_hotkey_device(cfg: configparser.ConfigParser, key: str) -> tuple[str, int]:
    """讀 [obCJK] <key>Device / <key>GamepadButton，回傳 ("kbd"/"hold"/"combo", button_index)。
    缺 key（舊 ini）一律視為鍵盤；舊版單純"Gamepad"值（沒有長按/組合鍵之分的
    舊架構）語意跟新的hold/combo都不同，不強制轉換，同樣視為鍵盤，交給使用者
    重新設定手把熱鍵。"""
    device_raw = cfg.get("obCJK", f"{key}Device", fallback="Keyboard").strip().lower()
    device = {"gamepadhold": "hold", "gamepadcombo": "combo"}.get(device_raw, "kbd")
    try:
        button = int(cfg.get("obCJK", f"{key}GamepadButton", fallback="0").strip() or "0")
    except ValueError:
        button = 0
    return device, max(0, min(37, button))


def parse_obcjk_hotkey_gamepad_extra(cfg: configparser.ConfigParser, key: str) -> tuple[int, float]:
    """讀 [obCJK] <key>GamepadModifierButton(組合鍵左鍵) / <key>HoldSeconds(長按秒數)。"""
    try:
        mod_button = int(cfg.get("obCJK", f"{key}GamepadModifierButton", fallback="4").strip() or "4")
    except ValueError:
        mod_button = 4
    if mod_button not in _GAMEPAD_COMBO_MODIFIER_CHOICES:
        mod_button = _GAMEPAD_COMBO_MODIFIER_CHOICES[0]
    try:
        hold_seconds = float(cfg.get("obCJK", f"{key}HoldSeconds", fallback="1.0").strip() or "1.0")
    except ValueError:
        hold_seconds = 1.0
    return mod_button, max(0.1, min(10.0, hold_seconds))


def parse_obcjk_hotkey_modifier(cfg: configparser.ConfigParser, key: str, default: str) -> str:
    """讀 [obCJK] <key>Modifier（"None"/"Ctrl"/"Shift"），回傳 HotkeyCapture 用的
    "none"/"ctrl"/"shift" token。default 是找不到 key 時的 token（非 ini 字串）。"""
    raw = cfg.get("obCJK", f"{key}Modifier", fallback="").strip().lower()
    return raw if raw in ("none", "ctrl", "shift") else default


def parse_obcjk_hotkey_multi(cfg: configparser.ConfigParser, key: str,
                              default_kbd_raw: str, default_kbd_modifier: str) -> dict:
    """讀 [obCJK] <key>Kbd*/<key>Hold*/<key>Combo*/<key>AllDisabled 這組新版
    「3組同時綁定」格式，回傳可直接展開餵給 MultiHotkeyCapture() 的 dict。

    若這些新 key 通通不存在（舊版 ini，只有 <key>Device 那一份單一互斥綁定），
    改用 parse_obcjk_hotkey_device()/_modifier()/_gamepad_extra() 讀舊格式並
    搬過來：舊綁定當時實際生效的那個裝置→保留設定內容且啟用，其餘兩個裝置
    →預設停用，讓使用者原本設定的熱鍵不會憑空消失，之後才需要手動另外設定
    第二、第三組綁定。"""
    has_new = (cfg.has_option("obCJK", f"{key}KbdCode")
               or cfg.has_option("obCJK", f"{key}HoldButton")
               or cfg.has_option("obCJK", f"{key}ComboButton"))

    if not has_new:
        old_device, old_button = parse_obcjk_hotkey_device(cfg, key)
        old_modifier = parse_obcjk_hotkey_modifier(cfg, key, "none")
        old_mod_btn, old_hold_s = parse_obcjk_hotkey_gamepad_extra(cfg, key)
        old_kbd_raw = parse_obcjk_hotkey_raw(cfg, key, default_kbd_raw)
        return {
            "kbd_code":     old_kbd_raw if old_device == "kbd" else default_kbd_raw,
            "kbd_modifier": old_modifier if old_device == "kbd" else default_kbd_modifier,
            "kbd_disabled": old_device != "kbd",
            "hold_button":  old_button if old_device == "hold" else 0,
            "hold_seconds": old_hold_s,
            "hold_disabled": old_device != "hold",
            "combo_button": old_button if old_device == "combo" else 0,
            "combo_modifier_button": old_mod_btn,
            "combo_disabled": old_device != "combo",
            "all_disabled": False,
        }

    try:
        hold_button = int(cfg.get("obCJK", f"{key}HoldButton", fallback="0").strip() or "0")
    except ValueError:
        hold_button = 0
    try:
        combo_button = int(cfg.get("obCJK", f"{key}ComboButton", fallback="0").strip() or "0")
    except ValueError:
        combo_button = 0
    try:
        hold_seconds = float(cfg.get("obCJK", f"{key}HoldSeconds", fallback="1.0").strip() or "1.0")
    except ValueError:
        hold_seconds = 1.0
    try:
        combo_mod_btn = int(cfg.get("obCJK", f"{key}ComboModifierButton", fallback="4").strip() or "4")
    except ValueError:
        combo_mod_btn = 4
    if combo_mod_btn not in _GAMEPAD_COMBO_MODIFIER_CHOICES:
        combo_mod_btn = _GAMEPAD_COMBO_MODIFIER_CHOICES[0]

    def _flag(opt: str, default: bool) -> bool:
        try:
            return bool(cfg.getint("obCJK", opt, fallback=1 if default else 0))
        except ValueError:
            return default

    return {
        "kbd_code":     parse_obcjk_hotkey_raw(cfg, f"{key}KbdCode", default_kbd_raw),
        "kbd_modifier": parse_obcjk_hotkey_modifier(cfg, f"{key}Kbd", default_kbd_modifier),
        "kbd_disabled": _flag(f"{key}KbdDisabled", False),
        "hold_button":  max(0, min(37, hold_button)),
        "hold_seconds": max(0.1, min(10.0, hold_seconds)),
        "hold_disabled": _flag(f"{key}HoldDisabled", True),
        "combo_button": max(0, min(37, combo_button)),
        "combo_modifier_button": combo_mod_btn,
        "combo_disabled": _flag(f"{key}ComboDisabled", True),
        "all_disabled": _flag(f"{key}AllDisabled", False),
    }


# ── 規則C標點清單：唯讀預覽 + 表格編輯視窗 ────────────────────────────────

class GlyphXAlignCCharsPicker(ttk.Frame):
    """規則C(GlyphXAlign=2)標點清單原本是一個直接編輯整串字元的Entry，改成
    唯讀預覽 + 「編輯...」按鈕，點擊後彈出序號/符號/UTF8碼三欄表格視窗做
    新增/刪除，比在一行Entry裡直接打整串字元清楚。var（StringVar）仍是
    _build_content()/存檔邏輯唯一讀取的來源，這裡只是換一種編輯介面，不
    改變資料流，存檔那端完全不用跟著改。"""

    def __init__(self, parent, var: tk.StringVar):
        super().__init__(parent)
        self._var = var
        ttk.Entry(
            self, textvariable=var, width=16, state="readonly",
        ).pack(side="left")
        ttk.Button(
            self, text=tr("glyph_x_align_c_chars_edit_btn"), command=self._open_dialog,
        ).pack(side="left", padx=(4, 0))

    @staticmethod
    def _code_of(ch: str) -> str:
        return ch.encode("utf-8").hex(" ").upper()

    def _open_dialog(self):
        dlg = tk.Toplevel(self)
        dlg.title(tr("glyph_x_align_c_chars_dlg_title"))
        dlg.transient(self.winfo_toplevel())
        dlg.grab_set()
        dlg.resizable(False, True)
        dlg.rowconfigure(0, weight=1)
        dlg.columnconfigure(0, weight=1)

        tree = ttk.Treeview(
            dlg, columns=("idx", "char", "code"), show="headings", height=10,
        )
        tree.heading("idx",  text=tr("glyph_x_align_c_chars_col_idx"))
        tree.heading("char", text=tr("glyph_x_align_c_chars_col_char"))
        tree.heading("code", text=tr("glyph_x_align_c_chars_col_code"))
        tree.column("idx",  width=50,  anchor="center")
        tree.column("char", width=70,  anchor="center")
        tree.column("code", width=140, anchor="center")
        tree.grid(row=0, column=0, columnspan=3, sticky="nsew", padx=(10, 0), pady=(10, 4))

        scroll = ttk.Scrollbar(dlg, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=scroll.set)
        scroll.grid(row=0, column=3, sticky="ns", padx=(0, 10), pady=(10, 4))

        def _reload(chars: str):
            tree.delete(*tree.get_children())
            for i, ch in enumerate(chars, start=1):
                tree.insert("", "end", iid=str(i - 1), values=(i, ch, self._code_of(ch)))

        _reload(self._var.get())

        def _current_chars() -> list[str]:
            return [tree.set(iid, "char") for iid in tree.get_children()]

        def _renumber():
            for i, iid in enumerate(tree.get_children(), start=1):
                tree.set(iid, "idx", i)

        add_frame = ttk.Frame(dlg)
        add_frame.grid(row=1, column=0, columnspan=4, sticky="ew", padx=10, pady=(0, 4))
        ttk.Label(add_frame, text=tr("glyph_x_align_c_chars_new_lbl")).pack(side="left")
        new_var = tk.StringVar()
        new_entry = ttk.Entry(add_frame, textvariable=new_var, width=6)
        new_entry.pack(side="left", padx=(4, 8))

        def _on_add(_event=None):
            text = new_var.get()
            if not text:
                return
            # 一次只加輸入框裡的第一個字元——貼上整串字時，逐一分開新增比
            # 猜測使用者是要一個符號還是整段字串安全。
            chars = _current_chars()
            chars.append(text[0])
            _reload("".join(chars))
            new_var.set("")
            new_entry.focus_set()

        new_entry.bind("<Return>", _on_add)

        def _on_delete():
            for iid in tree.selection():
                tree.delete(iid)
            _renumber()

        ttk.Button(add_frame, text=tr("glyph_x_align_c_chars_add_btn"),
                   command=_on_add).pack(side="left")
        ttk.Button(add_frame, text=tr("glyph_x_align_c_chars_del_btn"),
                   command=_on_delete).pack(side="left", padx=(6, 0))

        btn_frame = ttk.Frame(dlg)
        btn_frame.grid(row=2, column=0, columnspan=4, sticky="e", padx=10, pady=(0, 10))

        def _on_ok():
            self._var.set("".join(_current_chars()))
            dlg.destroy()

        ttk.Button(btn_frame, text=tr("glyph_x_align_c_chars_cancel_btn"),
                   command=dlg.destroy).pack(side="left", padx=(0, 6))
        ttk.Button(btn_frame, text=tr("glyph_x_align_c_chars_ok_btn"),
                   command=_on_ok).pack(side="left")

        dlg.update_idletasks()
        pw = self.winfo_toplevel()
        x = pw.winfo_x() + (pw.winfo_width()  - dlg.winfo_width())  // 2
        y = pw.winfo_y() + (pw.winfo_height() - dlg.winfo_height()) // 2
        dlg.geometry(f"+{x}+{y}")
        dlg.focus_set()


# ── iOS 風格開關（1=綠底+右側白圈打勾／0=紅底+左側白圈打叉）──────────────────

class ToggleSwitch(tk.Canvas):
    """MenuQueEnable/LootMenuEnable 用的圓圈開關：圓圈在右＝1(啟用/綠)，
    圓圈在左＝0(停用/紅)。disabled 時灰階顯示且不回應點擊（LootMenu 依附
    MenuQue 時鎖用，見 App._sync_lootmenu_state()）。"""

    _W, _H, _PAD = 50, 24, 2

    def __init__(self, parent, variable: tk.IntVar, command=None, **kwargs):
        super().__init__(parent, width=self._W, height=self._H,
                          highlightthickness=0, bd=0, **kwargs)
        self._var     = variable
        self._command = command
        self._enabled = True
        self.bind("<Button-1>", self._on_click)
        self._var.trace_add("write", lambda *_: self._redraw())
        self._redraw()

    def _on_click(self, _event=None):
        if not self._enabled:
            return
        self._var.set(0 if self._var.get() else 1)
        if self._command:
            self._command()

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = enabled
        self.configure(cursor="hand2" if enabled else "arrow")
        self._redraw()

    def _redraw(self):
        self.delete("all")
        on = bool(self._var.get())
        w, h, pad = self._W, self._H, self._PAD
        r = h / 2
        if not self._enabled:
            body_color, knob_color, mark_color = "#c8c8c8", "#f5f5f5", "#9a9a9a"
        elif on:
            body_color, knob_color, mark_color = "#43a047", "#ffffff", "#43a047"
        else:
            body_color, knob_color, mark_color = "#e53935", "#ffffff", "#e53935"

        # 藥丸形背景：兩個半圓端點 + 中間矩形拼接
        self.create_oval(0, 0, h, h, fill=body_color, outline=body_color)
        self.create_oval(w - h, 0, w, h, fill=body_color, outline=body_color)
        self.create_rectangle(r, 0, w - r, h, fill=body_color, outline=body_color)

        # 圓圈本體：1 貼右／0 貼左
        kr = r - pad
        cx = (w - r) if on else r
        cy = r
        self.create_oval(cx - kr, cy - kr, cx + kr, cy + kr,
                          fill=knob_color, outline=knob_color)

        # 圓圈內的勾/叉
        m = kr * 0.5
        if on:
            self.create_line(cx - m, cy, cx - m * 0.2, cy + m * 0.8,
                              cx + m, cy - m * 0.6,
                              fill=mark_color, width=2,
                              capstyle="round", joinstyle="round")
        else:
            self.create_line(cx - m, cy - m, cx + m, cy + m,
                              fill=mark_color, width=2, capstyle="round")
            self.create_line(cx - m, cy + m, cx + m, cy - m,
                              fill=mark_color, width=2, capstyle="round")


def _get_work_area() -> "tuple[int, int, int, int]":
    """回傳Windows「工作區域」(不含工作列)的(left, top, right, bottom)。用
    SPI_GETWORKAREA(0x0030)向系統直接要目前的實際值，不猜工作列固定高度——
    工作列可能貼在螢幕上/下/左/右任一邊、高度也因DPI縮放或使用者手動拉大
    （Windows 11「工作列大小」設定、或直接拖曳邊界）而不同，並沒有單一
    「預設值」可以寫死；100% DPI下常見是40px，但放大DPI或設定成「大」之後
    很容易變成48/56/64px以上。tkinter的winfo_screenwidth/height()回傳的是
    整個螢幕解析度，不會扣掉工作列，這是視窗被工作列擋住的根本原因。"""
    rect = ctypes.wintypes.RECT()
    ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0)
    return rect.left, rect.top, rect.right, rect.bottom


# ── 主視窗 ────────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("obCJK ini 設定")
        self.resizable(True, True)

        self._current_cp    = self._init_active_codepage()
        self._cp_var        = tk.StringVar(value=self._current_cp)
        self._pending: "tuple[ModeTab, int] | None" = None
        self._font_saved    = False   # True if font DLL ini was written
        self._glyph_changed = False   # True if any _1/_2 field changed

        # 凍結監控（除錯用）狀態——跟著 App 本身活，不隨 _build_content() 重建
        # 選項頁而重置，見 _ensure_hang_watchdog_state()。
        self._watchdog_after_id = None
        self._watchdog_log      = None
        self.protocol("WM_DELETE_WINDOW", self._on_app_close)

        self._init_ui_language()
        self._init_northernui_mode()

        self._selector_frame = ttk.Frame(self)
        self._selector_frame.pack(fill="x")
        self._build_codepage_selector()

        self._content_frame = ttk.Frame(self)
        self._content_frame.pack(fill="both", expand=True)

        self._load_codepage(self._current_cp)
        self.update_idletasks()
        # 用工作區域（扣掉工作列）置中，而不是 winfo_screenwidth/height()
        # 回傳的整個螢幕解析度——後者不知道工作列存在，視窗長高後即使有
        # 置中也可能被工作列擋住底部（見 _get_work_area() 說明）。
        wa_left, wa_top, wa_right, wa_bottom = _get_work_area()
        wa_w = wa_right - wa_left
        wa_h = wa_bottom - wa_top
        ww   = min(self.winfo_reqwidth(), wa_w - 80)
        wh   = min(self.winfo_reqheight(), wa_h - 80)
        x    = wa_left + (wa_w - ww) // 2
        y    = wa_top  + (wa_h - wh) // 2
        self.geometry(f"{ww}x{wh}+{x}+{y}")

    def _init_active_codepage(self) -> str:
        """在建立編碼選擇 UI 之前，讀一次 ini 的 [obCJK] ActiveCodePage，決定
        開啟時預設選取哪個編碼——跟 _init_ui_language() 同樣的讀取時機，確保
        單選鈕/內容分頁的初始值跟上次儲存時選的編碼一致，而不是永遠回到 BIG5。"""
        ini_path = (
            _OBL_DIR / "Data" / "OBSE" / "Plugins" / "obCJK" / "obCJK.ini"
            if _OBL_DIR else None
        )
        cfg = read_ini(ini_path) if ini_path else configparser.ConfigParser()
        cp = cfg.get("obCJK", "ActiveCodePage", fallback="BIG5") if cfg.has_section("obCJK") else "BIG5"
        return cp if cp in SELECTABLE_CP else "BIG5"

    def _init_ui_language(self) -> None:
        """在建立任何用到 tr() 的元件之前，讀一次 ini 的 [obCJK] UILang，設定
        全域顯示語言 _UI_LANG。跟使用者正在編輯哪個編碼（BIG5/GBK/SJIS/KOREAN）
        完全脫鉤，只在這裡跟語言下拉選單變更時（_on_ui_language_change）更新。"""
        global _UI_LANG
        ini_path = (
            _OBL_DIR / "Data" / "OBSE" / "Plugins" / "obCJK" / "obCJK.ini"
            if _OBL_DIR else None
        )
        cfg = read_ini(ini_path) if ini_path else configparser.ConfigParser()
        code = cfg.get("obCJK", "UILang", fallback=DEFAULT_UI_LANG) if cfg.has_section("obCJK") else DEFAULT_UI_LANG
        _UI_LANG = code if code in LANGUAGES else DEFAULT_UI_LANG

    def _init_northernui_mode(self) -> None:
        """讀一次 ini 的 [obCJK] NorthernUIEnable，決定字型分頁要不要多開
        NorthernUI 專屬分頁——跟 _init_ui_language() 同樣的讀取時機，存成
        self._nui_mode（純 App 實例屬性，不像 _UI_LANG 需要給 tr() 全域讀取）。"""
        ini_path = (
            _OBL_DIR / "Data" / "OBSE" / "Plugins" / "obCJK" / "obCJK.ini"
            if _OBL_DIR else None
        )
        cfg = read_ini(ini_path) if ini_path else configparser.ConfigParser()
        mode = cfg.get("obCJK", "NorthernUIEnable", fallback="0") if cfg.has_section("obCJK") else "0"
        self._nui_mode = mode if mode in NORTHERNUI_MODES else "0"

    def _build_codepage_selector(self):
        """建到 self._selector_frame 內；顯示語言切換時會清空重建，讓這裡的
        tr() 文字（LabelFrame 標題、顯示語言下拉選單本身）套用新語言。"""
        for widget in self._selector_frame.winfo_children():
            widget.destroy()

        sel_frame = ttk.LabelFrame(self._selector_frame, text=tr("cp_frame"))
        sel_frame.pack(fill="x", padx=4, pady=(4, 0))

        for cp_name in SELECTABLE_CP:
            cfg_info = CODEPAGE_CONFIG[cp_name]
            label    = f"{cp_name}  {cfg_info['lang']}"
            state    = "normal"
            ttk.Radiobutton(
                sel_frame, text=label,
                variable=self._cp_var, value=cp_name,
                command=self._on_codepage_change,
                state=state,
            ).pack(side="left", padx=10, pady=4)

        ttk.Label(sel_frame, text=tr("korean_note"),
                  foreground="gray").pack(side="left", padx=(0, 8))

        # 顯示語言下拉選單 — 跟上面編碼單選鈕完全脫鉤，見 _init_ui_language()/
        # _on_ui_language_change() 說明。
        lang_frame = ttk.Frame(self._selector_frame)
        lang_frame.pack(fill="x", padx=4, pady=(2, 2))
        ttk.Label(lang_frame, text=tr("ui_lang_lbl")).pack(side="left", padx=(6, 4))
        self._lang_codes = sorted(LANGUAGES.keys())
        self._lang_names = [LANGUAGES[c]["display_name"] for c in self._lang_codes]
        self._lang_var = tk.StringVar(
            value=LANGUAGES.get(_UI_LANG, {}).get("display_name", _UI_LANG))
        lang_combo = ttk.Combobox(
            lang_frame, textvariable=self._lang_var, values=self._lang_names,
            state="readonly", width=_combo_width(self._lang_names))
        lang_combo.pack(side="left")
        lang_combo.bind("<<ComboboxSelected>>", self._on_ui_language_change)

        # NorthernUI 相容模式下拉選單 — 跟顯示語言同一列但邏輯脫鉤，見
        # _init_northernui_mode()/_on_northernui_mode_change()。決定字型分頁
        # 要不要多開 SLOT9/10 佔位分頁 + 5 個 NorthernUI 角色分頁。
        self._nui_mode_labels = {
            "0": tr("northernui_opt_vanilla"),
            "1": tr("northernui_opt_official"),
            "2": tr("northernui_opt_patched"),
        }
        self._nui_mode_by_label = {v: k for k, v in self._nui_mode_labels.items()}
        ttk.Label(lang_frame, text=tr("northernui_lbl")).pack(side="left", padx=(16, 4))
        self._nui_mode_var = tk.StringVar(value=self._nui_mode_labels[self._nui_mode])
        nui_combo = ttk.Combobox(
            lang_frame, textvariable=self._nui_mode_var,
            values=list(self._nui_mode_labels.values()),
            state="readonly", width=_combo_width(self._nui_mode_labels.values()))
        nui_combo.pack(side="left")
        nui_combo.bind("<<ComboboxSelected>>", self._on_northernui_mode_change)

    def _on_codepage_change(self):
        self._load_codepage(self._cp_var.get())

    def _on_ui_language_change(self, _event=None):
        global _UI_LANG
        try:
            idx = self._lang_names.index(self._lang_var.get())
        except ValueError:
            return
        code = self._lang_codes[idx]
        if code == _UI_LANG:
            return
        _UI_LANG = code
        if getattr(self, "ini_path", None):
            try:
                if not self.cfg.has_section("obCJK"):
                    self.cfg.add_section("obCJK")
                self.cfg.set("obCJK", "UILang", code)
                write_ini(self.cfg, self.ini_path)
            except Exception:
                pass
        self._build_codepage_selector()
        self._load_codepage(self._current_cp)

    def _on_northernui_mode_change(self, _event=None):
        code = self._nui_mode_by_label.get(self._nui_mode_var.get(), "0")
        if code == self._nui_mode:
            return
        self._nui_mode = code
        if getattr(self, "ini_path", None):
            try:
                if not self.cfg.has_section("obCJK"):
                    self.cfg.add_section("obCJK")
                self.cfg.set("obCJK", "NorthernUIEnable", code)
                write_ini(self.cfg, self.ini_path)
            except Exception:
                pass
        self._load_codepage(self._current_cp)

    def _load_codepage(self, cp_name: str):
        cfg_info   = CODEPAGE_CONFIG[cp_name]
        ok, errmsg = check_obcjk_dll()
        if not ok:
            messagebox.showerror("錯誤", errmsg)
            self._cp_var.set(self._current_cp)
            return

        section = cfg_info["section"]

        # 單一 obCJK.ini：字體參數（[BIG5]/[GBK]/[SJIS]/[KOREAN]）跟外掛熱鍵/
        # IME模式（[obCJK]）現在是同一個 ConfigParser、同一個檔案，不再分開
        # 存在 Documents\My Games\Oblivion\ 跟 Data\OBSE\Plugins\obCJK\ 兩處。
        ini_path = (
            _OBL_DIR / "Data" / "OBSE" / "Plugins" / "obCJK" / "obCJK.ini"
            if _OBL_DIR else None
        )
        cfg = read_ini(ini_path) if ini_path else configparser.ConfigParser()
        if not cfg.has_section(section):
            cfg.add_section(section)
        ensure_obcjk_section_defaults(cfg, ini_path)

        print(f"[列舉] 正在掃描 {cfg_info['lang']} 字體 (charset=0x{cfg_info['charset']:02X})…")
        fonts_cjk = enum_cjk_fonts(cfg_info["charset"])
        print(f"[列舉] 找到 {len(fonts_cjk)} 個")
        print(f"[列舉] 正在掃描 ASCII 字體 (charset=0x{ANSI_CHARSET:02X})…")
        fonts_ascii = enum_cjk_fonts(ANSI_CHARSET)
        print(f"[列舉] 找到 {len(fonts_ascii)} 個")

        for widget in self._content_frame.winfo_children():
            widget.destroy()

        self._current_cp     = cp_name
        self.cfg             = cfg
        self.ini_path        = ini_path
        self.section         = section
        self.fonts_cjk        = fonts_cjk
        self.fonts_ascii      = fonts_ascii
        self._font_mode       = "cjk"
        self.fonts           = fonts_cjk
        self._pending        = None

        self.title(f"obCJK ini 設定 — {cfg_info['lang']} ({cp_name})")
        self._build_content()

    def _make_scrollable(self, parent) -> "tuple[tk.Canvas, ttk.Frame]":
        """在 parent 內放一個可垂直捲動的容器（Canvas+Scrollbar，滑鼠移進去時
        滾輪可用），回傳 (canvas, inner)，inner 是給呼叫端塞內容用的 frame。
        動機：Font 頁跟 Options 頁的內容都是直接 pack/grid 進頁面，完全沒有
        捲動機制，而 App.__init__ 會用 wh = min(reqheight, sh-80) 把整個視窗
        高度夾在螢幕高度以內——一旦頁面內容（例如 hint 文字換行變多行、或
        英文字型參數欄位比中文寬）比螢幕還高，超出視窗底部的控制項就會被
        直接裁掉且永遠點不到。回傳 canvas 是讓呼叫端在內容建好後可以用
        winfo_reqheight() 設定 canvas 的初始高度。"""
        canvas = tk.Canvas(parent, highlightthickness=0)
        vsb = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vsb.set)
        canvas.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")

        inner = ttk.Frame(canvas)
        inner_id = canvas.create_window((0, 0), window=inner, anchor="nw")

        def _on_inner_configure(_e=None):
            canvas.configure(scrollregion=canvas.bbox("all"))

        def _on_canvas_configure(e):
            canvas.itemconfigure(inner_id, width=e.width)

        inner.bind("<Configure>", _on_inner_configure)
        canvas.bind("<Configure>", _on_canvas_configure)

        def _on_mousewheel(e):
            canvas.yview_scroll(int(-1 * (e.delta / 120)), "units")

        canvas.bind("<Enter>", lambda _e: canvas.bind_all("<MouseWheel>", _on_mousewheel))
        canvas.bind("<Leave>", lambda _e: canvas.unbind_all("<MouseWheel>"))
        # 切換編碼會整個 destroy 掉舊 canvas（見 _load_codepage()），若滑鼠當時
        # 停在 Options 頁上，<Leave> 不保證先於 destroy 觸發，這裡補一道保險。
        canvas.bind("<Destroy>", lambda _e: canvas.unbind_all("<MouseWheel>"))

        return canvas, inner

    def _build_content(self):
        pad = {"padx": 6, "pady": 4}
        sec = self.section
        cfg_info = CODEPAGE_CONFIG[self._current_cp]

        # 頂層兩個分頁：「字型」（字體清單/參數/預覽）跟「選項」（熱鍵/IME/
        # 造字背景不透明度/標點對齊），儲存按鈕跟ini路徑放在分頁外，兩頁共用。
        self._pages_nb = ttk.Notebook(self._content_frame)
        self._pages_nb.pack(fill="both", expand=True)

        font_page = ttk.Frame(self._pages_nb, padding=4)
        self._pages_nb.add(font_page, text=tr("page_font"))
        font_canvas, font_inner = self._make_scrollable(font_page)
        self._build_font_page(font_inner, sec, cfg_info, pad)

        options_page = ttk.Frame(self._pages_nb, padding=4)
        self._pages_nb.add(options_page, text=tr("page_options"))
        options_canvas, options_inner = self._make_scrollable(options_page)
        self._build_options_page(options_inner)

        # 讓兩個捲動容器一開始就撐到各自內容的實際高度——視窗建立完成後若
        # 工作區域不夠高，App.__init__ 會把整個視窗高度夾到 wa_h-80，此時
        # canvas 會被 pack(fill="both", expand=True) 壓縮，多出來的內容就靠
        # 捲軸捲到，而不是像先前那樣直接被視窗邊界裁掉、永遠點不到/看不到
        # （見 _make_scrollable() 說明）。
        #
        # 但這裡不能直接無上限用 inner.winfo_reqheight()：pack 是按呼叫順序
        # 分配空間，Notebook（含這個 canvas）排在下面「儲存 ini」btn_frame
        # 前面，內容一旦比螢幕還高，Notebook 會先把自己要的高度全部吃光，
        # btn_frame 分不到剩餘空間就被擠出視窗底部（看不到也點不到，使用者
        # 回報「捲動只包含字型改變，儲存那列不在捲動範圍內」正是這個原因）。
        # 修法：把 canvas 高度夾在「螢幕高度扣掉這個固定外殼預留值」以內，
        # 超出的部份一律交給 canvas 自己的捲軸捲——這樣不管內容多高，
        # btn_frame 永遠拿得到自己要的高度，維持在視窗最下方可見。
        # _CHROME_RESERVE_H 是編碼選單列+顯示語言列+分頁籤列+底部儲存列的
        # 高度粗估（非精確量測，故意抓寬鬆），內容不夠高時 min() 會自然
        # 選到 reqheight 那邊，跟原本行為完全一樣，不影響一般情況。
        self.update_idletasks()
        _, wa_top, _, wa_bottom = _get_work_area()
        budget = max(240, (wa_bottom - wa_top) - 80 - _CHROME_RESERVE_H)
        font_canvas.configure(
            width=font_inner.winfo_reqwidth(),
            height=min(font_inner.winfo_reqheight(), budget),
        )
        options_canvas.configure(
            width=options_inner.winfo_reqwidth(),
            height=min(options_inner.winfo_reqheight(), budget),
        )

        btn_frame = ttk.Frame(self._content_frame)
        btn_frame.pack(fill="x", **pad)
        ttk.Button(btn_frame, text=tr("save_ini"),
                   command=self._save_font).pack(side="right", padx=8)
        ttk.Label(btn_frame, text=str(self.ini_path),
                  foreground="gray").pack(side="left", padx=4)

    def _build_font_page(self, font_frame_, sec, cfg_info, pad):
        lf = ttk.LabelFrame(font_frame_,
                             text=tr("font_frame").format(lang=cfg_info["lang"]))
        # rowspan=2：跟右欄「SLOT參數面板(row0)+預覽窗口(row1)」疊起來的總高度對齊，
        # 而不是只跟row0(SLOT參數面板)一樣高——見obcjk_glyphxalign_rulecd_design
        # 記憶2026-07-28版面重排指示（使用者截圖標註）。
        lf.grid(row=0, column=0, rowspan=2, sticky="nsew", **pad)
        self._font_lf = lf

        mode_frame = ttk.Frame(lf)
        mode_frame.pack(fill="x", pady=(0, 4))
        self._font_mode_var = tk.StringVar(value=self._font_mode)
        ttk.Radiobutton(mode_frame, text=tr("font_mode_cjk"), value="cjk",
                         variable=self._font_mode_var,
                         command=self._on_font_mode_change).pack(side="left")
        ttk.Radiobutton(mode_frame, text=tr("font_mode_ascii"), value="ascii",
                         variable=self._font_mode_var,
                         command=self._on_font_mode_change).pack(side="left", padx=(8, 0))

        list_frame = ttk.Frame(lf)
        list_frame.pack(fill="both", expand=True)
        # height=24（原14）：font頁包在_make_scrollable()的固定高度Canvas裡，
        # 該Canvas高度用winfo_reqheight()一次性算死，grid的rowspan/weight對
        # 「捲動容器多高」沒有作用——只能直接加大這裡跟下面預覽canvas的基礎
        # 高度數字，reqheight才會跟著變大。見obcjk_glyphxalign_rulecd_design
        # 記憶2026-07-28使用者回報「拉不高」的後續修正。
        self.lb = tk.Listbox(list_frame, width=30, height=24, exportselection=False)
        sb = ttk.Scrollbar(list_frame, command=self.lb.yview)
        self.lb.configure(yscrollcommand=sb.set)
        self.lb.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        for f in self.fonts:
            self.lb.insert("end", f)
        self.lb.bind("<<ListboxSelect>>", self._on_font_select)

        right_frame = ttk.Frame(font_frame_)
        right_frame.grid(row=0, column=1, sticky="nsew", **pad)
        font_frame_.columnconfigure(1, weight=1)

        # 分頁列固定分成「兩排」，不是單一排靠自動換行延伸：第一排＝既有SLOT
        # （SLOT1/2/3/5/7/8），第二排＝SLOT9佔位（保留未來用，鎖住點不到）+5個
        # NorthernUI角色（僅NorthernUI模式非「原版/其他UI」時才出現）。
        # ttk.Notebook本身的分頁列只會依可視寬度自動換行，換行位置不可控，不
        # 符合這個固定兩排分組的需求，改用兩排ttk.Radiobutton(style=
        # "Toolbutton"，外觀類似分頁按鈕)手動切換，共用同一塊content_area，
        # 用tkraise()顯示目前選取的那個ModeTab/佔位框。
        self._tab_nav_row1 = ttk.Frame(right_frame)
        self._tab_nav_row1.pack(fill="x")
        self._tab_nav_row2 = ttk.Frame(right_frame)
        self._tab_nav_row2.pack(fill="x", pady=(2, 0))

        content_area = ttk.Frame(right_frame)
        content_area.pack(fill="both", expand=True, pady=(4, 0))
        content_area.columnconfigure(0, weight=1)
        content_area.rowconfigure(0, weight=1)

        self._tab_var = tk.StringVar()
        self._tab_frames: dict[str, tk.Widget] = {}
        self._SLOT_TABS: list[ModeTab] = []

        def _add_tab(row_frame, tab_id, label, frame_widget, selectable=True):
            frame_widget.grid(row=0, column=0, sticky="nsew")
            self._tab_frames[tab_id] = frame_widget
            btn = ttk.Radiobutton(
                row_frame, text=label, variable=self._tab_var, value=tab_id,
                style="Toolbutton", command=lambda tid=tab_id: self._on_tab_select(tid))
            if not selectable:
                btn.state(["disabled"])
            btn.pack(side="left", padx=1, pady=1)

        defs = MODE_DEFAULTS.get(self._current_cp, _BIG5_DEFS)
        first_tab_id = None
        for (tab_name, prefix), (half_def, full_def) in zip(slot_tabs(), defs):
            tab = ModeTab(content_area, prefix, self.cfg, self._request_apply,
                          half_def, full_def, sec=sec,
                          on_param_change=self._schedule_preview)
            _add_tab(self._tab_nav_row1, prefix, tab_name, tab)
            self._SLOT_TABS.append(tab)
            if first_tab_id is None:
                first_tab_id = prefix

        # 第二排：SLOT9佔位（保留未來用）+5個NorthernUI角色分頁
        # （FontParam33~37，見 _NORTHERNUI_ROLES），預設值沿用SLOT1。
        if self._nui_mode != "0":
            placeholder = ttk.Frame(content_area)
            ttk.Label(placeholder, text=tr("slot_reserved_desc"),
                      foreground="gray").pack(padx=20, pady=20)
            _add_tab(self._tab_nav_row2, "slot9_reserved",
                     _slot_label(9, "slot_reserved_desc"), placeholder, selectable=False)

            nui_half_def, nui_full_def = defs[0]  # 沿用 SLOT1 預設值當起點
            for engine_id, role_name in _NORTHERNUI_ROLES:
                prefix = f"FontParam{engine_id}"
                nui_tab = ModeTab(content_area, prefix, self.cfg, self._request_apply,
                                   nui_half_def, nui_full_def, sec=sec,
                                   on_param_change=self._schedule_preview)
                _add_tab(self._tab_nav_row2, prefix, f"NorthernUI\n{role_name}", nui_tab)
                self._SLOT_TABS.append(nui_tab)

        self._tab_var.set(first_tab_id)
        self._tab_frames[first_tab_id].tkraise()

        # 只佔右欄(column=1)，緊接在SLOT參數面板(right_frame, row0)正下方，
        # 不再橫跨整列——避免蓋住左欄拉長後的字體清單(lf, rowspan=2)。
        preview_lf = ttk.LabelFrame(font_frame_, text=tr("preview"))
        preview_lf.grid(row=1, column=1, sticky="nsew", **pad)
        font_frame_.rowconfigure(1, weight=1)
        # height=260（原120）：同上，固定高度捲動Canvas不會動態撐開，直接加大
        # 這裡的基礎高度數字，避免CJK預覽文字被裁掉。
        self._preview_canvas = tk.Canvas(preview_lf, bg="white",
                                         bd=0, highlightthickness=0, height=260)
        self._preview_canvas.pack(fill="both", expand=True, padx=4, pady=4)

        disp_frame = ttk.Frame(font_frame_)
        disp_frame.grid(row=2, column=0, columnspan=2, sticky="ew", **pad)

        # 造字背景不透明度（[obCJK] BackgroundOpacity，0..100）—
        # 0=透明背景（ObCJKApplyDensityContrast 保證背景 alpha 恆為0），
        # 100=全黑底，中間值＝半透明黑底，見 obCJK_GlyphAtlas.h 的
        # ObCJKCompositeGlyphPixel()。
        ttk.Label(disp_frame, text=tr("bg_opacity_lbl")).grid(
            row=0, column=0, sticky="e", padx=(8, 4), pady=6)
        cur_bg_opacity = self.cfg.getint("obCJK", "BackgroundOpacity", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        cur_bg_opacity = max(0, min(100, cur_bg_opacity))
        self._bg_opacity_var = tk.IntVar(value=cur_bg_opacity)
        self._bg_opacity_pct_label = ttk.Label(disp_frame, text=f"{cur_bg_opacity}%", width=5)

        def _on_bg_opacity_change(raw):
            val = round(float(raw))
            self._bg_opacity_pct_label.config(text=f"{val}%")
            self._schedule_preview()

        ttk.Scale(
            disp_frame, from_=0, to=100, orient="horizontal",
            variable=self._bg_opacity_var, length=160,
            command=_on_bg_opacity_change,
        ).grid(row=0, column=1, padx=(0, 4), pady=6, sticky="w")
        self._bg_opacity_pct_label.grid(row=0, column=1, padx=(166, 0), pady=6, sticky="w")
        ttk.Label(disp_frame, text=tr("bg_opacity_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=0, column=2, sticky="w", padx=(0, 8))

        # 標點/窄字水平對齊（[obCJK] GlyphXAlign，0=規則A/1=規則B）— 見
        # obCJK_GlyphAtlas.h 的 ObCJKGlyphXAlignMode()/ObCJKComputeGlyphXTerms()：
        # 規則A沿用字型自己的 GLYPHMETRICS::gmptGlyphOrigin.x（左邊界，跟字型
        # 設計一致，多數CJK字型的標點/小假名仍是貼左下），規則B則不管字型
        # bearing，強制把黑盒置中在整個cell內。只對「黑盒比cell窄」的字有感
        # （標點、日文小假名等），一般全形漢字不受影響。
        ttk.Label(disp_frame, text=tr("glyph_x_align_lbl")).grid(
            row=1, column=0, sticky="e", padx=(8, 4), pady=6)
        self._glyph_x_align_labels = {"0": tr("glyph_x_align_rule_a"), "1": tr("glyph_x_align_rule_b")}
        # 規則C（強制置中「指定標點清單」，而非規則B的全部窄字）在
        # BIG5/GBK/UTF8顯示——見obcjk_glyphxalign_rulecd_design記憶設計點3；
        # SJIS/KOREAN目前沒有驗證過的標點清單可用，暫不開放。UTF8下
        # obCJK_GlyphAtlas.h的ObCJKBuildGlyphXAlignCTable()改走Unicode碼位
        # 直接比對（不經DBCS編碼），同一份預設標點清單照樣有效。
        if self._current_cp in ("BIG5", "GBK", "UTF8"):
            self._glyph_x_align_labels["2"] = tr("glyph_x_align_rule_c")
        self._glyph_x_align_by_label = {v: k for k, v in self._glyph_x_align_labels.items()}
        cur_glyph_x_align = self.cfg.get("obCJK", "GlyphXAlign", fallback="0") \
            if self.cfg.has_section("obCJK") else "0"
        if cur_glyph_x_align not in self._glyph_x_align_labels:
            cur_glyph_x_align = "0"
        self._glyph_x_align_var = tk.StringVar(
            value=self._glyph_x_align_labels[cur_glyph_x_align])
        ttk.Combobox(
            disp_frame, textvariable=self._glyph_x_align_var,
            values=list(self._glyph_x_align_labels.values()),
            state="readonly",
            width=_combo_width(self._glyph_x_align_labels.values()),
        ).grid(row=1, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(disp_frame, text=tr("glyph_x_align_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=1, column=2, sticky="w", padx=(0, 8))

        # 規則C的標點清單（[obCJK] GlyphXAlignCChars，UTF-8文字，使用者可自行
        # 增減）——只在規則C可選(BIG5/GBK/UTF8)時才顯示這一列，預設值＝
        # obCJK_GlyphAtlas.h kObCJKGlyphXAlignCDefaultChars同一份6個標點
        # （。，、；：·），兩邊須同步。
        if "2" in self._glyph_x_align_labels:
            ttk.Label(disp_frame, text=tr("glyph_x_align_c_chars_lbl")).grid(
                row=2, column=0, sticky="e", padx=(8, 4), pady=6)
            cur_c_chars = self.cfg.get("obCJK", "GlyphXAlignCChars",
                                        fallback=_GLYPH_X_ALIGN_C_DEFAULT_CHARS) \
                if self.cfg.has_section("obCJK") else _GLYPH_X_ALIGN_C_DEFAULT_CHARS
            self._glyph_x_align_c_chars_var = tk.StringVar(value=cur_c_chars)
            GlyphXAlignCCharsPicker(
                disp_frame, self._glyph_x_align_c_chars_var,
            ).grid(row=2, column=1, padx=(0, 4), pady=6, sticky="w")
            ttk.Label(disp_frame, text=tr("glyph_x_align_c_chars_hint"),
                      foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
                row=2, column=2, sticky="w", padx=(0, 8))
        else:
            self._glyph_x_align_c_chars_var = None

    def _build_options_page(self, options_page):
        # ── 熱鍵設定（[obCJK] 節，main.cpp 實際讀取的唯一一份熱鍵設定）──────────
        # 上半部：跟「哪個熱鍵動作綁在哪個按鍵」無關的共用設定（手把熱鍵總
        # 開關 + IME模式）；下半部改成編輯器/IME各自一個框，框內用
        # MultiHotkeyCapture 同時管理鍵盤/手把長按/手把組合鍵 3 組互不排斥
        # 的獨立綁定（可以同時生效，不再是舊版「只能三選一」的Device下拉）。
        hk_frame = ttk.LabelFrame(options_page, text=tr("hk_main_frame"))
        hk_frame.pack(fill="x", pady=(4, 0))

        # GamepadHotkeyEnable — 手把熱鍵總開關：不論下面編輯器/IME框內的
        # 手把長按、手把組合鍵各自有沒有停用，這個關掉時手把熱鍵一律不會
        # 生效，見 main.cpp IsHotkeyDown()/obCJK_Gamepad.h
        # ObCJKGamepadHotkeyEnabled()。非反相：checkbox=1 就是 ini 值=1，
        # 預設關閉。
        cur_gamepad_hotkey = self.cfg.getint("obCJK", "GamepadHotkeyEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._gamepad_hotkey_var = tk.IntVar(value=1 if cur_gamepad_hotkey else 0)
        ttk.Label(hk_frame, text=tr("gamepad_hotkey_enable_lbl")).grid(
            row=0, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            hk_frame, variable=self._gamepad_hotkey_var,
        ).grid(row=0, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(hk_frame, text=tr("gamepad_hotkey_enable_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=0, column=2, sticky="w", padx=(0, 8))

        # IMEMode（Out=外部視窗 obCJK_IME_Out.h／In=內部輸入 obCJK_IME_In.h）。
        ttk.Label(hk_frame, text=tr("ime_mode_lbl")).grid(
            row=1, column=0, sticky="e", padx=(8, 4), pady=6)
        self._ime_mode_labels = {"Out": tr("ime_mode_out"), "In": tr("ime_mode_in")}
        self._ime_mode_by_label = {v: k for k, v in self._ime_mode_labels.items()}
        cur_ime_mode = self.cfg.get("obCJK", "IMEMode", fallback="Out") if self.cfg.has_section("obCJK") else "Out"
        self._ime_mode_var = tk.StringVar(
            value=self._ime_mode_labels.get(cur_ime_mode, self._ime_mode_labels["Out"]))
        ttk.Combobox(
            hk_frame, textvariable=self._ime_mode_var,
            values=list(self._ime_mode_labels.values()),
            state="readonly",
            width=_combo_width(self._ime_mode_labels.values()),
        ).grid(row=1, column=1, padx=(0, 4), pady=6, sticky="w")

        hk_bind_frame = ttk.Frame(options_page)
        hk_bind_frame.pack(fill="x", pady=(8, 0))

        editor_frame = ttk.LabelFrame(hk_bind_frame, text=tr("hk_editor_frame_title"))
        editor_frame.grid(row=0, column=0, sticky="nw", padx=(0, 8))
        _editor = parse_obcjk_hotkey_multi(self.cfg, "EditorHotkey", "58", "ctrl")
        self._hk_editor = MultiHotkeyCapture(editor_frame, **_editor)
        self._hk_editor.pack(padx=6, pady=6)
        ttk.Label(editor_frame, text=tr("hk_editor_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left",
                  ).pack(anchor="w", padx=6, pady=(0, 6))

        ime_frame = ttk.LabelFrame(hk_bind_frame, text=tr("hk_ime_frame_title"))
        ime_frame.grid(row=0, column=1, sticky="nw")
        _ime = parse_obcjk_hotkey_multi(self.cfg, "ImeHotkey", "57", "none")
        self._hk_ime = MultiHotkeyCapture(ime_frame, **_ime)
        self._hk_ime.pack(padx=6, pady=6)
        ttk.Label(ime_frame, text=tr("hk_ime_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left",
                  ).pack(anchor="w", padx=6, pady=(0, 6))

        # ── 功能開關（[obCJK] MenuQueEnable/LootMenuEnable）─────────────────
        # MenuQueEnable=0 時 C++ 端跳過 ObCJKInstallMenuQueDelimHook()
        # （obCJK_MenuQueDelimHook.h），LootMenuEnable=0 時跳過
        # ObCJKInstallLootMenuTrailByteFixHook()（obCJK_LootMenuTrailByteFixHook.h，
        # 即 slot7/8 亂碼修正）。LootMenu 依附於 MenuQue：UI 上只有 MenuQue
        # 滑塊=1 時 LootMenu 滑塊才可調整，C++ 端安裝 LootMenu hook 前也會
        # 同時檢查兩個旗標，避免使用者手動改壞 ini 造成不一致。
        feat_frame = ttk.LabelFrame(options_page, text=tr("features_frame"))
        feat_frame.pack(fill="x", pady=(8, 0))

        # #1 AsciiRenderEnable — 半形ASCII是否套用obCJK自選字型（obCJK_GlyphAtlas.h
        # ObCJKAsciiRenderEnabled()），非反相：checkbox=1 就是 ini 值=1。
        cur_ascii_render = self.cfg.getint("obCJK", "AsciiRenderEnable", fallback=1) \
            if self.cfg.has_section("obCJK") else 1
        self._ascii_render_var = tk.IntVar(value=1 if cur_ascii_render else 0)

        cur_menuque = self.cfg.getint("obCJK", "MenuQueEnable", fallback=1) \
            if self.cfg.has_section("obCJK") else 1
        self._menuque_var = tk.IntVar(value=1 if cur_menuque else 0)
        cur_lootmenu = self.cfg.getint("obCJK", "LootMenuEnable", fallback=1) \
            if self.cfg.has_section("obCJK") else 1
        self._lootmenu_var = tk.IntVar(value=1 if cur_lootmenu else 0)

        # #4 TexSwapEnable — 除錯用「一鍵關閉CJK顯示」開關，checkbox=1 代表「取消
        # 顯示」，跟 ini 實際值互為反相：checkbox=1 → 存 TexSwapEnable=0。
        cur_texswap = self.cfg.getint("obCJK", "TexSwapEnable", fallback=1) \
            if self.cfg.has_section("obCJK") else 1
        self._texswap_off_var = tk.IntVar(value=0 if cur_texswap else 1)

        # LineBreakSpaceEnablePathA — 長字串換行時（obCJK_WordWrapHook.h的
        # sub_575B40 forced-line-break分支，見事前md/09_WordWrapHook置中與
        # 亂碼修復.md「四、」）原版引擎會插入連字號"-"標記，開啟本開關後
        # 改用空白" "取代，關閉則維持原版"-"。sub_575B40只有Path A
        # （sub_576670選單按鈕文字迴圈）會呼叫，故僅影響Path A。非反相：
        # checkbox=1 就是 ini 值=1。預設開啟。DBCS/UTF8兩種編碼都適用
        # （C++端共用同一組native VA，2026-07-24已用IDA連線核對
        # byte-for-byte，非猜測）。
        cur_lb_space_patha = self.cfg.getint("obCJK", "LineBreakSpaceEnablePathA", fallback=1) \
            if self.cfg.has_section("obCJK") else 1
        self._linebreak_space_patha_var = tk.IntVar(value=1 if cur_lb_space_patha else 0)

        # LineBreakSpaceEnablePathBC — 同上概念，但作用在完全不同、由
        # Path B（對話/選單本文）與Path C（書籍/卷軸）共用的排版樹函式
        # sub_5772A0（VA 0x577470的`push 2Dh`），非sub_575B40。原生引擎在
        # 這一層並未區分Path B/C，因此無法拆成兩個獨立開關，一個開關同時
        # 涵蓋兩者。2026-07-29用IDA連線核對byte-for-byte。
        cur_lb_space_pathbc = self.cfg.getint("obCJK", "LineBreakSpaceEnablePathBC", fallback=1) \
            if self.cfg.has_section("obCJK") else 1
        self._linebreak_space_pathbc_var = tk.IntVar(value=1 if cur_lb_space_pathbc else 0)

        # SaveDiagEnable — obCJK_SaveDiag.h 的存檔/刪檔log完整記錄開關，涵蓋
        # obCJK_CreateFileWShim.h/obCJK_SaveNameTruncateHook.h/obCJK_
        # SaveListFindShim.h/obCJK_DeleteFileWShim.h 四個檔案的log輸出。非
        # 反相：checkbox=1 就是 ini 值=1。關閉（預設）時每個檔案只印出該次
        # 存檔/刪檔動作本身的成功/失敗一行，失敗附簡短原因；開啟後才印出各
        # 檔案原本的完整診斷內容（含背景存檔清單掃描的逐筆診斷行）。
        cur_save_diag = self.cfg.getint("obCJK", "SaveDiagEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._save_diag_var = tk.IntVar(value=1 if cur_save_diag else 0)

        # GamepadInputDiagEnable — obCJK_Gamepad.h ObCJKGamepadInputDiagTick()，
        # 每幀掃描手把全部38個controlIndex並把「剛按下」的印進log，跟
        # GamepadHotkeyEnable 是否開啟無關，用來在設定手把熱鍵之前先確認
        # 手把本身有沒有被正確辨識。非反相：checkbox=1 就是 ini 值=1，
        # 預設關閉。
        cur_gamepad_input_diag = self.cfg.getint("obCJK", "GamepadInputDiagEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._gamepad_input_diag_var = tk.IntVar(value=1 if cur_gamepad_input_diag else 0)

        # #5 DebugLogEnable — 非反相：checkbox=1 就是 ini 值=1。關閉時
        # main.cpp 端把 IDebugLog print level 設回 kLevel_Message（HookUtil的
        # hook ok/fail、各處_WARNING仍會印），開啟則額外印出 kLevel_VerboseMessage
        # （GlyphAtlas/MenuQueDelimFix的診斷dump，見obCJK_GlyphAtlas.h/
        # obCJK_MenuQueDelimHook.h裡改用_VMESSAGE的那幾行）。
        cur_debuglog = self.cfg.getint("obCJK", "DebugLogEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._debuglog_var = tk.IntVar(value=1 if cur_debuglog else 0)

        # #6 PathDiagEnable/PathDiagCap — obCJK_GlyphHook.h Path A/B/C的glyph
        # 替換catch(成功套用自選字型)/miss(退回native)診斷log，非反相：
        # checkbox=1 就是 ini PathDiagEnable=1。旁邊下拉選單是PathDiagCap，
        # 每條路徑各自的印出行數上限（300/500/1000三選一），預設關閉+300。
        cur_path_diag = self.cfg.getint("obCJK", "PathDiagEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._path_diag_var = tk.IntVar(value=1 if cur_path_diag else 0)
        cur_path_diag_cap = self.cfg.get("obCJK", "PathDiagCap", fallback="300") \
            if self.cfg.has_section("obCJK") else "300"
        if cur_path_diag_cap not in ("300", "500", "1000"):
            cur_path_diag_cap = "300"
        self._path_diag_cap_var = tk.StringVar(value=cur_path_diag_cap)

        # #6-a PathDiagSlot78Only — #6 PathDiagEnable 的子選項，不論 Path
        # A/B/C，開啟後只印 fontID==7/8（MenuQue 額外字型 slot）的診斷行，
        # 其餘 slot 的命中不印也不佔 PathDiagCap 印出行數上限（見
        # obCJK_GlyphHook.h ObCJKPathDiagSlotAllowed()）。非反相：
        # checkbox=1 就是 ini 值=1，預設關閉。
        cur_path_diag_slot78 = self.cfg.getint("obCJK", "PathDiagSlot78Only", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._path_diag_slot78_var = tk.IntVar(value=1 if cur_path_diag_slot78 else 0)

        # #7 LootMenuDiagEnable/LootMenuDiagCap — obCJK_LootMenuTrailByteFixHook.h
        # slot7/8亂碼修正的repair命中log，與#6 PathDiagEnable/PathDiagCap同款
        # 寫法（checkbox=1 就是 ini 值=1，旁邊下拉選單是印出行數上限），預設
        # 關閉+300。
        cur_lootmenu_diag = self.cfg.getint("obCJK", "LootMenuDiagEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._lootmenu_diag_var = tk.IntVar(value=1 if cur_lootmenu_diag else 0)
        cur_lootmenu_diag_cap = self.cfg.get("obCJK", "LootMenuDiagCap", fallback="300") \
            if self.cfg.has_section("obCJK") else "300"
        if cur_lootmenu_diag_cap not in ("300", "500", "1000"):
            cur_lootmenu_diag_cap = "300"
        self._lootmenu_diag_cap_var = tk.StringVar(value=cur_lootmenu_diag_cap)

        ttk.Label(feat_frame, text=tr("ascii_render_lbl")).grid(
            row=0, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._ascii_render_var,
        ).grid(row=0, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("ascii_render_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=0, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("menuque_lbl")).grid(
            row=1, column=0, sticky="e", padx=(8, 4), pady=6)
        self._menuque_switch = ToggleSwitch(
            feat_frame, variable=self._menuque_var, command=self._sync_lootmenu_state,
        )
        self._menuque_switch.grid(row=1, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("menuque_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=1, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("lootmenu_lbl")).grid(
            row=2, column=0, sticky="e", padx=(8, 4), pady=6)
        self._lootmenu_switch = ToggleSwitch(feat_frame, variable=self._lootmenu_var)
        self._lootmenu_switch.grid(row=2, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("lootmenu_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=2, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("linebreak_space_patha_lbl")).grid(
            row=3, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._linebreak_space_patha_var,
        ).grid(row=3, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("linebreak_space_patha_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=3, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("linebreak_space_pathbc_lbl")).grid(
            row=4, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._linebreak_space_pathbc_var,
        ).grid(row=4, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("linebreak_space_pathbc_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=4, column=2, sticky="w", padx=(0, 8))

        # 以下皆為 (DEBUG) 標記的診斷/除錯用開關，集中放在功能開關之後、
        # 整個 feat_frame 最下面。
        ttk.Label(feat_frame, text=tr("texswap_lbl")).grid(
            row=5, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._texswap_off_var,
        ).grid(row=5, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("texswap_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=5, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("debuglog_lbl")).grid(
            row=6, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._debuglog_var,
        ).grid(row=6, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("debuglog_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=6, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("path_diag_lbl")).grid(
            row=7, column=0, sticky="e", padx=(8, 4), pady=6)
        path_diag_ctrl = ttk.Frame(feat_frame)
        path_diag_ctrl.grid(row=7, column=1, padx=(0, 4), pady=6, sticky="w")
        ToggleSwitch(
            path_diag_ctrl, variable=self._path_diag_var,
        ).pack(side="left")
        ttk.Combobox(
            path_diag_ctrl, textvariable=self._path_diag_cap_var,
            values=["300", "500", "1000"], state="readonly", width=6,
        ).pack(side="left", padx=(6, 0))
        ttk.Checkbutton(
            path_diag_ctrl, text=tr("path_diag_slot78_lbl"),
            variable=self._path_diag_slot78_var,
        ).pack(side="left", padx=(10, 0))
        ttk.Label(feat_frame, text=tr("path_diag_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=7, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("lootmenu_diag_lbl")).grid(
            row=8, column=0, sticky="e", padx=(8, 4), pady=6)
        lootmenu_diag_ctrl = ttk.Frame(feat_frame)
        lootmenu_diag_ctrl.grid(row=8, column=1, padx=(0, 4), pady=6, sticky="w")
        ToggleSwitch(
            lootmenu_diag_ctrl, variable=self._lootmenu_diag_var,
        ).pack(side="left")
        ttk.Combobox(
            lootmenu_diag_ctrl, textvariable=self._lootmenu_diag_cap_var,
            values=["300", "500", "1000"], state="readonly", width=6,
        ).pack(side="left", padx=(6, 0))
        ttk.Label(feat_frame, text=tr("lootmenu_diag_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=8, column=2, sticky="w", padx=(0, 8))

        # #8 HangWatchdogEnable — 純 iniEdit.py 自身用的除錯開關，C++ 端不讀
        # 這個 key。開啟後見 App._ensure_hang_watchdog_state()：用
        # faulthandler.dump_traceback_later() 監控 Tk mainloop 是否還在正常
        # 處理事件，逾時未重新武裝就自動把所有執行緒的呼叫堆疊寫進
        # obCJK_iniEdit_hang.log，用來抓「偶爾切換字型後 py 進程卡死」這類難
        # 重現的問題實際卡在哪一行。
        cur_hang_watchdog = self.cfg.getint("obCJK", "HangWatchdogEnable", fallback=0) \
            if self.cfg.has_section("obCJK") else 0
        self._hang_watchdog_var = tk.IntVar(value=1 if cur_hang_watchdog else 0)

        ttk.Label(feat_frame, text=tr("hang_watchdog_lbl")).grid(
            row=9, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._hang_watchdog_var,
            command=self._on_hang_watchdog_toggle,
        ).grid(row=9, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("hang_watchdog_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=9, column=2, sticky="w", padx=(0, 8))

        self._ensure_hang_watchdog_state(bool(cur_hang_watchdog))

        ttk.Label(feat_frame, text=tr("save_diag_lbl")).grid(
            row=10, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._save_diag_var,
        ).grid(row=10, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("save_diag_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=10, column=2, sticky="w", padx=(0, 8))

        ttk.Label(feat_frame, text=tr("gamepad_input_diag_lbl")).grid(
            row=11, column=0, sticky="e", padx=(8, 4), pady=6)
        ToggleSwitch(
            feat_frame, variable=self._gamepad_input_diag_var,
        ).grid(row=11, column=1, padx=(0, 4), pady=6, sticky="w")
        ttk.Label(feat_frame, text=tr("gamepad_input_diag_hint"),
                  foreground="gray", wraplength=_HINT_WRAPLENGTH, justify="left").grid(
            row=11, column=2, sticky="w", padx=(0, 8))

        self._sync_lootmenu_state()

    def _sync_lootmenu_state(self):
        """menuque 開關=0 時鎖住並歸零 lootmenu 開關——見 _build_options_page()
        頂端的依附關係說明。UTF8 模式下 MenuQue/LootMenu 兩個修正在 C++ 端
        結構性不需要（main.cpp 對 ObCJKInstallMenuQueDelimHook()/
        ObCJKInstallLootMenuTrailByteFixHook() 都 gate 成非 UTF8 才安裝，見
        obcjk-utf8-plan 記憶續7/續8/續11），此時兩個開關都鎖住不給改，但保留
        目前的值不強制歸零——那是共用給 DBCS 編碼的設定，UTF8 模式只是暫時
        不讓經手，不代表使用者要放棄 DBCS 模式下原本的選擇。"""
        is_utf8 = (self._current_cp == "UTF8")
        self._menuque_switch.set_enabled(not is_utf8)
        enabled = bool(self._menuque_var.get()) and not is_utf8
        self._lootmenu_switch.set_enabled(enabled)
        if not is_utf8 and not enabled:
            self._lootmenu_var.set(0)

    @staticmethod
    def _hang_watchdog_log_path() -> Path:
        return Path(__file__).resolve().parent / "obCJK_iniEdit_hang.log"

    def _ensure_hang_watchdog_state(self, enabled: bool) -> None:
        """開/關「凍結監控」。開啟時每秒用 self.after() 重新武裝
        faulthandler.dump_traceback_later(5s)——只要 Tk mainloop 還在正常跑，
        這個 tick 就會準時執行，不斷把逾時時間往後延；mainloop 真的卡住的話
        tick 排不上，5 秒前武裝的那次就會照常觸發，把當下所有執行緒的呼叫
        堆疊寫進 log，不需要使用者手動重現時盯著除錯器。_build_content()
        每次切換編碼都會重建這個 checkbox（見 _build_options_page()），所以
        這個函式必須是幂等的：已經是目標狀態就不重複啟動/停止。"""
        if enabled and self._watchdog_after_id is None:
            if self._watchdog_log is None:
                try:
                    self._watchdog_log = open(self._hang_watchdog_log_path(), "a", encoding="utf-8")
                except OSError as e:
                    print(f"[警告] 凍結監控log開啟失敗: {e}")
                    return
            self._hang_watchdog_tick()
        elif not enabled and self._watchdog_after_id is not None:
            self.after_cancel(self._watchdog_after_id)
            self._watchdog_after_id = None
            try:
                faulthandler.cancel_dump_traceback_later()
            except Exception:
                pass

    def _hang_watchdog_tick(self) -> None:
        faulthandler.dump_traceback_later(5, exit=False, file=self._watchdog_log)
        self._watchdog_after_id = self.after(1000, self._hang_watchdog_tick)

    def _on_hang_watchdog_toggle(self) -> None:
        self._ensure_hang_watchdog_state(bool(self._hang_watchdog_var.get()))

    def _on_app_close(self) -> None:
        self._ensure_hang_watchdog_state(False)
        if self._watchdog_log is not None:
            try:
                self._watchdog_log.close()
            except Exception:
                pass
            self._watchdog_log = None
        self.destroy()

    def _on_tab_select(self, tab_id: str) -> None:
        """兩排分頁按鈕（ttk.Radiobutton, style=Toolbutton）共用同一塊
        content_area，點擊時只是把對應frame tkraise()到最上層——手動切換是
        為了讓分頁列固定分成兩排（第一排既有SLOT，第二排SLOT9佔位+
        NorthernUI），不受ttk.Notebook自動換行的不可控排版影響，見
        _build_font_page() 說明。"""
        frame = self._tab_frames.get(tab_id)
        if frame is not None:
            frame.tkraise()
        self._update_preview()

    def _request_apply(self, tab: ModeTab, idx: int):
        sel = self.lb.curselection()
        if sel:
            tab.set_name(idx, self.fonts[sel[0]])
        else:
            self._pending = (tab, idx)
            messagebox.showinfo(tr("hint_title"), tr("hint_msg"))

    def _on_font_mode_change(self):
        """CJK / ASCII 字型清單切換——self.fonts 換源後重灌 Listbox，
        LabelFrame 標題也跟著換，_pending（等待套用的半角/全角欄位）不清除，
        讓使用者切換清單後仍可繼續完成原本的套用動作。"""
        mode = self._font_mode_var.get()
        self._font_mode = mode
        self.fonts = self.fonts_ascii if mode == "ascii" else self.fonts_cjk
        self.lb.delete(0, "end")
        for f in self.fonts:
            self.lb.insert("end", f)
        cfg_info = CODEPAGE_CONFIG[self._current_cp]
        title = (tr("font_frame_ascii") if mode == "ascii"
                 else tr("font_frame").format(lang=cfg_info["lang"]))
        self._font_lf.configure(text=title)

    def _on_font_select(self, _event):
        sel = self.lb.curselection()
        if not sel:
            return
        font = self.fonts[sel[0]]
        if self._pending is not None:
            tab, idx = self._pending
            tab.set_name(idx, font)
            self._pending = None
        self._update_preview(font)

    def _schedule_preview(self):
        if hasattr(self, "_preview_after"):
            self.after_cancel(self._preview_after)
        self._preview_after = self.after(150, self._update_preview)

    def _update_preview(self, font_name: str = None):
        if not hasattr(self, "_preview_canvas"):
            return
        # 用self._tab_var（兩排分頁按鈕共用的目前選取值，見_build_font_page()/
        # _on_tab_select()）查表拿目前顯示中的frame——SLOT9佔位分頁不在
        # self._SLOT_TABS裡，查到它時tab會是None直接跳過。
        tab = self._tab_frames.get(self._tab_var.get())
        if tab not in self._SLOT_TABS:
            return
        params = tab.get_preview_params(1)
        if font_name is None:
            sel = self.lb.curselection()
            font_name = self.fonts[sel[0]] if sel else params["name"]
        if not font_name:
            return
        # density -8..+8 → text brightness: +8(densest)→0(black), -8(lightest)→gray.
        # [2026-07-27] 原本用 *7 只能對應到 0~112（255 的不到一半），每格只差
        # 7/255，肉眼幾乎看不出濃度調整有作用；改用 *15 讓 -8..+8 整個範圍
        # 對應到接近 0~240，滑桿每格差距放大，預覽對比更明顯（純預覽模擬用途，
        # 不是要精確還原 ObCJKApplyDensityContrast 的 alpha 合成，那邊仍是
        # 「文字恆為白色、只變透明度」，見同函式下方註解）。
        brightness = max(0, min(255, (8 - params["density"]) * 15))
        # 對比（-1/0/+1）套用跟 ObCJKApplyDensityContrast()（obCJK_GlyphAtlas.h）
        # 相同的「往/離 128 推」邏輯，作用在上面算出的 density brightness 上
        # （brightness 是 ink alpha 的反相灰階，128 一樣是對稱中心）。
        if params["contrast"] != 0:
            factor = 1.4 if params["contrast"] > 0 else 0.75
            brightness = 128 + (brightness - 128) * factor
        brightness = max(0, min(200, round(brightness)))
        tk_weight  = "bold" if params["weight"] >= 700 else "normal"
        tk_slant   = "italic" if params["italic"] else "roman"
        px_size    = max(8, min(params["size"], 60))
        spacing    = params["spacing"]
        # 字寬(p0)：真正的 GDI lfWidth 會連字形本身一起橫向拉伸/壓縮，但 Tk
        # 的 canvas text item 無法重新縮放字形（已實測 canvas.scale() 對
        # text bbox 無效），這裡只能近似成「每字的水平步進距離」——width>0
        # 時每字都佔滿 width px 寬的格子，0 則沿用字型量出的自然寬度。
        width      = params["width"]
        sample     = PREVIEW_SAMPLE.get(self._current_cp, "AaBbCc 123")

        # [obCJK] BackgroundOpacity 滑塊（0=透明,100=全黑底）即時反映在預覽窗：
        # 背景框從「跟畫布一樣白」線性混到黑；字身顏色從舊有的密度灰階
        # 混到純白 — 對應 ObCJKCompositeGlyphPixel() 的兩端行為（透明時
        # 白字疊alpha看起來較暗；全黑底時字身趨近純白）。
        bg_pct    = self._bg_opacity_var.get() if hasattr(self, "_bg_opacity_var") else 0
        bg_level  = round(255 * (1 - bg_pct / 100))
        box_color = f"#{bg_level:02x}{bg_level:02x}{bg_level:02x}"
        text_level = round(brightness + (255 - brightness) * (bg_pct / 100))
        color      = f"#{text_level:02x}{text_level:02x}{text_level:02x}"

        self._preview_canvas.delete("all")
        try:
            fnt    = tkfont.Font(family=font_name, size=-px_size, weight=tk_weight, slant=tk_slant)
            line_h = fnt.metrics("linespace")
            lines  = sample.split("\n")
            def _advance(ch):
                return width if width > 0 else fnt.measure(ch)

            # 實際造字（ObCJKCompositeGlyphPixel）是逐字獨立合成，黑底只貼在
            # 該字自己的 GDI glyph 點陣範圍內，不會連成一整條——字距
            # （spacing）跟空白字元（GDI 通常給空白版點陣，不會造字/合成）之間
            # 都是透明縫隙。這裡逐字各畫一塊緊貼該字寬度的背景框，如實反映
            # 這個「單獨黑底」而非整塊連續黑底的行為。
            y = 6
            for line in lines:
                x = 6
                for ch in line:
                    adv = _advance(ch)
                    if bg_pct > 0 and not ch.isspace():
                        self._preview_canvas.create_rectangle(
                            x, y, x + adv, y + line_h, fill=box_color, outline=""
                        )
                    self._preview_canvas.create_text(
                        x, y, text=ch, font=fnt, fill=color, anchor="nw"
                    )
                    x += adv + spacing
                y += line_h + 2
        except tk.TclError:
            self._preview_canvas.create_text(6, 6, text=f"({font_name})", anchor="nw")

    def _save_font(self):
        sec = self.section

        # 半角/全角字型名稱（obCJK_GlyphAtlas.h 真正讀取的 FontParam<N>_1/_2
        # 名稱欄位）存在性檢查——使用者可能手動打字打錯，對照 self.fonts
        # （enum_cjk_fonts() 列舉出的目前編碼系統字型清單）非強制擋下，只是
        # 先確認一次。
        known_fonts_lower = {f.lower() for f in self.fonts}
        not_found = []
        for tab in self._SLOT_TABS:
            pairs = [("全角", tab._name_vars[1].get())]
            if not tab._native_var.get():  # 勾了「遊戲原生字型」時半角欄位已停用，不檢查
                pairs.insert(0, ("半角", tab._name_vars[0].get()))
            for label, name in pairs:
                face = name.strip()
                if face and face.lower() not in known_fonts_lower:
                    not_found.append(f"{tab._prefix} {label}: {face}")
        if not_found:
            proceed = messagebox.askyesno(
                tr("font_not_found_title"),
                tr("font_not_found_msg").format(items="\n".join(not_found)),
            )
            if not proceed:
                return

        if not self.cfg.has_section("obCJK"):
            self.cfg.add_section("obCJK")

        # Detect changed fields BEFORE modifying cfg. Any change to a
        # _SLOT_TABS _1/_2 field (font name, height, weight, spacing, ypos,
        # density, contrast, italic…) needs ObCJKFontReload; a save with no
        # font param changes (hotkey/IME/UILang-only) still writes ini but
        # only needs the lighter ObCJKResetFont path.
        def _ascii_safe(s: str) -> str:
            return s.encode("mbcs", errors="replace").decode("ascii", errors="backslashreplace")

        glyph_changed = False
        changed_fields = []
        for tab in self._SLOT_TABS:
            for key, val in tab.collect().items():
                old = self.cfg.get(sec, key, fallback="").strip() if self.cfg.has_section(sec) else ""
                if val.strip() != old:
                    changed_fields.append(f"{key}: {_ascii_safe(old)!r} -> {_ascii_safe(val.strip())!r}")
                    glyph_changed = True

        for tab in self._SLOT_TABS:
            for key, val in tab.collect().items():
                self.cfg.set(sec, key, val)

        # [obCJK] 節：main.cpp 實際讀取的熱鍵設定（唯一一份，不再有per-編碼重複）。
        for key, cap in (("EditorHotkey", self._hk_editor),
                         ("ImeHotkey",    self._hk_ime)):
            binding = cap.get_ini_value()
            raw = binding["kbd_code"]
            try:
                val = int(raw, 16)
                self.cfg.set("obCJK", f"{key}KbdCode", f"0x{val:02X}")
            except ValueError:
                self.cfg.set("obCJK", f"{key}KbdCode", raw)
            self.cfg.set("obCJK", f"{key}KbdModifier", binding["kbd_modifier"])
            self.cfg.set("obCJK", f"{key}KbdDisabled", "1" if binding["kbd_disabled"] else "0")
            self.cfg.set("obCJK", f"{key}HoldButton", str(binding["hold_button"]))
            self.cfg.set("obCJK", f"{key}HoldSeconds", f"{binding['hold_seconds']:.1f}")
            self.cfg.set("obCJK", f"{key}HoldDisabled", "1" if binding["hold_disabled"] else "0")
            self.cfg.set("obCJK", f"{key}ComboButton", str(binding["combo_button"]))
            self.cfg.set("obCJK", f"{key}ComboModifierButton", str(binding["combo_modifier_button"]))
            self.cfg.set("obCJK", f"{key}ComboDisabled", "1" if binding["combo_disabled"] else "0")
            self.cfg.set("obCJK", f"{key}AllDisabled", "1" if binding["all_disabled"] else "0")
        self.cfg.set("obCJK", "GamepadHotkeyEnable",
                      "1" if bool(self._gamepad_hotkey_var.get()) else "0")
        # 讓 obCJK.dll 下次啟動時載入同一個編碼 — 這是單一DLL架構下第一次
        # 真正需要寫這個欄位（舊架構靠「哪個DLL檔案存在」決定，不靠 ini）。
        self.cfg.set("obCJK", "ActiveCodePage", self._current_cp)
        self.cfg.set("obCJK", "IMEMode",
                      self._ime_mode_by_label.get(self._ime_mode_var.get(), "Out"))
        self.cfg.set("obCJK", "BackgroundOpacity",
                      str(max(0, min(100, int(self._bg_opacity_var.get())))))
        self.cfg.set("obCJK", "GlyphXAlign",
                      self._glyph_x_align_by_label.get(self._glyph_x_align_var.get(), "0"))
        if self._glyph_x_align_c_chars_var is not None:
            self.cfg.set("obCJK", "GlyphXAlignCChars",
                          self._glyph_x_align_c_chars_var.get() or _GLYPH_X_ALIGN_C_DEFAULT_CHARS)
        menuque_on = bool(self._menuque_var.get())
        self.cfg.set("obCJK", "MenuQueEnable", "1" if menuque_on else "0")
        # LootMenu 依附於 MenuQue（見 _sync_lootmenu_state()），存檔時再強制
        # 確認一次，就算 UI 狀態因故沒同步也不會寫出矛盾的組合。
        self.cfg.set("obCJK", "LootMenuEnable",
                      "1" if (menuque_on and bool(self._lootmenu_var.get())) else "0")
        self.cfg.set("obCJK", "AsciiRenderEnable",
                      "1" if bool(self._ascii_render_var.get()) else "0")
        self.cfg.set("obCJK", "LineBreakSpaceEnablePathA",
                      "1" if bool(self._linebreak_space_patha_var.get()) else "0")
        self.cfg.set("obCJK", "LineBreakSpaceEnablePathBC",
                      "1" if bool(self._linebreak_space_pathbc_var.get()) else "0")
        # checkbox 語意是「取消CJK全部顯示」，跟 ini 的 TexSwapEnable 互為反相。
        self.cfg.set("obCJK", "TexSwapEnable",
                      "0" if bool(self._texswap_off_var.get()) else "1")
        self.cfg.set("obCJK", "DebugLogEnable",
                      "1" if bool(self._debuglog_var.get()) else "0")
        self.cfg.set("obCJK", "PathDiagEnable",
                      "1" if bool(self._path_diag_var.get()) else "0")
        cap_val = self._path_diag_cap_var.get()
        self.cfg.set("obCJK", "PathDiagCap",
                      cap_val if cap_val in ("300", "500", "1000") else "300")
        self.cfg.set("obCJK", "PathDiagSlot78Only",
                      "1" if bool(self._path_diag_slot78_var.get()) else "0")
        self.cfg.set("obCJK", "LootMenuDiagEnable",
                      "1" if bool(self._lootmenu_diag_var.get()) else "0")
        self.cfg.set("obCJK", "SaveDiagEnable",
                      "1" if bool(self._save_diag_var.get()) else "0")
        lootmenu_diag_cap_val = self._lootmenu_diag_cap_var.get()
        self.cfg.set("obCJK", "LootMenuDiagCap",
                      lootmenu_diag_cap_val if lootmenu_diag_cap_val in ("300", "500", "1000") else "300")
        self.cfg.set("obCJK", "HangWatchdogEnable",
                      "1" if bool(self._hang_watchdog_var.get()) else "0")
        self.cfg.set("obCJK", "GamepadInputDiagEnable",
                      "1" if bool(self._gamepad_input_diag_var.get()) else "0")
        try:
            write_ini(self.cfg, self.ini_path)
            self._font_saved    = True
            self._glyph_changed = glyph_changed
            if changed_fields:
                changes_path = self.ini_path.parent / "obCJK_changes.tmp"
                with open(changes_path, "w", encoding="mbcs") as _cf:
                    _cf.write("\n".join(changed_fields))
            messagebox.showinfo(tr("done"),
                                tr("saved_utf8").format(path=self.ini_path))
        except Exception as e:
            messagebox.showerror(tr("error"), str(e))


# ── 進入點 ────────────────────────────────────────────────────────────────────

def main():
    app = App()
    app.mainloop()
    # Exit codes consumed by obCJK C++ plugin:
    #   0 = nothing saved (or only obCJK hotkeys saved)      → no font rebuild
    #   1 = font DLL ini saved, glyph params changed         → ObCJKFontReload
    #   2 = font DLL ini saved, no glyph param actually changed → ObCJKResetFont
    if app._font_saved:
        sys.exit(1 if app._glyph_changed else 2)
    sys.exit(0)


if __name__ == "__main__":
    main()
