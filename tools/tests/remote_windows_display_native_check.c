/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_display_native_check
 *
 * @file      remote_windows_display_native_check.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Native guest check for the private sharing display (eh6.3). Run in the signed-in
   guest session as a STANDARD user after installing this branch's AppSandboxVDD:

       remote-windows-display-native-check.exe [watch-seconds]

   It proves, against the real driver and Windows display stack, that the private
   output is driven through the standard-user control path; that the original desktop
   output stays primary at (0,0) with the same size, the absolute-input/screenshot
   target, while the private output sits beyond its right edge; that unsupported
   requests are refused; and that removal only happens after source restoration.
   During the optional watch period, resize the viewer from the host and take API
   screenshots: every sample must keep the desktop primary.
   Results: console and %LOCALAPPDATA%\AppSandbox\eh6.3-native-check.log */
#include "../agent/remote_windows_display.c"
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

static FILE *g_log;
static int g_failures;

static void say(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    SYSTEMTIME now;
    GetLocalTime(&now);
    printf("%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line);
    if (g_log) {
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line);
        fflush(g_log);
    }
}
#define EXPECT(cond, ...) do { if (cond) say("PASS " __VA_ARGS__); \
    else { ++g_failures; say("FAIL " __VA_ARGS__); } } while (0)

typedef struct Layout {
    int monitors;
    BOOL desktop_found, private_found, desktop_primary;
    RECT desktop, private_rect;
    HMONITOR desktop_monitor, private_monitor;
    int screen_w, screen_h;
} Layout;

/* GDI device name (\\.\DISPLAYn) -> role, via CCD source/target names. */
static UINT32 gdi_role(const WCHAR *gdi)
{
    UINT32 paths_count = 0, modes_count = 0;
    UINT32 role = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_count, &modes_count)) return 0;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(paths_count + 1, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(modes_count + 1, sizeof(*modes));
    if (paths && modes && !QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_count, paths, &modes_count, modes, NULL)) {
        for (UINT32 i = 0; i < paths_count && !role; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {0};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) || _wcsicmp(source.viewGdiDeviceName, gdi)) continue;
            role = path_role(&paths[i]);
        }
    }
    free(paths);
    free(modes);
    return role;
}

static BOOL CALLBACK monitor_proc(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data)
{
    (void)dc; (void)rect;
    Layout *layout = (Layout *)data;
    MONITORINFOEXW info = {0};
    info.cbSize = sizeof(info);
    ++layout->monitors;
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info)) return TRUE;
    UINT32 role = gdi_role(info.szDevice);
    if (role == ASB_DISPLAY_DESKTOP) {
        layout->desktop_found = TRUE;
        layout->desktop = info.rcMonitor;
        layout->desktop_monitor = monitor;
        layout->desktop_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    } else if (role == ASB_DISPLAY_PRIVATE) {
        layout->private_found = TRUE;
        layout->private_rect = info.rcMonitor;
        layout->private_monitor = monitor;
    }
    return TRUE;
}

static Layout read_layout(void)
{
    Layout layout = {0};
    EnumDisplayMonitors(NULL, NULL, monitor_proc, (LPARAM)&layout);
    layout.screen_w = GetSystemMetrics(SM_CXSCREEN);
    layout.screen_h = GetSystemMetrics(SM_CYSCREEN);
    return layout;
}

static void print_layout(const char *label, const Layout *l)
{
    say("%s: monitors=%d desktop=%d primary=%d (%ld,%ld)-(%ld,%ld) private=%d (%ld,%ld)-(%ld,%ld) SM_CXSCREEN=%dx%d",
        label, l->monitors, l->desktop_found, l->desktop_primary, l->desktop.left, l->desktop.top,
        l->desktop.right, l->desktop.bottom, l->private_found, l->private_rect.left, l->private_rect.top,
        l->private_rect.right, l->private_rect.bottom, l->screen_w, l->screen_h);
}

static BOOL desktop_is_target(const Layout *l)
{
    /* Desktop output is primary at (0,0) and is what SM_CXSCREEN (absolute SendInput
       without VIRTUALDESK, the guest input path) maps to. */
    return l->desktop_found && l->desktop_primary && l->desktop.left == 0 && l->desktop.top == 0 &&
        l->screen_w == l->desktop.right && l->screen_h == l->desktop.bottom;
}

