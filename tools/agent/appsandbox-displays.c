#include <windows.h>
#include <dbt.h>
#include <wtsapi32.h>
#include <stdlib.h>
#include <errno.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include "../transport/asb_private_display.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")

static HANDLE g_display_changed;
static HANDLE g_display_stop;
/* Desktop and private output rectangles (physical, desktop primary at 0,0) from the last
   successful pass; g_private_rect is empty unless a placed private output exists. */
static RECT g_desktop_rect, g_private_rect;

static UINT32 appsandbox_display_role(const DISPLAYCONFIG_PATH_INFO *path)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME target = {0};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path->targetInfo.adapterId;
    target.header.id = path->targetInfo.id;
    return DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS ?
        asb_display_role(target.monitorDevicePath) : 0;
}

static int disable_extra_displays(void)
{
    HDESK original_desktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK input_desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 path_count = 0, mode_count = 0;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    SetRectEmpty(&g_private_rect);

    if (!input_desktop || !SetThreadDesktop(input_desktop)) {
        result = (LONG)GetLastError();
        goto done;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        free(paths);
        free(modes);
        paths = NULL;
        modes = NULL;
        result = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS || !path_count) break;
        paths = calloc(path_count, sizeof(*paths));
        modes = calloc(mode_count ? mode_count : 1, sizeof(*modes));
        if (!paths || !modes) {
            result = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        result = QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths,
                                    &mode_count, modes, NULL);
        if (result != ERROR_INSUFFICIENT_BUFFER) break;
    }

    if (result == ERROR_SUCCESS && paths && modes) {
        UINT32 kept = 0, active = 0;
        BOOL activate = FALSE;
        for (UINT32 i = 0; i < path_count; i++) {
            UINT32 role = appsandbox_display_role(&paths[i]);
            if (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) active++;
            if (!paths[i].targetInfo.targetAvailable || !role) continue;
            /* Only the sharing helper activates/places the private output. Keep
               active private paths, but never resurrect a removed/disabled one. */
            if (role == ASB_DISPLAY_PRIVATE && !(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
            UINT32 j;
            for (j = 0; j < kept; j++) {
                if (paths[j].targetInfo.id == paths[i].targetInfo.id &&
                    paths[j].targetInfo.adapterId.LowPart == paths[i].targetInfo.adapterId.LowPart &&
                    paths[j].targetInfo.adapterId.HighPart == paths[i].targetInfo.adapterId.HighPart) break;
            }
            if (j < kept) continue;
            if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) {
                activate = TRUE;
                paths[i].flags |= DISPLAYCONFIG_PATH_ACTIVE;
            }
            paths[kept++] = paths[i];
        }
        if (kept) {
            POINTL origin = {0};
            BOOL have_origin = FALSE;
            BOOL reposition = FALSE, have_private = FALSE;
            UINT32 desktop_mode = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            UINT32 desktop_path = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            UINT32 private_mode = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            for (UINT32 i = 0; i < kept; i++) {
                if (appsandbox_display_role(&paths[i]) != ASB_DISPLAY_DESKTOP) continue;
                UINT32 index = paths[i].sourceInfo.modeInfoIdx;
                if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) continue;
                if (index >= mode_count || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    result = ERROR_INVALID_DATA;
                    goto done;
                }
                POINTL position = modes[index].sourceMode.position;
                origin = position;
                have_origin = TRUE;
                desktop_mode = index;
                desktop_path = i;
            }
            for (UINT32 i = 0; i < kept; i++) {
                if (appsandbox_display_role(&paths[i]) != ASB_DISPLAY_PRIVATE) continue;
                have_private = TRUE;
                /* While the desktop output is being (re)activated it has no source mode yet:
                   let this pass activate it and place the private output on the next
                   WM_DISPLAYCHANGE. A clone/unplaceable private output is left alone; the
                   sharing helper's topology verification reports it as unsupported. */
                UINT32 index = paths[i].sourceInfo.modeInfoIdx;
                if (!have_origin || index >= mode_count || index == desktop_mode ||
                    modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
                    (paths[i].sourceInfo.id == paths[desktop_path].sourceInfo.id &&
                     paths[i].sourceInfo.adapterId.LowPart == paths[desktop_path].sourceInfo.adapterId.LowPart &&
                     paths[i].sourceInfo.adapterId.HighPart == paths[desktop_path].sourceInfo.adapterId.HighPart))
                    continue;
                /* Keep the desktop primary at (0,0) and the private output beyond its right
                   edge, outside the region the viewer/screenshots/absolute input cover. */
                LONG right = origin.x + (LONG)modes[desktop_mode].sourceMode.width;
                if (private_mode == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) private_mode = index;
                if (modes[index].sourceMode.position.x != right ||
                    modes[index].sourceMode.position.y != origin.y) {
                    modes[index].sourceMode.position.x = right;
                    modes[index].sourceMode.position.y = origin.y;
                    reposition = TRUE;
                }
            }
            for (UINT32 i = 0; i < mode_count; i++) {
                if (modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    modes[i].sourceMode.position.x -= origin.x;
                    modes[i].sourceMode.position.y -= origin.y;
                }
            }
            /* Never save a topology that includes the transient private output: the saved
               (database) configuration stays the user's single-desktop preference. */
            if ((activate || kept < active || reposition || origin.x || origin.y) &&
                WaitForSingleObject(g_display_stop, 0) != WAIT_OBJECT_0)
                result = SetDisplayConfig(kept, paths, mode_count, modes,
                    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                    (have_private ? 0 : SDC_SAVE_TO_DATABASE) |
                    (activate ? SDC_ALLOW_CHANGES : 0));
            if (result == ERROR_SUCCESS && have_origin) {
                const DISPLAYCONFIG_SOURCE_MODE *desktop = &modes[desktop_mode].sourceMode;
                SetRect(&g_desktop_rect, desktop->position.x, desktop->position.y,
                        desktop->position.x + (LONG)desktop->width, desktop->position.y + (LONG)desktop->height);
                if (private_mode != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
                    const DISPLAYCONFIG_SOURCE_MODE *placed = &modes[private_mode].sourceMode;
                    SetRect(&g_private_rect, placed->position.x, placed->position.y,
                            placed->position.x + (LONG)placed->width, placed->position.y + (LONG)placed->height);
                }
            }
        }
    }
