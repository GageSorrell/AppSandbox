/**
 *
 *
 * @module AppSandbox/tools/agent/appsandbox-remote-windows
 *
 * @file      appsandbox-remote-windows.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * appsandbox-remote-windows.exe: the standard-user Remote Windows helper (AppSandbox-eh6.4).
 *
 * Launched and monitored by the AppSandbox guest service (agent.c) in the active console
 * session with the signed-in user's token:
 *
 *   appsandbox-remote-windows.exe [--stop-event <handle>] [--private-mode <W>x<H>]
 *       Serves Remote Windows control channel 9. Before serving, restores anything a previous
 *       helper left in the journal. Selections end on disconnect, stop or exit.
 *   appsandbox-remote-windows.exe --recover
 *       Independent recovery after a helper died: restores surviving journaled windows and
 *       removes the private display. Exit 0 when nothing is left, 1 when windows are still
 *       pending (not responding), 2 when a live helper owns the session (nothing done).
 *
 * The service writes the private-display lease token (16 hex digits) to stdin and closes it;
 * run by hand, the helper makes its own. One helper/recovery per session (named mutex).
 *
 * Threads: the control thread (COM STA) does all window work and protocol I/O; the owner
 * thread only owns the hidden suppression owner and pumps messages. The capture engine
 * (remote_windows_capture.cpp, eh6.5) has its own thread: it serves frame channel 10 for the
 * current control session and captures the surfaces the control thread hands it. Cross-process ownership
 * attaches input queues, so that thread must never block. If the helper dies, Windows unlinks
 * (does not destroy) the windows it owned; recovery then restores the rest.
 *
 * Proxy support (eh6.6): geometry/close/hit-test/window-state/icon window control lives in
 * remote_windows_control_win32.c; ASB_RW_CAP_GEOMETRY is advertised with the capture engine.
 *
 * Guest tray (eh6.8, remote_windows_tray.c): a tray icon on its own UI thread opens the shared
 * selection flyout (left click) or offers "Restore all windows" / "Exit" (right click). The
 * control thread owns every action: toggles become guest selector requests to the host,
 * Restore all restores every shared window, Exit restores everything and exits with
 * EXIT_USER, which tells the service not to start the helper again in this session.
 *
 * Drag and drop (eh6.9, remote_windows_files.cpp): the file bridge serves channel 11 while a
 * control session exists; DRAG_PROBE / DRAG_ACK reach it through the helper session and its
 * DRAG_STATE messages are queued here and sent by the control thread. Staging lives in
 * %LOCALAPPDATA%\AppSandbox\RemoteWindows\Drag.
 *
 * Logs: %LOCALAPPDATA%\AppSandbox\RemoteWindows\helper.log; journal: journal.bin there.
 * Test hook: signalling the named event Local\AppSandbox.RemoteWindows.SimulateDeviceLoss makes
 * the capture engine take its device-loss recovery path (only restarts capture).
 */
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remote_windows_capture.h"
#include "remote_windows_control_win32.h"
#include "remote_windows_files.h"
#include "remote_windows_helper.h"
#include "remote_windows_inventory.h"
#include "remote_windows_share_win32.h"
#include "remote_windows_tray.h"
#include "../transport/asb_transport.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
/* The flyout uses ListView checkbox state images and themes: common controls 6. */
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define EXIT_DONE      0
#define EXIT_PENDING   1
#define EXIT_BUSY      2
#define EXIT_FAILED    3
#define EXIT_USER      4   /* the user chose Exit in the guest tray (everything restored) */

#define IDLE_DISPLAY_MS   5000u   /* keep the private display briefly after the last unselect */
#define TICK_MS            100u
#define INVENTORY_MS       500u
#define EXIT_RESTORE_MS  10000u

static WCHAR g_dir[MAX_PATH];
static CRITICAL_SECTION g_log_lock;
static HANDLE g_stop;              /* service stop event (inherited) or local */
static volatile LONG g_ending;     /* WM_ENDSESSION */
static HWND g_owner;
static volatile LONG g_user_exit;  /* guest tray Exit */

/* ---- guest tray actions (eh6.8): posted by the tray thread, run by the control thread ---- */

