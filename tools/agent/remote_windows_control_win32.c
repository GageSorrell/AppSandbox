/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_control_win32
 *
 * @file      remote_windows_control_win32.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Win32 window control for host proxies (AppSandbox-eh6.6). See remote_windows_control_win32.h. */
#include "remote_windows_control_win32.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <string.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#define MESSAGE_TIMEOUT_MS 150u

BOOL asb_rw_ctl_frames(HWND hwnd, RECT *window, RECT *visible)
{
    if (!IsWindow(hwnd) || !GetWindowRect(hwnd, window)) return FALSE;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, visible, sizeof(*visible))) ||
        IsRectEmpty(visible))
        *visible = *window;
    return TRUE;
}

BOOL asb_rw_ctl_set_rect(HWND hwnd, const RECT *window, BOOL size)
{
    UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | (size ? 0u : SWP_NOSIZE);
    return SetWindowPos(hwnd, NULL, window->left, window->top, window->right - window->left,
                        window->bottom - window->top, flags) || IsWindow(hwnd);
}

BOOL asb_rw_ctl_responsive(HWND hwnd)
{
    DWORD_PTR result = 0;
    if (!IsWindow(hwnd) || IsHungAppWindow(hwnd)) return FALSE;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) != 0;
}

BOOL asb_rw_ctl_post_close(HWND hwnd)
{
    return IsWindow(hwnd) && PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
}

static LRESULT nc_hit(HWND hwnd, int x, int y, BOOL *ok)
{
    DWORD_PTR result = 0;
    *ok = SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM((WORD)(SHORT)x, (WORD)(SHORT)y),
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, MESSAGE_TIMEOUT_MS, &result) != 0;
    return (LRESULT)(LONG)(DWORD)result;
}

uint32_t asb_rw_ctl_hit_test(HWND root, int x, int y)
{
    HWND chain[16];
    int depth = 0;
    BOOL ok = FALSE;
    if (!IsWindow(root)) return HTNOWHERE;
    chain[depth++] = root;
    /* The deepest visible child under the point receives the mouse; it decides first. */
    while (depth < (int)(sizeof(chain) / sizeof(chain[0]))) {
        HWND parent = chain[depth - 1];
        POINT pt = { x, y };
        ScreenToClient(parent, &pt);
        HWND child = ChildWindowFromPointEx(parent, pt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!child || child == parent) break;
        chain[depth++] = child;
    }
    for (int i = depth - 1; i >= 0; --i) {
        LRESULT code = nc_hit(chain[i], x, y, &ok);
        if (!ok) return HTNOWHERE;
        if (code == HTTRANSPARENT && i > 0) continue;   /* falls through to the parent */
        if (code < 0) return HTNOWHERE;
        if (i > 0 && code == HTCLIENT) return HTCLIENT;
        return (uint32_t)code & 0xFFFFu;
    }
    return HTNOWHERE;
}

BOOL asb_rw_ctl_window_state(HWND hwnd, BOOL limits, AsbRwWindowState *state, UINT *show_cmd)
{
    WINDOWPLACEMENT wp;
    if (!IsWindow(hwnd)) return FALSE;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    memset(state, 0, sizeof(*state));
    if (style & WS_DISABLED) state->flags |= ASB_RW_WS_DISABLED;
    if (style & WS_THICKFRAME) state->flags |= ASB_RW_WS_RESIZABLE;
    if (style & WS_MINIMIZEBOX) state->flags |= ASB_RW_WS_MINIMIZABLE;
    if (style & WS_MAXIMIZEBOX) state->flags |= ASB_RW_WS_MAXIMIZABLE;
    if (style & WS_SYSMENU) state->flags |= ASB_RW_WS_SYSMENU;
    if (IsHungAppWindow(hwnd)) state->flags |= ASB_RW_WS_HUNG;
    state->dpi = GetDpiForWindow(hwnd);
    if (state->dpi > 1536) state->dpi = 1536;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    *show_cmd = GetWindowPlacement(hwnd, &wp) ? wp.showCmd : SW_SHOWNORMAL;
    if (limits) {
        RECT win, vis;
        MINMAXINFO mmi, defaults;
        DWORD_PTR result = 0;
        UINT dpi = state->dpi ? state->dpi : 96;
        if (!asb_rw_ctl_frames(hwnd, &win, &vis)) return FALSE;
        memset(&mmi, 0, sizeof(mmi));
        mmi.ptMinTrackSize.x = GetSystemMetricsForDpi(SM_CXMINTRACK, dpi);
        mmi.ptMinTrackSize.y = GetSystemMetricsForDpi(SM_CYMINTRACK, dpi);
        mmi.ptMaxTrackSize.x = GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi);
        mmi.ptMaxTrackSize.y = GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi);
        mmi.ptMaxSize = mmi.ptMaxTrackSize;
        defaults = mmi;
        if (SendMessageTimeoutW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mmi, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                MESSAGE_TIMEOUT_MS, &result)) {
            LONG mx = (vis.left - win.left) + (win.right - vis.right);
            LONG my = (vis.top - win.top) + (win.bottom - vis.bottom);
            LONG v;
            v = mmi.ptMinTrackSize.x - mx; state->min_width = v < 0 ? 0 : v > (LONG)ASB_RW_MAX_FRAME_WIDTH ? (LONG)ASB_RW_MAX_FRAME_WIDTH : v;
            v = mmi.ptMinTrackSize.y - my; state->min_height = v < 0 ? 0 : v > (LONG)ASB_RW_MAX_FRAME_HEIGHT ? (LONG)ASB_RW_MAX_FRAME_HEIGHT : v;
            if (mmi.ptMaxTrackSize.x > 0 && mmi.ptMaxTrackSize.x < defaults.ptMaxTrackSize.x) {
                v = mmi.ptMaxTrackSize.x - mx;
                state->max_width = v < 1 ? 1 : v > (LONG)ASB_RW_MAX_FRAME_WIDTH ? (LONG)ASB_RW_MAX_FRAME_WIDTH : v;
            }
            if (mmi.ptMaxTrackSize.y > 0 && mmi.ptMaxTrackSize.y < defaults.ptMaxTrackSize.y) {
                v = mmi.ptMaxTrackSize.y - my;
                state->max_height = v < 1 ? 1 : v > (LONG)ASB_RW_MAX_FRAME_HEIGHT ? (LONG)ASB_RW_MAX_FRAME_HEIGHT : v;
            }
            if (state->max_width && state->max_width < state->min_width) state->max_width = state->min_width;
            if (state->max_height && state->max_height < state->min_height) state->max_height = state->min_height;
        }
    }
    return TRUE;
}

