/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_selector_native_test
 *
 * @file      remote_windows_selector_native_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows selectors (AppSandbox-eh6.8), native UI: real flyout windows on this desktop.
 * Toggles only request (pending until acknowledged), rejected requests show their reason, live
 * title/window changes keep the focused row, keyboard navigation/Enter/Space/Esc, disabled
 * (upgrade) and empty states, deactivation closes, light/dark theme, DPI scaling, scrolling,
 * MSAA accessibility (checkable named list items), placement on every taskbar edge, tray icon
 * re-creation on TaskbarCreated (Explorer restart) and the guest tray thread end to end.
 *
 *   remote-windows-selector-native-test.exe [--evidence <dir>]   (saves flyout screenshots)
 */
#define COBJMACROS
#include "../agent/remote_windows_flyout.h"
#include "../agent/remote_windows_tray.h"
#include <commctrl.h>
#include <oleacc.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static const wchar_t *g_evidence;

static void pump(DWORD ms)
{
    ULONGLONG until = GetTickCount64() + ms;
    MSG msg;
    do {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (ms) Sleep(5);
    } while (GetTickCount64() < until);
}

typedef struct Toggle { uint64_t id; int select; } Toggle;
static Toggle g_toggles[32];
static int g_ntoggles, g_closed, g_closed_deactivated;
static void on_toggle(void *ctx, uint64_t id, int select) { (void)ctx; if (g_ntoggles < 32) { g_toggles[g_ntoggles].id = id; g_toggles[g_ntoggles].select = select; } ++g_ntoggles; }
static void on_closed(void *ctx, int deactivated) { (void)ctx; ++g_closed; g_closed_deactivated = deactivated; }

static AsbRwFlyoutModel g_model;

static void item(uint32_t i, uint64_t id, const wchar_t *label, uint32_t state, uint32_t error)
{
    AsbRwFlyoutItem *it = &g_model.items[i];
    memset(it, 0, sizeof(*it));
    it->id = id;
    it->state = state;
    it->error = error;
    wcscpy_s(it->label, ASB_RW_FLYOUT_LABEL, label);
    if (g_model.count <= i) g_model.count = i + 1;
}

static void base_model(void)
{
    memset(&g_model, 0, sizeof(g_model));
    wcscpy_s(g_model.heading, 160, L"Windows on Main");
    g_model.status = ASB_RW_FLYOUT_READY;
    wcscpy_s(g_model.message, 320, L"Turn a window on to show it on this PC.");
    item(0, 101, L"Untitled - Notepad", ASB_RW_STATE_NONE, 0);
    item(1, 102, L"Untitled - Paint", ASB_RW_STATE_SHARED, 0);
    item(2, 103, L"Report", ASB_RW_STATE_NONE, ASB_RW_ERR_ELEVATED);
    g_model.items[0].icon = LoadIconW(NULL, IDI_INFORMATION);
}

static UINT image_of(HWND list, int i) { return (ListView_GetItemState(list, i, LVIS_STATEIMAGEMASK) & LVIS_STATEIMAGEMASK) >> 12; }
static void text_of(HWND list, int i, int sub, wchar_t *out, int cap) { ListView_GetItemText(list, i, sub, out, cap); }

static void key(HWND hwnd, UINT vk)
{
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM down = 1 | ((LPARAM)scan << 16), up = down | (1LL << 30) | (1LL << 31);
    SendMessageW(hwnd, WM_KEYDOWN, vk, down);
    if (vk == VK_SPACE) SendMessageW(hwnd, WM_CHAR, L' ', down);
    if (vk == VK_RETURN) SendMessageW(hwnd, WM_CHAR, L'\r', down);
    SendMessageW(hwnd, WM_KEYUP, vk, up);
}

