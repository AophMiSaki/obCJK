# obCJK_iniEdit User Guide

This document explains the meaning of each field, menu, and toggle in the `obCJK_iniEdit.py`
(font / plugin settings editor GUI) interface, to help you cross-reference the contents of
`obCJK.ini` or look things up when adjusting effects. The tool is split into two tabs:
"Font" and "Options".

---

## 1. Font Tab

### 1. Encoding / Language Selection
The top-left corner lets you switch which encoding you are currently editing: `BIG5`
(Traditional Chinese) / `GBK` (Simplified Chinese) / `SJIS` (Japanese) / `KOREAN` (Korean) /
`UTF8` (Unicode). All five encodings share the same `obCJK.dll`; each encoding's font
parameters are stored separately in the ini's `[BIG5]` / `[GBK]` / `[SJIS]` / `[KOREAN]` /
`[UTF8]` sections.

On the right side of the same row are the "Display Language" dropdown (see II-3) and the
"NorthernUI Compatibility" dropdown, which write to the `NorthernUIEnable` key (0/1/2) in the
`[obCJK]` section:
- **Vanilla/Other UI** (`NorthernUIEnable=0`, default): NorthernUI bridging is not enabled.
- **Original NorthernUI** (`=1`): The official, unmodified `NorthernUI.dll`. At runtime,
  `obCJK.dll` reads the rawID at FontInfo+0x08 directly from that DLL to obtain font role
  information.
- **New NorthernUI** (`=2`): A patched `NorthernUI.dll` recompiled by the user, which instead
  communicates font role information to `obCJK.dll` via OBSE Messaging broadcasts.

Selecting any option other than "Vanilla/Other UI" adds an extra row of tabs to the Font page:
a locked SLOT9/10 placeholder tab, and the 5 NorthernUI role tabs — see "4. NorthernUI Font
Role Tabs" below.

If the content of the Font or Options tab exceeds the window height, each becomes an
independently vertically scrollable area (scroll with the mouse wheel after hovering over it);
the "Save ini" row at the bottom stays fixed outside the scroll area.

### 2. CJK Font / ASCII Font (Half-width)
The two radio buttons above the font list on the left switch between "Edit CJK Custom Font" and
"Edit Half-width Alphanumeric Custom Font" — these are two independent sets of SLOT parameters.

### 3. SLOT (Font Usage Location)
- **SLOT 1**: Titles, most UI, books
- **SLOT 2**: HUD, dialogue subtitles
- **SLOT 3**: Map place names, popup text, remaining UI…
- **SLOT 5**: Books, letters (handwritten style)
- **SLOT 7**: MenuQue extra font 1 (only editable in-game when launched and MenuQue is detected
  loading an extra font)
- **SLOT 8**: MenuQue extra font 2 (same as above)

Each SLOT tab has "Half-width" and "Full-width" rows, each of which can have a font name and the
following parameters filled in.
Reference: https://oblivionjapanize.wordpress.com/2022/06/29/font_settings/

### 4. NorthernUI Font Role Tabs
Only when the "NorthernUI Compatibility" dropdown (see 1) is set to "Original NorthernUI" or
"New NorthernUI" does the Font page grow a second row of tabs: a locked SLOT9/10 placeholder
tab (reserved for future expansion, currently not wired to any font parameters at all),
followed immediately by 5 NorthernUI role tabs:

- **Normal**
- **Large**
- **MediumLargeUpper**
- **Shadowed**
- **Small**

These 5 role names are font traits defined by NorthernUI itself inside
`Data\Menus\NorthernUI\datastore.xml` (`_fontNormal`/`_fontLarge`/`_fontMediumLargeUpper`/
`_fontShadowed`/`_fontSmall`) — they are not UI locations categorized by obCJK. Which in-game
UI element a given role actually corresponds to depends entirely on how the user's installed
NorthernUI XML interface assigns it; obCJK simply reuses these 5 role names as-is and does not
redefine them.

Each role tab's interface is identical to a regular SLOT tab (half-width/full-width rows plus
the full set of "5. Font Parameters" and the "Apply Selection"/"Reset" buttons), with initial
default values inherited from SLOT 1's built-in defaults. The ini keys written are, in order,
`FontParam33`–`FontParam37` (Normal=33, Large=34, MediumLargeUpper=35, Shadowed=36, Small=37),
following the same key-naming scheme as native SLOT1/2/3/5/7/8's `FontParam1`/`FontParam2`/
`FontParam3`/`FontParam5`/`FontParam7`/`FontParam8` — just numbered onward to 33–37.

All 5 tabs are always editable, unlike the locked SLOT9/10 placeholder tab — even if a given
role is currently undefined in datastore.xml, or its corresponding font file doesn't actually
exist on disk (in which case `obCJK.dll`'s bridge for that role stays inactive, and the log
prints a corresponding warning), the iniEdit interface still lets you adjust and save that
role's parameters normally; you simply won't see any in-game effect until that role becomes
active later, at which point the saved parameters take effect.