typedef struct TrayAction { uint32_t action; uint64_t id; int select; } TrayAction;
static CRITICAL_SECTION g_actions_lock;
static TrayAction g_actions[32];
static uint32_t g_action_count;
static AsbRwGuestTray *g_tray;
static uint64_t g_published_hash;

static void tray_post(void *ctx, uint32_t action, uint64_t id, int select)
{
    (void)ctx;
    EnterCriticalSection(&g_actions_lock);
    if (g_action_count < ARRAYSIZE(g_actions)) {
        g_actions[g_action_count].action = action;
        g_actions[g_action_count].id = id;
        g_actions[g_action_count].select = select;
        ++g_action_count;
    }
    LeaveCriticalSection(&g_actions_lock);
}

static uint32_t take_actions(TrayAction *out, uint32_t max)
{
    EnterCriticalSection(&g_actions_lock);
    uint32_t n = g_action_count < max ? g_action_count : max;
    memcpy(out, g_actions, n * sizeof(*out));
    g_action_count = 0;
    LeaveCriticalSection(&g_actions_lock);
    return n;
}

static uint64_t model_hash(const AsbRwFlyoutModel *m)
{
    uint64_t h = 1469598103934665603ull;
#define MIX(p, n) do { const uint8_t *b_ = (const uint8_t *)(p); for (size_t k_ = 0; k_ < (n); ++k_) { h ^= b_[k_]; h *= 1099511628211ull; } } while (0)
    MIX(&m->status, sizeof(m->status));
    MIX(m->message, wcslen(m->message) * sizeof(wchar_t));
    MIX(&m->count, sizeof(m->count));
    for (uint32_t i = 0; i < m->count; ++i) {
        const AsbRwFlyoutItem *it = &m->items[i];
        MIX(&it->id, sizeof(it->id));
        MIX(&it->state, sizeof(it->state));
        MIX(&it->error, sizeof(it->error));
        MIX(&it->pending, sizeof(it->pending));
        MIX(it->label, wcslen(it->label) * sizeof(wchar_t));
    }
#undef MIX
    return h;
}

/* Publishes the selector model to the tray when it changed (helper NULL: not connected). */
static void publish_tray(const AsbRwHelper *helper)
{
    static AsbRwFlyoutModel model;
    static uint64_t hwnds[ASB_RW_FLYOUT_MAX_ITEMS];
    if (!g_tray) return;
    if (helper) {
        asb_rw_helper_flyout(helper, &model, hwnds);
    } else {
        memset(&model, 0, offsetof(AsbRwFlyoutModel, items));
        wcscpy_s(model.heading, ARRAYSIZE(model.heading), L"Show windows on the host");
        model.status = ASB_RW_FLYOUT_DISCONNECTED;
        wcscpy_s(model.message, ARRAYSIZE(model.message), L"Not connected to App Sandbox on the host.");
    }
    uint64_t h = model_hash(&model);
    if (h == g_published_hash) return;
    g_published_hash = h;
    asb_rw_guest_tray_publish(g_tray, &model, helper ? hwnds : NULL);
}

static void log_line(const char *message)
{
    WCHAR path[MAX_PATH], old[MAX_PATH];
    SYSTEMTIME t;
    FILE *file = NULL;
    if (!g_dir[0]) return;
    EnterCriticalSection(&g_log_lock);
    swprintf_s(path, MAX_PATH, L"%s\\helper.log", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > 1024 * 1024)) {
        swprintf_s(old, MAX_PATH, L"%s\\helper.old.log", g_dir);
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    if (!_wfopen_s(&file, path, L"a") && file) {
        GetLocalTime(&t);
        fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), message);
        fclose(file);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void logf_(const char *fmt, ...)
{
    char text[640];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    log_line(text);
}

