/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_display_guest_test
 *
 * @file      remote_windows_display_guest_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* The actual guest private-display client (tools/agent/remote_windows_display.c) and
   display watcher (tools/agent/appsandbox-displays.c) against a stateful model of the
   Windows CCD API and the VDD private-display IOCTL. */
#include <windows.h>
#include <setupapi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "../agent/remote_windows_display.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

/* ---- CCD model ---------------------------------------------------------------------- */
enum { OUT_DESKTOP, OUT_PRIVATE, OUT_FOREIGN, OUT_COUNT };
typedef struct Output {
    const WCHAR *path;
    UINT32 target, source;     /* target and source IDs differ, as on real adapters */
    BOOL available, active;
    UINT32 width, height;
    LONG x, y;
} Output;
static Output g_out[OUT_COUNT];
static int g_applies, g_validates, g_saves, g_allow_changes, g_last_count;
static BOOL g_reject_set, g_strict_modes;
static int g_reject_count;
static LONG g_reject_error = ERROR_INVALID_PARAMETER;
static int g_rejections;

static void model_reset(void)
{
    static const WCHAR *names[OUT_COUNT] = {
        L"\\\\?\\DISPLAY#ASB0001#5&1&0&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}",
        L"\\\\?\\DISPLAY#ASB0002#5&1&0&UID257#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}",
        L"\\\\?\\DISPLAY#MSH062E#5&2&0&UID0#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}"};
    ZeroMemory(g_out, sizeof(g_out));
    for (int i = 0; i < OUT_COUNT; ++i) {
        g_out[i].path = names[i];
        g_out[i].target = 0x100 + 0x10 * (UINT32)i;
        g_out[i].source = 7 - (UINT32)i;
    }
    g_out[OUT_DESKTOP].available = g_out[OUT_DESKTOP].active = TRUE;
    g_out[OUT_DESKTOP].width = 1920; g_out[OUT_DESKTOP].height = 1080;
    g_applies = g_validates = g_saves = g_allow_changes = g_last_count = 0;
    g_reject_set = FALSE; g_reject_count = 0; g_reject_error = ERROR_INVALID_PARAMETER;
}

static void private_arrives(void) { g_out[OUT_PRIVATE].available = TRUE; }
static void private_active(UINT32 w, UINT32 h, LONG x, LONG y)
{
    Output *o = &g_out[OUT_PRIVATE];
    o->available = o->active = TRUE; o->width = w; o->height = h; o->x = x; o->y = y;
}

static LONG WINAPI fake_sizes(UINT32 flags, UINT32 *paths, UINT32 *modes)
{
    UINT32 p = 0, m = 0;
    for (int i = 0; i < OUT_COUNT; ++i) {
        if (!g_out[i].available) continue;
        if (g_out[i].active) { ++p; m += 2; }
        else if (flags & QDC_ALL_PATHS) ++p;
    }
    *paths = p; *modes = m;
    return ERROR_SUCCESS;
}

static LONG WINAPI fake_query(UINT32 flags, UINT32 *count, DISPLAYCONFIG_PATH_INFO *paths,
    UINT32 *modes_count, DISPLAYCONFIG_MODE_INFO *modes, DISPLAYCONFIG_TOPOLOGY_ID *topology)
{
    (void)topology;
    UINT32 p = 0, m = 0, need_p = 0, need_m = 0;
    fake_sizes(flags, &need_p, &need_m);
    if (*count < need_p || *modes_count < need_m) return ERROR_INSUFFICIENT_BUFFER;
    ZeroMemory(paths, *count * sizeof(*paths));
    ZeroMemory(modes, *modes_count * sizeof(*modes));
    /* Enumerate the private output first so code cannot depend on ordering. */
    static const int order[OUT_COUNT] = {OUT_PRIVATE, OUT_FOREIGN, OUT_DESKTOP};
    for (int k = 0; k < OUT_COUNT; ++k) {
        const Output *o = &g_out[order[k]];
        if (!o->available || (!o->active && !(flags & QDC_ALL_PATHS))) continue;
        DISPLAYCONFIG_PATH_INFO *path = &paths[p++];
        path->sourceInfo.adapterId.LowPart = 1;
        path->sourceInfo.id = o->source;
        path->targetInfo.adapterId.LowPart = 1;
        path->targetInfo.id = o->target;
        path->targetInfo.targetAvailable = TRUE;
        path->sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        if (!o->active) continue;
        path->flags = DISPLAYCONFIG_PATH_ACTIVE;
        DISPLAYCONFIG_MODE_INFO *source = &modes[m];
        source->infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        source->adapterId.LowPart = 1;
        source->id = o->source;
        source->sourceMode.width = o->width;
        source->sourceMode.height = o->height;
        source->sourceMode.position.x = o->x;
        source->sourceMode.position.y = o->y;
        path->sourceInfo.modeInfoIdx = m++;
        DISPLAYCONFIG_MODE_INFO *target = &modes[m];
        target->infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        target->adapterId.LowPart = 1;
        target->id = o->target;
        target->targetMode.targetVideoSignalInfo.activeSize.cx = o->width;
        target->targetMode.targetVideoSignalInfo.activeSize.cy = o->height;
        path->targetInfo.modeInfoIdx = m++;
    }
    *count = p; *modes_count = m;
    return ERROR_SUCCESS;
}

