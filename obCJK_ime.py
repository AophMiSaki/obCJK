# obCJK_ime_input.py — obCJK CJK IME 輸入對話框
# Usage: obCJK_ime_input.py [encoding]
#   encoding: big5 (default) | gbk | shift_jis
# 流程：使用者用 Windows IME 輸入 → Enter 確認 → 編碼 bytes 寫入 ime_result.tmp
# obCJK.dll 偵測此進程退出後讀取 temp file → InjectIntoTextEdit 注入 TextEdit / race_name tile

import tkinter as tk
import os, sys

ENCODING    = sys.argv[1] if len(sys.argv) > 1 else "big5"
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ime_result.tmp")
FONT        = ("Noto Sans Mono CJK TC", 18)

# Windows 檔名禁用符號 + 控制字元（此輸入框的文字可能最終被當存檔名使用）
FORBIDDEN_CHARS = set('<>:"/\\|?*')

def commit(event=None):
    text = entry.get().strip()
    if text:
        try:
            with open(RESULT_FILE, "wb") as f:
                f.write(text.encode(ENCODING, errors="replace"))
        except Exception as e:
            print(f"write error: {e}", file=sys.stderr)
    root.destroy()

def cancel(event=None):
    # 不寫 temp file → obCJK 偵測到進程退出但無 file → 不注入
    root.destroy()

root = tk.Tk()
root.title(f"CJK IME [{ENCODING}]")
root.resizable(False, False)
root.attributes("-topmost", True)

root.update_idletasks()
w, h = 440, 90
sw = root.winfo_screenwidth()
sh = root.winfo_screenheight()
root.geometry(f"{w}x{h}+{(sw - w) // 2}+{(sh - h) // 2}")

tk.Label(root, text="輸入中文（Enter 確認，Esc 取消）：", anchor="w").pack(
    padx=10, pady=(8, 2), fill="x")

def validate_char(action, inserted):
    # action "1" = insert; reject if the inserted text contains a forbidden
    # char or control byte (Tab included) so it never lands in the entry
    if action == "1":
        return not any(ch in FORBIDDEN_CHARS or ord(ch) < 0x20 for ch in inserted)
    return True

vcmd = (root.register(validate_char), "%d", "%S")
entry = tk.Entry(root, font=FONT, validate="key", validatecommand=vcmd)
entry.pack(padx=10, pady=(0, 8), fill="x")
entry.bind("<Return>", commit)
entry.bind("<Escape>", cancel)
entry.focus_set()

root.mainloop()