done:
    free(modes);
    free(paths);
    if (input_desktop) {
        SetThreadDesktop(original_desktop);
        CloseDesktop(input_desktop);
    }
    return (int)result;
}

/* ---- Keep unparked windows off the private output -------------------------------- */

typedef struct RescueState {
    DWORD parked_pids[64];
    UINT parked_count;
    POINT workspace;   /* rcNormalPosition is in workspace coordinates */
    UINT desktop_dpi, private_dpi;
    int moved;
} RescueState;

static UINT monitor_dpi(const RECT *area)
{
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromRect(area, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y)) || !x)
        x = 96;
    return x;
}

static BOOL window_marked(HWND window)
{
    for (int depth = 0; window && depth < 32; ++depth, window = GetWindow(window, GW_OWNER))
        if (GetPropW(window, ASB_PRIVATE_PARKED_PROPERTY)) return TRUE;
    return FALSE;
}

static BOOL CALLBACK collect_parked(HWND window, LPARAM parameter)
{
    RescueState *state = (RescueState *)parameter;
    DWORD pid = 0;
    if (GetPropW(window, ASB_PRIVATE_PARKED_PROPERTY) && GetWindowThreadProcessId(window, &pid) &&
        state->parked_count < ARRAYSIZE(state->parked_pids))
        state->parked_pids[state->parked_count++] = pid;
    return TRUE;
}