BOOL asb_rw_ctl_normalize(HWND hwnd)
{
    if (!IsWindow(hwnd)) return FALSE;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return TRUE;
}

static HICON window_icon(HWND hwnd)
{
    static const WPARAM kinds[] = { ICON_BIG, ICON_SMALL2, ICON_SMALL };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, kinds[i], 96, SMTO_ABORTIFHUNG | SMTO_BLOCK, MESSAGE_TIMEOUT_MS,
                                &result) && result)
            return (HICON)result;
    }
    HICON icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    if (!icon) icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    return icon;
}

/* The executable's own icon (packaged/XAML applications often set no window icon). */
static HICON executable_icon(HWND hwnd)
{
    DWORD pid = 0, len = MAX_PATH;
    WCHAR path[MAX_PATH];
    HICON large = NULL;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    if (!process) return NULL;
    BOOL ok = QueryFullProcessImageNameW(process, 0, path, &len);
    CloseHandle(process);
    if (!ok || ExtractIconExW(path, 0, &large, NULL, 1) == 0) return NULL;
    return large;
}

static int icon_size(HICON icon)
{
    ICONINFO info;
    BITMAP bm;
    int size = 32;
    memset(&info, 0, sizeof(info));
    if (!GetIconInfo(icon, &info)) return size;
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bm), &bm)) size = bm.bmWidth;
    else if (info.hbmMask && GetObjectW(info.hbmMask, sizeof(bm), &bm)) size = bm.bmWidth;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return size < 16 ? 16 : size > (int)ASB_RW_MAX_ICON ? (int)ASB_RW_MAX_ICON : size;
}

/* Renders on black and on white: alpha = 255 - (white - black), colour = the black render
   (already premultiplied). Works for alpha, masked and monochrome icons alike. */
static BOOL render_icon(HICON icon, AsbRwWindowIcon *out)
{
    int n = icon_size(icon);
    BITMAPINFO bi;
    void *bits[2] = { NULL, NULL };
    HBITMAP dib[2] = { NULL, NULL };
    BOOL ok = FALSE;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = n;
    bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    for (int pass = 0; pass < 2 && dc; ++pass) {
        dib[pass] = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits[pass], NULL, 0);
        if (!dib[pass]) break;
        memset(bits[pass], pass ? 0xFF : 0x00, (size_t)n * n * 4);
        HGDIOBJ old = SelectObject(dc, dib[pass]);
        DrawIconEx(dc, 0, 0, icon, n, n, 0, NULL, DI_NORMAL);
        GdiFlush();
        SelectObject(dc, old);
    }
    if (bits[0] && bits[1]) {
        const uint8_t *black = (const uint8_t *)bits[0], *white = (const uint8_t *)bits[1];
        for (int i = 0; i < n * n; ++i) {
            int diff = ((white[i * 4] - black[i * 4]) + (white[i * 4 + 1] - black[i * 4 + 1]) +
                        (white[i * 4 + 2] - black[i * 4 + 2])) / 3;
            int a = 255 - diff;
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            for (int c = 0; c < 3; ++c) {
                int v = black[i * 4 + c];
                out->pixels[i * 4 + c] = (uint8_t)(v > a ? a : v);
            }
            out->pixels[i * 4 + 3] = (uint8_t)a;
        }
        out->width = out->height = (uint32_t)n;
        ok = TRUE;
    }
    for (int pass = 0; pass < 2; ++pass) if (dib[pass]) DeleteObject(dib[pass]);
    if (dc) DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return ok;
}

