/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_files
 *
 * @file      remote_windows_files.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows guest drag-and-drop bridge (AppSandbox-eh6.9). See remote_windows_files.h.
   The I/O thread makes every decision; the OLE thread only executes commands (start a drag,
   probe, release) and reports back, so the session state has one owner. */
#include "remote_windows_files.h"
#include "remote_windows_files_win32.h"

#include <ole2.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include <string>
#include <vector>

#include "../../src/core/protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

const wchar_t kOleClass[] = L"AppSandbox.RemoteWindows.Files";
const wchar_t kCatcherClass[] = L"AppSandbox.RemoteWindows.Catcher";
const int kCatcherSize = 64;
const DWORD kAckMs = 3000;
const DWORD kProgressMs = 250;
const DWORD kFirstMoveMs = 40;

enum : UINT {
    WM_FILES_START = WM_APP + 0x200,   /* lParam Cmd* */
    WM_FILES_PROBE,
    WM_FILES_RELEASE,
};

enum ResultKind { RES_DRAG_DONE = 1, RES_PROBE_DONE, RES_RELEASED };

struct Cmd {
    uint64_t session = 0;
    uint32_t probe = 0, button = INPUT_BTN_LEFT;
    std::vector<std::wstring> paths;
};

struct OleResult {
    int kind = 0;
    uint64_t session = 0;
    uint32_t probe = 0;
    DWORD effect = 0;
    HRESULT hr = S_OK;
    uint32_t outcome = 0;                /* PROBE_DONE: ASB_RW_DRAG_NONE / _SINK / _ACTIVE */
    uint32_t error = 0;
    AsbRwSource *source = nullptr;       /* PROBE_DONE ACTIVE: ownership passes to the I/O thread */
};

struct Session {
    uint32_t phase = ASB_RW_FILES_IDLE;
    uint64_t id = 0;
    uint32_t direction = 0;
    /* host -> guest */
    AsbRwStage *stage = nullptr;
    uint64_t surface = 0;
    int32_t x = 0, y = 0;
    bool point_dirty = false, drop = false, staged = false, release_sent = false;
    bool state_sent = false, host_cancelled = false, counted = false;
    ULONGLONG ack_deadline = 0, running_since = 0, last_progress = 0;
    uint32_t sent_effect = ~0u;
    uint32_t error = 0;
    /* guest -> host */
    uint32_t probe = 0, button = INPUT_BTN_LEFT;
    bool release_after_probe = false;
    AsbRwSource *source = nullptr;
    HANDLE sender = nullptr;
};

} // namespace

struct AsbRwFiles {
    AsbRwFilesConfig cfg = {};
    std::wstring stage_parent;
    CRITICAL_SECTION lock;               /* control/OLE mailboxes, surfaces, stats */
    CRITICAL_SECTION send_lock;
    HANDLE io_thread = nullptr, ole_thread = nullptr, stop_event = nullptr, ole_ready = nullptr;
    volatile LONG stopping = 0;
    volatile LONG tag = 0;
    HWND ole_hwnd = nullptr, catcher = nullptr;
    IDropTarget *catcher_target = nullptr;
    SOCKET sock = INVALID_SOCKET;        /* I/O thread; closed under send_lock */
    uint32_t peer_directions = 0, chunk = ASB_RW_FILE_CHUNK_DEFAULT;
    std::vector<AsbRwFilesSurface> surfaces;
    RECT catcher_area = {};
    std::vector<std::pair<uint16_t, AsbRwDrag>> control_in;
    std::vector<OleResult> ole_out;
    Session s;                           /* I/O thread only */
    /* shared with the OLE thread's drag source / catcher */
    volatile LONG cancel = 0, release = 0, drag_running = 0, effect = 0;
    volatile LONG self_drag = 0, probe_waiting = 0, from_guest_active = 0;
    uint32_t probe_outcome = 0, probe_error = 0;  /* OLE thread */
    AsbRwSource *probe_source = nullptr;          /* OLE thread */
    volatile LONG sender_cancel = 0, sender_done = 0;
    volatile LONG pressed = 0;           /* logical left button pressed through inject_button */
    uint32_t sender_result = 0;
    AsbRwFilesStats stats = {};
    std::vector<uint8_t> rx;
};

namespace {

/* ================================ logging / control ======================= */

void flog(AsbRwFiles *f, const char *fmt, ...)
{
    if (!f->cfg.log) return;
    char text[640];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    f->cfg.log(f->cfg.ctx, text);
}

void drag_state(AsbRwFiles *f, uint32_t state, uint32_t error)
{
    AsbRwDrag d = {};
    Session &s = f->s;
    d.state = state;
    d.error = error;
    d.probe = s.probe;
    if (state == ASB_RW_DRAG_ACTIVE) {
        d.session_id = s.id;
        d.direction = s.direction;
        d.surface_id = s.direction == ASB_RW_DIR_TO_GUEST ? s.surface : 0;
    } else if (state == ASB_RW_DRAG_ENDED) {
        d.session_id = s.id;
    }
    if (f->cfg.drag_state) f->cfg.drag_state(f->cfg.ctx, &d);
    if (state == ASB_RW_DRAG_ACTIVE) s.state_sent = true;
}

/* ================================ pointer ================================= */

void inject_move(AsbRwFiles *f, int32_t x, int32_t y)
{
    if (f->cfg.inject_move) { f->cfg.inject_move(f->cfg.ctx, x, y); return; }
    int32_t vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int32_t vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    uint32_t nx = 0, ny = 0;
    asb_input_virtual_normalize(x, y, vx, vy, vw, vh, &nx, &ny);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)nx;
    in.mi.dy = (LONG)ny;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
    POINT now;
    if (GetPhysicalCursorPos(&now) && (now.x != x || now.y != y) && x >= vx && x < vx + vw && y >= vy && y < vy + vh)
        SetPhysicalCursorPos(x, y);
}