static uint64_t random64(void)
{
    uint64_t v = 0;
    while (!v) BCryptGenRandom(NULL, (PUCHAR)&v, sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v;
}

/* Lease token from the service (stdin pipe), else a fresh one. */
static uint64_t read_lease(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char text[64] = {0};
    DWORD got = 0, n = 0;
    if (in && in != INVALID_HANDLE_VALUE && GetFileType(in) == FILE_TYPE_PIPE) {
        while (got < sizeof(text) - 1 && ReadFile(in, text + got, (DWORD)(sizeof(text) - 1 - got), &n, NULL) && n)
            got += n;
        unsigned long long value = _strtoui64(text, NULL, 16);
        if (value) return value;
    }
    return random64();
}

/* ---- owner thread -------------------------------------------------------------------- */

static LRESULT CALLBACK owner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            InterlockedExchange(&g_ending, 1);
            SetEvent(g_stop);
        }
        return 0;
    case WM_CLOSE:
        return 0;   /* the suppression owner lives as long as the helper */
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI owner_thread(LPVOID ready)
{
    WNDCLASSEXW wc = {sizeof(wc)};
    MSG msg;
    wc.lpfnWndProc = owner_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = ASB_RW_OWNER_CLASS;
    RegisterClassExW(&wc);
    /* Hidden, never shown, never activated. Owned roots lose their taskbar button and
       Alt+Tab entry while their own frame stays unchanged. */
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ASB_RW_OWNER_CLASS, L"AppSandbox Remote Windows",
                              WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    SetEvent((HANDLE)ready);
    if (!g_owner) return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ---- file bridge link (eh6.9) ---------------------------------------------------------- */

static AsbRwFiles *g_files;
static AsbListener *g_files_listener;
static AsbConn *g_files_conn;
static CRITICAL_SECTION g_drag_lock;
static AsbRwDrag g_drag_out[32];
static uint32_t g_drag_count;

static SOCKET files_accept(void *ctx, DWORD timeout_ms)
{
    (void)ctx;
    AsbConn *conn = g_files_listener ? asb_accept(g_files_listener, (int)timeout_ms) : NULL;
    if (!conn) return INVALID_SOCKET;
    if (g_files_conn) asb_close(g_files_conn);
    g_files_conn = conn;
    return (SOCKET)asb_conn_socket_u64(conn);
}

static void files_close(void *ctx, SOCKET s)
{
    (void)ctx;
    if (g_files_conn && (SOCKET)asb_conn_socket_u64(g_files_conn) == s) {
        asb_close(g_files_conn);
        g_files_conn = NULL;
    } else {
        closesocket(s);
    }
}

static void files_drag_state(void *ctx, const AsbRwDrag *state)
{
    (void)ctx;
    EnterCriticalSection(&g_drag_lock);
    if (g_drag_count < ARRAYSIZE(g_drag_out)) g_drag_out[g_drag_count++] = *state;
    LeaveCriticalSection(&g_drag_lock);
}

static void files_log(void *ctx, const char *message)
{
    (void)ctx;
    log_line(message);
}

static void helper_drag(void *ctx, uint16_t type, const AsbRwDrag *msg)
{
    (void)ctx;
    asb_rw_files_control(g_files, type, msg);
}

/* Control thread: sends queued DRAG_STATEs and publishes the surface table when it changed. */
static void pump_files(AsbRwHelper *helper, AsbRwShare *share, uint64_t *surface_hash)
{
    AsbRwDrag out[32];
    uint32_t n;
    if (!g_files) return;
    EnterCriticalSection(&g_drag_lock);
    n = g_drag_count;
    memcpy(out, g_drag_out, n * sizeof(out[0]));
    g_drag_count = 0;
    LeaveCriticalSection(&g_drag_lock);
    for (uint32_t i = 0; i < n; ++i) asb_rw_helper_drag_state(helper, &out[i]);
    static uint64_t ids[ASB_RW_SHARE_MAX * ASB_RW_MAX_SURFACES];
    static AsbRwRect rects[ASB_RW_SHARE_MAX * ASB_RW_MAX_SURFACES];
    static AsbRwFilesSurface table[ASB_RW_SHARE_MAX * ASB_RW_MAX_SURFACES];
    uint32_t count = asb_rw_helper_drag_surfaces(helper, ids, rects, ARRAYSIZE(ids));
    RECT area = {0};
    if (share->display_active) area = share->private_rect;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; ++i) {
        table[i].surface_id = ids[i];
        table[i].rect = rects[i];
        const uint8_t *b = (const uint8_t *)&table[i];
        for (size_t k = 0; k < sizeof(table[i]); ++k) { h ^= b[k]; h *= 1099511628211ull; }
    }
    const uint8_t *ab = (const uint8_t *)&area;
    for (size_t k = 0; k < sizeof(area); ++k) { h ^= ab[k]; h *= 1099511628211ull; }
    if (h == *surface_hash) return;
    *surface_hash = h;
    asb_rw_files_surfaces(g_files, table, count, &area);
}