"Original NorthernUI" (`=1`) and "New NorthernUI" (`=2`) only affect how `obCJK.dll` detects/
obtains the actual font objects for these 5 roles at runtime (see 1 above) — the role tab
interface and parameter-saving behavior on the iniEdit side are identical between the two
modes, so you don't need to change how you operate the tool based on which mode is selected.

### 5. Font Parameters (present in both the half-width/full-width rows of every SLOT/NorthernUI role tab)

- **Width**: Adjusts the horizontal cell width each character occupies (unit: pixels). 0 means
  use the font's own natural measured width; a positive value stretches/compresses every
  character to fit that pixel-wide cell (not including character spacing).
- **Height**: Adjusts the overall character size (unit: pixels, i.e. the GDI font's lfHeight).
- **Spacing**: Adjusts the extra gap between characters (unit: pixels). A negative value packs
  characters tighter together; a positive value increases the gap between characters.
- **Y Offset**: Adjusts the vertical displacement of the glyph (unit: pixels). **Positive shifts
  up, negative shifts down**, 0 = original position. This is actually added to the glyph
  drawing origin, not an absolute coordinate.
- **Weight Density**: Adjusts how black/light the character body appears (range: -8 to +8).
  Higher values make characters look blacker/bolder; lower values make them look lighter/
  thinner.
- **Boldness**: Font weight (choose from 300 Light / 400 Regular / 500 Medium / 700 Bold, i.e.
  the GDI font weight).
- **Contrast**: Adjusts the contrast level of the character color (choose from Weak / Normal /
  Strong).
- **Italic**: When checked, the glyph is rendered in italic style.

Each row has two buttons on the right: "Apply Selection" (applies the font name currently
selected in the font list to this row) and "Reset" (restores this row to the built-in default
for this encoding/SLOT).

### 6. Glyph Background Opacity
Range 0–100 (unit: percent %). 0 = fully transparent background, 100 = fully black background,
values in between produce a semi-transparent black background. This only affects how the
background is composited when rendering glyphs; it does not affect the character body's own
weight density/contrast.

### 7. Punctuation/Narrow Character Horizontal Alignment
Only affects characters whose "black box is narrower than the cell" (punctuation marks,
Japanese small kana, etc.); regular full-width CJK characters are unaffected:
- **Rule A (by font bearing)**: Follows the left-edge position designed into the font itself
  (most CJK fonts place punctuation/small kana flush to the bottom-left).
- **Rule B (centered)**: Ignores font bearing and forcibly centers the character's black box
  within the entire cell — applies to all narrow characters.
- **Rule C (force-center specified punctuation)**: This option is only available for `BIG5`/
  `GBK`/`UTF8` encodings (`SJIS`/`KOREAN` currently lack a verified punctuation list, so this
  is not yet offered for them). The difference from Rule B is that only the characters
  specified in the "Rule C Punctuation List" are forced to center; narrow characters outside
  the list still follow Rule A's logic. When Rule C is selected, an extra "Rule C Punctuation
  List:" row appears below, with a read-only preview field and a "Set" button on the right —
  clicking it opens a table editor window where you can add/remove characters from the list
  (6 characters by default: 。，、；：·). This option is for users who feel Rule B's
  "center all narrow characters" is too aggressive, but still want a few specific symbols
  centered.

### 8. Preview
The canvas below live-previews the style of the currently selected font with the above
parameters applied (this is only an approximate preview, not the actual in-game rendering).

---

## 2. Options Tab

### 1. Hotkey Settings and IME Mode
The top two rows are shared settings unrelated to "which key is bound to which action":
- **Controller Hotkey Detection**: The master switch for controller hotkeys, off by default.
  When off, controller hotkeys will never trigger regardless of whether "Controller Long-Press"
  or "Controller Combo" below are individually enabled; only when this master switch is checked
  will controller hotkeys actually be detected. Trigger buttons cannot be directly assigned as
  hotkeys (the analog axis differences between controllers are too large, making detection
  unreliable); to trigger a hotkey with a trigger button, use a tool like JoyToKey to first map
  the trigger to a controller button or keyboard key.
- **IME Mode**: **External Window (Original)** invokes the system's native external IME window;
  **Inline Input** displays the input interface directly within the game screen, without
  popping up an external window.

Below that, the "Editor" and "IME" boxes each contain the same set of 3 groups of bindings that
are **mutually independent and can all be active simultaneously** (no longer the old version's
"keyboard/controller pick-one" device dropdown):
- **Keyboard**: Key + modifier (None/Ctrl/Shift). "Open Editor" defaults to Ctrl+F12 (F12 alone
  is Steam's built-in screenshot key); "Open IME" defaults to F11 (DIK_F11 = 0x57, no
  modifier). When setting a keyboard key, if it conflicts with a common system/Steam shortcut
  (such as Ctrl+C/V/S, Shift+Tab, etc.), a warning dialog will appear naming the conflicting
  shortcut, but you may still continue to use it.
- **Controller Long-Press**: Holding a single controller button for longer than a custom
  duration (0.1–10.0 seconds, default 1.0 second) triggers the action.
- **Controller Combo**: A modifier button on the left (only the four buttons LB/RB, Back
  (Menu), and Start are allowed, to avoid accidentally selecting A/B/X/Y, which are often used
  standalone) + a main button on the right (left/right stick clicks are not allowed).

Each row (Keyboard / Controller Long-Press / Controller Combo) has its own "Disable" checkbox
on the left, which only grays out that row's own controls without affecting the other rows;
there is also a "Disable All" master checkbox at the bottom of the box — checking it makes the
3 individual disable checkboxes above unselectable (but preserves their original checked
state). Controller Long-Press and Controller Combo are disabled by default; only the keyboard
binding is active by default. After clicking "Set", press the corresponding keyboard key or
controller button for that device to capture it; if a controller cannot be detected, an error
message will be shown in the dialog.

### 2. Feature Toggles
From top to bottom:
- **Apply Custom Font to ASCII**: When enabled, half-width alphanumeric characters use the
  custom font; when disabled, half-width characters use the game's native font (unaffected by
  this tool's "ASCII Font" tab parameters).
- **Enable MenuQue Hook**: Fixes CJK delimiter character misdetection, used for text displayed
  by MenuQue such as loot lists. Under the `UTF8` encoding, this fix is not structurally needed
  on the C++ side, so this toggle is locked (unadjustable) when UTF8 is selected.
- **Enable LootMenu Fix**: Fixes garbled text in SLOT 7/8 of the loot list. This toggle depends
  on "Enable MenuQue Hook" — MenuQue must be enabled first before this can be adjusted (also
  locked under `UTF8` encoding).
- **Replace PATH A Line-Break "-" with Space**: When the original engine force-wraps a long
  string, it inserts a hyphen "-" mark at the break point; enabling this replaces it with a
  space instead, while disabling it keeps the original "-". Only affects Path A (forced line
  wrapping of menu button text). Applies to BIG5/GBK/SJIS/KOREAN/UTF8; enabled by default.
- **Replace PATH B/C Line-Break "-" with Space**: Same concept as above, but applies to the
  layout function shared by Path B (console/menu body text) and Path C (books/scrolls). Since
  the two are not separated in the native engine, there is only one shared toggle (B and C
  cannot be configured separately). Enabled by default.
- **Disable All CJK Display**: A one-click debug toggle; when enabled, all CJK and ASCII custom
  fonts are disabled, reverting entirely to the game's native font rendering, making it easy to
  compare "with vs. without custom fonts applied".
- **Show Debug Log**: When off, the log only shows whether each Hook was successfully applied;
  when on, additional detailed diagnostic content is printed.
- **PATH A/B/C Diagnostic Log**: When enabled, prints catch (successfully applied) / miss
  (fell back to native font) messages during the CJK font replacement process. The dropdown on
  the right sets the maximum number of printed lines per path (choose from 300/500/1000). When
  "Slot 7/8 Only" is checked, only diagnostic lines for SLOT 7/8 (MenuQue extra fonts) are
  printed; hits on other SLOTs do not count against the line limit.
- **Slot 7/8 Fix Diagnostic Log**: When enabled, prints hit messages for the loot list SLOT 7/8
  garbled-text fix. The dropdown on the right is likewise the maximum printed line count
  (300/500/1000).
- **Hang Monitor (Debug)**: If the interface hangs for more than 5 seconds, automatically
  writes the call stacks of all current threads to `obCJK_iniEdit_hang.log`. This is purely a
  debugging feature of this tool itself (iniEdit.py) for diagnosing hard-to-reproduce freezes;
  `obCJK.dll` does not read this setting.
- **Full Save/Delete Log (Debug)**: When off, each save/delete action only prints a single
  success/failure line (with a brief reason on failure); when on, prints full diagnostic
  content, including line-by-line diagnostics from the background save-list scan.
- **Controller Input Detection (Debug)**: When enabled, scans all controller buttons every
  frame and logs "just pressed" actions (the first line records which controller was detected).
  This is independent of whether "Controller Hotkey Detection" is enabled, and is used to
  confirm the controller itself is being correctly recognized before setting up controller
  hotkeys.

### 3. Display Language
The top-right corner switches the display language of this tool's **own interface** (button/
label text): Traditional Chinese / Simplified Chinese / Japanese / Korean. This is a separate,
independent setting from "Encoding/Language Selection" (which encoding's font parameters you
are editing) — the two do not affect each other.

---

## 3. Saving

The "Save ini" button at the bottom of the window writes all current settings (across all
tabs) back to the same `obCJK.ini` file at once (UTF-8 encoding). If a filled-in font name
cannot be found in the system's font list, a confirmation prompt appears before saving, but
you may still choose to save anyway.