static BOOL parked_popup(const RescueState *state, HWND window)
{
    LONG style = GetWindowLongW(window, GWL_STYLE);
    DWORD pid = 0;
    if (!(style & WS_POPUP) || (style & WS_CAPTION) == WS_CAPTION || !GetWindowThreadProcessId(window, &pid))
        return FALSE;
    for (UINT i = 0; i < state->parked_count; ++i)
        if (state->parked_pids[i] == pid) return TRUE;
    return FALSE;
}

static BOOL center_in(const RECT *rect, const RECT *area)
{
    POINT center = {rect->left + (rect->right - rect->left) / 2, rect->top + (rect->bottom - rect->top) / 2};
    return PtInRect(area, center);
}

/* Same offset from the private origin, clamped (size included) into the desktop. The size
   was laid out at the private output's DPI (a window migrated there grew with it), so it is
   scaled to the desktop's DPI: natively a 640x420 window came back as 800x525 (120 -> 96). */
static void move_rect_to_desktop(RECT *rect, UINT from_dpi, UINT to_dpi)
{
    LONG width = rect->right - rect->left, height = rect->bottom - rect->top;
    if (from_dpi && to_dpi && from_dpi != to_dpi) {
        width = MulDiv(width, (int)to_dpi, (int)from_dpi);
        height = MulDiv(height, (int)to_dpi, (int)from_dpi);
    }
    width = min(width, g_desktop_rect.right - g_desktop_rect.left);
    height = min(height, g_desktop_rect.bottom - g_desktop_rect.top);
    LONG x = g_desktop_rect.left + max(0, min(rect->left - g_private_rect.left,
                                              g_desktop_rect.right - g_desktop_rect.left - width));
    LONG y = g_desktop_rect.top + max(0, min(rect->top - g_private_rect.top,
                                             g_desktop_rect.bottom - g_desktop_rect.top - height));
    SetRect(rect, x, y, x + width, y + height);
}

/* Only ordinary application windows are moved. Explorer gives every output its own
   surfaces (the secondary taskbar Shell_SecondaryTrayWnd on the private output when
   "show taskbar on all displays" is on; Progman/WorkerW span the whole virtual screen):
   moving those duplicated the taskbar on the desktop natively. Tool windows (shell
   overlays, OSDs, palettes) and cloaked windows are left alone too. */
