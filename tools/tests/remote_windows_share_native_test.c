/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_share_native_test
 *
 * @file      remote_windows_share_native_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Real windows, real processes, real Win32 ops (tools/agent/remote_windows_share_win32.c).
 *
 *   remote-windows-share-native-test.exe            host mode: the "desktop" and "private display"
 *                                                    are rectangles far off-screen (no VDD needed)
 *   remote-windows-share-native-test.exe --guest    guest mode: the real private display (run in a
 *                                                    Windows 11 guest with this branch's VDD,
 *                                                    unelevated); adds a maximized original
 *
 * A separate "application" process owns the windows: a normal root with WS_EX_APPWINDOW, a
 * minimized root, an owned dialog and an independent root of the same process. This process
 * plays the helper (hidden owner window on its own thread). A second helper process parks a
 * window and is killed to prove Windows unlinks (does not destroy) the window and that recovery
 * restores it from the journal. The application process must survive everything.
 */
#include <windows.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../agent/remote_windows_share_win32.h"
#include "../agent/remote_windows_inventory.h"

#define CHECK(x) do { if (!(x)) { report("FAIL %d: %s", __LINE__, #x); finish(1); } } while (0)

static FILE *g_log;
static HANDLE g_app, g_helper;
static AsbRwShare *g_cleanup;          /* restored (and display removed) on failure */
static WCHAR g_journal[MAX_PATH];

static void report(const char *fmt, ...)
{
    char text[600];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    printf("%s\n", text);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", text); fflush(g_log); }
}

static void quiet_log(const char *message);

/* On failure, leave nothing parked and no private display behind: restore this process's
   windows, then recover whatever a killed second helper journaled. */
static void cleanup_after_failure(void)
{
    if (g_helper) { TerminateProcess(g_helper, 0); WaitForSingleObject(g_helper, 3000); g_helper = NULL; }
    if (g_cleanup) {
        uint32_t result = asb_rw_share_restore_all(g_cleanup);
        report("  cleanup: restore-all -> %s (display %s)", asb_rw_error_name(result),
               g_cleanup->display_active ? "still present" : "removed");
    }
    uint8_t *bytes = NULL;
    size_t len = 0;
    if (g_journal[0] && asb_rw_journal_read(g_journal, &bytes, &len) && len) {
        static AsbRwShare recovery;
        static AsbRwWin32 w;
        AsbRwShareOps ops;
        asb_rw_win32_init(&w, g_journal, quiet_log);
        asb_rw_win32_ops(&w, &ops);
        asb_rw_share_init(&recovery, &ops, NULL, 1, 2560, 1440);
        report("  cleanup: journal recovery -> %s", asb_rw_error_name(asb_rw_share_recover(&recovery, bytes, len)));
        asb_rw_win32_close(&w);
    }
    free(bytes);
}

static void finish(int code)
{
    if (code) cleanup_after_failure();
    if (g_app) { TerminateProcess(g_app, 0); CloseHandle(g_app); }   /* test cleanup only */
    if (g_log) fclose(g_log);
    exit(code);
}

static void quiet_log(const char *message) { if (g_log) { fprintf(g_log, "  %s\n", message); fflush(g_log); } }