static void focus_row(HWND list, int i)
{
    ListView_SetItemState(list, -1, 0, LVIS_FOCUSED | LVIS_SELECTED);
    ListView_SetItemState(list, i, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
}

static int focused(HWND list) { return ListView_GetNextItem(list, -1, LVNI_FOCUSED); }

static void save_shot(HWND hwnd, const wchar_t *name)
{
    if (!g_evidence) return;
    RECT r;
    GetWindowRect(hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
    DWORD stride = ((DWORD)w * 3 + 3) & ~3u, size = stride * (DWORD)h;
    BYTE *bits = (BYTE *)malloc(size);
    GetDIBits(dc, bmp, 0, (UINT)h, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\%s.bmp", g_evidence, name);
    FILE *f = NULL;
    if (bits && !_wfopen_s(&f, path, L"wb") && f) {
        BITMAPFILEHEADER fh;
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi); fh.bfSize = fh.bfOffBits + size;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&bi, sizeof(bi), 1, f); fwrite(bits, size, 1, f);
        fclose(f);
    }
    free(bits);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
}

static RECT primary_work(void)
{
    MONITORINFO mi;
    POINT origin = { 0, 0 };
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}

static void test_flyout(int theme, const wchar_t *shot)
{
    AsbRwFlyoutConfig fc;
    AsbRwFlyoutInfo info;
    wchar_t text[400];
    memset(&fc, 0, sizeof(fc));
    fc.toggle = on_toggle;
    fc.closed = on_closed;
    fc.theme = theme;
    g_ntoggles = g_closed = 0;
    AsbRwFlyout *f = asb_rw_flyout_create(&fc);
    CHECK(f != NULL);
    if (!f) return;
    base_model();
    asb_rw_flyout_update(f, &g_model);
    RECT work = primary_work();
    RECT anchor = { work.right - 120, work.bottom + 8, work.right - 96, work.bottom + 32 };   /* bottom taskbar icon */
    asb_rw_flyout_show(f, &anchor);
    pump(100);
    asb_rw_flyout_info(f, &info);
    HWND list = info.list;
    CHECK(asb_rw_flyout_visible(f) && IsWindowVisible(info.hwnd));
    CHECK_EQ(ListView_GetItemCount(list), 3);
    text_of(list, 0, 0, text, 400);
    CHECK(!wcscmp(text, L"Untitled - Notepad"));
    CHECK_EQ(image_of(list, 0), 1);
    CHECK_EQ(image_of(list, 1), 2);
    text_of(list, 1, 1, text, 400);
    CHECK(!wcscmp(text, L"Shared"));
    text_of(list, 2, 1, text, 400);
    CHECK(!wcsncmp(text, L"Failed: ", 8));                    /* rejected earlier: reason shown */
    GetWindowTextW(info.heading, text, 400);
    CHECK(!wcscmp(text, L"Windows on Main"));
    GetWindowTextW(info.status, text, 400);
    CHECK(!wcscmp(text, L"Turn a window on to show it on this PC."));
    /* placement: above the bottom taskbar, inside the work area */
    RECT wr;
    GetWindowRect(info.hwnd, &wr);
    int gap = MulDiv(12, (int)info.dpi, 96);
    CHECK_EQ(wr.bottom, work.bottom - gap);
    CHECK(wr.left >= work.left && wr.right <= work.right - gap + 1);
    /* theme */
    CHECK_EQ(ListView_GetBkColor(list), theme == ASB_RW_THEME_DARK ? RGB(43, 43, 43) : RGB(249, 249, 249));
    CHECK_EQ(info.dark, theme == ASB_RW_THEME_DARK);
    save_shot(info.hwnd, shot);

    /* Space on a row: a request only; the row waits for acknowledgement. */
    focus_row(list, 0);
    key(list, VK_SPACE);
    pump(50);
    CHECK_EQ(g_ntoggles, 1);
    CHECK(g_toggles[0].id == 101 && g_toggles[0].select == 1);
    asb_rw_flyout_info(f, &info);
    CHECK_EQ(info.requested, 1);
    CHECK_EQ(image_of(list, 0), 4);                            /* dimmed "on": heading there */
    text_of(list, 0, 1, text, 400);
    CHECK(!wcscmp(text, L"Requesting…"));
    key(list, VK_SPACE);                                       /* pending: refused */
    pump(50);
    CHECK_EQ(g_ntoggles, 1);
    /* Acknowledged: pending -> shared. */
    g_model.items[0].state = ASB_RW_STATE_PENDING;
    asb_rw_flyout_update(f, &g_model);
    asb_rw_flyout_info(f, &info);
    CHECK_EQ(info.requested, 0);
    text_of(list, 0, 1, text, 400);
    CHECK(!wcscmp(text, L"Sharing…"));
    g_model.items[0].state = ASB_RW_STATE_SHARED;
    asb_rw_flyout_update(f, &g_model);
    CHECK_EQ(image_of(list, 0), 2);
    /* Enter toggles too (unselect a shared row). */
    focus_row(list, 1);
    key(list, VK_RETURN);
    pump(50);
    CHECK(g_ntoggles == 2 && g_toggles[1].id == 102 && g_toggles[1].select == 0);
    /* A rejected select: the reason replaces "Requesting...". */
    focus_row(list, 2);
    key(list, VK_SPACE);
    pump(50);
    CHECK(g_ntoggles == 3 && g_toggles[2].id == 103 && g_toggles[2].select == 1);
    g_model.items[2].error = ASB_RW_ERR_LIMIT;
    asb_rw_flyout_update(f, &g_model);
    text_of(list, 2, 1, text, 400);
    CHECK(!wcsncmp(text, L"Failed: ", 8));
    CHECK_EQ(image_of(list, 2), 1);
    /* Keyboard navigation. */
    focus_row(list, 0);
    key(list, VK_END);
    CHECK_EQ(focused(list), 2);
    key(list, VK_HOME);
    CHECK_EQ(focused(list), 0);
    key(list, VK_DOWN);
    CHECK_EQ(focused(list), 1);
    /* Live changes: a new window above and a new title keep the focus on the same window. */
    g_model.items[3] = g_model.items[2];
    g_model.items[2] = g_model.items[1];
    g_model.items[1] = g_model.items[0];
    item(0, 104, L"New window", ASB_RW_STATE_NONE, 0);
    g_model.count = 4;
    wcscpy_s(g_model.items[2].label, ASB_RW_FLYOUT_LABEL, L"Untitled - Paint (2)");
    asb_rw_flyout_update(f, &g_model);
    CHECK_EQ(ListView_GetItemCount(list), 4);
    CHECK_EQ(focused(list), 2);                                /* still Paint */
    text_of(list, 2, 0, text, 400);
    CHECK(!wcscmp(text, L"Untitled - Paint (2)"));
    /* Accessibility: a checkable, named list item per window. */
    IAccessible *acc = NULL;
    if (SUCCEEDED(AccessibleObjectFromWindow(list, (DWORD)OBJID_CLIENT, &IID_IAccessible, (void **)&acc)) && acc) {
        long children = 0;
        VARIANT child, state;
        BSTR name = NULL;
        VARIANT role;
        IAccessible_get_accChildCount(acc, &children);
        CHECK(children >= 4);   /* the rows (+ the hidden header) */
        VariantInit(&child);
        child.vt = VT_I4;
        for (long c = 1; c <= 4; ++c) {   /* rows are child ids 1..n */
            child.lVal = c;
            VariantInit(&role);
            HRESULT hr = IAccessible_get_accRole(acc, child, &role);
            CHECK(SUCCEEDED(hr) && role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_CHECKBUTTON);   /* a toggle per window */
        }
        child.lVal = 3;   /* row 2 (1-based child id) */
        CHECK(SUCCEEDED(IAccessible_get_accName(acc, child, &name)) && name && !wcscmp(name, L"Untitled - Paint (2)"));
        SysFreeString(name);
        child.lVal = 2;   /* Notepad: shared -> checked */
        VariantInit(&state);
        CHECK(SUCCEEDED(IAccessible_get_accState(acc, child, &state)) && state.vt == VT_I4 && (state.lVal & STATE_SYSTEM_CHECKED));
        child.lVal = 1;   /* "New window": off */
        VariantInit(&state);
        CHECK(SUCCEEDED(IAccessible_get_accState(acc, child, &state)) && state.vt == VT_I4 && !(state.lVal & STATE_SYSTEM_CHECKED));
        IAccessible_Release(acc);
    } else {
        CHECK(!"AccessibleObjectFromWindow failed");
    }
    /* Esc closes. */
    g_closed = 0;
    key(list, VK_ESCAPE);
    pump(20);
    CHECK(!asb_rw_flyout_visible(f) && !IsWindowVisible(info.hwnd));
    CHECK(g_closed == 1 && g_closed_deactivated == 0);
    /* Losing activation closes it as a deactivation (the tray then ignores the dismissing click). */
    asb_rw_flyout_show(f, &anchor);
    pump(20);
    g_closed = 0;
    SendMessageW(info.hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
    CHECK(!asb_rw_flyout_visible(f) && g_closed == 1 && g_closed_deactivated == 1);

    /* Upgrade required: rows shown but disabled; toggles refused. */
    g_model.status = ASB_RW_FLYOUT_UPGRADE;
    g_model.message[0] = 0;
    asb_rw_flyout_update(f, &g_model);
    asb_rw_flyout_show(f, &anchor);
    pump(20);
    CHECK(!IsWindowEnabled(list));
    GetWindowTextW(info.status, text, 400);
    CHECK(wcsstr(text, L"Update") != NULL);
    int before = g_ntoggles;
    PostMessageW(info.hwnd, WM_APP + 1, 0, 0);
    pump(20);
    CHECK_EQ(g_ntoggles, before);
    /* Empty inventory: list hidden, message shown. */
    g_model.status = ASB_RW_FLYOUT_READY;
    g_model.count = 0;
    asb_rw_flyout_update(f, &g_model);
    CHECK(!IsWindowVisible(list));
    GetWindowTextW(info.status, text, 400);
    CHECK(!wcscmp(text, L"No application windows are open."));
    /* No guest / several guests. */
    g_model.status = ASB_RW_FLYOUT_MULTIPLE;
    asb_rw_flyout_update(f, &g_model);
    GetWindowTextW(info.status, text, 400);
    CHECK(wcsstr(text, L"exactly one") != NULL);
    asb_rw_flyout_hide(f);
    asb_rw_flyout_destroy(f);
}

static void test_dpi_scroll_edges(void)
{
    AsbRwFlyoutConfig fc;
    AsbRwFlyoutInfo info;
    RECT wr, work = primary_work();
    memset(&fc, 0, sizeof(fc));
    fc.toggle = on_toggle;
    fc.dpi = 144;
    AsbRwFlyout *f = asb_rw_flyout_create(&fc);
    CHECK(f != NULL);
    if (!f) return;
    memset(&g_model, 0, sizeof(g_model));
    g_model.status = ASB_RW_FLYOUT_READY;
    wcscpy_s(g_model.heading, 160, L"Windows on Main");
    for (uint32_t i = 0; i < 20; ++i) {
        wchar_t label[32];
        swprintf_s(label, 32, L"Window %u", i + 1);
        item(i, 200 + i, label, i == 3 ? ASB_RW_STATE_SHARED : ASB_RW_STATE_NONE, 0);
    }
    asb_rw_flyout_update(f, &g_model);
    RECT top = { work.left + 400, work.top - 40, work.left + 424, work.top - 16 };
    asb_rw_flyout_show(f, &top);
    pump(50);
    asb_rw_flyout_info(f, &info);
    CHECK_EQ(info.dpi, 144);
    CHECK_EQ(info.row_height, 54);                              /* 36 dip at 150 % */
    GetWindowRect(info.hwnd, &wr);
    CHECK_EQ(wr.right - wr.left, 570);                          /* 380 dip */
    CHECK_EQ(info.visible_rows, 8);
    CHECK_EQ(ListView_GetCountPerPage(info.list), 8);           /* the rest scrolls */
    CHECK(GetWindowLongW(info.list, GWL_STYLE) & WS_VSCROLL);
    key(info.list, VK_END);
    CHECK_EQ(ListView_GetTopIndex(info.list), 12);
    int gap = MulDiv(12, 144, 96);
    CHECK_EQ(wr.top, work.top + gap);                           /* top taskbar */
    save_shot(info.hwnd, L"flyout-150-scroll");
    /* Left/right of the primary work area may be another monitor: the flyout must then lie on
       the anchor's monitor, inside its work area (exact edge math: the model test). */
    RECT sides[2] = { { work.left - 40, work.top + 300, work.left - 16, work.top + 324 },
                      { work.right + 16, work.top + 300, work.right + 40, work.top + 324 } };
    for (int k = 0; k < 2; ++k) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromRect(&sides[k], MONITOR_DEFAULTTONEAREST), &mi);
        asb_rw_flyout_show(f, &sides[k]);
        pump(20);
        GetWindowRect(info.hwnd, &wr);
        int g = MulDiv(12, (int)GetDpiForWindow(info.hwnd), 96);
        CHECK(wr.left >= mi.rcWork.left + g - 1 && wr.right <= mi.rcWork.right - g + 1 && wr.top >= mi.rcWork.top &&
              wr.bottom <= mi.rcWork.bottom);
        if (EqualRect(&mi.rcWork, &work)) CHECK_EQ(k ? wr.right : wr.left, k ? work.right - gap : work.left + gap);
    }
    asb_rw_flyout_hide(f);
    asb_rw_flyout_destroy(f);
}