static BOOL shell_or_tool_window(HWND window)
{
    static const WCHAR *shell_classes[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
    };
    WCHAR name[64] = {0};
    if (GetClassNameW(window, name, ARRAYSIZE(name)))
        for (size_t i = 0; i < ARRAYSIZE(shell_classes); ++i)
            if (!wcscmp(name, shell_classes[i])) return TRUE;
    if (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

/* SetWindowPlacement does not move a window that is already maximized: it only
   updates the restore rectangle (verified natively, sync and async), so a window
   Windows re-maximized on the private output during a resize stayed there. Move the
   maximized frame onto the desktop's work area, keeping its border overhang; it stays
   maximized, is not activated, and restores to its (desktop) normal rectangle. */
static BOOL move_maximized_to_desktop(HWND window, const RECT *actual)
{
    MONITORINFO from = {sizeof(from)}, to = {sizeof(to)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &from) ||
        !GetMonitorInfoW(MonitorFromRect(&g_desktop_rect, MONITOR_DEFAULTTONEAREST), &to))
        return FALSE;
    RECT target = {to.rcWork.left - (from.rcWork.left - actual->left),
                   to.rcWork.top - (from.rcWork.top - actual->top),
                   to.rcWork.right + (actual->right - from.rcWork.right),
                   to.rcWork.bottom + (actual->bottom - from.rcWork.bottom)};
    return SetWindowPos(window, NULL, target.left, target.top, target.right - target.left,
                        target.bottom - target.top,
                        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

static BOOL CALLBACK rescue_window(HWND window, LPARAM parameter)
{
    RescueState *state = (RescueState *)parameter;
    if (!IsWindowVisible(window) || window_marked(window) || parked_popup(state, window) ||
        shell_or_tool_window(window)) return TRUE;
    WINDOWPLACEMENT placement = {sizeof(placement)};
    RECT actual, normal;
    if (!GetWindowPlacement(window, &placement) || !GetWindowRect(window, &actual)) return TRUE;
    normal = placement.rcNormalPosition;
    OffsetRect(&normal, state->workspace.x, state->workspace.y);
    BOOL normal_private = center_in(&normal, &g_private_rect);
    BOOL shown_private = !IsIconic(window) && center_in(&actual, &g_private_rect);
    if (!normal_private && !shown_private) return TRUE;
    BOOL maximized = placement.showCmd == SW_SHOWMAXIMIZED && !IsIconic(window);
    BOOL moved = FALSE;
    /* A maximized window only needs its restore rectangle fixed when that is private. */
    if (normal_private || !maximized) {
        if (normal_private) move_rect_to_desktop(&normal, state->private_dpi, state->desktop_dpi);
        OffsetRect(&normal, -state->workspace.x, -state->workspace.y);
        placement.rcNormalPosition = normal;
        placement.length = sizeof(placement);
        placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
        if (placement.showCmd == SW_SHOWMINIMIZED) placement.showCmd = SW_SHOWMINNOACTIVE;
        else if (placement.showCmd == SW_SHOWNORMAL) placement.showCmd = SW_SHOWNOACTIVATE;
        moved = SetWindowPlacement(window, &placement);
        /* Crossing to another DPI the window rescales (WM_DPICHANGED) after the first
           placement; the second re-applies the rectangle at the desktop's DPI. Both are
           queued to the window's thread in order (asynchronous, never waits on it). */
        if (moved && normal_private && state->private_dpi != state->desktop_dpi)
            SetWindowPlacement(window, &placement);
    }
    if (maximized && shown_private) moved |= move_maximized_to_desktop(window, &actual);
    if (moved) ++state->moved;
    return TRUE;
}

/* Windows land on the private output when Windows migrates them while the desktop
   output is re-created (viewer resize), or when dragged past the desktop edge. The
   private output only ever hosts windows the sharing helper marked before parking. */
static int rescue_private_windows(void)
{
    if (IsRectEmpty(&g_private_rect) || IsRectEmpty(&g_desktop_rect)) return 0;
    HDESK original_desktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK input_desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    RescueState state = {0};
    if (input_desktop && SetThreadDesktop(input_desktop)) {
        RECT work = g_desktop_rect;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            state.workspace.x = work.left - g_desktop_rect.left;
            state.workspace.y = work.top - g_desktop_rect.top;
        }
        state.desktop_dpi = monitor_dpi(&g_desktop_rect);
        state.private_dpi = monitor_dpi(&g_private_rect);
        EnumWindows(collect_parked, (LPARAM)&state);
        EnumWindows(rescue_window, (LPARAM)&state);
        SetThreadDesktop(original_desktop);
    }
    if (input_desktop) CloseDesktop(input_desktop);
    return state.moved;
}

/* Windows can refuse a topology while it is still reconfiguring (natively, when the
   desktop output was re-created during a resize while the private sharing output
   existed, SetDisplayConfig returned ERROR_INVALID_PARAMETER and the same layout was
   accepted seconds later). Retry for up to ~5 s before treating the failure as fatal,
   so the desktop is promptly restored as primary. */
#define DISPLAY_RETRY_COUNT 20
#define DISPLAY_RETRY_MS 250
static DWORD maintain_displays(void)
{
    DWORD result = (DWORD)disable_extra_displays();
    for (int retry = 0; result != ERROR_SUCCESS && retry < DISPLAY_RETRY_COUNT; ++retry) {
        if (WaitForSingleObject(g_display_stop, DISPLAY_RETRY_MS) == WAIT_OBJECT_0) return ERROR_SUCCESS;
        result = (DWORD)disable_extra_displays();
    }
    return result;
}

static DWORD WINAPI display_worker(LPVOID parameter)
{
    HWND window = (HWND)parameter;
    HANDLE events[] = {g_display_stop, g_display_changed};
    /* Physical coordinates for windows on outputs with different DPI. */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for (;;) {
        /* While a private output exists, also sweep once a second for windows dragged or
           created on it. */
        DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
                                            IsRectEmpty(&g_private_rect) ? INFINITE : 1000);
        if (wait == WAIT_TIMEOUT) {
            rescue_private_windows();
            continue;
        }
        if (wait != WAIT_OBJECT_0 + 1) break;
        DWORD result = maintain_displays();
        if (result != ERROR_SUCCESS) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return result;
        }
        rescue_private_windows();
    }
    return 0;
}

