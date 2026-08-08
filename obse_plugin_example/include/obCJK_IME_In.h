#pragma once
// Native (non-popup-process) IME path: a small blocking input bar drawn as a
// plain Win32 window, not the game's own D3D surface. Counterpart to
// obCJK_IME_Out.h (external process window). See IME輸入設計.md.
//
// Design mirrors obtc.dll's own IME dialog (reverse-engineered via IDA,
// sub_10006E00/sub_10007030/sub_1000715D): capture+darken a screenshot as a
// backdrop, pop a small Edit-control bar with the OS IME attached, and block
// the calling thread until the user confirms/cancels. obtc used a resource
// dialog template (DialogBoxParamA); this reimplementation gets the same
// "freeze + in-window input bar" effect from a plain RegisterClass/
// CreateWindow + manual message loop instead, so no .rc/resource-compiler
// wiring is needed. Blocking is what produces the freeze — the calling
// thread is the game's own main thread (this runs from PerFrameTask), so
// nothing else in the engine can run until the bar closes, exactly like
// obtc's DialogBoxParamA modal loop. Since the bar is an ordinary GDI
// window (not a D3D exclusive-mode surface), the system IME candidate/
// composition UI can always render over it regardless of the game's
// fullscreen mode — this sidesteps the once-open question of whether IME
// candidate windows can display over Oblivion's D3D surface directly.
#include <windows.h>
#include <imm.h>
#include "common/IDebugLog.h"
#include "obCJK_Path.h"
#include "obCJK_Encoding.h"  // ObCJKCodePage / kCP_UTF8
#include "obCJK_IME_Out.h"  // reuse InjectIntoTextEdit()

#pragma comment(lib, "imm32.lib")

static HWND   g_imeInBarWnd  = NULL;
static HWND   g_imeInEditWnd = NULL;
static HBRUSH g_imeInBackdropBrush  = NULL;
static HBITMAP g_imeInBackdropBmp   = NULL;

static LRESULT CALLBACK ObCJKImeInBarProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        return (LRESULT)(g_imeInBackdropBrush ? g_imeInBackdropBrush : GetStockObject(BLACK_BRUSH));
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void ObCJKImeInEnsureClass(HINSTANCE hInst)
{
    static bool s_registered = false;
    if (s_registered) return;
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ObCJKImeInBarProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "ObCJKImeInBar";
    RegisterClassExA(&wc);
    s_registered = true;
}

// Captures a strip of the game's current screen and halves every channel's
// brightness (same "dim the frozen frame" trick obtc used, done here with a
// DIB byte loop instead of obtc's raw MMX asm) to use as the input bar's
// backdrop, so the bar reads as an overlay on the paused frame rather than a
// plain gray box.
static HBRUSH ObCJKImeInMakeBackdrop(HWND gameWnd, int width, int height, HBITMAP* outBmp)
{
    HDC hdcScreen = GetDC(gameWnd);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm   = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);
    BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    ReleaseDC(gameWnd, hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcTmp = GetDC(NULL);
    BYTE* bits = new BYTE[(size_t)width * height * 4];
    if (GetDIBits(hdcTmp, hbm, 0, height, bits, &bmi, DIB_RGB_COLORS)) {
        for (size_t i = 0; i < (size_t)width * height * 4; i += 4) {
            bits[i + 0] = bits[i + 0] / 2;  // B
            bits[i + 1] = bits[i + 1] / 2;  // G
            bits[i + 2] = bits[i + 2] / 2;  // R
        }
        SetDIBits(hdcTmp, hbm, 0, height, bits, &bmi, DIB_RGB_COLORS);
    }
    ReleaseDC(NULL, hdcTmp);
    delete[] bits;

    *outBmp = hbm;
    return CreatePatternBrush(hbm);
}

