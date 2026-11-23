/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_tray
 *
 * @file      remote_windows_tray.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Guest tray icon + selection flyout of the Remote Windows helper (AppSandbox-eh6.8). */
#include "remote_windows_tray.h"
#include "remote_windows_control_win32.h"
#include <stdlib.h>
#include <string.h>

#define TRAY_CLASS     L"AppSandbox.RemoteWindows.GuestTray"
#define WM_TRAY_ICON   (WM_APP + 1)
#define WM_TRAY_MODEL  (WM_APP + 2)
#define WM_TRAY_STOP   (WM_APP + 3)
#define TIMER_CLICK    1
#define TIMER_ADD      2
#define CMD_RESTORE    1
#define CMD_EXIT       2
#define ICON_RECHECK_MS 2000u

typedef struct IconCache { uint64_t hwnd, key, tick; HICON icon; } IconCache;

struct AsbRwGuestTray {
    AsbRwGuestTrayConfig cfg;
    HANDLE thread, ready;
    DWORD thread_id;
    HWND hwnd;
    AsbRwTrayIcon tray;
    AsbRwTrayClicks clicks;
    AsbRwFlyout *flyout;
    CRITICAL_SECTION lock;
    AsbRwFlyoutModel *pending, *model;   /* pending: written by publishers; model: tray thread */
    uint64_t *pending_hwnds, *hwnds;
    volatile LONG dirty;
    IconCache icons[ASB_RW_FLYOUT_MAX_ITEMS];
};

static IconCache *icon_slot(AsbRwGuestTray *t, uint64_t hwnd)
{
    IconCache *free_slot = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < ASB_RW_FLYOUT_MAX_ITEMS; ++i) {
        IconCache *c = &t->icons[i];
        if (c->hwnd == hwnd) return c;
        if (!c->hwnd && !free_slot) free_slot = c;
        if (!oldest || c->tick < oldest->tick) oldest = c;
    }
    IconCache *c = free_slot ? free_slot : oldest;
    if (c->icon) DestroyIcon(c->icon);
    memset(c, 0, sizeof(*c));
    c->hwnd = hwnd;
    c->key = ~0ull;
    return c;
}

static HICON icon_for(AsbRwGuestTray *t, uint64_t hwnd)
{
    static AsbRwWindowIcon pixels;
    if (!hwnd) return NULL;
    IconCache *c = icon_slot(t, hwnd);
    uint64_t now = GetTickCount64();
    if (c->tick && now - c->tick < ICON_RECHECK_MS) return c->icon;
    c->tick = now;
    uint64_t key = 0;
    if (!asb_rw_ctl_icon((HWND)(ULONG_PTR)hwnd, &key, NULL)) key = 0;
    if (key == c->key) return c->icon;
    c->key = key;
    if (c->icon) { DestroyIcon(c->icon); c->icon = NULL; }
    memset(&pixels, 0, offsetof(AsbRwWindowIcon, pixels));
    if (key && asb_rw_ctl_icon((HWND)(ULONG_PTR)hwnd, &key, &pixels)) c->icon = asb_rw_icon_to_hicon(&pixels);
    return c->icon;
}

static void apply_model(AsbRwGuestTray *t)
{
    EnterCriticalSection(&t->lock);
    InterlockedExchange(&t->dirty, 0);
    memcpy(t->model, t->pending, sizeof(*t->model));
    memcpy(t->hwnds, t->pending_hwnds, sizeof(uint64_t) * ASB_RW_FLYOUT_MAX_ITEMS);
    LeaveCriticalSection(&t->lock);
    for (uint32_t i = 0; i < t->model->count; ++i) t->model->items[i].icon = icon_for(t, t->hwnds[i]);
    asb_rw_flyout_update(t->flyout, t->model);
}

static void toggle(void *ctx, uint64_t id, int select)
{
    AsbRwGuestTray *t = (AsbRwGuestTray *)ctx;
    if (t->cfg.post) t->cfg.post(t->cfg.ctx, ASB_RW_ACTION_SELECT, id, select);
}