BOOL asb_rw_ctl_icon(HWND hwnd, uint64_t *key, AsbRwWindowIcon *icon)
{
    if (!IsWindow(hwnd)) return FALSE;
    HICON handle = window_icon(hwnd);
    *key = handle ? (uint64_t)(ULONG_PTR)handle : 1u;
    if (!icon) return TRUE;
    if (handle) return render_icon(handle, icon);
    HICON exe = executable_icon(hwnd);
    if (!exe) return FALSE;
    BOOL ok = render_icon(exe, icon);
    DestroyIcon(exe);
    return ok;
}

/* ---- activation (eh6.7) ------------------------------------------------------------------ */

uint32_t asb_rw_ctl_foreground(uint64_t *chain, uint32_t max)
{
    uint32_t n = 0;
    HWND w = GetForegroundWindow();
    while (w && n < max) {
        chain[n++] = (uint64_t)(ULONG_PTR)w;
        w = GetWindow(w, GW_OWNER);
    }
    return n;
}

BOOL asb_rw_ctl_activate(HWND hwnd)
{
    if (!IsWindow(hwnd)) return FALSE;
    if (GetForegroundWindow() == hwnd) return TRUE;
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return TRUE;
    /* The foreground lock refused a background process: join the current foreground thread's
       input state for the call (never waits on that thread's message loop). */
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    BOOL attached = other && other != me && AttachThreadInput(me, other, TRUE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    if (attached) AttachThreadInput(me, other, FALSE);
    if (GetForegroundWindow() != hwnd) SwitchToThisWindow(hwnd, TRUE);
    return GetForegroundWindow() == hwnd;
}

uint64_t asb_rw_ctl_desktop(void)
{
    return (uint64_t)(ULONG_PTR)GetShellWindow();
}

/* ---- AsbRwHelperOps adapters ------------------------------------------------------------- */

static BOOL op_frames(void *ctx, HWND hwnd, RECT *window, RECT *visible) { (void)ctx; return asb_rw_ctl_frames(hwnd, window, visible); }
static BOOL op_set_rect(void *ctx, HWND hwnd, const RECT *window, BOOL size) { (void)ctx; return asb_rw_ctl_set_rect(hwnd, window, size); }
static BOOL op_responsive(void *ctx, HWND hwnd) { (void)ctx; return asb_rw_ctl_responsive(hwnd); }
static BOOL op_post_close(void *ctx, HWND hwnd) { (void)ctx; return asb_rw_ctl_post_close(hwnd); }
static uint32_t op_hit_test(void *ctx, HWND hwnd, int x, int y) { (void)ctx; return asb_rw_ctl_hit_test(hwnd, x, y); }
static BOOL op_window_state(void *ctx, HWND hwnd, BOOL limits, AsbRwWindowState *state, UINT *show_cmd)
{
    (void)ctx;
    return asb_rw_ctl_window_state(hwnd, limits, state, show_cmd);
}
static BOOL op_normalize(void *ctx, HWND hwnd) { (void)ctx; return asb_rw_ctl_normalize(hwnd); }
static BOOL op_icon(void *ctx, HWND hwnd, uint64_t *key, AsbRwWindowIcon *icon) { (void)ctx; return asb_rw_ctl_icon(hwnd, key, icon); }
static uint32_t op_foreground(void *ctx, uint64_t *chain, uint32_t max) { (void)ctx; return asb_rw_ctl_foreground(chain, max); }
static BOOL op_activate(void *ctx, HWND hwnd) { (void)ctx; return asb_rw_ctl_activate(hwnd); }
static uint64_t op_desktop(void *ctx) { (void)ctx; return asb_rw_ctl_desktop(); }

void asb_rw_ctl_ops(AsbRwHelperOps *ops)
{
    ops->frames = op_frames;
    ops->set_rect = op_set_rect;
    ops->responsive = op_responsive;
    ops->post_close = op_post_close;
    ops->hit_test = op_hit_test;
    ops->window_state = op_window_state;
    ops->normalize = op_normalize;
    ops->icon = op_icon;
    ops->foreground = op_foreground;
    ops->activate = op_activate;
    ops->desktop = op_desktop;
}