void inject_button(AsbRwFiles *f, uint32_t button, int down)
{
    if (button == INPUT_BTN_LEFT) InterlockedExchange(&f->pressed, down ? 1 : 0);
    if (f->cfg.inject_button) { f->cfg.inject_button(f->cfg.ctx, button, down); return; }
    uint32_t physical = button;
    if (GetSystemMetrics(SM_SWAPBUTTON) && button != INPUT_BTN_MIDDLE)
        physical = button == INPUT_BTN_LEFT ? INPUT_BTN_RIGHT : INPUT_BTN_LEFT;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = physical == INPUT_BTN_LEFT ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                  : physical == INPUT_BTN_RIGHT ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                                                : (down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
    SendInput(1, &in, sizeof(in));
}

bool button_down(AsbRwFiles *f)
{
    if (f->cfg.inject_button) return f->pressed != 0;   /* tests: no real pointer */
    return asb_rw_primary_down() != 0;
}

bool map_point(AsbRwFiles *f, uint64_t surface, int32_t x, int32_t y, int32_t *gx, int32_t *gy)
{
    bool found = false;
    EnterCriticalSection(&f->lock);
    for (const AsbRwFilesSurface &sf : f->surfaces) {
        if (sf.surface_id != surface) continue;
        *gx = sf.rect.left + x;
        *gy = sf.rect.top + y;
        found = true;
        break;
    }
    LeaveCriticalSection(&f->lock);
    return found;
}

POINT catcher_center(AsbRwFiles *f)
{
    RECT r;
    GetWindowRect(f->catcher, &r);
    POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    return c;
}

void place_catcher(AsbRwFiles *f)
{
    RECT area;
    EnterCriticalSection(&f->lock);
    area = f->catcher_area;
    LeaveCriticalSection(&f->lock);
    if (area.right - area.left < kCatcherSize * 2 || area.bottom - area.top < kCatcherSize * 2) {
        MONITORINFO mi = { sizeof(mi) };
        POINT origin = {};
        GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
        area = mi.rcWork;
    }
    SetWindowPos(f->catcher, HWND_TOPMOST, area.right - kCatcherSize - 16, area.bottom - kCatcherSize - 16, kCatcherSize,
                 kCatcherSize, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void hide_catcher(AsbRwFiles *f)
{
    ShowWindow(f->catcher, SW_HIDE);
}

/* Pumps the OLE thread's messages (COM calls into the catcher arrive this way) until done(). */
template <typename Done>
void pump_until(DWORD ms, Done done)
{
    ULONGLONG until = GetTickCount64() + ms;
    while (!done()) {
        ULONGLONG now = GetTickCount64();
        if (now >= until) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(until - now < 20 ? until - now : 20), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void post_result(AsbRwFiles *f, const OleResult &r)
{
    EnterCriticalSection(&f->lock);
    f->ole_out.push_back(r);
    LeaveCriticalSection(&f->lock);
}

/* ================================ OLE thread ============================== */

HRESULT source_decide(void *ctx, BOOL escape, DWORD keys)
{
    AsbRwFiles *f = (AsbRwFiles *)ctx;
    (void)keys;
    if (escape || f->cancel) return DRAGDROP_S_CANCEL;
    if (!button_down(f)) return f->release ? DRAGDROP_S_DROP : DRAGDROP_S_CANCEL;
    return S_OK;
}

HRESULT source_feedback(void *ctx, DWORD effect)
{
    AsbRwFiles *f = (AsbRwFiles *)ctx;
    InterlockedExchange(&f->effect, (effect & DROPEFFECT_COPY) ? 1 : 0);
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

DWORD catcher_enter(void *ctx, IDataObject *data, POINTL, DWORD)
{
    AsbRwFiles *f = (AsbRwFiles *)ctx;
    if (f->self_drag) return DROPEFFECT_NONE;     /* our own host -> guest drag starts here */
    if (f->probe_waiting && !f->probe_outcome) {
        wchar_t **paths = nullptr;
        uint32_t n = 0;
        uint32_t r = asb_rw_hdrop_paths(data, &paths, &n);
        AsbRwSource *src = nullptr;
        if (!r) {
            src = new (std::nothrow) AsbRwSource;
            if (!src) r = ASB_RW_ERR_LIMIT;
            else {
                asb_rw_source_init(src);
                r = asb_rw_source_top(src, paths, n);
                if (r) flog(f, "files: guest drag rejected (%s): %ls", asb_rw_error_name(r), src->detail);
            }
        } else {
            flog(f, "files: guest drag carries no files (%s)", asb_rw_error_name(r));
        }
        asb_rw_free_paths(paths, n);
        if (r) {
            if (src) { asb_rw_source_free(src); delete src; }
            f->probe_outcome = ASB_RW_DRAG_SINK;
            f->probe_error = r;
            return DROPEFFECT_NONE;
        }
        f->probe_source = src;
        f->probe_outcome = ASB_RW_DRAG_ACTIVE;
        return DROPEFFECT_COPY;
    }
    return f->from_guest_active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

DWORD catcher_over(void *ctx, POINTL, DWORD)
{
    AsbRwFiles *f = (AsbRwFiles *)ctx;
    return !f->self_drag && f->from_guest_active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

void catcher_leave(void *) {}

DWORD catcher_drop(void *ctx, IDataObject *, POINTL, DWORD)
{
    AsbRwFiles *f = (AsbRwFiles *)ctx;
    if (!f->self_drag) flog(f, "files: guest drag ended on the catcher with DROPEFFECT_NONE (source kept)");
    return DROPEFFECT_NONE;   /* copy only: the guest source never deletes */
}

void ole_start(AsbRwFiles *f, Cmd *c)
{
    OleResult r;
    r.kind = RES_DRAG_DONE;
    r.session = c->session;
    r.hr = E_ABORT;
    if (f->cancel) { post_result(f, r); return; }
    std::vector<const wchar_t *> ptrs;
    for (const std::wstring &p : c->paths) ptrs.push_back(p.c_str());
    IDataObject *data = nullptr;
    HRESULT hr = asb_rw_shell_data_object(ptrs.data(), (uint32_t)ptrs.size(), &data);
    if (FAILED(hr)) {
        flog(f, "files: shell data object failed (0x%08lX)", (unsigned long)hr);
        r.error = ASB_RW_ERR_FILE_IO;
        post_result(f, r);
        return;
    }
    place_catcher(f);
    POINT ctr = catcher_center(f);
    InterlockedExchange(&f->release, 0);
    inject_move(f, ctr.x, ctr.y);
    inject_button(f, INPUT_BTN_LEFT, 1);
    pump_until(500, [&] { return button_down(f); });
    if (!button_down(f)) {
        inject_button(f, INPUT_BTN_LEFT, 0);
        data->Release();
        flog(f, "files: guest pointer press was not seen");
        r.error = ASB_RW_ERR_INTERNAL;
        post_result(f, r);
        return;
    }
    AsbRwDropSourceOps ops = { f, source_decide, source_feedback };
    IDropSource *src = asb_rw_drop_source_create(&ops);
    DWORD effect = DROPEFFECT_NONE;
    InterlockedExchange(&f->self_drag, 1);
    InterlockedExchange(&f->effect, 0);
    InterlockedExchange(&f->drag_running, 1);
    if (!src) hr = E_OUTOFMEMORY;
    else if (f->cfg.run_drag) hr = (HRESULT)f->cfg.run_drag(f->cfg.ctx, data, src, &effect);
    else hr = DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
    InterlockedExchange(&f->drag_running, 0);
    InterlockedExchange(&f->self_drag, 0);
    if (src) src->Release();
    data->Release();
    if (hr != DRAGDROP_S_DROP) {
        /* Cancelled or failed: the button goes up over our own catcher, never over an app. */
        POINT c2 = catcher_center(f);
        inject_move(f, c2.x, c2.y);
        if (button_down(f)) inject_button(f, INPUT_BTN_LEFT, 0);
    }
    hide_catcher(f);
    flog(f, "files: guest DoDragDrop hr=0x%08lX effect=%lu", (unsigned long)hr, effect);
    r.hr = hr;
    r.effect = effect;
    post_result(f, r);
}

void ole_probe(AsbRwFiles *f, Cmd *c)
{
    OleResult r;
    r.kind = RES_PROBE_DONE;
    r.probe = c->probe;
    if (f->probe_source) { asb_rw_source_free(f->probe_source); delete f->probe_source; f->probe_source = nullptr; }
    f->probe_outcome = 0;
    f->probe_error = 0;
    place_catcher(f);
    POINT ctr = catcher_center(f);
    InterlockedExchange(&f->probe_waiting, 1);
    if (f->cfg.probe) {
        f->cfg.probe(f->cfg.ctx, f->catcher_target);
    } else {
        inject_move(f, ctr.x - 8, ctr.y - 8);
        pump_until(30, [] { return false; });
        inject_move(f, ctr.x, ctr.y);
    }
    pump_until(f->cfg.probe_ms ? f->cfg.probe_ms : 1500, [&] { return f->probe_outcome != 0; });
    InterlockedExchange(&f->probe_waiting, 0);
    r.outcome = f->probe_outcome ? f->probe_outcome : ASB_RW_DRAG_NONE;
    r.error = f->probe_error;
    if (r.outcome == ASB_RW_DRAG_ACTIVE) {
        InterlockedExchange(&f->from_guest_active, 1);
        r.source = f->probe_source;
        f->probe_source = nullptr;
    }
    if (r.outcome == ASB_RW_DRAG_NONE) hide_catcher(f);
    post_result(f, r);
}

void ole_release(AsbRwFiles *f, Cmd *c)
{
    /* The guest pointer (pressed by the application, or ours) goes up over the catcher. */
    if (IsWindowVisible(f->catcher)) {
        POINT ctr = catcher_center(f);
        inject_move(f, ctr.x, ctr.y);
    }
    inject_button(f, c->button, 0);
    pump_until(300, [] { return false; });   /* the guest source's Drop reaches the catcher */
    InterlockedExchange(&f->from_guest_active, 0);
    hide_catcher(f);
    OleResult r;
    r.kind = RES_RELEASED;
    r.session = c->session;
    r.probe = c->probe;
    post_result(f, r);
}

LRESULT CALLBACK ole_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwFiles *f = (AsbRwFiles *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_FILES_START: case WM_FILES_PROBE: case WM_FILES_RELEASE: {
        Cmd *c = (Cmd *)lp;
        if (f && c) {
            if (msg == WM_FILES_START) ole_start(f, c);
            else if (msg == WM_FILES_PROBE) ole_probe(f, c);
            else ole_release(f, c);
        }
        delete c;
        return 0;
    }
    }
    (void)wp;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK catcher_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTCLIENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI ole_thread(LPVOID arg)
{
    AsbRwFiles *f = (AsbRwFiles *)arg;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT init = OleInitialize(nullptr);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ole_wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kOleClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = catcher_wndproc;
    wc.lpszClassName = kCatcherClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    f->ole_hwnd = CreateWindowExW(0, kOleClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    f->catcher = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kCatcherClass,
                                 L"App Sandbox drag", WS_POPUP, 0, 0, kCatcherSize, kCatcherSize, nullptr, nullptr, inst, nullptr);
    if (f->ole_hwnd) SetWindowLongPtrW(f->ole_hwnd, GWLP_USERDATA, (LONG_PTR)f);
    if (f->catcher) {
        SetLayeredWindowAttributes(f->catcher, 0, 1, LWA_ALPHA);   /* invisible but hit-testable */
        AsbRwDropTargetOps ops = { f, catcher_enter, catcher_over, catcher_leave, catcher_drop };
        f->catcher_target = asb_rw_drop_target_create(&ops);
        if (!f->catcher_target || FAILED(RegisterDragDrop(f->catcher, f->catcher_target))) flog(f, "files: catcher registration failed");
    }
    SetEvent(f->ole_ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (f->catcher) {
        RevokeDragDrop(f->catcher);
        DestroyWindow(f->catcher);
    }
    if (f->catcher_target) f->catcher_target->Release();
    if (f->probe_source) { asb_rw_source_free(f->probe_source); delete f->probe_source; f->probe_source = nullptr; }
    if (f->ole_hwnd) DestroyWindow(f->ole_hwnd);
    if (SUCCEEDED(init)) OleUninitialize();
    return 0;
}

void post_cmd(AsbRwFiles *f, UINT msg, Cmd *c)
{
    if (!f->ole_hwnd || !PostMessageW(f->ole_hwnd, msg, 0, (LPARAM)c)) delete c;
}

/* ================================ I/O thread ============================== */

int send_pair(void *ctx, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    AsbRwFiles *f = (AsbRwFiles *)ctx;
    int ok = 1;
    EnterCriticalSection(&f->send_lock);
    SOCKET s = f->sock;
    const uint8_t *parts[2] = { a, b };
    size_t lens[2] = { alen, blen };
    for (int i = 0; i < 2 && ok; ++i) {
        const uint8_t *p = parts[i];
        size_t n = lens[i];
        while (n && ok) {
            int k = s == INVALID_SOCKET ? -1 : send(s, (const char *)p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
            if (k <= 0) ok = 0;
            else { p += k; n -= (size_t)k; }
        }
    }
    LeaveCriticalSection(&f->send_lock);
    return ok;
}

int sender_cancelled(void *ctx) { return ((AsbRwFiles *)ctx)->sender_cancel != 0; }

AsbRwFileSendOps send_ops(AsbRwFiles *f)
{
    AsbRwFileSendOps ops = {};
    ops.ctx = f;
    ops.send = send_pair;
    ops.cancelled = sender_cancelled;
    return ops;
}

void send_payload(AsbRwFiles *f, uint16_t type, const AsbRwPayload *m)
{
    uint8_t buf[ASB_RW_HEADER_BYTES + 256];
    size_t n = 0;
    if (!asb_rw_encode(type, 0, m, buf, sizeof(buf), &n)) send_pair(f, buf, n, nullptr, 0);
}

void send_status(AsbRwFiles *f, uint16_t type, uint64_t session, uint32_t status, uint64_t done, uint64_t total)
{
    AsbRwFileSendOps ops = send_ops(f);
    asb_rw_files_send_status(&ops, type, session, status, done, total);
}

DWORD WINAPI sender_thread(LPVOID arg)
{
    AsbRwFiles *f = (AsbRwFiles *)arg;
    AsbRwSource *src = f->s.source;
    uint64_t session = f->s.id;
    AsbRwFileSendOps ops = send_ops(f);
    uint32_t r = asb_rw_source_expand(src, &f->sender_cancel);
    if (!r) r = asb_rw_source_send_stream(src, session, f->chunk, &ops);
    if (r && r != ASB_RW_ERR_CANCELLED) {
        flog(f, "files: guest -> host stream failed (%s): %ls", asb_rw_error_name(r), src->detail);
        asb_rw_files_send_status(&ops, ASB_RW_FILE_CANCEL, session, r, 0, 0);
    } else if (!r) {
        flog(f, "files: guest -> host stream complete (%u entries, %llu bytes)", src->count, (unsigned long long)src->total_bytes);
    }
    f->sender_result = r;
    InterlockedExchange(&f->sender_done, 1);
    return 0;
}

void stop_sender(AsbRwFiles *f)
{
    Session &s = f->s;
    if (s.sender) {
        InterlockedExchange(&f->sender_cancel, 1);
        WaitForSingleObject(s.sender, INFINITE);
        CloseHandle(s.sender);
        s.sender = nullptr;
    }
    if (s.source) { asb_rw_source_free(s.source); delete s.source; s.source = nullptr; }
}

void stat_end(AsbRwFiles *f, uint32_t error)
{
    if (f->s.counted) return;
    f->s.counted = true;
    EnterCriticalSection(&f->lock);
    f->stats.last_error = error;
    if (error == ASB_RW_ERR_CANCELLED || error == ASB_RW_ERR_DISCONNECTED) f->stats.cancelled++;
    else if (error == ASB_RW_ERR_FILE_REJECTED || error == ASB_RW_ERR_UNSUPPORTED_DATA) f->stats.rejected++;
    else if (error) f->stats.failed++;
    LeaveCriticalSection(&f->lock);
}

void session_reset(AsbRwFiles *f)
{
    Session &s = f->s;
    if (s.stage) { asb_rw_stage_close(s.stage, 0); s.stage = nullptr; }
    stop_sender(f);
    s = Session();
    InterlockedExchange(&f->cancel, 0);
    InterlockedExchange(&f->release, 0);
    InterlockedExchange(&f->sender_done, 0);
    f->sender_result = 0;
}

/* Ends the current session. notify: tell the host (FILE_CANCEL with the error). */
void fail_session(AsbRwFiles *f, uint32_t error, bool notify)
{
    Session &s = f->s;
    if (s.phase == ASB_RW_FILES_IDLE || s.phase == ASB_RW_FILES_ENDING) return;
    if (!s.error) s.error = error;
    if (notify && s.id && f->sock != INVALID_SOCKET) send_status(f, ASB_RW_FILE_CANCEL, s.id, error, 0, 0);
    if (!notify) s.host_cancelled = true;
    flog(f, "files: session %016llx ends (%s)", (unsigned long long)s.id, asb_rw_error_name(error));
    switch (s.phase) {
    case ASB_RW_FILES_RECEIVING: case ASB_RW_FILES_WAIT_ACK:
        if (s.state_sent) drag_state(f, ASB_RW_DRAG_ENDED, error);
        stat_end(f, error);
        session_reset(f);
        break;
    case ASB_RW_FILES_HOVER: case ASB_RW_FILES_DROPPING:
        InterlockedExchange(&f->cancel, 1);   /* DoDragDrop returns; RES_DRAG_DONE cleans up */
        s.phase = ASB_RW_FILES_ENDING;
        break;
    case ASB_RW_FILES_SENDING: case ASB_RW_FILES_SINK: {
        InterlockedExchange(&f->sender_cancel, 1);
        Cmd *c = new (std::nothrow) Cmd;
        if (c) { c->session = s.id; c->probe = s.probe; c->button = s.button; }
        s.phase = ASB_RW_FILES_ENDING;
        post_cmd(f, WM_FILES_RELEASE, c);
        break;
    }
    case ASB_RW_FILES_PROBING:
        s.release_after_probe = true;   /* resolved when the probe returns */
        break;
    }
}

void disconnect(AsbRwFiles *f, const char *why)
{
    if (f->sock == INVALID_SOCKET) return;
    flog(f, "files: host broker disconnected (%s)", why);
    fail_session(f, ASB_RW_ERR_DISCONNECTED, false);
    EnterCriticalSection(&f->send_lock);
    SOCKET s = f->sock;
    f->sock = INVALID_SOCKET;
    LeaveCriticalSection(&f->send_lock);
    shutdown(s, SD_BOTH);
    if (f->cfg.close) f->cfg.close(f->cfg.ctx, s);
    else closesocket(s);
    EnterCriticalSection(&f->lock);
    f->stats.connected = 0;
    LeaveCriticalSection(&f->lock);
}

bool recv_exact(SOCKET s, uint8_t *p, size_t n)
{
    while (n) {
        int k = recv(s, (char *)p, n > 65536 ? 65536 : (int)n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* One complete message into f->rx; ASB_RW_OK or an error (drop the connection). */
uint32_t read_message(AsbRwFiles *f, AsbRwHeader *h, AsbRwPayload *m)
{
    size_t total = 0;
    if (!recv_exact(f->sock, f->rx.data(), ASB_RW_HEADER_BYTES)) return ASB_RW_ERR_DISCONNECTED;
    uint32_t r = asb_rw_frame_check(ASB_CH_RW_FILES, f->rx.data(), ASB_RW_HEADER_BYTES, &total);
    if (r) return r;
    if (!recv_exact(f->sock, f->rx.data() + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES)) return ASB_RW_ERR_DISCONNECTED;
    return asb_rw_decode(ASB_CH_RW_FILES, f->rx.data(), total, h, m, nullptr, nullptr);
}

bool handshake(AsbRwFiles *f)
{
    DWORD t = 5000;
    setsockopt(f->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    setsockopt(f->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
    AsbRwPayload m = {};
    m.files_hello.role = ASB_RW_ROLE_GUEST;
    m.files_hello.version = ASB_RW_FILES_VERSION;
    m.files_hello.directions = f->cfg.directions;
    m.files_hello.max_chunk = ASB_RW_FILE_CHUNK_DEFAULT;
    m.files_hello.session_tag = (uint32_t)f->tag;
    send_payload(f, ASB_RW_FILES_HELLO, &m);
    AsbRwHeader h;
    AsbRwPayload in;
    if (read_message(f, &h, &in) || h.type != ASB_RW_FILES_HELLO || in.files_hello.role != ASB_RW_ROLE_HOST) return false;
    f->peer_directions = in.files_hello.directions & f->cfg.directions;
    f->chunk = in.files_hello.max_chunk < ASB_RW_FILE_CHUNK_DEFAULT ? in.files_hello.max_chunk : ASB_RW_FILE_CHUNK_DEFAULT;
    t = 15000;
    setsockopt(f->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    setsockopt(f->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
    flog(f, "files: host broker connected (directions %u, chunk %u)", f->peer_directions, f->chunk);
    EnterCriticalSection(&f->lock);
    f->stats.connected = 1;
    LeaveCriticalSection(&f->lock);
    return true;
}

void start_to_guest_drag(AsbRwFiles *f)
{
    Session &s = f->s;
    Cmd *c = new (std::nothrow) Cmd;
    if (!c) { fail_session(f, ASB_RW_ERR_LIMIT, true); return; }
    c->session = s.id;
    const wchar_t *const *paths = nullptr;
    uint32_t n = asb_rw_stage_top(s.stage, &paths);
    for (uint32_t i = 0; i < n; ++i) c->paths.push_back(paths[i]);
    s.phase = s.drop ? ASB_RW_FILES_DROPPING : ASB_RW_FILES_HOVER;
    s.running_since = 0;
    post_cmd(f, WM_FILES_START, c);
}

/* Returns false when the connection must be dropped. */
bool handle_message(AsbRwFiles *f, const AsbRwHeader *h, const AsbRwPayload *m)
{
    Session &s = f->s;
    switch (h->type) {
    case ASB_RW_FILE_BEGIN: {
        if (s.phase != ASB_RW_FILES_IDLE) {
            send_status(f, ASB_RW_FILE_CANCEL, m->file_begin.session_id, ASB_RW_ERR_BUSY, 0, 0);
            return true;
        }
        if (m->file_begin.direction != ASB_RW_DIR_TO_GUEST || !(f->peer_directions & ASB_RW_DIR_TO_GUEST) || !f->tag) {
            send_status(f, ASB_RW_FILE_CANCEL, m->file_begin.session_id, ASB_RW_ERR_UPGRADE_REQUIRED, 0, 0);
            return true;
        }
        s.id = m->file_begin.session_id;
        s.direction = ASB_RW_DIR_TO_GUEST;
        s.surface = m->file_begin.target_id;
        s.x = m->file_begin.x;
        s.y = m->file_begin.y;
        s.point_dirty = true;
        s.phase = ASB_RW_FILES_RECEIVING;
        s.stage = asb_rw_stage_open(f->stage_parent.c_str());
        EnterCriticalSection(&f->lock);
        f->stats.session_id = s.id;
        LeaveCriticalSection(&f->lock);
        uint32_t r = s.stage ? asb_rw_stage_message(s.stage, h->type, m) : ASB_RW_ERR_FILE_IO;
        if (r) fail_session(f, r, true);
        return true;
    }
    case ASB_RW_FILE_ENTRY: case ASB_RW_FILE_STREAM: case ASB_RW_FILE_DATA: case ASB_RW_FILE_END: {
        uint64_t id = h->type == ASB_RW_FILE_ENTRY ? m->file_entry.session_id : h->type == ASB_RW_FILE_STREAM ? m->file_stream.session_id
                    : h->type == ASB_RW_FILE_DATA ? m->file_data.session_id : m->file_status.session_id;
        if (!s.stage || id != s.id) return true;   /* a cancelled session's tail */
        uint32_t r = asb_rw_stage_message(s.stage, h->type, m);
        if (r) {
            flog(f, "files: staging rejected a message (%s)", asb_rw_error_name(r));
            fail_session(f, r, true);
            return true;
        }
        if (s.phase == ASB_RW_FILES_RECEIVING && asb_rw_stage_phase(s.stage) == ASB_RW_RX_READY) {
            s.phase = ASB_RW_FILES_WAIT_ACK;
            s.ack_deadline = GetTickCount64() + kAckMs;
            drag_state(f, ASB_RW_DRAG_ACTIVE, 0);   /* ask the host for the guest pointer */
        }
        if (asb_rw_stage_phase(s.stage) == ASB_RW_RX_COMPLETE) {
            s.staged = true;
            uint64_t done, total;
            asb_rw_stage_progress(s.stage, &done, &total);
            send_status(f, ASB_RW_FILE_PROGRESS, s.id, ASB_RW_OK, done, total);
            EnterCriticalSection(&f->lock);
            f->stats.staged_bytes = done;
            LeaveCriticalSection(&f->lock);
        }
        return true;
    }
    case ASB_RW_FILE_POINTER:
        if (m->file_pointer.session_id != s.id || s.direction != ASB_RW_DIR_TO_GUEST) return true;
        s.surface = m->file_pointer.surface_id;
        s.x = m->file_pointer.x;
        s.y = m->file_pointer.y;
        s.point_dirty = true;
        if (m->file_pointer.action == ASB_RW_POINTER_DROP && (s.phase == ASB_RW_FILES_HOVER || s.phase == ASB_RW_FILES_WAIT_ACK)) {
            s.drop = true;
            if (s.phase == ASB_RW_FILES_HOVER) s.phase = ASB_RW_FILES_DROPPING;
        }
        return true;
    case ASB_RW_FILE_CANCEL:
        if (m->file_status.session_id == s.id) fail_session(f, m->file_status.status ? m->file_status.status : ASB_RW_ERR_CANCELLED, false);
        return true;
    case ASB_RW_FILE_RESULT:
        /* The host drag (guest -> host) ended: release the guest pointer on the catcher. */
        if (m->file_status.session_id == s.id && s.direction == ASB_RW_DIR_FROM_GUEST) {
            uint32_t st = m->file_status.status;
            flog(f, "files: host drop finished (%s)", asb_rw_error_name(st));
            if (!st) {
                EnterCriticalSection(&f->lock);
                f->stats.from_guest_ok++;
                LeaveCriticalSection(&f->lock);
                s.counted = true;
            } else {
                stat_end(f, st);
            }
            fail_session(f, st, false);
        }
        return true;
    case ASB_RW_FILE_PROGRESS:
        return true;
    case ASB_RW_FILES_HELLO: case ASB_RW_FILE_FEEDBACK:
        return false;
    case ASB_RW_ERROR:
        flog(f, "files: host error %s", asb_rw_error_name(m->error.code));
        return true;
    }
    return false;
}

void handle_control(AsbRwFiles *f, uint16_t type, const AsbRwDrag &d)
{
    Session &s = f->s;
    if (type == ASB_RW_DRAG_PROBE) {
        if (s.phase != ASB_RW_FILES_IDLE || f->sock == INVALID_SOCKET || !(f->peer_directions & ASB_RW_DIR_FROM_GUEST)) {
            uint32_t why = s.phase != ASB_RW_FILES_IDLE ? ASB_RW_ERR_BUSY : ASB_RW_ERR_UPGRADE_REQUIRED;
            uint32_t keep = s.probe;
            s.probe = d.probe;
            drag_state(f, ASB_RW_DRAG_NONE, why);
            s.probe = keep;
            return;
        }
        s.phase = ASB_RW_FILES_PROBING;
        s.probe = d.probe;
        s.button = d.button;
        s.direction = ASB_RW_DIR_FROM_GUEST;
        Cmd *c = new (std::nothrow) Cmd;
        if (!c) { drag_state(f, ASB_RW_DRAG_NONE, ASB_RW_ERR_LIMIT); s = Session(); return; }
        c->probe = d.probe;
        c->button = d.button;
        post_cmd(f, WM_FILES_PROBE, c);
        return;
    }
    if (type != ASB_RW_DRAG_ACK) return;
    switch (d.state) {
    case ASB_RW_DRAG_ALLOW:
        if (s.phase == ASB_RW_FILES_WAIT_ACK && d.session_id == s.id) start_to_guest_drag(f);
        break;
    case ASB_RW_DRAG_CANCEL:
        if ((d.session_id && d.session_id == s.id) || (!d.session_id && d.probe && d.probe == s.probe))
            fail_session(f, d.error ? d.error : ASB_RW_ERR_CANCELLED, true);
        break;
    case ASB_RW_DRAG_RELEASE:
        if (d.probe && d.probe == s.probe && (s.phase == ASB_RW_FILES_SINK || s.phase == ASB_RW_FILES_PROBING))
            fail_session(f, ASB_RW_ERR_CANCELLED, false);
        break;
    }
}

void handle_result(AsbRwFiles *f, const OleResult &r)
{
    Session &s = f->s;
    switch (r.kind) {
    case RES_DRAG_DONE: {
        if (r.session != s.id) return;
        uint32_t result;
        bool dropped = r.hr == DRAGDROP_S_DROP;
        if (dropped) result = (r.effect & DROPEFFECT_COPY) ? ASB_RW_OK : ASB_RW_ERR_FILE_REJECTED;
        else result = s.error ? s.error : r.error ? r.error : ASB_RW_ERR_CANCELLED;
        uint64_t done = 0, total = 0;
        if (s.stage) asb_rw_stage_progress(s.stage, &done, &total);
        if (!s.host_cancelled && f->sock != INVALID_SOCKET) send_status(f, ASB_RW_FILE_RESULT, s.id, result, done, total);
        flog(f, "files: host -> guest drop %s (effect %lu)", asb_rw_error_name(result), r.effect);
        if (s.stage) { asb_rw_stage_close(s.stage, result == ASB_RW_OK); s.stage = nullptr; }
        drag_state(f, ASB_RW_DRAG_ENDED, result);
        EnterCriticalSection(&f->lock);
        f->stats.last_effect = r.effect;
        if (!result) f->stats.to_guest_ok++;
        LeaveCriticalSection(&f->lock);
        if (result) stat_end(f, result);
        session_reset(f);
        return;
    }
    case RES_PROBE_DONE: {
        if (r.probe != s.probe || s.phase != ASB_RW_FILES_PROBING) {
            if (r.source) { asb_rw_source_free(r.source); delete r.source; }
            return;
        }
        if (s.release_after_probe || (r.outcome == ASB_RW_DRAG_ACTIVE && f->sock == INVALID_SOCKET)) {
            /* The host button went up (or the broker left) while probing: release on the catcher. */
            if (r.source) { asb_rw_source_free(r.source); delete r.source; }
            Cmd *c = new (std::nothrow) Cmd;
            if (c) { c->probe = s.probe; c->button = s.button; }
            s.phase = ASB_RW_FILES_ENDING;
            post_cmd(f, WM_FILES_RELEASE, c);
            return;
        }
        if (r.outcome == ASB_RW_DRAG_NONE) {
            drag_state(f, ASB_RW_DRAG_NONE, 0);
            EnterCriticalSection(&f->lock);
            f->stats.probes_none++;
            LeaveCriticalSection(&f->lock);
            s = Session();
            return;
        }
        if (r.outcome == ASB_RW_DRAG_SINK) {
            s.phase = ASB_RW_FILES_SINK;
            s.error = r.error;
            drag_state(f, ASB_RW_DRAG_SINK, r.error);
            EnterCriticalSection(&f->lock);
            f->stats.probes_sink++;
            LeaveCriticalSection(&f->lock);
            return;
        }
        s.id = asb_rw_files_random64();
        s.source = r.source;
        s.phase = ASB_RW_FILES_SENDING;
        EnterCriticalSection(&f->lock);
        f->stats.session_id = s.id;
        LeaveCriticalSection(&f->lock);
        drag_state(f, ASB_RW_DRAG_ACTIVE, 0);
        AsbRwFileSendOps ops = send_ops(f);
        InterlockedExchange(&f->sender_cancel, 0);
        InterlockedExchange(&f->sender_done, 0);
        uint32_t e = asb_rw_source_send_top(s.source, s.id, ASB_RW_DIR_FROM_GUEST, 0, 0, 0, &ops);
        if (!e) {
            s.sender = CreateThread(nullptr, 0, sender_thread, f, 0, nullptr);
            if (!s.sender) e = ASB_RW_ERR_INTERNAL;
        }
        flog(f, "files: guest drag offered to the host (%u item(s))", s.source->top_count);
        if (e) fail_session(f, e, true);
        return;
    }
    case RES_RELEASED:
        if (s.phase != ASB_RW_FILES_ENDING) return;
        drag_state(f, ASB_RW_DRAG_ENDED, s.error);
        if (s.id && s.error) stat_end(f, s.error);
        session_reset(f);
        return;
    }
}

void housekeeping(AsbRwFiles *f)
{
    Session &s = f->s;
    ULONGLONG now = GetTickCount64();
    if (s.phase == ASB_RW_FILES_WAIT_ACK && now > s.ack_deadline) fail_session(f, ASB_RW_ERR_BUSY, true);
    if ((s.phase == ASB_RW_FILES_HOVER || s.phase == ASB_RW_FILES_DROPPING) && f->drag_running) {
        if (!s.running_since) s.running_since = now;
        if (s.point_dirty && now - s.running_since >= kFirstMoveMs && !s.release_sent) {
            int32_t gx, gy;
            if (map_point(f, s.surface, s.x, s.y, &gx, &gy)) inject_move(f, gx, gy);
            s.point_dirty = false;
        }
        uint32_t eff = f->effect ? ASB_RW_EFFECT_COPY : ASB_RW_EFFECT_NONE;
        if (eff != s.sent_effect && now - s.running_since >= kFirstMoveMs + 30) {
            AsbRwPayload m = {};
            m.file_feedback.session_id = s.id;
            m.file_feedback.effect = eff;
            m.file_feedback.status = eff ? ASB_RW_OK : ASB_RW_ERR_FILE_REJECTED;
            send_payload(f, ASB_RW_FILE_FEEDBACK, &m);
            s.sent_effect = eff;
        }
    }
    if (s.phase == ASB_RW_FILES_DROPPING && s.staged && f->drag_running && !s.release_sent && !s.point_dirty) {
        /* Staging verified: deliver the genuine drop at the host's drop point. */
        int32_t gx, gy;
        InterlockedExchange(&f->release, 1);
        if (map_point(f, s.surface, s.x, s.y, &gx, &gy)) inject_move(f, gx, gy);
        inject_button(f, INPUT_BTN_LEFT, 0);
        s.release_sent = true;
    }
    if (s.phase == ASB_RW_FILES_DROPPING && s.stage && now - s.last_progress >= kProgressMs && !s.staged) {
        uint64_t done, total;
        asb_rw_stage_progress(s.stage, &done, &total);
        if (total) send_status(f, ASB_RW_FILE_PROGRESS, s.id, ASB_RW_OK, done, total);
        s.last_progress = now;
    }
    if (s.phase == ASB_RW_FILES_SENDING && f->sender_done && f->sender_result && f->sender_result != ASB_RW_ERR_CANCELLED) {
        uint32_t e = f->sender_result;
        f->sender_result = 0;
        stat_end(f, e);
        fail_session(f, e, false);   /* the sender already sent FILE_CANCEL */
    }
    EnterCriticalSection(&f->lock);
    f->stats.phase = s.phase;
    LeaveCriticalSection(&f->lock);
}

void drain_mailboxes(AsbRwFiles *f)
{
    std::vector<std::pair<uint16_t, AsbRwDrag>> control;
    std::vector<OleResult> results;
    EnterCriticalSection(&f->lock);
    control.swap(f->control_in);
    results.swap(f->ole_out);
    LeaveCriticalSection(&f->lock);
    for (const OleResult &r : results) handle_result(f, r);
    for (const auto &c : control) handle_control(f, c.first, c.second);
}

DWORD WINAPI io_thread(LPVOID arg)
{
    AsbRwFiles *f = (AsbRwFiles *)arg;
    f->rx.resize(ASB_RW_HEADER_BYTES + ASB_RW_MAX_FILES_PAYLOAD);
    AsbRwPayload *msg = new AsbRwPayload;
    while (!f->stopping) {
        if (f->sock == INVALID_SOCKET) {
            if (f->tag && f->cfg.accept) {
                SOCKET s = f->cfg.accept(f->cfg.ctx, 100);
                if (s != INVALID_SOCKET) {
                    f->sock = s;
                    if (!handshake(f)) disconnect(f, "handshake failed");
                }
            } else {
                WaitForSingleObject(f->stop_event, 50);
            }
        }
        if (f->sock != INVALID_SOCKET && !f->tag) disconnect(f, "control session ended");
        drain_mailboxes(f);
        if (f->sock != INVALID_SOCKET) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(f->sock, &rd);
            timeval tv = { 0, 20000 };
            int ready = select(0, &rd, nullptr, nullptr, &tv);
            if (ready < 0) disconnect(f, "select failed");
            else if (ready > 0) {
                AsbRwHeader h;
                uint32_t r = read_message(f, &h, msg);
                if (r) disconnect(f, asb_rw_error_name(r));
                else if (!handle_message(f, &h, msg)) disconnect(f, "unexpected message");
            }
        } else if (f->s.phase != ASB_RW_FILES_IDLE) {
            fail_session(f, ASB_RW_ERR_DISCONNECTED, false);
        }
        housekeeping(f);
    }
    disconnect(f, "stopping");
    fail_session(f, ASB_RW_ERR_DISCONNECTED, false);
    /* Let the OLE thread finish its drag/release so the guest pointer is left released. */
    ULONGLONG until = GetTickCount64() + 3000;
    while (f->s.phase != ASB_RW_FILES_IDLE && GetTickCount64() < until) {
        Sleep(20);
        drain_mailboxes(f);
    }
    session_reset(f);
    delete msg;
    return 0;
}

} // namespace

/* ================================ public API ============================== */

extern "C" AsbRwFiles *asb_rw_files_start(const AsbRwFilesConfig *config)
{
    AsbRwFiles *f = new (std::nothrow) AsbRwFiles();
    if (!f) return nullptr;
    f->cfg = *config;
    if (config->stage_parent) f->stage_parent = config->stage_parent;
    f->cfg.stage_parent = nullptr;
    if (!f->cfg.directions) f->cfg.directions = ASB_RW_DIR_TO_GUEST | ASB_RW_DIR_FROM_GUEST;
    InitializeCriticalSection(&f->lock);
    InitializeCriticalSection(&f->send_lock);
    f->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f->ole_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    asb_rw_stage_collect(f->stage_parent.c_str());
    f->ole_thread = CreateThread(nullptr, 0, ole_thread, f, 0, nullptr);
    if (f->ole_thread) WaitForSingleObject(f->ole_ready, 10000);
    if (!f->ole_hwnd || !f->catcher) {
        if (f->ole_thread) {
            if (f->ole_hwnd) PostMessageW(f->ole_hwnd, WM_QUIT, 0, 0);
            else PostThreadMessageW(GetThreadId(f->ole_thread), WM_QUIT, 0, 0);
            WaitForSingleObject(f->ole_thread, 5000);
            CloseHandle(f->ole_thread);
        }
        CloseHandle(f->stop_event);
        CloseHandle(f->ole_ready);
        DeleteCriticalSection(&f->lock);
        DeleteCriticalSection(&f->send_lock);
        delete f;
        return nullptr;
    }
    f->io_thread = CreateThread(nullptr, 0, io_thread, f, 0, nullptr);
    return f;
}

extern "C" void asb_rw_files_stop(AsbRwFiles *f)
{
    if (!f) return;
    InterlockedExchange(&f->stopping, 1);
    SetEvent(f->stop_event);
    if (f->io_thread) { WaitForSingleObject(f->io_thread, INFINITE); CloseHandle(f->io_thread); }
    PostThreadMessageW(GetThreadId(f->ole_thread), WM_QUIT, 0, 0);
    WaitForSingleObject(f->ole_thread, INFINITE);
    CloseHandle(f->ole_thread);
    CloseHandle(f->stop_event);
    CloseHandle(f->ole_ready);
    DeleteCriticalSection(&f->lock);
    DeleteCriticalSection(&f->send_lock);
    delete f;
}

extern "C" void asb_rw_files_session(AsbRwFiles *f, uint32_t control_tag)
{
    if (f) InterlockedExchange(&f->tag, (LONG)control_tag);
}

extern "C" void asb_rw_files_surfaces(AsbRwFiles *f, const AsbRwFilesSurface *surfaces, uint32_t n, const RECT *catcher_area)
{
    if (!f) return;
    EnterCriticalSection(&f->lock);
    f->surfaces.assign(surfaces, surfaces + n);
    if (catcher_area) f->catcher_area = *catcher_area;
    LeaveCriticalSection(&f->lock);
}

extern "C" void asb_rw_files_control(AsbRwFiles *f, uint16_t type, const AsbRwDrag *msg)
{
    if (!f) return;
    EnterCriticalSection(&f->lock);
    if (f->control_in.size() < 64) f->control_in.push_back(std::make_pair(type, *msg));
    LeaveCriticalSection(&f->lock);
}

extern "C" void asb_rw_files_stats(AsbRwFiles *f, AsbRwFilesStats *out)
{
    EnterCriticalSection(&f->lock);
    *out = f->stats;
    LeaveCriticalSection(&f->lock);
}