/* ---- control channel ----------------------------------------------------------------- */

typedef struct Session {
    AsbConn *conn;
    AsbRwWin32 *w;
    AsbRwCapture *capture;
} Session;

/* ---- frame channel link for the capture engine (engine thread only) ------------------ */

typedef struct FrameLink {
    AsbListener *listener;
    AsbConn *conn;
} FrameLink;

static SOCKET frame_accept(void *ctx, DWORD timeout_ms)
{
    FrameLink *link = (FrameLink *)ctx;
    if (!link->listener) {
        Sleep(timeout_ms);
        return INVALID_SOCKET;
    }
    AsbConn *conn = asb_accept(link->listener, (int)timeout_ms);
    if (!conn) return INVALID_SOCKET;
    if (link->conn) asb_close(link->conn);
    link->conn = conn;
    return (SOCKET)asb_conn_socket_u64(conn);
}

static void frame_close(void *ctx, SOCKET s)
{
    FrameLink *link = (FrameLink *)ctx;
    if (link->conn && (SOCKET)asb_conn_socket_u64(link->conn) == s) {
        asb_close(link->conn);
        link->conn = NULL;
    } else {
        closesocket(s);
    }
}

static void capture_log(void *ctx, const char *message)
{
    (void)ctx;
    log_line(message);
}

static BOOL send_bytes(void *ctx, const uint8_t *bytes, size_t len)
{
    Session *s = (Session *)ctx;
    while (len) {
        int chunk = len > 65536 ? 65536 : (int)len;
        int n = asb_send(s->conn, bytes, chunk);
        if (n <= 0) return FALSE;
        bytes += n;
        len -= (size_t)n;
    }
    return TRUE;
}

static uint32_t enumerate(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx;
    return asb_rw_enumerate_windows(out, max, GetCurrentProcessId(), count);
}

static BOOL facts(void *ctx, HWND hwnd, AsbRwWindowFacts *f)
{
    (void)ctx;
    return asb_rw_collect_facts(hwnd, GetCurrentProcessId(), asb_rw_active_session(), f);
}

static int probe(void *ctx, uint64_t hwnd, AsbRwIdentity *live)
{
    (void)ctx;
    return asb_rw_probe_identity(NULL, hwnd, live);
}

static void session_log(void *ctx, const char *message)
{
    (void)ctx;
    log_line(message);
}

static uint32_t scan(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx;
    return asb_rw_scan_visible_windows(out, max, count);
}

static void capture_set(void *ctx, uint64_t root_id, const AsbRwCaptureTarget *targets, uint32_t n)
{
    Session *s = (Session *)ctx;
    asb_rw_capture_set(s->capture, root_id, targets, n);
}

static BOOL recv_exact(AsbConn *c, uint8_t *buf, size_t len)
{
    while (len) {
        int n = asb_recv(c, buf, len > 65536 ? 65536 : (int)len);
        if (n <= 0) return FALSE;
        buf += n;
        len -= (size_t)n;
    }
    return TRUE;
}

static BOOL stopping(void) { return g_user_exit || WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0; }

/* Runs the tray's actions on the control thread (helper NULL: no host connected). */
static void run_actions(AsbRwHelper *helper, AsbRwShare *share)
{
    TrayAction actions[32];
    uint32_t n = take_actions(actions, ARRAYSIZE(actions));
    for (uint32_t i = 0; i < n; ++i) {
        switch (actions[i].action) {
        case ASB_RW_ACTION_SELECT:
            if (helper) {
                uint32_t r = asb_rw_helper_guest_select(helper, actions[i].id, actions[i].select);
                if (r) logf_("tray: %s %016llx -> %s", actions[i].select ? "select" : "unselect",
                             (unsigned long long)actions[i].id, asb_rw_error_name(r));
            }
            break;
        case ASB_RW_ACTION_RESTORE_ALL:
            log_line("tray: restore all windows");
            if (helper) asb_rw_helper_restore_all(helper);
            else asb_rw_share_restore_all(share);
            break;
        case ASB_RW_ACTION_EXIT:
            log_line("tray: exit requested; restoring every window first");
            if (helper) asb_rw_helper_restore_all(helper);
            else asb_rw_share_restore_all(share);
            InterlockedExchange(&g_user_exit, 1);
            break;
        }
    }
}