/* ---- application role ---------------------------------------------------------------- */
static LRESULT CALLBACK app_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static int app_main(int guest, LONG ox, LONG oy)
{
    /* Host mode: every window keeps an 8-pixel sliver on a real monitor (oy = its bottom - 8),
       because SetWindowPlacement pulls completely off-screen windows back onto a monitor. */
    LONG ya = guest ? oy + 120 : oy, yb = guest ? oy + 200 : oy, yd = guest ? oy + 300 : oy, ye = guest ? oy + 500 : oy;
    WNDCLASSW wc = {0};
    MSG msg;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wc.lpfnWndProc = app_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AsbShareNativeApp";
    RegisterClassW(&wc);
    HWND a = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"eh6.4 normal root", WS_OVERLAPPEDWINDOW,
                             ox + 200, ya, 640, 420, NULL, NULL, wc.hInstance, NULL);
    HWND b = CreateWindowExW(0, wc.lpszClassName, L"eh6.4 minimized root", WS_OVERLAPPEDWINDOW,
                             ox + 300, yb, 500, 320, NULL, NULL, wc.hInstance, NULL);
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"eh6.4 owned dialog",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, ox + 900, yd, 360, 240, a, NULL, wc.hInstance, NULL);
    HWND e = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"eh6.4 independent root", WS_OVERLAPPEDWINDOW,
                             ox + 600, ye, 420, 300, NULL, NULL, wc.hInstance, NULL);
    HWND c = guest ? CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"eh6.4 maximized root", WS_OVERLAPPEDWINDOW,
                                     ox + 100, oy + 100, 700, 500, NULL, NULL, wc.hInstance, NULL) : NULL;
    ShowWindow(a, SW_SHOWNOACTIVATE);
    ShowWindow(b, SW_SHOWMINNOACTIVE);
    ShowWindow(d, SW_SHOWNOACTIVATE);
    ShowWindow(e, SW_SHOWNOACTIVATE);
    if (c) ShowWindow(c, SW_SHOWMAXIMIZED);
    printf("%llx %llx %llx %llx %llx\n", (unsigned long long)(ULONG_PTR)a, (unsigned long long)(ULONG_PTR)b,
           (unsigned long long)(ULONG_PTR)d, (unsigned long long)(ULONG_PTR)e, (unsigned long long)(ULONG_PTR)c);
    fflush(stdout);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

/* ---- helper role (owner thread + share) ---------------------------------------------- */
static HWND g_owner;
static LRESULT CALLBACK owner_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return m == WM_CLOSE ? 0 : DefWindowProcW(h, m, w, l); }
static DWORD WINAPI owner_thread(LPVOID ready)
{
    WNDCLASSW wc = {0};
    MSG msg;
    wc.lpfnWndProc = owner_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = ASB_RW_OWNER_CLASS;
    RegisterClassW(&wc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"owner", WS_POPUP, 0, 0, 0, 0,
                              NULL, NULL, wc.hInstance, NULL);
    SetEvent((HANDLE)ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
static void start_owner(void)
{
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, owner_thread, ready, 0, NULL));
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
}

static RECT g_desk, g_priv;
static int g_guest;

/* Host mode: "desktop" = the real monitor with the lowest bottom edge (windows hang below it);
   "private display" = a region far right of every monitor. */
static BOOL CALLBACK lowest_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    RECT *best = (RECT *)lp;
    (void)m; (void)dc;
    if (IsRectEmpty(best) || r->bottom > best->bottom || (r->bottom == best->bottom && r->left < best->left)) *best = *r;
    return TRUE;
}
static void host_regions(void)
{
    SetRectEmpty(&g_desk);
    EnumDisplayMonitors(NULL, NULL, lowest_monitor, (LPARAM)&g_desk);
    LONG right = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    SetRect(&g_priv, right + 20000, 0, right + 22560, 1440);
}

static void setup_share(AsbRwWin32 *w, AsbRwShareOps *ops, AsbRwShare *share, const WCHAR *journal, uint64_t lease)
{
    asb_rw_win32_init(w, journal, quiet_log);
    if (!g_guest) {
        w->fake_display = TRUE;
        w->fake_desktop = g_desk;
        w->fake_private = g_priv;
    }
    asb_rw_win32_ops(w, ops);
    asb_rw_share_init(share, ops, g_owner, lease, 2560, 1440);
}