static LONG WINAPI fake_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *header)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME *name = (DISPLAYCONFIG_TARGET_DEVICE_NAME *)header;
    CHECK(header->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME && header->size == sizeof(*name));
    for (int i = 0; i < OUT_COUNT; ++i) {
        if (g_out[i].target != header->id) continue;
        ZeroMemory(name->monitorDevicePath, sizeof(name->monitorDevicePath));
        wcscpy_s(name->monitorDevicePath, ARRAYSIZE(name->monitorDevicePath), g_out[i].path);
        /* Friendly names are identical and must not be used for identity. */
        wcscpy_s(name->monitorFriendlyDeviceName, ARRAYSIZE(name->monitorFriendlyDeviceName), L"AppSandboxVDD");
        return ERROR_SUCCESS;
    }
    return ERROR_NOT_FOUND;
}

static Output *output_for_target(UINT32 target)
{
    for (int i = 0; i < OUT_COUNT; ++i) if (g_out[i].target == target) return &g_out[i];
    return NULL;
}

static LONG WINAPI fake_set(UINT32 count, DISPLAYCONFIG_PATH_INFO *paths, UINT32 mode_count,
    DISPLAYCONFIG_MODE_INFO *modes, UINT32 flags)
{
    CHECK(flags & SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    if (flags & SDC_SAVE_TO_DATABASE) ++g_saves;
    if (flags & SDC_ALLOW_CHANGES) ++g_allow_changes;
    g_last_count = (int)count;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 s = paths[i].sourceInfo.modeInfoIdx, t = paths[i].targetInfo.modeInfoIdx;
        CHECK(output_for_target(paths[i].targetInfo.id));
        CHECK(s == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
              (s < mode_count && modes[s].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
               modes[s].id == paths[i].sourceInfo.id));
        CHECK(t == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
              (t < mode_count && modes[t].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
               modes[t].id == paths[i].targetInfo.id));
        if (s == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || t == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
            CHECK(flags & SDC_ALLOW_CHANGES);
    }
    /* The sharing client supplies a compact table: every mode is referenced by a path. */
    for (UINT32 m = 0; g_strict_modes && m < mode_count; ++m) {
        BOOL used = FALSE;
        for (UINT32 i = 0; i < count; ++i)
            used |= paths[i].sourceInfo.modeInfoIdx == m || paths[i].targetInfo.modeInfoIdx == m;
        CHECK(used);
    }
    if (g_reject_set || g_reject_count) {
        if (g_reject_count) --g_reject_count;
        ++g_rejections;
        return g_reject_error;
    }
    if (flags & SDC_VALIDATE) { ++g_validates; return ERROR_SUCCESS; }
    CHECK(flags & SDC_APPLY);
    ++g_applies;
    for (int i = 0; i < OUT_COUNT; ++i) {
        BOOL supplied = FALSE;
        for (UINT32 k = 0; k < count; ++k) supplied |= paths[k].targetInfo.id == g_out[i].target;
        if (!supplied) g_out[i].active = FALSE;
    }
    for (UINT32 i = 0; i < count; ++i) {
        Output *o = output_for_target(paths[i].targetInfo.id);
        o->active = (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        UINT32 s = paths[i].sourceInfo.modeInfoIdx;
        if (s != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            o->width = modes[s].sourceMode.width; o->height = modes[s].sourceMode.height;
            o->x = modes[s].sourceMode.position.x; o->y = modes[s].sourceMode.position.y;
        } else if (o->active) {
            /* SDC_ALLOW_CHANGES: Windows picks a mode and places it after the others. */
            if (!o->width) { o->width = 1920; o->height = 1080; }
            LONG right = 0;
            for (int k = 0; k < OUT_COUNT; ++k)
                if (&g_out[k] != o && g_out[k].active && g_out[k].x + (LONG)g_out[k].width > right)
                    right = g_out[k].x + (LONG)g_out[k].width;
            o->x = right; o->y = 0;
        }
    }
    return ERROR_SUCCESS;
}

/* ---- VDD private-display IOCTL model ------------------------------------------------ */
static int stage, restored, removes, activates, queries, sleeps;
static BOOL fail_restore, fail_remove, bad_reply;
static DWORD activate_error;
static UINT32 driver_state, driver_w, driver_h;
static BOOL WINAPI fake_ioctl(HANDLE h, DWORD code, LPVOID input, DWORD input_size,
    LPVOID output, DWORD output_size, LPDWORD bytes, LPOVERLAPPED overlap)
{
    (void)h; (void)overlap;
    CHECK(code == ASB_PRIVATE_IOCTL && input_size == sizeof(AsbPrivateDisplayRequest));
    CHECK(output_size == sizeof(AsbPrivateDisplayReply));
    AsbPrivateDisplayRequest *request = input;
    CHECK(request->size == sizeof(*request) && request->version == ASB_PRIVATE_DISPLAY_VERSION);
    CHECK(request->token == 42 && !request->reserved);
    switch (request->operation) {
    case ASB_PRIVATE_QUERY: ++queries; break;
    case ASB_PRIVATE_ACTIVATE:
        ++activates;
        if (activate_error) { SetLastError(activate_error); return FALSE; }
        driver_state = ASB_PRIVATE_READY; driver_w = request->width; driver_h = request->height;
        private_arrives();
        break;
    case ASB_PRIVATE_RESTORE: stage = 1; driver_state = ASB_PRIVATE_RESTORING; break;
    case ASB_PRIVATE_REMOVE:
        CHECK(stage == 2 && restored); ++removes;
        if (fail_remove) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
        stage = 3; driver_state = ASB_PRIVATE_ABSENT; driver_w = driver_h = 0;
        g_out[OUT_PRIVATE].available = g_out[OUT_PRIVATE].active = FALSE;
        break;
    default: CHECK(!"unexpected operation");
    }
    AsbPrivateDisplayReply reply = {sizeof(reply), 1, driver_state, driver_w, driver_h,
                                    ASB_DISPLAY_DESKTOP, ASB_DISPLAY_PRIVATE, 0};
    if (bad_reply) reply.private_id = ASB_DISPLAY_DESKTOP;
    memcpy(output, &reply, sizeof(reply)); *bytes = sizeof(reply);
    return TRUE;
}
static void WINAPI fake_sleep(DWORD ms) { (void)ms; ++sleeps; }

#define DeviceIoControl fake_ioctl
#define GetDisplayConfigBufferSizes fake_sizes
#define QueryDisplayConfig fake_query
#define DisplayConfigGetDeviceInfo fake_info
#define SetDisplayConfig fake_set
#define Sleep fake_sleep
#include "../agent/remote_windows_display.c"
#undef Sleep
static HDESK WINAPI fake_open_desktop(DWORD flags, BOOL inherit, ACCESS_MASK access)
{ (void)flags; (void)inherit; (void)access; return (HDESK)1; }
static BOOL WINAPI fake_set_desktop(HDESK desktop) { (void)desktop; return TRUE; }
static BOOL WINAPI fake_close_desktop(HDESK desktop) { (void)desktop; return TRUE; }
/* ---- Window model for the stray-window sweep ---------------------------------------- */
typedef struct FakeWindow {
    const WCHAR *cls;
    LONG exstyle;
    DWORD cloaked;
    BOOL visible, marked, iconic;
    int owner;            /* index + 1, 0 = none */
    DWORD pid;
    LONG style;
    RECT normal, actual;  /* normal in workspace coordinates, actual in screen coordinates */
    UINT show;
    int sets, positions;
    WINDOWPLACEMENT applied;
    RECT positioned;
    UINT position_flags;
} FakeWindow;
static FakeWindow g_win[32];
static int g_win_count;
static RECT g_work_area;
#define WIN_HANDLE(i) ((HWND)(ULONG_PTR)(0x1000 + (i)))
static FakeWindow *fake_window(HWND h)
{
    ULONG_PTR i = (ULONG_PTR)h - 0x1000;
    CHECK(i < (ULONG_PTR)g_win_count);
    return &g_win[i];
}
static BOOL WINAPI fake_enum_windows(WNDENUMPROC proc, LPARAM parameter)
{
    for (int i = 0; i < g_win_count; ++i) if (!proc(WIN_HANDLE(i), parameter)) break;
    return TRUE;
}
static BOOL WINAPI fake_visible(HWND h) { return fake_window(h)->visible; }
static BOOL WINAPI fake_iconic(HWND h) { return fake_window(h)->iconic; }
static HANDLE WINAPI fake_get_prop(HWND h, LPCWSTR name)
{
    CHECK(!wcscmp(name, ASB_PRIVATE_PARKED_PROPERTY));
    return fake_window(h)->marked ? (HANDLE)1 : NULL;
}
static HWND WINAPI fake_get_window(HWND h, UINT command)
{
    CHECK(command == GW_OWNER);
    int owner = fake_window(h)->owner;
    return owner ? WIN_HANDLE(owner - 1) : NULL;
}
static DWORD WINAPI fake_thread_pid(HWND h, LPDWORD pid) { *pid = fake_window(h)->pid; return 1; }
static LONG WINAPI fake_window_long(HWND h, int index)
{
    CHECK(index == GWL_STYLE || index == GWL_EXSTYLE);
    return index == GWL_STYLE ? fake_window(h)->style : fake_window(h)->exstyle;
}
static int WINAPI fake_class_name(HWND h, LPWSTR name, int count)
{
    wcscpy_s(name, (size_t)count, fake_window(h)->cls ? fake_window(h)->cls : L"AppWindowClass");
    return (int)wcslen(name);
}
static HRESULT WINAPI fake_dwm_attribute(HWND h, DWORD attribute, PVOID value, DWORD size)
{
    CHECK(attribute == DWMWA_CLOAKED && size == sizeof(DWORD));
    *(DWORD *)value = fake_window(h)->cloaked;
    return S_OK;
}
static BOOL WINAPI fake_get_placement(HWND h, WINDOWPLACEMENT *p)
{
    CHECK(p->length == sizeof(*p));
    FakeWindow *w = fake_window(h);
    ZeroMemory(p, sizeof(*p));
    p->length = sizeof(*p);
    p->showCmd = w->show;
    p->rcNormalPosition = w->normal;
    return TRUE;
}
static BOOL WINAPI fake_set_placement(HWND h, const WINDOWPLACEMENT *p)
{
    FakeWindow *w = fake_window(h);
    ++w->sets;
    w->applied = *p;
    return TRUE;
}
static BOOL WINAPI fake_window_rect(HWND h, LPRECT r) { *r = fake_window(h)->actual; return TRUE; }
static BOOL WINAPI fake_set_pos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    FakeWindow *w = fake_window(h);
    CHECK(after == NULL);
    ++w->positions;
    SetRect(&w->positioned, x, y, x + cx, y + cy);
    w->position_flags = flags;
    return TRUE;
}
/* Two monitors: the desktop (work area g_work_area) and the private output, which has
   Explorer's secondary taskbar along its bottom edge. */
static RECT g_desktop_rect, g_private_rect; /* tentative; defined by the watcher below */
#define FAKE_DESKTOP_MONITOR ((HMONITOR)(ULONG_PTR)0x2001)
#define FAKE_PRIVATE_MONITOR ((HMONITOR)(ULONG_PTR)0x2002)
static HMONITOR fake_monitor_of(const RECT *r)
{
    POINT center = {r->left + (r->right - r->left) / 2, r->top + (r->bottom - r->top) / 2};
    return PtInRect(&g_private_rect, center) ? FAKE_PRIVATE_MONITOR : FAKE_DESKTOP_MONITOR;
}
static HMONITOR WINAPI fake_monitor_from_window(HWND h, DWORD flags)
{
    CHECK(flags == MONITOR_DEFAULTTONEAREST);
    return fake_monitor_of(&fake_window(h)->actual);
}
static HMONITOR WINAPI fake_monitor_from_rect(LPCRECT r, DWORD flags)
{
    CHECK(flags == MONITOR_DEFAULTTONEAREST);
    return fake_monitor_of(r);
}
static BOOL WINAPI fake_monitor_info(HMONITOR m, LPMONITORINFO info)
{
    CHECK(info->cbSize == sizeof(MONITORINFO));
    if (m == FAKE_DESKTOP_MONITOR) {
        info->rcMonitor = g_desktop_rect;
        info->rcWork = g_work_area;
    } else {
        CHECK(m == FAKE_PRIVATE_MONITOR);
        info->rcMonitor = info->rcWork = g_private_rect;
        info->rcWork.bottom -= 48;
    }
    return TRUE;
}
static UINT g_desktop_dpi = 96, g_private_dpi = 96;
static HRESULT WINAPI fake_dpi_for_monitor(HMONITOR m, MONITOR_DPI_TYPE type, UINT *x, UINT *y)
{
    CHECK(type == MDT_EFFECTIVE_DPI);
    *x = *y = m == FAKE_PRIVATE_MONITOR ? g_private_dpi : g_desktop_dpi;
    return S_OK;
}
static BOOL WINAPI fake_spi(UINT action, UINT u, PVOID value, UINT w)
{
    (void)u; (void)w;
    CHECK(action == SPI_GETWORKAREA);
    *(RECT *)value = g_work_area;
    return TRUE;
}
static int add_window(BOOL visible, BOOL marked, int owner, DWORD pid, LONG style,
                      RECT normal, RECT actual, UINT show, BOOL iconic)
{
    CHECK(g_win_count < (int)ARRAYSIZE(g_win));
    FakeWindow *w = &g_win[g_win_count];
    ZeroMemory(w, sizeof(*w));
    w->visible = visible; w->marked = marked; w->owner = owner; w->pid = pid; w->style = style;
    w->normal = normal; w->actual = actual; w->show = show; w->iconic = iconic;
    return g_win_count++;
}
static RECT R(LONG l, LONG t, LONG r, LONG b) { RECT x = {l, t, r, b}; return x; }
#define EnumWindows fake_enum_windows
#define IsWindowVisible fake_visible
#define IsIconic fake_iconic
#define GetPropW fake_get_prop
#define GetWindow fake_get_window
#define GetWindowThreadProcessId fake_thread_pid
#define GetWindowLongW fake_window_long
#define GetWindowPlacement fake_get_placement
#define SetWindowPlacement fake_set_placement
#define GetWindowRect fake_window_rect
#define SetWindowPos fake_set_pos
#define MonitorFromWindow fake_monitor_from_window
#define MonitorFromRect fake_monitor_from_rect
#define GetMonitorInfoW fake_monitor_info
#define SystemParametersInfoW fake_spi
#define GetDpiForMonitor fake_dpi_for_monitor
#define GetClassNameW fake_class_name
#define DwmGetWindowAttribute fake_dwm_attribute
#define OpenInputDesktop fake_open_desktop
#define SetThreadDesktop fake_set_desktop
#define CloseDesktop fake_close_desktop
#define wmain display_watcher_main
#include "../agent/appsandbox-displays.c"
#undef wmain

static DWORD restore_sources(void *context)
{
    (void)context;
    CHECK(stage == 1 && driver_state == ASB_PRIVATE_RESTORING);
    CHECK(g_out[OUT_PRIVATE].available); /* sources are restored while the output still exists */
    stage = 2; restored = 1;
    return fail_restore ? ERROR_BUSY : ERROR_SUCCESS;
}

static void desktop_is_primary(void)
{
    CHECK(g_out[OUT_DESKTOP].active && g_out[OUT_DESKTOP].x == 0 && g_out[OUT_DESKTOP].y == 0);
}

static AsbRwDisplay leased(void)
{
    AsbRwDisplay display = {0};
    display.driver = (HANDLE)1; display.token = 42;
    return display;
}

static void client_tests(void)
{
    /* Arrival, apply and verify; desktop source/target modes are left untouched. */
    model_reset(); driver_state = ASB_PRIVATE_ABSENT;
    AsbRwDisplay display = leased();
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS);
    CHECK(activates == 1 && g_validates == 1 && g_applies == 1 && !g_saves && g_last_count == 2);
    desktop_is_primary();
    CHECK(g_out[OUT_DESKTOP].width == 1920 && g_out[OUT_DESKTOP].height == 1080);
    CHECK(g_out[OUT_PRIVATE].active && g_out[OUT_PRIVATE].x == 1920 && g_out[OUT_PRIVATE].y == 0);
    CHECK(g_out[OUT_PRIVATE].width == 2560 && g_out[OUT_PRIVATE].height == 1440);
    CHECK(display.desktop.right == 1920 && display.desktop.bottom == 1080);
    CHECK(display.private_rect.left == 1920 && display.private_rect.right == 4480 &&
          display.private_rect.bottom == 1440);
    UINT32 state = 0, w = 0, h = 0;
    CHECK(asb_rw_display_query(&display, &state, &w, &h) == ERROR_SUCCESS &&
          state == ASB_PRIVATE_READY && w == 2560 && h == 1440);

    /* Already correct: re-activation verifies without another mode set. */
    int applies = g_applies;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS && g_applies == applies);

    /* Desktop resized by the viewer while sharing: rectangles follow, no failure. */
    g_out[OUT_DESKTOP].width = 3440; g_out[OUT_DESKTOP].height = 1334; g_out[OUT_PRIVATE].x = 3440;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS && g_applies == applies);
    CHECK(display.desktop.right == 3440 && display.private_rect.left == 3440);

    /* Misplaced (e.g. Windows moved it left of the desktop): re-placed to the right. */
    g_out[OUT_PRIVATE].x = -2560;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS && g_applies == applies + 1);
    desktop_is_primary();
    CHECK(g_out[OUT_PRIVATE].x == 3440);

    /* Foreign output activated by Windows alongside the arrival (native: the Hyper-V
       synthetic display): kept with its modes, private placed beyond it. */
    model_reset(); g_out[OUT_FOREIGN].available = g_out[OUT_FOREIGN].active = TRUE;
    g_out[OUT_FOREIGN].width = 1024; g_out[OUT_FOREIGN].height = 768; g_out[OUT_FOREIGN].x = 1920;
    display = leased();
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS && g_applies == 1 && g_last_count == 3);
    desktop_is_primary();
    CHECK(g_out[OUT_FOREIGN].active && g_out[OUT_FOREIGN].x == 1920 && g_out[OUT_FOREIGN].width == 1024);
    CHECK(g_out[OUT_PRIVATE].x == 2944 && display.private_rect.left == 2944);
    /* The display watcher then disables the foreign output and closes the gap; the
       client re-verifies without another mode set. */
    g_display_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_strict_modes = FALSE;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 2);
    g_strict_modes = TRUE;
    CloseHandle(g_display_stop);
    CHECK(!g_out[OUT_FOREIGN].active && g_out[OUT_PRIVATE].x == 1920);
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS && g_applies == 2);
    CHECK(display.private_rect.left == 1920);

    /* Clone of the desktop source: unsupported, never applied over. */
    model_reset(); private_active(1920, 1080, 0, 0);
    g_out[OUT_PRIVATE].source = g_out[OUT_DESKTOP].source;
    display = leased();
    CHECK(asb_rw_display_activate(&display, 1920, 1080) == ERROR_NOT_SUPPORTED && !g_applies);
    CHECK(IsRectEmpty(&display.private_rect));

    /* Windows refuses the topology: retried, then reported as unsupported. */
    model_reset(); g_reject_set = TRUE; display = leased(); g_rejections = 0;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_NOT_SUPPORTED && !g_applies);
    CHECK(g_rejections == ASB_TOPOLOGY_REJECT_RETRIES);
    /* Transient rejections right after arrival (natively ERROR_NOT_SUPPORTED for ~1 s,
       ~10 SetDisplayConfig attempts) are retried. */
    model_reset(); g_reject_count = 10; g_reject_error = ERROR_NOT_SUPPORTED; display = leased(); g_rejections = 0;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS && g_applies == 1 && g_rejections == 10);
    desktop_is_primary();
    CHECK(g_out[OUT_PRIVATE].x == 1920);
    /* Refresh republishes rectangles after the layout moves, and reports drift. */
    g_out[OUT_DESKTOP].width = 1280; g_out[OUT_PRIVATE].x = 1280;
    CHECK(asb_rw_display_refresh(&display, 2560, 1440) == ERROR_SUCCESS && display.private_rect.left == 1280);
    g_out[OUT_PRIVATE].x = 0;
    CHECK(asb_rw_display_refresh(&display, 2560, 1440) == ERROR_INVALID_STATE && IsRectEmpty(&display.private_rect));
    CHECK(g_applies == 1);

    /* Desktop not primary (watcher has not normalized yet): bounded retries, not ready. */
    model_reset(); g_out[OUT_DESKTOP].x = 2560; sleeps = 0; display = leased();
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_NOT_READY && !g_applies);
    CHECK(sleeps == 50);

    /* Output never enumerates: not ready, lease kept for supervised restoration. */
    model_reset(); activates = 0; sleeps = 0; display = leased();
    g_out[OUT_PRIVATE].path = L"\\\\?\\DISPLAY#ASB0003#x";
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_NOT_READY && activates == 1);

    /* Unsupported modes never reach the driver; driver failures propagate. */
    model_reset(); activates = 0; display = leased();
    CHECK(asb_rw_display_activate(&display, 7681, 1440) == ERROR_NOT_SUPPORTED);
    CHECK(asb_rw_display_activate(&display, 639, 480) == ERROR_NOT_SUPPORTED && !activates);
    CHECK(asb_rw_display_activate(&display, 7680, 4320) == ERROR_SUCCESS);
    model_reset(); display = leased();
    activate_error = ERROR_ACCESS_DENIED;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_ACCESS_DENIED && !g_applies);
    activate_error = ERROR_BUSY;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_BUSY);
    activate_error = 0;
    bad_reply = TRUE;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_REVISION_MISMATCH && !g_applies);
    bad_reply = FALSE;

    /* Removal strictly follows source restoration; failures keep the output. */
    model_reset(); display = leased(); stage = 0; restored = 0; removes = 0;
    CHECK(asb_rw_display_activate(&display, 2560, 1440) == ERROR_SUCCESS);
    fail_restore = TRUE;
    CHECK(asb_rw_display_restore(&display, restore_sources, NULL) == ERROR_BUSY && removes == 0);
    CHECK(g_out[OUT_PRIVATE].active && driver_state == ASB_PRIVATE_RESTORING);
    CHECK(!IsRectEmpty(&display.private_rect));
    fail_restore = FALSE; fail_remove = TRUE;
    CHECK(asb_rw_display_restore(&display, restore_sources, NULL) == ERROR_GEN_FAILURE && removes == 1);
    CHECK(g_out[OUT_PRIVATE].available);
    fail_remove = FALSE;
    CHECK(asb_rw_display_restore(&display, restore_sources, NULL) == ERROR_SUCCESS && stage == 3 && removes == 2);
    CHECK(IsRectEmpty(&display.private_rect) && !g_out[OUT_PRIVATE].available);
    desktop_is_primary();
    CHECK(asb_rw_display_restore(&display, NULL, NULL) == ERROR_INVALID_PARAMETER);
}