static void maybe_release_display(AsbRwShare *share, ULONGLONG *idle_since)
{
    if (share->count || !share->display_active) { *idle_since = 0; return; }
    ULONGLONG now = GetTickCount64();
    if (!*idle_since) *idle_since = now;
    else if (now - *idle_since >= IDLE_DISPLAY_MS) asb_rw_share_release_display(share);
}

static void serve(AsbConn *conn, AsbRwShare *share, AsbRwWin32 *w, AsbRwCapture *capture, uint32_t capabilities,
                  uint32_t driver_version)
{
    Session session = {conn, w, capture};
    AsbRwHelperOps ops = {&session, send_bytes, enumerate, facts, probe, session_log,
                          capture ? scan : NULL, capture ? capture_set : NULL};
    AsbRwHelper helper;
    uint64_t surface_hash = 0;
    asb_rw_ctl_ops(&ops);   /* eh6.6: geometry, close, hit-test, window state, icons; eh6.7: activation */
    if (g_files) ops.drag = helper_drag;   /* eh6.9 */
    uint8_t *buffer = (uint8_t *)malloc(ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD);
    uint32_t tag = 0;
    ULONGLONG last_inventory = 0, idle_since = 0;
    while (!tag) tag = (uint32_t)random64();
    asb_set_timeout(conn, 10000, 10000);   /* bounds a stalled partial message or send */
    if (!buffer || asb_rw_helper_open(&helper, &ops, share, capabilities, driver_version, tag)) {
        log_line("session: open failed");
        free(buffer);
        return;
    }
    logf_("session: connected, tag %08x caps %08x", tag, capabilities);
    asb_rw_capture_session(capture, tag);   /* frame channel 10 must name this session */
    EnterCriticalSection(&g_drag_lock);
    g_drag_count = 0;
    LeaveCriticalSection(&g_drag_lock);
    asb_rw_files_session(g_files, tag);     /* file channel 11 is served while this session lasts */
    for (;;) {
        if (stopping()) { log_line("session: stop requested"); break; }
        int ready = asb_poll(conn, TICK_MS);
        if (ready < 0) { log_line("session: host disconnected"); break; }
        if (ready > 0) {
            size_t total = 0;
            if (!recv_exact(conn, buffer, ASB_RW_HEADER_BYTES)) { log_line("session: host disconnected"); break; }
            uint32_t result = asb_rw_frame_check(ASB_CH_RW_CONTROL, buffer, ASB_RW_HEADER_BYTES, &total);
            if (result || total < ASB_RW_HEADER_BYTES ||
                !recv_exact(conn, buffer + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES)) {
                logf_("session: bad frame (%s); dropping connection", asb_rw_error_name(result));
                break;
            }
            result = asb_rw_helper_receive(&helper, buffer, total);
            if (result) { logf_("session: protocol error %s; dropping connection", asb_rw_error_name(result)); break; }
        }
        ULONGLONG now = GetTickCount64();
        int inventory = now - last_inventory >= INVENTORY_MS;
        if (inventory) last_inventory = now;
        run_actions(&helper, share);
        asb_rw_helper_tick(&helper, inventory);
        pump_files(&helper, share, &surface_hash);
        publish_tray(&helper);
        if (helper.send_failed) { log_line("session: send failed"); break; }
        maybe_release_display(share, &idle_since);
    }
    asb_rw_files_session(g_files, 0);   /* cancels file sessions; the guest pointer is released */
    asb_rw_helper_close(&helper);   /* selections end with the session */
    publish_tray(NULL);
    asb_rw_capture_session(capture, 0);
    free(buffer);
    logf_("session: ended (%u window(s) still restoring)", share->count);
}

/* ---- main ---------------------------------------------------------------------------- */

static void finish_restoration(AsbRwShare *share, DWORD budget_ms)
{
    ULONGLONG deadline = GetTickCount64() + budget_ms;
    asb_rw_share_restore_all(share);
    while ((share->count || share->display_active) && GetTickCount64() < deadline) {
        asb_rw_share_maintain(share, NULL, 0, NULL, NULL, 0, NULL);
        if (!share->count) asb_rw_share_release_display(share);
        if (share->count || share->display_active) Sleep(250);
    }
}