static BOOL private_beyond_desktop(const Layout *l)
{
    RECT overlap;
    return l->private_found && l->private_rect.left >= l->desktop.right && l->private_rect.top == 0 &&
        !IntersectRect(&overlap, &l->private_rect, &l->desktop);
}

static DWORD restore_nothing(void *data) { (void)data; return ERROR_SUCCESS; }

typedef struct RestoreContext {
    HWND window;
    RECT original;
    BOOL called, private_present, restored_to_desktop;
} RestoreContext;

static DWORD restore_parked(void *data)
{
    RestoreContext *context = data;
    Layout before = read_layout();
    context->called = TRUE;
    context->private_present = before.private_found; /* output must still exist here */
    RemovePropW(context->window, ASB_PRIVATE_PARKED_PROPERTY);
    SetWindowPos(context->window, NULL, context->original.left, context->original.top,
                 context->original.right - context->original.left,
                 context->original.bottom - context->original.top, SWP_NOZORDER | SWP_NOACTIVATE);
    context->restored_to_desktop = MonitorFromWindow(context->window, MONITOR_DEFAULTTONULL) == before.desktop_monitor;
    return context->restored_to_desktop ? ERROR_SUCCESS : ERROR_INVALID_WINDOW_HANDLE;
}

static void pump(DWORD milliseconds)
{
    ULONGLONG end = GetTickCount64() + milliseconds;
    MSG message;
    do {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    } while (GetTickCount64() < end);
}