static void watcher_tests(void)
{
    g_display_stop = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Private output overlapping the desktop is moved right; nothing is saved. */
    model_reset(); private_active(2560, 1440, 0, 0);
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 1 && !g_saves);
    desktop_is_primary();
    CHECK(g_out[OUT_PRIVATE].active && g_out[OUT_PRIVATE].x == 1920 && g_out[OUT_PRIVATE].y == 0);
    CHECK(g_out[OUT_DESKTOP].width == 1920);

    /* Steady state: no mode set. */
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 1);

    /* Private became primary (e.g. user action): desktop is restored as primary. */
    model_reset(); private_active(2560, 1440, 0, 0); g_out[OUT_DESKTOP].x = 2560;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 1 && !g_saves);
    desktop_is_primary();
    CHECK(g_out[OUT_PRIVATE].x == 1920 && g_out[OUT_PRIVATE].y == 0);

    /* Desktop departed/re-arrived during a viewer resize: first pass reactivates it
       without failing, the following WM_DISPLAYCHANGE pass restores the layout. */
    model_reset(); private_active(2560, 1440, 0, 0); g_out[OUT_DESKTOP].active = FALSE;
    g_out[OUT_DESKTOP].width = 3440; g_out[OUT_DESKTOP].height = 1334;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 1 && g_allow_changes == 1);
    CHECK(g_out[OUT_DESKTOP].active && g_out[OUT_PRIVATE].active && !g_saves);
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 2);
    desktop_is_primary();
    CHECK(g_out[OUT_PRIVATE].x == 3440);

    /* A removed/disabled private output is never resurrected. */
    model_reset(); private_arrives();
    CHECK(disable_extra_displays() == ERROR_SUCCESS && !g_applies && !g_out[OUT_PRIVATE].active);

    /* A cloned private output is left alone rather than stopping desktop maintenance. */
    model_reset(); private_active(1920, 1080, 0, 0);
    g_out[OUT_PRIVATE].source = g_out[OUT_DESKTOP].source;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && !g_applies);

    /* Existing behaviour without sharing: foreign outputs disabled, desktop activated and
       saved as the preference; private output (if any) excluded from the saved topology. */
    model_reset(); g_out[OUT_DESKTOP].active = FALSE;
    g_out[OUT_FOREIGN].available = g_out[OUT_FOREIGN].active = TRUE;
    g_out[OUT_FOREIGN].width = 1024; g_out[OUT_FOREIGN].height = 768;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 1 && g_saves == 1);
    CHECK(g_out[OUT_DESKTOP].active && !g_out[OUT_FOREIGN].active);
    model_reset(); private_active(2560, 1440, 1920, 0);
    g_out[OUT_FOREIGN].available = g_out[OUT_FOREIGN].active = TRUE;
    g_out[OUT_FOREIGN].width = 1024; g_out[OUT_FOREIGN].height = 768; g_out[OUT_FOREIGN].x = 4480;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && g_applies == 1 && !g_saves);
    CHECK(!g_out[OUT_FOREIGN].active && g_out[OUT_PRIVATE].active && g_out[OUT_PRIVATE].x == 1920);
    desktop_is_primary();

    /* Desktop re-created during a resize while sharing (native 07:34:30): Windows made
       the private output primary, re-enabled the Hyper-V output and refused the first
       corrective SetDisplayConfig (ERROR_INVALID_PARAMETER). The watcher retries instead
       of exiting, and the desktop is primary again. */
    model_reset(); private_active(2560, 1440, 0, 0);
    g_out[OUT_DESKTOP].x = 3584; g_out[OUT_DESKTOP].width = 2747;
    g_out[OUT_FOREIGN].available = g_out[OUT_FOREIGN].active = TRUE;
    g_out[OUT_FOREIGN].width = 1024; g_out[OUT_FOREIGN].height = 768; g_out[OUT_FOREIGN].x = 2560;
    g_reject_count = 2; g_rejections = 0;
    CHECK(maintain_displays() == ERROR_SUCCESS && g_rejections == 2 && g_applies == 1 && !g_saves);
    desktop_is_primary();
    CHECK(!g_out[OUT_FOREIGN].active && g_out[OUT_PRIVATE].x == 2747);
    /* A persistent refusal is still reported (the watcher exits and the agent restarts it). */
    model_reset(); private_active(2560, 1440, 0, 0); g_reject_set = TRUE; g_rejections = 0;
    CHECK(maintain_displays() == ERROR_INVALID_PARAMETER && g_rejections == DISPLAY_RETRY_COUNT + 1);
    /* Stop during retries ends them without an error. */
    model_reset(); private_active(2560, 1440, 0, 0); g_reject_set = TRUE; SetEvent(g_display_stop);
    CHECK(maintain_displays() == ERROR_SUCCESS);
    ResetEvent(g_display_stop);

    /* Stop requested: no mode set. */
    model_reset(); private_active(2560, 1440, 0, 0); SetEvent(g_display_stop);
    CHECK(disable_extra_displays() == ERROR_SUCCESS && !g_applies);
    CloseHandle(g_display_stop);
}


