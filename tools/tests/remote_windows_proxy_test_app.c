/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_proxy_test_app
 *
 * @file      remote_windows_proxy_test_app.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Guest application for the Remote Windows proxy checks (AppSandbox-eh6.6). Runs as its own
 * process (cross-process like a real guest application) and is driven by posted messages:
 *
 *   "eh6.6 standard"  standard frame, animated green client with a magenta 20x20 marker at
 *                     client (10,10), minimum tracking size 420 x 320 (window).
 *   "eh6.6 custom"    custom chrome: no system caption (WM_NCCALCSIZE), its own 32 px blue title
 *                     band with minimize/maximize/close boxes, WM_NCHITTEST returns HTCAPTION /
 *                     HTMINBUTTON / HTMAXBUTTON / HTCLOSE / HTTOP there; yellow body.
 *
 * Commands (posted to the standard root):
 *   APP+1 open a popup menu that overflows the root's left edge    APP+2 close it
 *   APP+3 open a modal owned dialog (root disabled)                 APP+4 close it
 *   APP+5 wParam: "unsaved" flag (WM_CLOSE then asks in a modal owned prompt)
 *   APP+6 cancel the prompt (window stays)                          APP+7 maximize itself
 *   APP+8 minimize itself                                           APP+9 quit
 */
#include <windows.h>
#include <windowsx.h>

#define APP_MENU_OPEN   (WM_APP + 1)
#define APP_MENU_CLOSE  (WM_APP + 2)
#define APP_MODAL_OPEN  (WM_APP + 3)
#define APP_MODAL_CLOSE (WM_APP + 4)
#define APP_UNSAVED     (WM_APP + 5)
#define APP_CANCEL      (WM_APP + 6)
#define APP_MAXIMIZE    (WM_APP + 7)
#define APP_MINIMIZE    (WM_APP + 8)
#define APP_QUIT        (WM_APP + 9)

#define TITLE_H   32
#define BUTTON_W  46

static HWND g_standard, g_custom, g_modal, g_prompt;
static BOOL g_unsaved;
static int g_tick;

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static LRESULT CALLBACK standard_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        g_tick++;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 420;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 320;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        fill(dc, 0, 0, rc.right, rc.bottom, RGB(0x20, 0xA0, 0x40));
        fill(dc, 10, 10, 30, 30, RGB(0xFF, 0x00, 0xFF));
        int x = 40 + (g_tick * 7) % (rc.right > 120 ? rc.right - 80 : 40);
        fill(dc, x, 60, x + 30, 90, RGB(0xFF, 0xFF, 0xFF));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case APP_MENU_OPEN: {
        RECT wr;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"eh6.6 menu item one (overflows the root)");
        AppendMenuW(menu, MF_STRING, 2, L"eh6.6 menu item two");
        AppendMenuW(menu, MF_STRING, 3, L"eh6.6 menu item three");
        GetWindowRect(hwnd, &wr);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, wr.left + 60, wr.top + 80, 0, hwnd, NULL);
        DestroyMenu(menu);
        return 0;
    }
    case APP_MENU_CLOSE:
        EndMenu();
        return 0;
    case APP_MODAL_OPEN:
        if (!g_modal) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            g_modal = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AsbEh66Dialog", L"eh6.6 modal", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                      wr.left + 60, wr.top + 70, 320, 180, hwnd, NULL, GetModuleHandleW(NULL), NULL);
            EnableWindow(hwnd, FALSE);
            ShowWindow(g_modal, SW_SHOWNOACTIVATE);
        }
        return 0;
    case APP_MODAL_CLOSE:
        if (g_modal) { EnableWindow(hwnd, TRUE); DestroyWindow(g_modal); g_modal = NULL; }
        return 0;
    case APP_UNSAVED:
        g_unsaved = (BOOL)wp;
        return 0;
    case WM_CLOSE:
        if (g_unsaved) {
            if (!g_prompt) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                g_prompt = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AsbEh66Dialog", L"eh6.6 save changes?",
                                           WS_POPUP | WS_CAPTION | WS_SYSMENU, wr.left + 80, wr.top + 90, 300, 160, hwnd, NULL,
                                           GetModuleHandleW(NULL), NULL);
                EnableWindow(hwnd, FALSE);
                ShowWindow(g_prompt, SW_SHOWNOACTIVATE);
            }
            return 0;   /* the user decides */
        }
        DestroyWindow(hwnd);
        return 0;
    case APP_CANCEL:
        if (g_prompt) { EnableWindow(hwnd, TRUE); DestroyWindow(g_prompt); g_prompt = NULL; }
        return 0;
    case APP_MAXIMIZE:
        ShowWindow(hwnd, SW_MAXIMIZE);
        return 0;
    case APP_MINIMIZE:
        ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
    case APP_QUIT:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        g_standard = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK custom_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            /* Keep the side/bottom resize borders, drop the system caption. */
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            UINT dpi = GetDpiForWindow(hwnd);
            int f = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            p->rgrc[0].left += f;
            p->rgrc[0].right -= f;
            p->rgrc[0].bottom -= f;
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcW(hwnd, msg, wp, lp);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        if (def != HTCLIENT) return def;
        ScreenToClient(hwnd, &pt);
        GetClientRect(hwnd, &rc);
        if (pt.y < 4) return HTTOP;
        if (pt.y < TITLE_H) {
            if (pt.x >= rc.right - BUTTON_W) return HTCLOSE;
            if (pt.x >= rc.right - 2 * BUTTON_W) return HTMAXBUTTON;
            if (pt.x >= rc.right - 3 * BUTTON_W) return HTMINBUTTON;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        fill(dc, 0, 0, rc.right, TITLE_H, RGB(0x10, 0x30, 0x90));
        fill(dc, rc.right - 3 * BUTTON_W + 14, 10, rc.right - 2 * BUTTON_W - 14, 22, RGB(0xC0, 0xC0, 0xC0));
        fill(dc, rc.right - 2 * BUTTON_W + 14, 10, rc.right - BUTTON_W - 14, 22, RGB(0xC0, 0xC0, 0xC0));
        fill(dc, rc.right - BUTTON_W, 0, rc.right, TITLE_H, RGB(0xD0, 0x20, 0x20));
        fill(dc, 0, TITLE_H, rc.right, rc.bottom, RGB(0xE0, 0xC0, 0x20));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_custom = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        fill(dc, 0, 0, rc.right, rc.bottom, RGB(0x30, 0x30, 0xD0));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_CLOSE) return 0;   /* closed by commands only */
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    MSG msg;
    (void)prev; (void)cmd; (void)show;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_INFORMATION);
    wc.lpfnWndProc = standard_proc;
    wc.lpszClassName = L"AsbEh66Standard";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = custom_proc;
    wc.lpszClassName = L"AsbEh66Custom";
    wc.hIcon = LoadIconW(NULL, IDI_WARNING);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = dialog_proc;
    wc.lpszClassName = L"AsbEh66Dialog";
    RegisterClassExW(&wc);
    g_standard = CreateWindowExW(0, L"AsbEh66Standard", L"eh6.6 standard", WS_OVERLAPPEDWINDOW, 140, 120, 640, 420, NULL,
                                 NULL, inst, NULL);
    g_custom = CreateWindowExW(0, L"AsbEh66Custom", L"eh6.6 custom", WS_OVERLAPPEDWINDOW, 820, 160, 560, 380, NULL, NULL,
                               inst, NULL);
    SetWindowPos(g_custom, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(g_standard, SW_SHOWNOACTIVATE);
    ShowWindow(g_custom, SW_SHOWNOACTIVATE);
    SetTimer(g_standard, 1, 50, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