static int helper_main(const WCHAR *journal, HWND target)
{
    AsbRwWin32 w;
    AsbRwShareOps ops;
    static AsbRwShare share;
    AsbRwIdentity id;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    start_owner();
    setup_share(&w, &ops, &share, journal, 0x4E4154495645ull);
    if (!asb_rw_probe_identity(NULL, (uint64_t)(ULONG_PTR)target, &id)) return 2;
    uint32_t result = asb_rw_share_select(&share, &id, 1);
    printf("%s\n", result ? asb_rw_error_name(result) : "PARKED");
    fflush(stdout);
    Sleep(INFINITE);   /* killed by the test */
    return 0;
}

/* ---- test ----------------------------------------------------------------------------- */
typedef struct Orig { WINDOWPLACEMENT wp; RECT rect; LONG_PTR ex; HWND owner; } Orig;

static Orig orig(HWND h)
{
    Orig o;
    o.wp.length = sizeof(o.wp);
    GetWindowPlacement(h, &o.wp);
    GetWindowRect(h, &o.rect);
    o.ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    o.owner = GetWindow(h, GW_OWNER);
    return o;
}

static BOOL center_in(HWND h, const RECT *area)
{
    RECT r;
    GetWindowRect(h, &r);
    POINT p = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    return PtInRect(area, p);
}

static void settle(void) { Sleep(150); }

static const char *where(HWND h)
{
    return center_in(h, &g_priv) ? "private" : center_in(h, &g_desk) ? "desktop" : "elsewhere";
}

static void describe(const char *label, HWND h)
{
    Orig o = orig(h);
    report("  %-12s hwnd=%p show=%u rect=(%ld,%ld,%ld,%ld) on %s owner=%p parked=%d ex=%llx dpi=%u", label, (void *)h,
           o.wp.showCmd, o.rect.left, o.rect.top, o.rect.right, o.rect.bottom, where(h), (void *)o.owner,
           GetPropW(h, ASB_PRIVATE_PARKED_PROPERTY) != NULL, (unsigned long long)o.ex, GetDpiForWindow(h));
}

static void expect_parked(HWND h)
{
    CHECK(GetPropW(h, ASB_PRIVATE_PARKED_PROPERTY) != NULL);
    CHECK(GetWindow(h, GW_OWNER) == g_owner);
    CHECK(!(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_APPWINDOW));
    CHECK(!IsIconic(h) && !IsZoomed(h) && IsWindowVisible(h));
    CHECK(center_in(h, &g_priv));
}

static void expect_restored(HWND h, const Orig *o, LONG_PTR extra_ex)
{
    Orig now = orig(h);
    report("  hwnd=%p show %u/%u normal (%ld,%ld,%ld,%ld)/(%ld,%ld,%ld,%ld) rect (%ld,%ld,%ld,%ld)/(%ld,%ld,%ld,%ld) ex %llx/%llx dpi %u",
           (void *)h, now.wp.showCmd, o->wp.showCmd,
           now.wp.rcNormalPosition.left, now.wp.rcNormalPosition.top, now.wp.rcNormalPosition.right, now.wp.rcNormalPosition.bottom,
           o->wp.rcNormalPosition.left, o->wp.rcNormalPosition.top, o->wp.rcNormalPosition.right, o->wp.rcNormalPosition.bottom,
           now.rect.left, now.rect.top, now.rect.right, now.rect.bottom, o->rect.left, o->rect.top, o->rect.right, o->rect.bottom,
           (unsigned long long)now.ex, (unsigned long long)(o->ex | extra_ex), GetDpiForWindow(h));
    CHECK(IsWindow(h));
    CHECK(now.wp.showCmd == o->wp.showCmd);
    CHECK(EqualRect(&now.wp.rcNormalPosition, &o->wp.rcNormalPosition));
    if (o->wp.showCmd == SW_SHOWNORMAL) CHECK(EqualRect(&now.rect, &o->rect));
    CHECK(now.ex == (o->ex | extra_ex));
    CHECK(now.owner == o->owner);
    CHECK(GetPropW(h, ASB_PRIVATE_PARKED_PROPERTY) == NULL);
}