// Blocking: opens the input bar, waits for the user to confirm (Enter) or
// cancel (Escape), injects the text on confirm, then tears everything down.
// The call not returning until the bar closes is exactly what freezes the
// game — see the top-of-file comment.
static void ObCJKImeInRunModal(HWND gameWnd, HINSTANCE hInst, ObCJKCodePage cp)
{
    ObCJKImeInEnsureClass(hInst);

    RECT rc;
    GetClientRect(gameWnd, &rc);
    POINT topLeft = { rc.left, rc.top };
    ClientToScreen(gameWnd, &topLeft);
    int width = rc.right - rc.left;
    const int kBarHeight = 24;
    if (width <= 0) width = 640;

    g_imeInBackdropBrush = ObCJKImeInMakeBackdrop(gameWnd, width, kBarHeight, &g_imeInBackdropBmp);

    g_imeInBarWnd = CreateWindowExA(WS_EX_TOPMOST, "ObCJKImeInBar", "",
        WS_POPUP | WS_BORDER,
        topLeft.x, topLeft.y, width, kBarHeight,
        gameWnd, NULL, hInst, NULL);

    g_imeInEditWnd = CreateWindowExA(0, "Edit", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, width, kBarHeight,
        g_imeInBarWnd, (HMENU)0, hInst, NULL);

    _MESSAGE("obCJK:IME_In:RunModal: opening bar=%p edit=%p pos=(%d,%d) size=%dx%d",
              g_imeInBarWnd, g_imeInEditWnd, topLeft.x, topLeft.y, width, kBarHeight);

    while (ShowCursor(TRUE) < 0) {}
    ShowWindow(g_imeInBarWnd, SW_SHOW);
    SetFocus(g_imeInEditWnd);

    HIMC himc = ImmGetContext(g_imeInEditWnd);
    if (himc) { ImmSetOpenStatus(himc, TRUE); ImmReleaseContext(g_imeInEditWnd, himc); }

    bool confirmed = false;
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.hwnd == g_imeInEditWnd && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) { confirmed = true;  break; }
            if (msg.wParam == VK_ESCAPE) { confirmed = false; break; }
        }
        if (msg.hwnd == g_imeInEditWnd && msg.message == WM_CHAR &&
            ObCJKIsForbiddenFilenameChar((BYTE)msg.wParam))
            continue;  // drop: Tab / Windows-illegal filename symbols never reach the edit control
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (confirmed) {
        char buf[512] = {};
        int len;
        // [UTF-8] The Edit control (created below via CreateWindowExA) is an
        // ANSI window, so its text is already committed through CP_ACP by
        // the time any key/IME composition message lands in it — that part
        // is unchanged and unrelated to which encoding we want out. But
        // GetWindowTextA re-encodes the control's UTF-16 storage through
        // CP_ACP too on the way OUT, and Windows has no "system ANSI
        // codepage = UTF-8" setting, so it can never hand back UTF-8 bytes.
        // Under kCP_UTF8 we read the wide text directly and do the CP_UTF8
        // conversion ourselves; every other (DBCS) codepage keeps the
        // original GetWindowTextA call unchanged.
        if (cp == kCP_UTF8) {
            WCHAR wbuf[512] = {};
            int wlen = GetWindowTextW(g_imeInEditWnd, wbuf, 512);
            len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, buf, sizeof(buf) - 1, NULL, NULL);
        } else {
            len = GetWindowTextA(g_imeInEditWnd, buf, sizeof(buf) - 1);
        }
        _MESSAGE("obCJK:IME_In:RunModal: confirmed, len=%d text=\"%s\"", len, buf);
        if (len > 0)
            InjectIntoTextEdit(buf, (DWORD)len);
    } else {
        _MESSAGE("obCJK:IME_In:RunModal: cancelled");
    }

    himc = ImmGetContext(g_imeInEditWnd);
    if (himc) { ImmSetOpenStatus(himc, FALSE); ImmReleaseContext(g_imeInEditWnd, himc); }

    DestroyWindow(g_imeInBarWnd);
    g_imeInBarWnd  = NULL;
    g_imeInEditWnd = NULL;
    while (ShowCursor(FALSE) >= 0) {}

    if (g_imeInBackdropBrush) { DeleteObject(g_imeInBackdropBrush); g_imeInBackdropBrush = NULL; }
    if (g_imeInBackdropBmp)   { DeleteObject(g_imeInBackdropBmp);   g_imeInBackdropBmp   = NULL; }
}

// Per-frame hotkey handler: edge-triggered. ObCJKImeInRunModal blocks until
// the bar closes, so this call itself does not return until the user is
// done typing — that block is what pauses the game (see top-of-file
// comment), no separate pause logic is needed.
static void ObCJKImeInPerFrame(HWND hWnd, ObCJKCodePage cp, bool imeKeyDown, bool& imeKeyWasDown)
{
    if (imeKeyDown && !imeKeyWasDown && hWnd)
        ObCJKImeInRunModal(hWnd, GetModuleHandleA(NULL), cp);
    imeKeyWasDown = imeKeyDown;
}