static void flyout_closed(void *ctx, int deactivated)
{
    AsbRwGuestTray *t = (AsbRwGuestTray *)ctx;
    if (deactivated) asb_rw_tray_closed(&t->clicks, GetTickCount64());
}

static void open_flyout(AsbRwGuestTray *t)
{
    RECT anchor;
    if (asb_rw_flyout_visible(t->flyout)) { asb_rw_flyout_hide(t->flyout); return; }
    apply_model(t);
    asb_rw_flyout_show(t->flyout, asb_rw_tray_rect(&t->tray, &anchor) ? &anchor : NULL);
}

static void show_menu(AsbRwGuestTray *t)
{
    POINT pt;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, CMD_RESTORE, L"Restore all windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(t->hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, t->hwnd, NULL);
    PostMessageW(t->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == CMD_RESTORE && t->cfg.post) t->cfg.post(t->cfg.ctx, ASB_RW_ACTION_RESTORE_ALL, 0, 0);
    if (cmd == CMD_EXIT && t->cfg.post) {
        asb_rw_flyout_hide(t->flyout);
        t->cfg.post(t->cfg.ctx, ASB_RW_ACTION_EXIT, 0, 0);
    }
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwGuestTray *t = (AsbRwGuestTray *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!t) return DefWindowProcW(hwnd, msg, wp, lp);
    if (asb_rw_tray_message(&t->tray, msg)) return 0;   /* Explorer restarted */
    switch (msg) {
    case WM_TRAY_ICON: {
        uint64_t now = GetTickCount64();
        switch (asb_rw_tray_event(&t->clicks, (UINT)lp, now, GetDoubleClickTime())) {
        case ASB_RW_TRAY_ARM: SetTimer(hwnd, TIMER_CLICK, GetDoubleClickTime(), NULL); break;
        case ASB_RW_TRAY_DOUBLE: KillTimer(hwnd, TIMER_CLICK); open_flyout(t); break;   /* no main window */
        case ASB_RW_TRAY_MENU: KillTimer(hwnd, TIMER_CLICK); show_menu(t); break;
        default: break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_CLICK) {
            KillTimer(hwnd, TIMER_CLICK);
            if (asb_rw_tray_timer(&t->clicks, GetTickCount64()) == ASB_RW_TRAY_OPEN) open_flyout(t);
        } else if (wp == TIMER_ADD) {
            if (t->tray.added || asb_rw_tray_add(&t->tray)) KillTimer(hwnd, TIMER_ADD);
        }
        return 0;
    case WM_TRAY_MODEL:
        apply_model(t);
        return 0;
    case WM_TRAY_STOP:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        asb_rw_tray_remove(&t->tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI tray_thread(LPVOID arg)
{
    AsbRwGuestTray *t = (AsbRwGuestTray *)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    MSG msg;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = TRAY_CLASS;
    RegisterClassExW(&wc);
    /* A hidden top-level window (not message-only): TaskbarCreated is broadcast to top-level windows. */
    t->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, L"App Sandbox Remote Windows", WS_POPUP, 0, 0, 0, 0, NULL, NULL,
                              inst, NULL);
    AsbRwFlyoutConfig fc;
    memset(&fc, 0, sizeof(fc));
    fc.ctx = t;
    fc.toggle = toggle;
    fc.closed = flyout_closed;
    fc.theme = t->cfg.theme;
    t->flyout = t->hwnd ? asb_rw_flyout_create(&fc) : NULL;
    if (t->hwnd && t->flyout) {
        SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, (LONG_PTR)t);
        t->tray.hwnd = t->hwnd;
        t->tray.id = 1;
        t->tray.callback = WM_TRAY_ICON;
        t->tray.icon = t->cfg.icon ? t->cfg.icon : LoadIconW(inst, MAKEINTRESOURCEW(1));
        if (!t->tray.icon) t->tray.icon = LoadIconW(NULL, IDI_APPLICATION);
        wcscpy_s(t->tray.tip, ARRAYSIZE(t->tray.tip), L"App Sandbox: show windows on the host");
        if (!asb_rw_tray_add(&t->tray)) SetTimer(t->hwnd, TIMER_ADD, 2000, NULL);   /* taskbar not up yet */
    }
    SetEvent(t->ready);
    if (!t->hwnd || !t->flyout) return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    asb_rw_flyout_destroy(t->flyout);
    t->flyout = NULL;
    for (uint32_t i = 0; i < ASB_RW_FLYOUT_MAX_ITEMS; ++i) if (t->icons[i].icon) DestroyIcon(t->icons[i].icon);
    return 0;
}

AsbRwGuestTray *asb_rw_guest_tray_start(const AsbRwGuestTrayConfig *config)
{
    AsbRwGuestTray *t = (AsbRwGuestTray *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cfg = *config;
    InitializeCriticalSection(&t->lock);
    t->pending = (AsbRwFlyoutModel *)calloc(1, sizeof(*t->pending));
    t->model = (AsbRwFlyoutModel *)calloc(1, sizeof(*t->model));
    t->pending_hwnds = (uint64_t *)calloc(ASB_RW_FLYOUT_MAX_ITEMS, sizeof(uint64_t));
    t->hwnds = (uint64_t *)calloc(ASB_RW_FLYOUT_MAX_ITEMS, sizeof(uint64_t));
    t->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (t->pending) t->pending->status = ASB_RW_FLYOUT_DISCONNECTED;
    t->thread = t->pending && t->model && t->pending_hwnds && t->hwnds && t->ready
                    ? CreateThread(NULL, 0, tray_thread, t, 0, &t->thread_id) : NULL;
    if (t->thread) WaitForSingleObject(t->ready, INFINITE);
    if (!t->thread || !t->hwnd || !t->flyout) {
        asb_rw_guest_tray_stop(t);
        return NULL;
    }
    return t;
}

void asb_rw_guest_tray_publish(AsbRwGuestTray *t, const AsbRwFlyoutModel *m, const uint64_t *hwnds)
{
    if (!t) return;
    EnterCriticalSection(&t->lock);
    memcpy(t->pending, m, offsetof(AsbRwFlyoutModel, items) + (size_t)m->count * sizeof(m->items[0]));
    memset(t->pending_hwnds, 0, sizeof(uint64_t) * ASB_RW_FLYOUT_MAX_ITEMS);
    if (hwnds) memcpy(t->pending_hwnds, hwnds, sizeof(uint64_t) * (m->count < ASB_RW_FLYOUT_MAX_ITEMS ? m->count : ASB_RW_FLYOUT_MAX_ITEMS));
    LeaveCriticalSection(&t->lock);
    if (!InterlockedExchange(&t->dirty, 1)) PostMessageW(t->hwnd, WM_TRAY_MODEL, 0, 0);
}

void asb_rw_guest_tray_stop(AsbRwGuestTray *t)
{
    if (!t) return;
    if (t->thread) {
        if (t->hwnd) PostMessageW(t->hwnd, WM_TRAY_STOP, 0, 0);
        else PostThreadMessageW(t->thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(t->thread, 10000);
        CloseHandle(t->thread);
    }
    if (t->ready) CloseHandle(t->ready);
    DeleteCriticalSection(&t->lock);
    free(t->pending);
    free(t->model);
    free(t->pending_hwnds);
    free(t->hwnds);
    free(t);
}

HWND asb_rw_guest_tray_window(AsbRwGuestTray *t) { return t ? t->hwnd : NULL; }
AsbRwFlyout *asb_rw_guest_tray_flyout(AsbRwGuestTray *t) { return t ? t->flyout : NULL; }
uint32_t asb_rw_guest_tray_adds(AsbRwGuestTray *t) { return t ? t->tray.adds : 0; }