/* A window the share is NOT holding at this point. Turning the private display on or off
   re-commits the display layout; Windows may migrate ordinary windows onto the private output
   and the display watcher returns them about once a second (eh6.3), possibly elsewhere or
   DPI-scaled. Wait for that, then fail only on changes of ours (owner, mark, styles, left on
   the private display). A move by Windows/the watcher is reported as INFO. */
static void expect_unshared(const char *label, HWND h, const Orig *o, LONG_PTR extra_ex)
{
    for (int i = 0; i < 40 && center_in(h, &g_priv); ++i) Sleep(100);
    Orig now = orig(h);
    describe(label, h);
    CHECK(IsWindow(h) && IsWindowVisible(h) && !center_in(h, &g_priv));
    CHECK(now.owner == o->owner && !GetPropW(h, ASB_PRIVATE_PARKED_PROPERTY) && now.ex == (o->ex | extra_ex));
    CHECK(now.wp.showCmd == o->wp.showCmd);
    if (!EqualRect(&now.rect, &o->rect))
        report("  INFO: %s moved from (%ld,%ld,%ld,%ld) during a display change (Windows/display watcher, not the share)",
               label, o->rect.left, o->rect.top, o->rect.right, o->rect.bottom);
}

static HANDLE spawn_self(const WCHAR *args, HANDLE *out_read)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH * 2];
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE rd, wr;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf_s(cmd, MAX_PATH * 2, L"\"%s\" %s", exe, args);
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    *out_read = rd;
    return pi.hProcess;
}