/* ---- tray icon re-creation ---- */

static AsbRwTrayIcon g_tray;
static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (asb_rw_tray_message(&g_tray, msg)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void test_tray(void)
{
    WNDCLASSEXW wc;
    RECT r;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AsbRwSelectorTest.Tray";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    memset(&g_tray, 0, sizeof(g_tray));
    g_tray.hwnd = hwnd;
    g_tray.id = 7;
    g_tray.callback = WM_APP + 9;
    g_tray.icon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(g_tray.tip, 128, L"App Sandbox selector test");
    CHECK(asb_rw_tray_add(&g_tray));
    CHECK_EQ(g_tray.adds, 1);
    CHECK(asb_rw_tray_rect(&g_tray, &r) && r.right > r.left);
    /* Explorer restarted: the broadcast re-adds the icon. */
    SendMessageW(hwnd, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    CHECK_EQ(g_tray.adds, 2);
    CHECK(g_tray.added && asb_rw_tray_rect(&g_tray, &r));
    asb_rw_tray_remove(&g_tray);
    CHECK(!asb_rw_tray_rect(&g_tray, &r));
    DestroyWindow(hwnd);
}

/* ---- guest tray thread ---- */

static volatile LONG g_posts, g_post_action;
static volatile LONG64 g_post_id;
static volatile LONG g_post_select;
static void on_post(void *ctx, uint32_t action, uint64_t id, int select)
{
    (void)ctx;
    InterlockedExchange(&g_post_action, (LONG)action);
    InterlockedExchange64(&g_post_id, (LONG64)id);
    InterlockedExchange(&g_post_select, select);
    InterlockedIncrement(&g_posts);
}

static void test_guest_tray(void)
{
    AsbRwGuestTrayConfig tc;
    AsbRwFlyoutInfo info;
    memset(&tc, 0, sizeof(tc));
    tc.post = on_post;
    tc.theme = ASB_RW_THEME_LIGHT;
    AsbRwGuestTray *t = asb_rw_guest_tray_start(&tc);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(asb_rw_guest_tray_adds(t) >= 1);
    base_model();
    wcscpy_s(g_model.heading, 160, L"Show windows on the host");
    g_model.items[0].icon = NULL;
    asb_rw_guest_tray_publish(t, &g_model, NULL);
    AsbRwFlyout *f = asb_rw_guest_tray_flyout(t);
    for (int i = 0; i < 200; ++i) { asb_rw_flyout_info(f, &info); if (info.rows == 3) break; Sleep(10); }
    CHECK_EQ(info.rows, 3);
    /* A toggle in the guest flyout is handed to the control thread as a selection action. */
    ListView_SetItemState(info.list, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    key(info.list, VK_SPACE);
    for (int i = 0; i < 200 && !g_posts; ++i) Sleep(10);
    CHECK(g_posts == 1 && g_post_action == ASB_RW_ACTION_SELECT && g_post_id == 101 && g_post_select == 1);
    /* The same model drives both sides: an acknowledged state from the control thread. */
    g_model.items[0].state = ASB_RW_STATE_SHARED;
    asb_rw_guest_tray_publish(t, &g_model, NULL);
    for (int i = 0; i < 200; ++i) { if (image_of(info.list, 0) == 2) break; Sleep(10); }
    CHECK_EQ(image_of(info.list, 0), 2);
    /* Explorer restart re-adds the guest icon too. */
    uint32_t adds = asb_rw_guest_tray_adds(t);
    SendMessageW(asb_rw_guest_tray_window(t), RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    CHECK_EQ(asb_rw_guest_tray_adds(t), adds + 1);
    asb_rw_guest_tray_stop(t);
}

int main(int argc, char **argv)
{
    static wchar_t evidence[MAX_PATH];
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--evidence")) {
            MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, evidence, MAX_PATH);
            CreateDirectoryW(evidence, NULL);
            g_evidence = evidence;
        }
    int f = 0;
    test_flyout(ASB_RW_THEME_DARK, L"flyout-dark");
    printf("flyout (dark)          %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_flyout(ASB_RW_THEME_LIGHT, L"flyout-light");
    printf("flyout (light)         %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_dpi_scroll_edges();
    printf("dpi, scrolling, edges  %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_tray();
    printf("tray re-creation       %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_guest_tray();
    printf("guest tray             %s\n", g_failures > f ? "FAIL" : "ok");
    printf("RESULT %s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    CoUninitialize();
    return g_failures ? 1 : 0;
}