static void cycle(AsbRwDisplay *display, HWND window, UINT32 width, UINT32 height, DWORD watch_ms)
{
    say("---- cycle %ux%u ----", width, height);
    Layout baseline = read_layout();
    print_layout("baseline", &baseline);
    DWORD error = asb_rw_display_activate(display, width, height);
    EXPECT(error == ERROR_SUCCESS, "activate %ux%u as standard user (error %lu)", width, height, error);
    if (error) {
        Layout failed = read_layout();
        print_layout("after failure", &failed);
        error = asb_rw_display_restore(display, restore_nothing, NULL);
        EXPECT(error == ERROR_SUCCESS, "failed activation released (error %lu)", error);
        return;
    }
    pump(1500); /* let the display watcher settle (e.g. disabling the Hyper-V output) */
    error = asb_rw_display_refresh(display, width, height);
    EXPECT(error == ERROR_SUCCESS, "topology verified after settling (error %lu)", error);
    Layout active = read_layout();
    print_layout("active", &active);
    EXPECT(desktop_is_target(&active), "desktop stays primary at (0,0) and is the input/screenshot target");
    EXPECT(active.desktop.right == baseline.desktop.right && active.desktop.bottom == baseline.desktop.bottom,
           "desktop mode unchanged by activation");
    EXPECT(private_beyond_desktop(&active), "private output beyond the desktop's right edge, no overlap");
    EXPECT(active.private_rect.right - active.private_rect.left == (LONG)width &&
           active.private_rect.bottom - active.private_rect.top == (LONG)height, "private mode is %ux%u", width, height);
    EXPECT(EqualRect(&display->private_rect, &active.private_rect), "client rectangle matches Windows");

    UINT32 state = 0, w = 0, h = 0;
    error = asb_rw_display_query(display, &state, &w, &h);
    EXPECT(!error && state == ASB_PRIVATE_READY && w == width && h == height, "driver reports READY %ux%u", w, h);
    error = display_command(display, ASB_PRIVATE_REMOVE, 0, 0);
    EXPECT(error == ERROR_BUSY, "REMOVE before RESTORE refused (error %lu)", error);
    AsbRwDisplay other = *display;
    other.token = display->token ^ 0x5a5a5a5a5a5a5a5aull;
    error = display_command(&other, ASB_PRIVATE_ACTIVATE, width, height);
    EXPECT(error == ERROR_ACCESS_DENIED, "foreign lease refused (error %lu)", error);
    error = display_command(display, ASB_PRIVATE_ACTIVATE, width == 1920 ? 2560 : 1920, 1080);
    EXPECT(error == ERROR_BUSY, "mode change without restoration refused (error %lu)", error);

    RestoreContext context = {window};
    GetWindowRect(window, &context.original);
    /* Mark before parking (the display watcher returns unmarked windows to the desktop). */
    SetPropW(window, ASB_PRIVATE_PARKED_PROPERTY, (HANDLE)1);
    SetWindowPos(window, NULL, active.private_rect.left + 100, active.private_rect.top + 100, 640, 400,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    pump(300);
    EXPECT(MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == active.private_monitor,
           "test window parked on the private output (normal show state)");
    EXPECT(!IsIconic(window) && IsWindowVisible(window), "parked window is neither minimized nor hidden");
    {
        /* An unmarked window placed on the private output (as Windows does when it migrates
           desktop windows during a viewer resize) is returned to the desktop by the
           display watcher within a few seconds; the marked one stays parked. */
        HWND stray = CreateWindowExW(0, L"AsbEh63ParkedWindow", L"eh6.3 stray window", WS_OVERLAPPEDWINDOW,
            active.private_rect.left + 300, active.private_rect.top + 300, 500, 300, NULL, NULL,
            GetModuleHandleW(NULL), NULL);
        ShowWindow(stray, SW_SHOWNOACTIVATE);
        BOOL back = FALSE;
        for (int i = 0; i < 40 && !back; ++i) {
            pump(100);
            back = MonitorFromWindow(stray, MONITOR_DEFAULTTONULL) == active.desktop_monitor;
        }
        EXPECT(back, "unmarked window on the private output returned to the desktop (display watcher)");
        EXPECT(MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == active.private_monitor,
               "marked parked window left on the private output");
        DestroyWindow(stray);

        /* A window Windows re-maximized on the private output while its restore rectangle
           stayed on the desktop (native: VS Code after a viewer resize). SetWindowPlacement
           alone leaves an already-maximized window in place. */
        HWND maxed = CreateWindowExW(WS_EX_NOACTIVATE, L"AsbEh63ParkedWindow", L"eh6.3 maximized stray",
            WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
        WINDOWPLACEMENT placement = {sizeof(placement)};
        GetWindowPlacement(maxed, &placement);
        RECT work = active.desktop;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        LONG wx = work.left - active.desktop.left, wy = work.top - active.desktop.top;
        SetRect(&placement.rcNormalPosition, active.private_rect.left + 200 - wx, active.private_rect.top + 200 - wy,
                active.private_rect.left + 900 - wx, active.private_rect.top + 700 - wy);
        placement.showCmd = SW_SHOWMAXIMIZED;
        SetWindowPlacement(maxed, &placement);
        SetRect(&placement.rcNormalPosition, 200, 200, 900, 700);
        SetWindowPlacement(maxed, &placement);
        BOOL maxed_back = FALSE;
        for (int i = 0; i < 40 && !maxed_back; ++i) {
            pump(100);
            maxed_back = MonitorFromWindow(maxed, MONITOR_DEFAULTTONULL) == active.desktop_monitor;
        }
        EXPECT(maxed_back && IsZoomed(maxed), "maximized window on the private output returned to the desktop, still maximized");
        DestroyWindow(maxed);
    }

    if (watch_ms) {
        say("watching %lu s: resize the viewer and take API screenshots now", watch_ms / 1000);
        ULONGLONG end = GetTickCount64() + watch_ms;
        int samples = 0, bad = 0;
        Layout last = active;
        while (GetTickCount64() < end) {
            pump(250);
            Layout now = read_layout();
            ++samples;
            if (now.desktop.right != last.desktop.right || now.desktop.bottom != last.desktop.bottom ||
                now.private_rect.left != last.private_rect.left || now.monitors != last.monitors)
                print_layout("change", &now);
            /* Transient states while Windows re-enumerates are logged; settled ones count. */
            if (!desktop_is_target(&now) && now.desktop_found) { ++bad; print_layout("not-primary", &now); }
            last = now;
        }
        pump(3000);
        Layout settled = read_layout();
        print_layout("settled", &settled);
        say("watch: %d samples, %d with the desktop not primary/targeted", samples, bad);
        EXPECT(desktop_is_target(&settled), "desktop primary and targeted after watch");
        EXPECT(private_beyond_desktop(&settled), "private output beyond the desktop after watch");
        EXPECT(MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == settled.private_monitor,
               "parked window remained on the private output");
        error = asb_rw_display_refresh(display, width, height);
        EXPECT(!error && EqualRect(&display->private_rect, &settled.private_rect),
               "client rectangle matches Windows after watch (error %lu)", error);
    }

    error = asb_rw_display_restore(display, restore_parked, &context);
    EXPECT(error == ERROR_SUCCESS, "restore then remove (error %lu)", error);
    EXPECT(context.called && context.private_present, "sources restored while the private output still existed");
    EXPECT(context.restored_to_desktop, "test window restored to the desktop output");
    pump(1000);
    Layout after = read_layout();
    print_layout("removed", &after);
    EXPECT(!after.private_found, "private output departed after restoration");
    EXPECT(desktop_is_target(&after), "desktop primary and targeted after removal");
    EXPECT(IsWindow(window) && MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == after.desktop_monitor,
           "restored window survives on the desktop");
    error = asb_rw_display_query(display, &state, NULL, NULL);
    EXPECT(!error && state == ASB_PRIVATE_ABSENT, "driver reports ABSENT");
}

int wmain(int argc, wchar_t **argv)
{
    UINT64 recover = 0;
    if (argc > 2 && !wcscmp(argv[1], L"--recover")) recover = _wcstoui64(argv[2], NULL, 16);
    DWORD watch_ms = argc > 1 && !recover ? (DWORD)_wtoi(argv[1]) * 1000u : 0;
    WCHAR path[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (length && length < MAX_PATH - 64) {
        wcscat_s(path, MAX_PATH, L"\\AppSandbox");
        CreateDirectoryW(path, NULL);
        wcscat_s(path, MAX_PATH, L"\\eh6.3-native-check.log");
        _wfopen_s(&g_log, path, L"w");
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation = {0};
    DWORD bytes = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
        CloseHandle(token);
    }
    say("eh6.3 native check: elevated=%lu", elevation.TokenIsElevated);
    EXPECT(!elevation.TokenIsElevated, "running as a standard (unelevated) user");

    WNDCLASSW cls = {0};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"AsbEh63ParkedWindow";
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"eh6.3 parked test window", WS_OVERLAPPEDWINDOW,
                                  80, 80, 640, 400, NULL, NULL, cls.hInstance, NULL);
    ShowWindow(window, SW_SHOWNOACTIVATE);

    if (recover) {
        AsbRwDisplay held;
        DWORD status = asb_rw_display_open(&held, recover);
        if (!status) status = asb_rw_display_restore(&held, restore_nothing, NULL);
        say("recover lease %016llx: error %lu", recover, status);
        asb_rw_display_close(&held);
        DestroyWindow(window);
        if (g_log) fclose(g_log);
        return status ? 1 : 0;
    }
    UINT64 lease = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&lease, sizeof(lease), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !lease)
        lease = GetTickCount64() | 1; /* random per-run lease token */
    AsbRwDisplay display;
    DWORD error = asb_rw_display_open(&display, lease);
    EXPECT(error == ERROR_SUCCESS, "opened private-display interface as standard user (error %lu)", error);
    say("lease token %016llx (recover with: --recover %016llx)", lease, lease);
    if (!error) {
        UINT32 state = 0;
        error = asb_rw_display_query(&display, &state, NULL, NULL);
        EXPECT(!error && state == ASB_PRIVATE_ABSENT, "initially ABSENT (state %u)", state);
        EXPECT(asb_rw_display_activate(&display, 7681, 1440) == ERROR_NOT_SUPPORTED, "7681x1440 unsupported");
        error = display_command(&display, ASB_PRIVATE_ACTIVATE, 320, 200);
        EXPECT(error == ERROR_NOT_SUPPORTED, "driver refuses 320x200 (error %lu)", error);
        error = display_command(&display, ASB_PRIVATE_RESTORE, 0, 0);
        EXPECT(!error, "RESTORE while ABSENT is a no-op");
        error = display_command(&display, ASB_PRIVATE_REMOVE, 0, 0);
        EXPECT(!error, "REMOVE while ABSENT is a no-op");
        cycle(&display, window, 2560, 1440, watch_ms);
        cycle(&display, window, 1920, 1080, 0);
        cycle(&display, window, 2560, 1440, 0);
        asb_rw_display_close(&display);
    }
    DestroyWindow(window);
    say("RESULT %s (%d failures)", g_failures ? "FAIL" : "PASS", g_failures);
    if (g_log) fclose(g_log);
    return g_failures ? 1 : 0;
}