static BOOL read_line(HANDLE rd, char *buf, DWORD cap)
{
    DWORD n = 0, got;
    while (n + 1 < cap && ReadFile(rd, buf + n, 1, &got, NULL) && got) {
        if (buf[n] == '\n') break;
        n++;
    }
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) --n;
    buf[n] = 0;
    return n > 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2 && !wcscmp(argv[1], L"app")) return app_main(argc > 2 && !wcscmp(argv[2], L"guest"),
                                                              argc > 4 ? wcstol(argv[3], NULL, 10) : 0,
                                                              argc > 4 ? wcstol(argv[4], NULL, 10) : 0);
    if (argc >= 4 && !wcscmp(argv[1], L"helper")) {
        g_guest = argc > 4 && !wcscmp(argv[4], L"guest");
        if (!g_guest) host_regions();
        return helper_main(argv[2], (HWND)(ULONG_PTR)_wcstoui64(argv[3], NULL, 16));
    }
    g_guest = argc >= 2 && !wcscmp(argv[1], L"--guest");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WCHAR dir[MAX_PATH], journal[MAX_PATH], args[MAX_PATH * 2];
    if (g_guest) {
        CHECK(asb_rw_state_dir(dir, MAX_PATH));
        WCHAR logpath[MAX_PATH];
        swprintf_s(logpath, MAX_PATH, L"%s\\..\\eh6.4-native-check.log", dir);
        _wfopen_s(&g_log, logpath, L"w");
        swprintf_s(journal, MAX_PATH, L"%s\\native-check-journal.bin", dir);
    } else {
        GetTempPathW(MAX_PATH, dir);
        swprintf_s(journal, MAX_PATH, L"%sasb-share-native-journal.bin", dir);
        host_regions();
    }
    DeleteFileW(journal);
    wcscpy_s(g_journal, MAX_PATH, journal);
    report("remote windows share native test (%s mode)", g_guest ? "guest" : "host");
    start_owner();
    CHECK(g_owner != NULL);

    AsbRwWin32 w;
    AsbRwShareOps ops;
    static AsbRwShare share;
    setup_share(&w, &ops, &share, journal, 0xA5B0CAFE0064ull);
    g_cleanup = &share;
    if (g_guest) {
        /* Learn the real desktop; the private rectangle comes from activation. */
        SetRect(&g_desk, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }

    HANDLE app_out = NULL;
    swprintf_s(args, MAX_PATH * 2, L"app %s %ld %ld", g_guest ? L"guest" : L"host", g_desk.left + (g_guest ? 0 : 40),
               g_guest ? g_desk.top : g_desk.bottom - 8);
    g_app = spawn_self(args, &app_out);
    CHECK(g_app != NULL);
    char line[256];
    CHECK(read_line(app_out, line, sizeof(line)));
    unsigned long long va, vb, vd, ve, vc;
    CHECK(sscanf_s(line, "%llx %llx %llx %llx %llx", &va, &vb, &vd, &ve, &vc) == 5);
    HWND a = (HWND)(ULONG_PTR)va, b = (HWND)(ULONG_PTR)vb, d = (HWND)(ULONG_PTR)vd, e = (HWND)(ULONG_PTR)ve,
         c = (HWND)(ULONG_PTR)vc;
    settle();
    Orig oa = orig(a), ob = orig(b), od = orig(d), oe = orig(e), oc = {0};
    if (c) oc = orig(c);
    CHECK(oa.wp.showCmd == SW_SHOWNORMAL && ob.wp.showCmd == SW_SHOWMINIMIZED && (oa.ex & WS_EX_APPWINDOW));
    if (c) CHECK(oc.wp.showCmd == SW_SHOWMAXIMIZED);

    /* Select: normal, minimized (and maximized in the guest). */
    AsbRwIdentity ia, ib, ic, ie;
    CHECK(asb_rw_probe_identity(NULL, va, &ia) && asb_rw_probe_identity(NULL, vb, &ib) && asb_rw_probe_identity(NULL, ve, &ie));
    CHECK(asb_rw_share_select(&share, &ia, 1) == ASB_RW_OK);
    if (g_guest) g_priv = share.private_rect;
    CHECK(asb_rw_share_select(&share, &ib, 2) == ASB_RW_OK);
    if (c) { CHECK(asb_rw_probe_identity(NULL, vc, &ic)); CHECK(asb_rw_share_select(&share, &ic, 3) == ASB_RW_OK); }
    settle();
    expect_parked(a);
    expect_parked(b);
    if (c) expect_parked(c);
    CHECK(center_in(d, &g_priv));                                  /* owned dialog followed */
    report("  desktop=(%ld,%ld,%ld,%ld) private=(%ld,%ld,%ld,%ld)", g_desk.left, g_desk.top, g_desk.right, g_desk.bottom,
           g_priv.left, g_priv.top, g_priv.right, g_priv.bottom);
    describe("normal", a); describe("minimized", b); if (c) describe("maximized", c);
    describe("dialog", d); describe("independent", e);
    /* Independent root: never parked, owned, marked or suppressed. Turning the private display on
       re-commits the display layout, and Windows may migrate ordinary windows onto the new output;
       the display watcher returns them about once a second (eh6.3). Wait for that, then require
       that nothing of ours touched it. A position change made by Windows/the watcher is reported. */
    expect_unshared("independent", e, &oe, 0);

    report("PASS select: real cross-process windows parked renderable (normal, minimized%s), owned dialog followed, independent root untouched",
           c ? ", maximized" : "");

    /* The application changes its own styles while shared. */
    SetWindowLongPtrW(a, GWL_EXSTYLE, GetWindowLongPtrW(a, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    SetWindowLongPtrW(b, GWL_EXSTYLE, GetWindowLongPtrW(b, GWL_EXSTYLE) | WS_EX_APPWINDOW);
    asb_rw_share_maintain(&share, NULL, 0, NULL, NULL, 0, NULL);
    CHECK(!(GetWindowLongPtrW(b, GWL_EXSTYLE) & WS_EX_APPWINDOW));
    /* Windows migrates a parked window to the desktop (e.g. during a desktop re-creation). */
    SetWindowPos(a, NULL, g_desk.left + 50, g_desk.top + 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    asb_rw_share_maintain(&share, NULL, 0, NULL, NULL, 0, NULL);
    CHECK(center_in(a, &g_priv));

    CHECK(asb_rw_share_unselect(&share, a) == ASB_RW_OK);
    settle();
    expect_restored(a, &oa, WS_EX_CONTROLPARENT);
    CHECK(center_in(d, &g_desk) || !center_in(d, &g_priv));
    CHECK(asb_rw_share_unselect(&share, b) == ASB_RW_OK);
    settle();
    expect_restored(b, &ob, WS_EX_APPWINDOW);   /* the application's own wish survives */
    CHECK(IsIconic(b));
    if (c) { CHECK(asb_rw_share_unselect(&share, c) == ASB_RW_OK); settle(); expect_restored(c, &oc, 0); CHECK(IsZoomed(c)); }
    report("PASS unselect: exact placement/show state restored, application style changes kept, owned dialog returned");

    /* Helper death: a second helper parks a, then is killed. */
    CHECK(asb_rw_share_release_display(&share) == ASB_RW_OK);
    HANDLE helper_out = NULL;
    swprintf_s(args, MAX_PATH * 2, L"helper \"%s\" %llx%s", journal, va, g_guest ? L" guest" : L"");
    HANDLE helper = g_helper = spawn_self(args, &helper_out);
    CHECK(helper != NULL);
    CHECK(read_line(helper_out, line, sizeof(line)));
    report("  second helper: %s", line);
    CHECK(!strcmp(line, "PARKED"));
    settle();
    HWND foreign_owner = GetWindow(a, GW_OWNER);
    CHECK(foreign_owner && foreign_owner != g_owner && GetPropW(a, ASB_PRIVATE_PARKED_PROPERTY));
    TerminateProcess(helper, 9);
    WaitForSingleObject(helper, 5000);
    CloseHandle(helper);
    g_helper = NULL;
    CloseHandle(helper_out);
    settle();
    CHECK(IsWindow(a) && GetWindow(a, GW_OWNER) == NULL);          /* unlinked, not destroyed */
    CHECK(WaitForSingleObject(g_app, 0) == WAIT_TIMEOUT);
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(asb_rw_journal_read(journal, &bytes, &len) && len);
    static AsbRwShare recovery;
    AsbRwWin32 w2;
    AsbRwShareOps ops2;
    setup_share(&w2, &ops2, &recovery, journal, 0x1234);
    CHECK(asb_rw_share_recover(&recovery, bytes, len) == ASB_RW_OK);
    free(bytes);
    settle();
    expect_restored(a, &oa, WS_EX_CONTROLPARENT);
    CHECK(GetFileAttributesW(journal) == INVALID_FILE_ATTRIBUTES);
    asb_rw_win32_close(&w2);
    report("PASS helper killed: window unlinked (not destroyed), recovery restored it from the journal and removed it");

    /* A shared window the application closes is dropped. */
    uint64_t gone[4];
    uint32_t n_gone = 0;
    CHECK(asb_rw_share_select(&share, &ie, 9) == ASB_RW_OK);
    PostMessageW(e, WM_CLOSE, 0, 0);
    for (int i = 0; i < 40 && IsWindow(e); ++i) Sleep(50);
    asb_rw_share_maintain(&share, gone, 4, &n_gone, NULL, 0, NULL);
    CHECK(n_gone == 1 && gone[0] == 9 && share.count == 0);
    CHECK(asb_rw_share_restore_all(&share) == ASB_RW_OK);
    CHECK(WaitForSingleObject(g_app, 0) == WAIT_TIMEOUT && IsWindow(a) && IsWindow(b) && IsWindow(d));
    /* a was restored exactly above; the display was turned on and off since, without sharing a. */
    expect_unshared("normal", a, &oa, WS_EX_CONTROLPARENT);
    (void)od;
    report("PASS closed shared window dropped; application process and its other windows survived every step");
    asb_rw_win32_close(&w);
    report("RESULT PASS");
    finish(0);
    return 0;
}