static void expect_moved(int i, RECT normal, UINT show)
{
    FakeWindow *w = &g_win[i];
    CHECK(w->sets == 1 && (w->applied.flags & WPF_ASYNCWINDOWPLACEMENT));
    CHECK(EqualRect(&w->applied.rcNormalPosition, &normal) && w->applied.showCmd == show);
}

/* SetWindowPlacement leaves an already-maximized window where it is, so a maximized
   window shown on the private output is moved with SetWindowPos, without activation. */
static void expect_maximized_moved(int i, RECT frame)
{
    FakeWindow *w = &g_win[i];
    CHECK(w->positions == 1 && EqualRect(&w->positioned, &frame));
    CHECK((w->position_flags & (SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS)) ==
          (SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS));
}

static void rescue_tests(void)
{
    LONG tiled = WS_OVERLAPPEDWINDOW, popup = WS_POPUP | WS_BORDER;
    g_display_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* Rectangles come from the watcher pass that placed the private output. */
    model_reset(); private_active(2560, 1440, 0, 0);
    CHECK(disable_extra_displays() == ERROR_SUCCESS);
    CHECK(EqualRect(&g_desktop_rect, &(RECT){0, 0, 1920, 1080}));
    CHECK(EqualRect(&g_private_rect, &(RECT){1920, 0, 4480, 1440}));

    g_win_count = 0; g_work_area = R(0, 0, 1920, 1032);
    RECT priv = g_private_rect;
    int stray = add_window(TRUE, FALSE, 0, 10, tiled, R(2000, 100, 2800, 700), R(2000, 100, 2800, 700), SW_SHOWNORMAL, FALSE);
    /* Maximized frames overhang the work area by their resize border (8 px here). */
    RECT priv_frame = R(1912, -8, 4488, 1400), desk_frame = R(-8, -8, 1928, 1040);
    int maxed = add_window(TRUE, FALSE, 0, 11, tiled, R(2100, 200, 3000, 900), priv_frame, SW_SHOWMAXIMIZED, FALSE);
    /* Native case (VS Code during a viewer resize): Windows re-maximized the window on the
       private output while its restore rectangle stayed on the desktop. */
    int remaxed = add_window(TRUE, FALSE, 0, 14, tiled, R(100, 100, 900, 700), priv_frame, SW_SHOWMAXIMIZED, FALSE);
    int desk_maxed = add_window(TRUE, FALSE, 0, 15, tiled, R(100, 100, 900, 700), desk_frame, SW_SHOWMAXIMIZED, FALSE);
    int mini = add_window(TRUE, FALSE, 0, 12, tiled, R(2500, 300, 2900, 600), R(-32000, -32000, -31840, -31972), SW_SHOWMINIMIZED, TRUE);
    int huge = add_window(TRUE, FALSE, 0, 13, tiled, R(1950, 10, 4950, 1510), R(1950, 10, 4950, 1510), SW_SHOWNORMAL, FALSE);
    int parked = add_window(TRUE, TRUE, 0, 20, tiled, R(2000, 100, 2800, 700), R(2000, 100, 2800, 700), SW_SHOWNORMAL, FALSE);
    int dialog = add_window(TRUE, FALSE, parked + 1, 20, tiled, R(2100, 200, 2500, 500), R(2100, 200, 2500, 500), SW_SHOWNORMAL, FALSE);
    int menu = add_window(TRUE, FALSE, 0, 20, popup, R(2200, 300, 2400, 500), R(2200, 300, 2400, 500), SW_SHOWNORMAL, FALSE);
    int sibling = add_window(TRUE, FALSE, 0, 20, tiled, R(3000, 100, 3500, 500), R(3000, 100, 3500, 500), SW_SHOWNORMAL, FALSE);
    int desk = add_window(TRUE, FALSE, 0, 30, tiled, R(100, 100, 900, 700), R(100, 100, 900, 700), SW_SHOWNORMAL, FALSE);
    int hidden = add_window(FALSE, FALSE, 0, 31, tiled, R(2000, 100, 2800, 700), R(2000, 100, 2800, 700), SW_HIDE, FALSE);
    /* Explorer per-output surfaces (native regression: the private output's secondary
       taskbar was moved onto the desktop), tool windows and cloaked windows stay. */
    int taskbar = add_window(TRUE, FALSE, 0, 50, popup, R(1920, 1392, 4480, 1440), R(1920, 1392, 4480, 1440), SW_SHOWNORMAL, FALSE);
    g_win[taskbar].cls = L"Shell_SecondaryTrayWnd"; g_win[taskbar].exstyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    int wallpaper = add_window(TRUE, FALSE, 0, 50, popup, R(0, 0, 4480, 1440), R(0, 0, 4480, 1440), SW_SHOWNORMAL, FALSE);
    g_win[wallpaper].cls = L"Progman";
    int worker = add_window(TRUE, FALSE, 0, 50, popup, R(0, 0, 4480, 1440), R(0, 0, 4480, 1440), SW_SHOWNORMAL, FALSE);
    g_win[worker].cls = L"WorkerW";
    int tool = add_window(TRUE, FALSE, 0, 51, tiled, R(2000, 100, 2300, 300), R(2000, 100, 2300, 300), SW_SHOWNORMAL, FALSE);
    g_win[tool].exstyle = WS_EX_TOOLWINDOW;
    int cloaked = add_window(TRUE, FALSE, 0, 52, tiled, R(2000, 100, 2800, 700), R(2000, 100, 2800, 700), SW_SHOWNORMAL, FALSE);
    g_win[cloaked].cloaked = DWM_CLOAKED_SHELL;
    (void)priv;
    CHECK(rescue_private_windows() == 6);
    expect_moved(stray, R(80, 100, 880, 700), SW_SHOWNOACTIVATE);   /* same offset, no activation */
    expect_moved(maxed, R(180, 200, 1080, 900), SW_SHOWMAXIMIZED);  /* restore rectangle on the desktop */
    expect_maximized_moved(maxed, desk_frame);                      /* and the maximized frame */
    CHECK(!g_win[remaxed].sets);                                    /* restore rectangle already fine */
    expect_maximized_moved(remaxed, desk_frame);
    CHECK(!g_win[desk_maxed].sets && !g_win[desk_maxed].positions);
    for (int i = 0; i < g_win_count; ++i)
        if (i != maxed && i != remaxed) CHECK(!g_win[i].positions);
    expect_moved(mini, R(580, 300, 980, 600), SW_SHOWMINNOACTIVE);  /* restores onto the desktop */
    expect_moved(huge, R(0, 0, 1920, 1080), SW_SHOWNOACTIVATE);     /* clamped to the desktop */
    expect_moved(sibling, R(1080, 100, 1580, 500), SW_SHOWNOACTIVATE); /* independent root of a parked app */
    CHECK(!g_win[parked].sets && !g_win[dialog].sets && !g_win[menu].sets);
    CHECK(!g_win[desk].sets && !g_win[hidden].sets);
    CHECK(!g_win[taskbar].sets && !g_win[wallpaper].sets && !g_win[worker].sets);
    CHECK(!g_win[tool].sets && !g_win[cloaked].sets);

    /* Taskbar at the top: normal positions are workspace coordinates. */
    g_win_count = 0; g_work_area = R(0, 48, 1920, 1080);
    int top = add_window(TRUE, FALSE, 0, 40, tiled, R(2000, 52, 2400, 352), R(2000, 100, 2400, 400), SW_SHOWNORMAL, FALSE);
    CHECK(rescue_private_windows() == 1);
    expect_moved(top, R(80, 52, 480, 352), SW_SHOWNOACTIVATE);

    /* Mixed DPI (private 120, desktop 96): sizes laid out on the private output are scaled
       back, and the placement is applied twice (once to cross the DPI change, once at the
       desktop DPI). Natively a 640x420 window otherwise came back as 800x525. */
    g_win_count = 0; g_work_area = R(0, 0, 1920, 1032); g_private_dpi = 120;
    stray = add_window(TRUE, FALSE, 0, 10, tiled, R(2120, 120, 2920, 645), R(2120, 120, 2920, 645), SW_SHOWNORMAL, FALSE);
    mini = add_window(TRUE, FALSE, 0, 12, tiled, R(2500, 300, 2900, 600), R(-32000, -32000, -31840, -31972), SW_SHOWMINIMIZED, TRUE);
    huge = add_window(TRUE, FALSE, 0, 13, tiled, R(1950, 10, 4950, 1510), R(1950, 10, 4950, 1510), SW_SHOWNORMAL, FALSE);
    maxed = add_window(TRUE, FALSE, 0, 11, tiled, R(2100, 200, 3000, 900), priv_frame, SW_SHOWMAXIMIZED, FALSE);
    remaxed = add_window(TRUE, FALSE, 0, 14, tiled, R(100, 100, 900, 700), priv_frame, SW_SHOWMAXIMIZED, FALSE);
    CHECK(rescue_private_windows() == 5);
    CHECK(g_win[stray].sets == 2 && EqualRect(&g_win[stray].applied.rcNormalPosition, &(RECT){200, 120, 840, 540}));
    CHECK(g_win[mini].sets == 2 && EqualRect(&g_win[mini].applied.rcNormalPosition, &(RECT){580, 300, 900, 540}));
    CHECK(g_win[huge].sets == 2 && EqualRect(&g_win[huge].applied.rcNormalPosition, &(RECT){0, 0, 1920, 1080}));
    CHECK(g_win[maxed].sets == 2 && EqualRect(&g_win[maxed].applied.rcNormalPosition, &(RECT){180, 200, 900, 760}));
    expect_maximized_moved(maxed, desk_frame);
    CHECK(!g_win[remaxed].sets);                     /* restore rectangle on the desktop: untouched size */
    expect_maximized_moved(remaxed, desk_frame);
    g_private_dpi = 96;

    /* No private output: nothing is examined or moved. */
    model_reset();
    CHECK(disable_extra_displays() == ERROR_SUCCESS && IsRectEmpty(&g_private_rect));
    g_win_count = 0;
    stray = add_window(TRUE, FALSE, 0, 10, tiled, R(2000, 100, 2800, 700), R(2000, 100, 2800, 700), SW_SHOWNORMAL, FALSE);
    CHECK(rescue_private_windows() == 0 && !g_win[stray].sets);
    /* A clone/unplaceable private output is not swept (no trusted rectangle). */
    model_reset(); private_active(1920, 1080, 0, 0); g_out[OUT_PRIVATE].source = g_out[OUT_DESKTOP].source;
    CHECK(disable_extra_displays() == ERROR_SUCCESS && IsRectEmpty(&g_private_rect));
    CloseHandle(g_display_stop);
}

int main(void)
{
    CHECK(asb_display_role(L"\\\\?\\DISPLAY#ASB0001#5&1&0&UID256#{guid}") == ASB_DISPLAY_DESKTOP);
    CHECK(asb_display_role(L"\\\\?\\DISPLAY#asb0002#5&1&0&UID257#{guid}") == ASB_DISPLAY_PRIVATE);
    CHECK(!asb_display_role(L"\\\\?\\DISPLAY#ASB00021#x") && !asb_display_role(L"\\\\?\\DISPLAY#XASB0001#x"));
    CHECK(!asb_display_role(L"AppSandboxVDD") && !asb_display_role(NULL));
    g_strict_modes = TRUE;
    client_tests();
    g_strict_modes = FALSE;
    watcher_tests();
    rescue_tests();
    puts("PASS guest identity, private topology apply/verify, desktop stays primary, resize/re-arrival, "
         "unsupported topologies/modes, saved-preference isolation, restoration ordering, retries "
         "and unparked windows returned from the private output");
    return 0;
}