static BOOL parse_mode(const WCHAR *text, uint32_t *width, uint32_t *height)
{
    unsigned w = 0, h = 0;
    if (swscanf_s(text, L"%ux%u", &w, &h) != 2 || !asb_private_mode_supported(w, h)) return FALSE;
    *width = w;
    *height = h;
    return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
    BOOL recover = FALSE;
    uint32_t width = 2560, height = 1440;
    WCHAR journal[MAX_PATH];
    AsbRwWin32 w;
    AsbRwShareOps share_ops;
    static AsbRwShare share;
    uint8_t *bytes = NULL;
    size_t len = 0;
    int code = EXIT_DONE;

    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_actions_lock);
    InitializeCriticalSection(&g_drag_lock);
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--recover")) recover = TRUE;
        else if (!_wcsicmp(argv[i], L"--stop-event") && i + 1 < argc)
            g_stop = (HANDLE)(ULONG_PTR)_wcstoui64(argv[++i], NULL, 16);
        else if (!_wcsicmp(argv[i], L"--private-mode") && i + 1 < argc) parse_mode(argv[++i], &width, &height);
    }
    if (!g_stop || WaitForSingleObject(g_stop, 0) == WAIT_FAILED) g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!asb_rw_state_dir(g_dir, MAX_PATH)) return EXIT_FAILED;
    swprintf_s(journal, MAX_PATH, L"%s\\journal.bin", g_dir);
    uint64_t lease = read_lease();
    logf_("start: %s, session %lu, private mode %ux%u", recover ? "recovery" : "helper", asb_rw_active_session(),
          width, height);

    /* One helper or recovery per session. A helper that died leaves the mutex abandoned. */
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\AppSandbox.RemoteWindows.Helper");
    HANDLE waits[2] = {mutex, g_stop};
    DWORD waited = mutex ? WaitForMultipleObjects(2, waits, FALSE, recover ? 10000 : INFINITE) : WAIT_FAILED;
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED_0) {
        logf_("start: another helper owns this session (%lu)", waited);
        return EXIT_BUSY;
    }
    if (waited == WAIT_ABANDONED_0) log_line("start: previous helper ended without releasing; recovering");

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!recover) {
        HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread = ready ? CreateThread(NULL, 0, owner_thread, ready, 0, NULL) : NULL;
        if (thread) { WaitForSingleObject(ready, 5000); CloseHandle(thread); }
        if (ready) CloseHandle(ready);
        if (!g_owner) { log_line("start: owner window failed"); ReleaseMutex(mutex); return EXIT_FAILED; }
    }
    asb_rw_win32_init(&w, journal, log_line);
    asb_rw_win32_ops(&w, &share_ops);
    asb_rw_share_init(&share, &share_ops, g_owner, lease, width, height);

    /* Recovery never re-parks: the journal only says what to restore. */
    if (!asb_rw_journal_read(journal, &bytes, &len)) log_line("start: journal unreadable");
    if (len) {
        uint32_t result = asb_rw_share_recover(&share, bytes, len);
        logf_("recovery: %s, %u window(s) pending, display %s", asb_rw_error_name(result), share.count,
              share.display_active ? "present" : "absent");
    }
    free(bytes);

    if (recover) {
        if (share.count || share.display_active) finish_restoration(&share, EXIT_RESTORE_MS);
        code = share.count ? EXIT_PENDING : share.display_active ? EXIT_PENDING : EXIT_DONE;
        logf_("recovery: exit %d", code);
        asb_rw_win32_close(&w);
        ReleaseMutex(mutex);
        return code;
    }

    uint32_t capabilities = ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION, driver_version = 0;
    {
        AsbRwDisplay probe_display;
        if (!asb_rw_display_open(&probe_display, lease)) {
            capabilities |= ASB_RW_CAP_PRIVATE_DISPLAY;
            driver_version = ASB_PRIVATE_DISPLAY_VERSION;
            asb_rw_display_close(&probe_display);
        } else {
            log_line("start: private display interface unavailable (VDD without eh6.3 support)");
        }
    }
    AsbListener *listener = NULL;
    static FrameLink frame_link;
    AsbRwCapture *capture = NULL;
    if (!asb_transport_init() && !asb_transport_is_ivshmem()) {
        listener = asb_listen(ASB_CH_RW_CONTROL);
        frame_link.listener = asb_listen(ASB_CH_RW_FRAMES);
    }
    if (!listener) {
        log_line("start: control channel 9 unavailable");
        code = EXIT_FAILED;
    }
    if (listener && frame_link.listener) {
        AsbRwCaptureConfig cc;
        AsbRwCaptureStats cs;
        memset(&cc, 0, sizeof(cc));
        cc.ctx = &frame_link;
        cc.accept = frame_accept;
        cc.close = frame_close;
        cc.log = capture_log;
        cc.allow_gpu = 1;
        cc.slots = 3;
        cc.max_cpu_bytes = 128u << 20;
        cc.device_loss_event = L"Local\\AppSandbox.RemoteWindows.SimulateDeviceLoss";
        capture = asb_rw_capture_start(&cc);
        if (capture) {
            asb_rw_capture_stats(capture, &cs);
            capabilities |= ASB_RW_CAP_FRAMES_CPU | ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY | ASB_RW_CAP_ACTIVATION;   /* proxies need frames */
            if (cs.gpu_available) capabilities |= ASB_RW_CAP_FRAMES_GPU;
        } else {
            log_line("start: capture engine unavailable (Windows Graphics Capture); no frames");
        }
    } else if (listener) {
        log_line("start: frame channel 10 unavailable; no frames");
    }
    if (listener && capture && !asb_transport_is_ivshmem()) {
        /* eh6.9: drag and drop needs proxies (frames), so it starts with the capture engine. */
        WCHAR stage[MAX_PATH];
        AsbRwFilesConfig fc;
        g_files_listener = asb_listen(ASB_CH_RW_FILES);
        memset(&fc, 0, sizeof(fc));
        fc.log = files_log;
        fc.drag_state = files_drag_state;
        fc.accept = files_accept;
        fc.close = files_close;
        swprintf_s(stage, MAX_PATH, L"%s\\Drag", g_dir);
        fc.stage_parent = stage;
        fc.directions = ASB_RW_DIR_TO_GUEST | ASB_RW_DIR_FROM_GUEST;
        g_files = g_files_listener ? asb_rw_files_start(&fc) : NULL;
        if (g_files) capabilities |= ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST;
        else log_line("start: file channel 11 / drag bridge unavailable; no drag and drop");
    }
    if (listener) {
        AsbRwGuestTrayConfig tc;
        memset(&tc, 0, sizeof(tc));
        tc.post = tray_post;
        g_tray = asb_rw_guest_tray_start(&tc);
        if (g_tray) capabilities |= ASB_RW_CAP_GUEST_SELECTOR;
        else log_line("start: guest tray unavailable");
        publish_tray(NULL);
    }
    while (listener && !stopping()) {
        run_actions(NULL, &share);
        if (stopping()) break;
        AsbConn *conn = asb_accept(listener, 500);
        if (conn) {
            serve(conn, &share, &w, capture, capabilities, driver_version);
            asb_close(conn);
        }
        /* Without a host, keep retrying pending restorations; then drop the display. */
        if (share.count) asb_rw_share_maintain(&share, NULL, 0, NULL, NULL, 0, NULL);
        if (!share.count && share.display_active) asb_rw_share_release_display(&share);
    }
    if (listener) asb_close_listener(listener);
    asb_rw_files_stop(g_files);   /* releases the guest pointer if a drag was running */
    g_files = NULL;
    if (g_files_conn) asb_close(g_files_conn);
    g_files_conn = NULL;
    if (g_files_listener) asb_close_listener(g_files_listener);
    g_files_listener = NULL;
    asb_rw_capture_stop(capture);
    if (frame_link.listener) asb_close_listener(frame_link.listener);
    asb_rw_guest_tray_stop(g_tray);
    g_tray = NULL;
    if (!g_ending) finish_restoration(&share, EXIT_RESTORE_MS);
    logf_("exit: %u window(s) pending, display %s%s", share.count, share.display_active ? "present" : "absent",
          g_user_exit ? " (user exit)" : "");
    if (share.count || share.display_active) code = EXIT_PENDING;
    else if (g_user_exit && code == EXIT_DONE) code = EXIT_USER;
    asb_rw_win32_close(&w);
    CoUninitialize();
    ReleaseMutex(mutex);
    return code;
}
