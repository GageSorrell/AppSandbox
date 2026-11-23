/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_capture_test_app
 *
 * @file      remote_windows_capture_test_app.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Guest-side content for the eh6.5 native frame check (run unelevated in the Windows 11 guest).
 *
 * Shows "eh6.5 capture test": an animated root (color changes every 15 ms, magenta marker at
 * client 16,16 - 64,64, frame counter text), an owned per-pixel-alpha popup that overflows the
 * root's right edge, and - every 30 s cycle - a 14-item popup menu that overflows the root's right
 * edge (open from 8 s to 14 s of the cycle). With --device-loss (default) it signals the helper's
 * Local\AppSandbox.RemoteWindows.SimulateDeviceLoss event at 20 s of every cycle so the capture
 * engine's device-loss recovery is exercised and logged. Exits after --seconds (default 300).
 *
 *   remote-windows-capture-test-app.exe [--seconds N] [--no-device-loss]
 *
 * Log: %TEMP%\eh6.5-capture-test-app.log (timestamps of menu open/close and device-loss signals).
 */
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

#define CYCLE_MS     30000u
#define MENU_OPEN_MS  8000u
#define MENU_CLOSE_MS 14000u
#define LOSS_MS      20000u

static HWND g_root, g_alpha;
static volatile LONG g_ticks, g_menu_open;
static ULONGLONG g_start;
static int g_device_loss = 1;
static DWORD g_seconds = 300;
static FILE *g_log;

static void alog(const char *fmt, ...)
{
    va_list a;
    SYSTEMTIME t;
    if (!g_log) return;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u +%6.1fs ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (double)(GetTickCount64() - g_start) / 1000.0);
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static void paint_alpha(void)
{
    RECT r;
    GetWindowRect(g_alpha, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            BYTE a = (BYTE)(x * 255 / (w - 1));
            BYTE g = (BYTE)(200 * a / 255);
            ((DWORD *)bits)[y * w + x] = ((DWORD)a << 24) | ((DWORD)g << 8);
        }
    POINT src = { 0, 0 }, dst = { r.left, r.top };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_alpha, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static void signal_device_loss(void)
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\AppSandbox.RemoteWindows.SimulateDeviceLoss");
    if (!e) { alog("device loss: helper event not found (%lu)", GetLastError()); return; }
    SetEvent(e);
    CloseHandle(e);
    alog("device loss: signalled the capture engine");
}

static LRESULT CALLBACK root_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static ULONGLONG last_cycle_menu = (ULONGLONG)-1, last_cycle_loss = (ULONGLONG)-1;
    switch (msg) {
    case WM_TIMER: {
        ULONGLONG t = GetTickCount64() - g_start, cycle = t / CYCLE_MS, phase = t % CYCLE_MS;
        InterlockedIncrement(&g_ticks);
        InvalidateRect(hwnd, NULL, FALSE);
        {   /* like a real app, keep the owned popup at its place relative to the (parked) root */
            RECT rr, pr;
            GetWindowRect(hwnd, &rr);
            GetWindowRect(g_alpha, &pr);
            if (pr.left != rr.right - 80 || pr.top != rr.top + 200)
                SetWindowPos(g_alpha, NULL, rr.right - 80, rr.top + 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (t >= (ULONGLONG)g_seconds * 1000u) { DestroyWindow(g_alpha); DestroyWindow(hwnd); return 0; }
        if (g_menu_open && phase >= MENU_CLOSE_MS) EndMenu();
        if (!g_menu_open && phase >= MENU_OPEN_MS && phase < MENU_CLOSE_MS && last_cycle_menu != cycle) {
            last_cycle_menu = cycle;
            PostMessageW(hwnd, WM_APP, 0, 0);
        }
        if (g_device_loss && phase >= LOSS_MS && last_cycle_loss != cycle) {
            last_cycle_loss = cycle;
            signal_device_loss();
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        LONG t = g_ticks;
        HBRUSH bg = CreateSolidBrush(RGB((t * 7) & 255, 90, (t * 3) & 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT marker = { 16, 16, 64, 64 };
        HBRUSH mg = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &marker, mg);
        DeleteObject(mg);
        char text[64];
        int n = sprintf_s(text, sizeof(text), "eh6.5 frame %ld", t);
        TextOutA(dc, 80, 30, text, n);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_APP: {
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < 14; ++i) {
            char item[64];
            sprintf_s(item, sizeof(item), "Overflowing menu item %d with a long label", i + 1);
            AppendMenuA(menu, MF_STRING, 100 + i, item);
        }
        RECT r;
        GetWindowRect(hwnd, &r);
        InterlockedExchange(&g_menu_open, 1);
        alog("menu: open at (%ld,%ld), root (%ld,%ld,%ld,%ld)", r.right - 60, r.top + 90, r.left, r.top, r.right, r.bottom);
        TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, r.right - 60, r.top + 90, hwnd, NULL);
        InterlockedExchange(&g_menu_open, 0);
        alog("menu: closed");
        DestroyMenu(menu);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(cmd && *cmd ? GetCommandLineW() : L"app", &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--seconds") && i + 1 < argc) g_seconds = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--no-device-loss")) g_device_loss = 0;
    }
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, sizeof(path), "eh6.5-capture-test-app.log");
    fopen_s(&g_log, path, "w");
    g_start = GetTickCount64();
    alog("start: %lu s, device loss %s", g_seconds, g_device_loss ? "every cycle" : "off");
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = root_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"AsbEh65Root";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AsbEh65Alpha";
    RegisterClassW(&wc);
    g_root = CreateWindowExW(0, L"AsbEh65Root", L"eh6.5 capture test", WS_OVERLAPPEDWINDOW, 160, 140, 640, 420, NULL, NULL, inst, NULL);
    RECT r;
    GetWindowRect(g_root, &r);
    g_alpha = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"AsbEh65Alpha", L"", WS_POPUP,
                              r.right - 80, r.top + 200, 220, 120, g_root, NULL, inst, NULL);
    ShowWindow(g_root, SW_SHOWNORMAL);
    paint_alpha();
    ShowWindow(g_alpha, SW_SHOWNOACTIVATE);
    SetTimer(g_root, 1, 15, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    alog("exit");
    if (g_log) fclose(g_log);
    if (argv) LocalFree(argv);
    return 0;
}