static LRESULT CALLBACK display_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_WTSSESSION_CHANGE:
        SetEvent(g_display_changed);
        return 0;
    case WM_DEVICECHANGE:
        if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) &&
            lparam && ((DEV_BROADCAST_HDR *)lparam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            SetEvent(g_display_changed);
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int watch_hyperv_outputs(HANDLE stop_event, HANDLE parent)
{
    static const GUID monitor_interface =
        {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
    WNDCLASSW cls = {0};
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {0};
    HWND window = NULL;
    HANDLE worker = NULL;
    HDEVNOTIFY notification = NULL;
    BOOL session_notifications = FALSE;
    DWORD result = 1;
    MSG message;

    g_display_changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_display_stop = stop_event ? stop_event : CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_display_changed || !g_display_stop) goto done;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpfnWndProc = display_window_proc;
    cls.lpszClassName = L"AppSandboxDisplayMonitor";
    if (!RegisterClassW(&cls)) goto done;
    window = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, cls.hInstance, NULL);
    if (!window) goto done;
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = monitor_interface;
    notification = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    session_notifications = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
    if (!notification) goto done;
    worker = CreateThread(NULL, 0, display_worker, window, 0, NULL);
    if (!worker) goto done;
    SetEvent(g_display_changed);
    for (;;) {
        HANDLE events[] = {g_display_stop, parent};
        DWORD count = parent ? ARRAYSIZE(events) : 1;
        DWORD wait = MsgWaitForMultipleObjects(count, events, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + count) break;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) goto done;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
done:
    if (g_display_stop) SetEvent(g_display_stop);
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
        GetExitCodeThread(worker, &result);
        CloseHandle(worker);
    }
    if (notification) UnregisterDeviceNotification(notification);
    if (session_notifications) WTSUnRegisterSessionNotification(window);
    if (window && IsWindow(window)) DestroyWindow(window);
    if (g_display_stop) CloseHandle(g_display_stop);
    if (g_display_changed) CloseHandle(g_display_changed);
    return (int)result;
}

int wmain(int argc, wchar_t **argv)
{
    HANDLE stop_event = NULL;
    HANDLE parent = NULL;
    if (argc != 1) {
        if (argc != 3 || wcscmp(argv[1], L"--stop-event")) return ERROR_INVALID_PARAMETER;
        wchar_t *end;
        errno = 0;
        DWORD parent_id = wcstoul(argv[2], &end, 10);
        if (errno || !parent_id || end == argv[2] || *end != ':') return ERROR_INVALID_PARAMETER;
        wchar_t *start = end + 1;
        unsigned long long value = _wcstoui64(start, &end, 10);
        if (errno || !value || start == end || *end || value != (ULONG_PTR)value) return ERROR_INVALID_PARAMETER;
        parent = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, parent_id);
        if (!parent) return (int)GetLastError();
        BOOL copied = DuplicateHandle(parent, (HANDLE)(ULONG_PTR)value, GetCurrentProcess(),
                                      &stop_event, SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, 0);
        DWORD error = copied ? ERROR_SUCCESS : GetLastError();
        if (!copied) {
            CloseHandle(parent);
            return (int)error;
        }
    }
    int result = watch_hyperv_outputs(stop_event, parent);
    if (parent) CloseHandle(parent);
    return result;
}
