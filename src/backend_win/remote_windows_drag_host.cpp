/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_drag_host
 *
 * @file      remote_windows_drag_host.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows host drag broker (AppSandbox-eh6.9). See remote_windows_drag_host.h. */
#include <winsock2.h>
#include "remote_windows_drag_host.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include <string>
#include <vector>

#include "../../tools/agent/remote_windows_files_win32.h"
#include "../../src/core/protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#define HV_PROTOCOL_RAW 1
#define HVSOCKET_CONNECT_TIMEOUT 1

namespace {

struct HvSockAddr { ADDRESS_FAMILY family; USHORT reserved; GUID vm_id; GUID service_id; };

const wchar_t kMsgClass[] = L"AppSandbox.RemoteWindows.DragBroker";
const wchar_t kOverlayClass[] = L"AppSandbox.RemoteWindows.DragOverlay";
const wchar_t kProgressClass[] = L"AppSandbox.RemoteWindows.DragProgress";
const wchar_t kCookieProp[] = L"AsbRwDragCookie";
const wchar_t kSurfaceProp[] = L"AsbRwDragSurface";
const wchar_t kProxyClass[] = L"AppSandbox.RemoteWindows.Proxy";
const wchar_t kPopupClass[] = L"AppSandbox.RemoteWindows.Popup";

const UINT_PTR kPollTimer = 1, kHideTimer = 2;
const UINT kPollMs = 30;
const DWORD kPointerMs = 15;

enum : UINT {
    WM_DH_OUT_READY = WM_APP + 0x300,   /* from-guest top-level entries staged: start the host drag */
    WM_DH_UPDATE,                       /* progress / state changed */
    WM_DH_INVOKE,                       /* wParam fn, lParam arg */
};

enum { ID_CANCEL = 100 };

} // namespace

struct AsbRwDragHost {
    AsbRwDragHostConfig cfg = {};
    std::wstring stage_parent, guest_name;
    CRITICAL_SECTION lock, send_lock;
    HANDLE ui_thread = nullptr, conn_thread = nullptr, ready = nullptr, stop_event = nullptr;
    volatile LONG stopping = 0;
    HWND msg = nullptr, overlay = nullptr, progress = nullptr, progress_bar = nullptr, progress_text = nullptr;
    IDropTarget *target = nullptr;
    bool ui_ok = false;

    /* connection (sock written by the connection thread; closed under send_lock) */
    SOCKET sock = INVALID_SOCKET;
    uint32_t peer_directions = 0, chunk = ASB_RW_FILE_CHUNK_DEFAULT;

    /* session (under lock) */
    uint32_t phase = ASB_RW_DH_IDLE;
    uint64_t id = 0;
    /* to guest */
    AsbRwSource *src = nullptr;
    HANDLE expand = nullptr, sender = nullptr;
    volatile LONG src_cancel = 0, expand_done = 0;
    uint32_t expand_error = 0;
    uint64_t surface = 0;
    POINT origin = {};
    int32_t last_x = INT32_MIN, last_y = INT32_MIN;
    ULONGLONG last_pointer = 0;
    uint32_t guest_effect = 0, guest_status = 0;
    bool refused = false;               /* this hover cannot be dropped (rejected items, guest refusal) */
    /* from guest */
    AsbRwStage *stage = nullptr;
    bool staged = false, out_failed = false, waiting_drop = false, out_running = false;
    volatile LONG press_seen = 0;       /* the overlay received the re-pressed button (hand-off) */
    volatile LONG out_abort = 0;        /* watchdog: the drag loop stopped calling back */
    volatile LONGLONG last_callback = 0;
    DWORD ui_thread_id = 0;
    uint32_t out_error = 0;
    /* progress */
    uint64_t done = 0, total = 0;
    std::wstring status;
    bool status_final = false;

    HWND overlay_target = nullptr;      /* UI thread */
    AsbRwDragHostStats stats = {};
};

namespace {

/* ================================ logging ================================= */

void dlog(AsbRwDragHost *h, const char *fmt, ...)
{
    if (!h->cfg.log) return;
    char text[640];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    h->cfg.log(h->cfg.ctx, text);
}

void post_update(AsbRwDragHost *h)
{
    if (h->msg) PostMessageW(h->msg, WM_DH_UPDATE, 0, 0);
}

void set_status(AsbRwDragHost *h, const wchar_t *text, bool final)
{
    EnterCriticalSection(&h->lock);
    h->status = text;
    h->status_final = final;
    LeaveCriticalSection(&h->lock);
    post_update(h);
}

std::wstring error_text(uint32_t error)
{
    const char *t = asb_rw_error_text(error);
    return std::wstring(t, t + strlen(t));
}

/* ================================ sending ================================= */

int send_pair(void *ctx, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    int ok = 1;
    EnterCriticalSection(&h->send_lock);
    const uint8_t *parts[2] = { a, b };
    size_t lens[2] = { alen, blen };
    for (int i = 0; i < 2 && ok; ++i) {
        const uint8_t *p = parts[i];
        size_t n = lens[i];
        while (n && ok) {
            int k = h->sock == INVALID_SOCKET ? -1 : send(h->sock, (const char *)p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
            if (k <= 0) ok = 0;
            else { p += k; n -= (size_t)k; }
        }
    }
    LeaveCriticalSection(&h->send_lock);
    return ok;
}

int src_cancelled(void *ctx) { return ((AsbRwDragHost *)ctx)->src_cancel != 0; }

void src_progress(void *ctx, uint64_t done, uint64_t total)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    EnterCriticalSection(&h->lock);
    h->done = done;
    h->total = total;
    LeaveCriticalSection(&h->lock);
}

AsbRwFileSendOps ops_of(AsbRwDragHost *h)
{
    AsbRwFileSendOps ops = {};
    ops.ctx = h;
    ops.send = send_pair;
    ops.cancelled = src_cancelled;
    ops.progress = src_progress;
    return ops;
}

void send_payload(AsbRwDragHost *h, uint16_t type, const AsbRwPayload *m)
{
    uint8_t buf[ASB_RW_HEADER_BYTES + 128];
    size_t n = 0;
    if (!asb_rw_encode(type, 0, m, buf, sizeof(buf), &n)) send_pair(h, buf, n, nullptr, 0);
}

void send_status(AsbRwDragHost *h, uint16_t type, uint64_t id, uint32_t status)
{
    AsbRwFileSendOps ops = ops_of(h);
    asb_rw_files_send_status(&ops, type, id, status, 0, 0);
}

void send_pointer(AsbRwDragHost *h, uint32_t action)
{
    AsbRwPayload m = {};
    m.file_pointer.session_id = h->id;
    m.file_pointer.surface_id = h->surface;
    m.file_pointer.x = h->last_x;
    m.file_pointer.y = h->last_y;
    m.file_pointer.action = action;
    send_payload(h, ASB_RW_FILE_POINTER, &m);
    h->last_pointer = GetTickCount64();
}

void count_end(AsbRwDragHost *h, bool to_guest, uint32_t error)
{
    EnterCriticalSection(&h->lock);
    if (!error) (to_guest ? h->stats.to_guest_ok : h->stats.from_guest_ok)++;
    else if (error == ASB_RW_ERR_CANCELLED || error == ASB_RW_ERR_DISCONNECTED) h->stats.cancelled++;
    else if (error == ASB_RW_ERR_FILE_REJECTED || error == ASB_RW_ERR_UNSUPPORTED_DATA) h->stats.rejected++;
    else (to_guest ? h->stats.to_guest_failed : h->stats.from_guest_failed)++;
    if (error) h->stats.last_error = error;
    LeaveCriticalSection(&h->lock);
}

/* ================================ to guest ================================ */

DWORD WINAPI expand_thread(LPVOID arg)
{
    AsbRwDragHost *h = (AsbRwDragHost *)arg;
    uint32_t r = asb_rw_source_expand(h->src, &h->src_cancel);
    h->expand_error = r;
    if (r && r != ASB_RW_ERR_CANCELLED) dlog(h, "drag: dragged items rejected (%s): %ls", asb_rw_error_name(r), h->src->detail);
    InterlockedExchange(&h->expand_done, 1);
    return 0;
}

DWORD WINAPI sender_thread(LPVOID arg)
{
    AsbRwDragHost *h = (AsbRwDragHost *)arg;
    WaitForSingleObject(h->expand, INFINITE);
    uint64_t id = h->id;
    AsbRwFileSendOps ops = ops_of(h);
    uint32_t r = h->expand_error;
    if (!r) {
        EnterCriticalSection(&h->lock);
        h->total = h->src->total_bytes;
        LeaveCriticalSection(&h->lock);
        r = asb_rw_source_send_stream(h->src, id, h->chunk, &ops);
    }
    if (r && r != ASB_RW_ERR_CANCELLED) {
        if (r != ASB_RW_ERR_DISCONNECTED) asb_rw_files_send_status(&ops, ASB_RW_FILE_CANCEL, id, r, 0, 0);
        std::wstring t = L"Couldn't copy to " + h->guest_name + L": " + error_text(r);
        if (h->src->detail[0]) t += std::wstring(L"\n") + h->src->detail;
        set_status(h, t.c_str(), true);
        count_end(h, true, r);
        EnterCriticalSection(&h->lock);
        if (h->id == id) { h->phase = ASB_RW_DH_IDLE; h->id = 0; }
        LeaveCriticalSection(&h->lock);
    } else if (!r) {
        dlog(h, "drag: host -> guest stream complete (%u entries, %llu bytes)", h->src->count,
             (unsigned long long)h->src->total_bytes);
    }
    return 0;
}

void stop_to_guest(AsbRwDragHost *h)
{
    InterlockedExchange(&h->src_cancel, 1);
    if (h->sender) { WaitForSingleObject(h->sender, INFINITE); CloseHandle(h->sender); h->sender = nullptr; }
    if (h->expand) { WaitForSingleObject(h->expand, INFINITE); CloseHandle(h->expand); h->expand = nullptr; }
    if (h->src) { asb_rw_source_free(h->src); delete h->src; h->src = nullptr; }
    InterlockedExchange(&h->src_cancel, 0);
    InterlockedExchange(&h->expand_done, 0);
    h->expand_error = 0;
}

bool our_window(AsbRwDragHost *h, HWND w, uint64_t *surface)
{
    if (!w) return false;
    wchar_t cls[64];
    if (!GetClassNameW(w, cls, ARRAYSIZE(cls)) || (wcscmp(cls, kProxyClass) && wcscmp(cls, kPopupClass))) return false;
    if ((uint64_t)(UINT_PTR)GetPropW(w, kCookieProp) != h->cfg.cookie) return false;
    if (surface) *surface = (uint64_t)(UINT_PTR)GetPropW(w, kSurfaceProp);
    return true;
}

bool cloaked(HWND w)
{
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &c, sizeof(c))) && c;
}

/* The topmost visible window at pt, skipping the overlay (WindowFromPoint would find it). */
HWND window_at(AsbRwDragHost *h, POINT pt)
{
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == h->overlay || w == h->progress || !IsWindowVisible(w) || cloaked(w)) continue;
        LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT) continue;
        RECT r;
        if (!GetWindowRect(w, &r) || !PtInRect(&r, pt)) continue;
        if ((ex & WS_EX_LAYERED)) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(w, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && !alpha) continue;
        }
        return w;
    }
    return nullptr;
}

uint64_t surface_at(AsbRwDragHost *h, POINT pt, POINT *origin, HWND *hwnd)
{
    if (h->cfg.surface_at) { if (hwnd) *hwnd = nullptr; return h->cfg.surface_at(h->cfg.ctx, pt, origin); }
    HWND w = window_at(h, pt);
    uint64_t surface = 0;
    if (!our_window(h, w, &surface) || !surface) return 0;
    POINT o = { 0, 0 };
    ClientToScreen(w, &o);
    RECT rc;
    GetClientRect(w, &rc);
    if (pt.x < o.x || pt.y < o.y || pt.x >= o.x + rc.right || pt.y >= o.y + rc.bottom) return 0;   /* invisible frame */
    *origin = o;
    if (hwnd) *hwnd = w;
    return surface;
}

DWORD target_enter(void *ctx, IDataObject *data, POINTL ptl, DWORD keys)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    (void)keys;
    POINT pt = { ptl.x, ptl.y };
    EnterCriticalSection(&h->lock);
    bool busy = h->phase != ASB_RW_DH_IDLE || h->sock == INVALID_SOCKET || !(h->peer_directions & ASB_RW_DIR_TO_GUEST);
    LeaveCriticalSection(&h->lock);
    if (busy) return DROPEFFECT_NONE;
    if (h->sender || h->expand) stop_to_guest(h);   /* previous session's threads */
    POINT origin;
    uint64_t surface = surface_at(h, pt, &origin, nullptr);
    if (!surface) return DROPEFFECT_NONE;
    wchar_t **paths = nullptr;
    uint32_t n = 0;
    uint32_t r = asb_rw_hdrop_paths(data, &paths, &n);
    AsbRwSource *src = nullptr;
    if (!r) {
        src = new (std::nothrow) AsbRwSource;
        if (!src) r = ASB_RW_ERR_LIMIT;
        else { asb_rw_source_init(src); r = asb_rw_source_top(src, (const wchar_t *const *)paths, n); }
    }
    asb_rw_free_paths(paths, n);
    if (r) {
        dlog(h, "drag: host drag over a proxy rejected (%s)%s%ls", asb_rw_error_name(r), src ? ": " : "", src ? src->detail : L"");
        if (src) { asb_rw_source_free(src); delete src; }
        EnterCriticalSection(&h->lock);
        h->stats.rejected++;
        h->stats.last_error = r;
        LeaveCriticalSection(&h->lock);
        return DROPEFFECT_NONE;
    }
    EnterCriticalSection(&h->lock);
    h->id = asb_rw_files_random64();
    h->phase = ASB_RW_DH_HOVER;
    h->src = src;
    h->surface = surface;
    h->origin = origin;
    h->last_x = pt.x - origin.x;
    h->last_y = pt.y - origin.y;
    h->guest_effect = 0;
    h->guest_status = 0;
    h->refused = false;
    h->done = h->total = 0;
    LeaveCriticalSection(&h->lock);
    AsbRwFileSendOps ops = ops_of(h);
    r = asb_rw_source_send_top(src, h->id, ASB_RW_DIR_TO_GUEST, surface, h->last_x, h->last_y, &ops);
    h->expand = r ? nullptr : CreateThread(nullptr, 0, expand_thread, h, 0, nullptr);
    if (r || !h->expand) {
        EnterCriticalSection(&h->lock);
        h->phase = ASB_RW_DH_IDLE;
        h->id = 0;
        LeaveCriticalSection(&h->lock);
        stop_to_guest(h);
        return DROPEFFECT_NONE;
    }
    h->last_pointer = GetTickCount64();
    dlog(h, "drag: host drag entered surface %016llx (%u item(s))", (unsigned long long)surface, src->top_count);
    return DROPEFFECT_NONE;   /* until the guest drop target reports */
}

DWORD current_effect(AsbRwDragHost *h)
{
    bool ok = h->guest_effect == ASB_RW_EFFECT_COPY && !h->refused && !(h->expand_done && h->expand_error);
    return ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

DWORD target_over(void *ctx, POINTL ptl, DWORD keys)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    (void)keys;
    if (h->phase != ASB_RW_DH_HOVER) return DROPEFFECT_NONE;
    POINT pt = { ptl.x, ptl.y }, origin;
    uint64_t surface = surface_at(h, pt, &origin, nullptr);
    if (surface) {
        int32_t x = pt.x - origin.x, y = pt.y - origin.y;
        bool moved = surface != h->surface || x != h->last_x || y != h->last_y;
        EnterCriticalSection(&h->lock);
        h->surface = surface;
        h->origin = origin;
        h->last_x = x;
        h->last_y = y;
        LeaveCriticalSection(&h->lock);
        if (moved && GetTickCount64() - h->last_pointer >= kPointerMs) send_pointer(h, ASB_RW_POINTER_OVER);
    }
    return surface ? current_effect(h) : DROPEFFECT_NONE;
}

void target_leave(void *ctx)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    if (h->phase != ASB_RW_DH_HOVER) return;
    dlog(h, "drag: host drag left the proxy; cancelling");
    send_status(h, ASB_RW_FILE_CANCEL, h->id, ASB_RW_ERR_CANCELLED);
    EnterCriticalSection(&h->lock);
    h->phase = ASB_RW_DH_IDLE;
    h->id = 0;
    LeaveCriticalSection(&h->lock);
    stop_to_guest(h);
}

void show_progress(AsbRwDragHost *h);

DWORD target_drop(void *ctx, IDataObject *, POINTL ptl, DWORD keys)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    if (h->phase != ASB_RW_DH_HOVER) return DROPEFFECT_NONE;
    target_over(ctx, ptl, keys);
    DWORD effect = current_effect(h);
    if (!effect) {
        uint32_t why = h->expand_done && h->expand_error ? h->expand_error : h->guest_status ? h->guest_status : ASB_RW_ERR_FILE_REJECTED;
        dlog(h, "drag: drop on the proxy refused (%s)", asb_rw_error_name(why));
        send_status(h, ASB_RW_FILE_CANCEL, h->id, ASB_RW_ERR_CANCELLED);
        count_end(h, true, why);
        EnterCriticalSection(&h->lock);
        h->phase = ASB_RW_DH_IDLE;
        h->id = 0;
        LeaveCriticalSection(&h->lock);
        stop_to_guest(h);
        return DROPEFFECT_NONE;
    }
    send_pointer(h, ASB_RW_POINTER_DROP);
    EnterCriticalSection(&h->lock);
    h->phase = ASB_RW_DH_SENDING;
    LeaveCriticalSection(&h->lock);
    std::wstring t = L"Copying to " + h->guest_name + L"...";
    set_status(h, t.c_str(), false);
    h->sender = CreateThread(nullptr, 0, sender_thread, h, 0, nullptr);
    if (!h->sender) {
        send_status(h, ASB_RW_FILE_CANCEL, h->id, ASB_RW_ERR_INTERNAL);
        h->phase = ASB_RW_DH_IDLE;
        return DROPEFFECT_NONE;
    }
    show_progress(h);
    dlog(h, "drag: dropped on surface %016llx at %d,%d; streaming", (unsigned long long)h->surface, h->last_x, h->last_y);
    return DROPEFFECT_COPY;   /* copy only: the host source is never asked to delete */
}

/* ================================ from guest ============================== */

HRESULT out_decide(void *ctx, BOOL escape, DWORD keys)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    InterlockedExchange64(&h->last_callback, (LONGLONG)GetTickCount64());
    EnterCriticalSection(&h->lock);
    bool failed = h->out_failed, staged = h->staged;
    LeaveCriticalSection(&h->lock);
    if (escape || failed || h->stopping || h->out_abort) return DRAGDROP_S_CANCEL;
    if ((keys & (MK_LBUTTON | MK_RBUTTON)) || asb_rw_any_button_down()) return S_OK;
    if (staged) return DRAGDROP_S_DROP;
    if (!h->waiting_drop) {
        h->waiting_drop = true;   /* released early: the drop waits for complete staging */
        std::wstring t = L"Finishing the copy from " + h->guest_name + L"...";
        set_status(h, t.c_str(), false);
        show_progress(h);
    }
    return S_OK;
}

HRESULT out_feedback(void *ctx, DWORD effect)
{
    AsbRwDragHost *h = (AsbRwDragHost *)ctx;
    (void)effect;
    if (h->waiting_drop) {
        SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
        return S_OK;
    }
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

/* The guest drag's button went down in a proxy window of the (elevated) app, so this thread's
   input queue never saw it, and an OLE drag loop cannot track it (it spins without callbacks).
   Hand the press over: put our overlay under the cursor, take the foreground, and re-press the
   physically held button there (release + press, no movement). From then on the press belongs
   to this thread and DoDragDrop follows the real mouse; the user's physical release drops.
   Returns false when no button is held any more (the drag ended before it reached the host). */
bool hand_off_press(AsbRwDragHost *h)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        POINT c;
        GetCursorPos(&c);
        SetWindowPos(h->overlay, HWND_TOPMOST, c.x - 12, c.y - 12, 24, 24, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        for (int i = 0; i < 25 && GetForegroundWindow() != h->overlay; ++i) {
            SetForegroundWindow(h->overlay);
            if (GetForegroundWindow() != h->overlay) Sleep(20);
        }
        /* Only now read the buttons: while the elevated proxy is the foreground window, UIPI makes
           GetAsyncKeyState report every button as up to this (medium-integrity) process. */
        bool fg = GetForegroundWindow() == h->overlay;
        bool left = GetAsyncKeyState(VK_LBUTTON) < 0, right = GetAsyncKeyState(VK_RBUTTON) < 0;   /* physical */
        if (!left && !right) {
            if (fg) {
                dlog(h, "drag: hand-off: no button held any more (foreground ours)");
                return false;
            }
            /* Cannot see the buttons: the session hands a drag over only while it is held, so
               assume the primary (physical right when the buttons are swapped). */
            if (GetSystemMetrics(SM_SWAPBUTTON)) right = true;
            else left = true;
        }
        InterlockedExchange(&h->press_seen, 0);
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = left ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;      /* physical, like the held one */
        in[1].mi.dwFlags = left ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
        UINT sent = SendInput(2, in, sizeof(INPUT));
        ULONGLONG until = GetTickCount64() + 300;
        while (!h->press_seen && GetTickCount64() < until) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        dlog(h, "drag: hand-off press %s (attempt %d, %s button, sent %u, foreground %s)", h->press_seen ? "taken" : "not seen",
             attempt + 1, left ? "physical left" : "physical right", sent, GetForegroundWindow() == h->overlay ? "ours" : "other");
        if (h->press_seen) return true;
        /* The proxy may still hold the capture until its DRAG_STATE arrives: try again shortly. */
    }
    return false;
}

/* Cancels a host drag loop that stopped calling back (it must never spin forever). */
DWORD WINAPI drag_watchdog(LPVOID arg)
{
    AsbRwDragHost *h = (AsbRwDragHost *)arg;
    while (h->out_running && !h->stopping) {
        Sleep(250);
        ULONGLONG last = (ULONGLONG)InterlockedCompareExchange64(&h->last_callback, 0, 0);
        if (h->out_running && last && GetTickCount64() - last > 4000 && !h->out_abort) {
            InterlockedExchange(&h->out_abort, 1);
            dlog(h, "drag: host drag loop unresponsive for 4 s; cancelling it");
            /* Wake the loop with input it reacts to; the source then answers CANCEL. */
            PostThreadMessageW(h->ui_thread_id, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
            PostThreadMessageW(h->ui_thread_id, WM_KEYUP, VK_ESCAPE, 0xC0010001);
            if (h->overlay) PostMessageW(h->overlay, WM_CANCELMODE, 0, 0);
        }
    }
    return 0;
}

void run_drag_out(AsbRwDragHost *h)
{
    EnterCriticalSection(&h->lock);
    AsbRwStage *st = h->stage;
    uint64_t id = h->id;
    bool failed = h->out_failed;
    LeaveCriticalSection(&h->lock);
    if (!st || failed) return;
    const wchar_t *const *paths = nullptr;
    uint32_t n = asb_rw_stage_top(st, &paths);
    IDataObject *data = nullptr;
    HRESULT hr = asb_rw_shell_data_object(paths, n, &data);
    DWORD effect = DROPEFFECT_NONE;
    if (SUCCEEDED(hr)) {
        bool held = h->cfg.run_drag ? true : hand_off_press(h);
        h->waiting_drop = false;
        InterlockedExchange(&h->out_abort, 0);
        InterlockedExchange64(&h->last_callback, (LONGLONG)GetTickCount64());
        if (!held) {
            dlog(h, "drag: the guest drag ended before the host could take it over");
            hr = DRAGDROP_S_CANCEL;
        } else {
            h->out_running = true;
            HANDLE dog = h->cfg.run_drag ? nullptr : CreateThread(nullptr, 0, drag_watchdog, h, 0, nullptr);
            AsbRwDropSourceOps ops = { h, out_decide, out_feedback };
            IDropSource *src = asb_rw_drop_source_create(&ops);
            if (!src) hr = E_OUTOFMEMORY;
            else if (h->cfg.run_drag) hr = (HRESULT)h->cfg.run_drag(h->cfg.ctx, data, src, &effect);
            else hr = SHDoDragDrop(h->overlay, data, src, DROPEFFECT_COPY, &effect);   /* shell drag image */
            if (src) src->Release();
            h->out_running = false;
            if (dog) { WaitForSingleObject(dog, 2000); CloseHandle(dog); }
        }
        if (!h->cfg.run_drag) ShowWindow(h->overlay, SW_HIDE);
        data->Release();
    } else {
        dlog(h, "drag: shell data object for staged items failed (0x%08lX)", (unsigned long)hr);
    }
    uint32_t result = hr == DRAGDROP_S_DROP ? ((effect & DROPEFFECT_COPY) ? ASB_RW_OK : ASB_RW_ERR_FILE_REJECTED)
                    : hr == DRAGDROP_S_CANCEL ? ASB_RW_ERR_CANCELLED : ASB_RW_ERR_INTERNAL;
    EnterCriticalSection(&h->lock);
    if (h->out_failed && h->out_error) result = h->out_error;
    h->stats.last_effect = effect;
    LeaveCriticalSection(&h->lock);
    dlog(h, "drag: host DoDragDrop hr=0x%08lX effect=%lu -> %s", (unsigned long)hr, effect, asb_rw_error_name(result));
    if (h->sock != INVALID_SOCKET) send_status(h, ASB_RW_FILE_RESULT, id, result);
    count_end(h, false, result);
    EnterCriticalSection(&h->lock);
    if (h->stage == st) {
        asb_rw_stage_close(st, result == ASB_RW_OK);
        h->stage = nullptr;
        h->phase = ASB_RW_DH_IDLE;
        h->id = 0;
    }
    LeaveCriticalSection(&h->lock);
    if (result == ASB_RW_OK) set_status(h, (L"Copied from " + h->guest_name + L".").c_str(), true);
    else if (result != ASB_RW_ERR_CANCELLED && result != ASB_RW_ERR_FILE_REJECTED)
        set_status(h, (L"Couldn't copy from " + h->guest_name + L": " + error_text(result)).c_str(), true);
    else set_status(h, L"", true);
}

/* ================================ connection ============================== */

SOCKET connect_hv(const GUID *vm, int channel, DWORD timeout_ms)
{
    HvSockAddr addr = {};
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return s;
    DWORD t = timeout_ms;
    setsockopt(s, HV_PROTOCOL_RAW, HVSOCKET_CONNECT_TIMEOUT, (const char *)&t, sizeof(t));
    addr.family = AF_HYPERV;
    addr.vm_id = *vm;
    GUID svc = { 0xa5b0cafe, (unsigned short)channel, 0x4000, { 0x80, 0, 0, 0, 0, 0, 0, 1 } };   /* windows guests */
    addr.service_id = svc;
    if (connect(s, (const sockaddr *)&addr, sizeof(addr)) != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
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

uint32_t read_message(SOCKET s, std::vector<uint8_t> &buf, AsbRwHeader *hd, AsbRwPayload *m)
{
    size_t total = 0;
    if (!recv_exact(s, buf.data(), ASB_RW_HEADER_BYTES)) return ASB_RW_ERR_DISCONNECTED;
    uint32_t r = asb_rw_frame_check(ASB_CH_RW_FILES, buf.data(), ASB_RW_HEADER_BYTES, &total);
    if (r) return r;
    if (!recv_exact(s, buf.data() + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES)) return ASB_RW_ERR_DISCONNECTED;
    return asb_rw_decode(ASB_CH_RW_FILES, buf.data(), total, hd, m, nullptr, nullptr);
}

/* A from-guest staging failure: tell the guest, make the host drag cancel. */
void fail_out(AsbRwDragHost *h, uint32_t error, bool notify)
{
    uint64_t id = h->id;
    EnterCriticalSection(&h->lock);
    h->out_failed = true;
    if (!h->out_error) h->out_error = error;
    bool running = h->out_running;
    AsbRwStage *st = running ? nullptr : h->stage;
    if (!running) { h->stage = nullptr; h->phase = ASB_RW_DH_IDLE; h->id = 0; }
    LeaveCriticalSection(&h->lock);
    if (notify) send_status(h, ASB_RW_FILE_CANCEL, id, error);
    if (st) { asb_rw_stage_close(st, 0); count_end(h, false, error); }
    dlog(h, "drag: guest -> host copy failed (%s)", asb_rw_error_name(error));
    if (error != ASB_RW_ERR_CANCELLED && error != ASB_RW_ERR_DISCONNECTED)
        set_status(h, (L"Couldn't copy from " + h->guest_name + L": " + error_text(error)).c_str(), true);
}

void handle(AsbRwDragHost *h, const AsbRwHeader *hd, const AsbRwPayload *m)
{
    switch (hd->type) {
    case ASB_RW_FILE_BEGIN: {
        if (m->file_begin.direction != ASB_RW_DIR_FROM_GUEST || !(h->peer_directions & ASB_RW_DIR_FROM_GUEST)) {
            send_status(h, ASB_RW_FILE_CANCEL, m->file_begin.session_id, ASB_RW_ERR_UPGRADE_REQUIRED);
            return;
        }
        EnterCriticalSection(&h->lock);
        bool busy = h->phase != ASB_RW_DH_IDLE || h->out_running;
        if (!busy) {
            h->phase = ASB_RW_DH_RECEIVING;
            h->id = m->file_begin.session_id;
            h->staged = h->out_failed = false;
            h->out_error = 0;
            h->done = h->total = 0;
        }
        LeaveCriticalSection(&h->lock);
        if (busy) { send_status(h, ASB_RW_FILE_CANCEL, m->file_begin.session_id, ASB_RW_ERR_BUSY); return; }
        AsbRwStage *st = asb_rw_stage_open(h->stage_parent.c_str());
        uint32_t r = st ? asb_rw_stage_message(st, hd->type, m) : ASB_RW_ERR_FILE_IO;
        EnterCriticalSection(&h->lock);
        h->stage = st;
        LeaveCriticalSection(&h->lock);
        if (r) fail_out(h, r, true);
        return;
    }
    case ASB_RW_FILE_ENTRY: case ASB_RW_FILE_STREAM: case ASB_RW_FILE_DATA: case ASB_RW_FILE_END: {
        uint64_t id = hd->type == ASB_RW_FILE_ENTRY ? m->file_entry.session_id : hd->type == ASB_RW_FILE_STREAM ? m->file_stream.session_id
                    : hd->type == ASB_RW_FILE_DATA ? m->file_data.session_id : m->file_status.session_id;
        EnterCriticalSection(&h->lock);
        AsbRwStage *st = h->id == id && !h->out_failed ? h->stage : nullptr;
        LeaveCriticalSection(&h->lock);
        if (!st) return;   /* tail of a cancelled session */
        uint32_t before = asb_rw_stage_phase(st);
        uint32_t r = asb_rw_stage_message(st, hd->type, m);
        if (r) { fail_out(h, r, true); return; }
        uint64_t done, total;
        asb_rw_stage_progress(st, &done, &total);
        uint32_t phase = asb_rw_stage_phase(st);
        EnterCriticalSection(&h->lock);
        h->done = done;
        h->total = total;
        if (phase == ASB_RW_RX_COMPLETE) h->staged = true;
        LeaveCriticalSection(&h->lock);
        if (before == ASB_RW_RX_TOP && phase == ASB_RW_RX_READY) PostMessageW(h->msg, WM_DH_OUT_READY, 0, 0);
        if (phase == ASB_RW_RX_COMPLETE) {
            AsbRwFileSendOps ops = ops_of(h);
            asb_rw_files_send_status(&ops, ASB_RW_FILE_PROGRESS, id, ASB_RW_OK, done, total);
            dlog(h, "drag: guest -> host staging complete (%llu bytes)", (unsigned long long)total);
        }
        post_update(h);
        return;
    }
    case ASB_RW_FILE_FEEDBACK:
        if (m->file_feedback.session_id == h->id && h->phase == ASB_RW_DH_HOVER) {
            EnterCriticalSection(&h->lock);
            h->guest_effect = m->file_feedback.effect;
            h->guest_status = m->file_feedback.status;
            LeaveCriticalSection(&h->lock);
        }
        return;
    case ASB_RW_FILE_PROGRESS:
        if (m->file_status.session_id == h->id) {
            EnterCriticalSection(&h->lock);
            h->done = m->file_status.done_bytes;
            h->total = m->file_status.total_bytes;
            LeaveCriticalSection(&h->lock);
            post_update(h);
        }
        return;
    case ASB_RW_FILE_RESULT:
        if (m->file_status.session_id == h->id && (h->phase == ASB_RW_DH_SENDING || h->phase == ASB_RW_DH_HOVER)) {
            uint32_t r = m->file_status.status;
            dlog(h, "drag: guest drop finished (%s)", asb_rw_error_name(r));
            count_end(h, true, r);
            EnterCriticalSection(&h->lock);
            h->phase = ASB_RW_DH_IDLE;
            h->id = 0;
            LeaveCriticalSection(&h->lock);
            if (!r) set_status(h, (L"Copied to " + h->guest_name + L".").c_str(), true);
            else if (r == ASB_RW_ERR_FILE_REJECTED) set_status(h, (h->guest_name + L": the destination did not accept the files.").c_str(), true);
            else if (r != ASB_RW_ERR_CANCELLED) set_status(h, (L"Couldn't copy to " + h->guest_name + L": " + error_text(r)).c_str(), true);
            else set_status(h, L"", true);
        }
        return;
    case ASB_RW_FILE_CANCEL:
        if (m->file_status.session_id != h->id || !h->id) return;
        if (h->phase == ASB_RW_DH_HOVER) {
            /* The guest refused (busy, input owned by automation, staging error): no drop here. */
            EnterCriticalSection(&h->lock);
            h->refused = true;
            h->guest_effect = 0;
            h->guest_status = m->file_status.status;
            LeaveCriticalSection(&h->lock);
            dlog(h, "drag: guest refused the drag (%s)", asb_rw_error_name(m->file_status.status));
        } else if (h->phase == ASB_RW_DH_SENDING) {
            InterlockedExchange(&h->src_cancel, 1);
            count_end(h, true, m->file_status.status ? m->file_status.status : ASB_RW_ERR_CANCELLED);
            EnterCriticalSection(&h->lock);
            h->phase = ASB_RW_DH_IDLE;
            h->id = 0;
            LeaveCriticalSection(&h->lock);
            uint32_t r = m->file_status.status;
            if (r && r != ASB_RW_ERR_CANCELLED) set_status(h, (L"Couldn't copy to " + h->guest_name + L": " + error_text(r)).c_str(), true);
            else set_status(h, L"", true);
        } else if (h->phase == ASB_RW_DH_RECEIVING) {
            fail_out(h, m->file_status.status ? m->file_status.status : ASB_RW_ERR_CANCELLED, false);
        }
        return;
    }
}

void disconnected(AsbRwDragHost *h)
{
    EnterCriticalSection(&h->send_lock);
    SOCKET s = h->sock;
    h->sock = INVALID_SOCKET;
    LeaveCriticalSection(&h->send_lock);
    if (s != INVALID_SOCKET) closesocket(s);
    EnterCriticalSection(&h->lock);
    h->stats.connected = 0;
    uint32_t phase = h->phase;
    LeaveCriticalSection(&h->lock);
    if (phase == ASB_RW_DH_RECEIVING) fail_out(h, ASB_RW_ERR_DISCONNECTED, false);
    else if (phase == ASB_RW_DH_SENDING || phase == ASB_RW_DH_HOVER) {
        InterlockedExchange(&h->src_cancel, 1);
        EnterCriticalSection(&h->lock);
        h->refused = true;
        if (phase == ASB_RW_DH_SENDING) { h->phase = ASB_RW_DH_IDLE; h->id = 0; }
        LeaveCriticalSection(&h->lock);
        if (phase == ASB_RW_DH_SENDING) {
            count_end(h, true, ASB_RW_ERR_DISCONNECTED);
            set_status(h, (L"Couldn't copy to " + h->guest_name + L": " + error_text(ASB_RW_ERR_DISCONNECTED)).c_str(), true);
        }
    }
}

DWORD WINAPI conn_thread(LPVOID arg)
{
    AsbRwDragHost *h = (AsbRwDragHost *)arg;
    std::vector<uint8_t> buf(ASB_RW_HEADER_BYTES + ASB_RW_MAX_FILES_PAYLOAD);
    AsbRwPayload *m = new AsbRwPayload;
    DWORD backoff = 1000;
    while (!h->stopping) {
        SOCKET s = h->cfg.connect ? (SOCKET)h->cfg.connect(h->cfg.ctx, ASB_CH_RW_FILES, 3000) : connect_hv(&h->cfg.vm_id, ASB_CH_RW_FILES, 3000);
        if (s == INVALID_SOCKET) {
            if (WaitForSingleObject(h->stop_event, backoff) == WAIT_OBJECT_0) break;
            backoff = backoff * 2 > 15000 ? 15000 : backoff * 2;
            continue;
        }
        DWORD t = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
        EnterCriticalSection(&h->send_lock);
        h->sock = s;
        LeaveCriticalSection(&h->send_lock);
        AsbRwPayload hello = {};
        hello.files_hello.role = ASB_RW_ROLE_HOST;
        hello.files_hello.version = ASB_RW_FILES_VERSION;
        hello.files_hello.directions = ASB_RW_DIR_TO_GUEST | ASB_RW_DIR_FROM_GUEST;
        hello.files_hello.max_chunk = ASB_RW_FILE_CHUNK_DEFAULT;
        send_payload(h, ASB_RW_FILES_HELLO, &hello);
        AsbRwHeader hd;
        if (read_message(s, buf, &hd, m) || hd.type != ASB_RW_FILES_HELLO || m->files_hello.role != ASB_RW_ROLE_GUEST) {
            disconnected(h);
            if (WaitForSingleObject(h->stop_event, backoff) == WAIT_OBJECT_0) break;
            backoff = backoff * 2 > 15000 ? 15000 : backoff * 2;
            continue;
        }
        backoff = 1000;
        t = 0;   /* idle between drags is normal */
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
        t = 15000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
        EnterCriticalSection(&h->lock);
        h->peer_directions = m->files_hello.directions & (ASB_RW_DIR_TO_GUEST | ASB_RW_DIR_FROM_GUEST);
        h->chunk = m->files_hello.max_chunk < ASB_RW_FILE_CHUNK_DEFAULT ? m->files_hello.max_chunk : ASB_RW_FILE_CHUNK_DEFAULT;
        h->stats.connected = 1;
        h->stats.peer_directions = h->peer_directions;
        LeaveCriticalSection(&h->lock);
        dlog(h, "drag: connected to the guest file channel (directions %u)", h->peer_directions);
        for (;;) {
            uint32_t r = read_message(s, buf, &hd, m);
            if (r) {
                if (!h->stopping) dlog(h, "drag: file channel closed (%s)", asb_rw_error_name(r));
                break;
            }
            if (hd.type == ASB_RW_FILES_HELLO || hd.type == ASB_RW_FILE_POINTER) { dlog(h, "drag: unexpected message %u", hd.type); break; }
            handle(h, &hd, m);
        }
        disconnected(h);
        if (!h->stopping) WaitForSingleObject(h->stop_event, 1000);
    }
    delete m;
    return 0;
}

/* ================================ UI ====================================== */

void place_progress(AsbRwDragHost *h)
{
    MONITORINFO mi = { sizeof(mi) };
    POINT c;
    GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    UINT dpi = GetDpiForWindow(h->progress);
    int w = MulDiv(380, dpi, 96), ht = MulDiv(118, dpi, 96), pad = MulDiv(12, dpi, 96);
    SetWindowPos(h->progress, HWND_TOPMOST, mi.rcWork.right - w - pad, mi.rcWork.bottom - ht - pad, w, ht, SWP_NOACTIVATE);
    SetWindowPos(h->progress_text, nullptr, pad, pad, w - 2 * pad, MulDiv(40, dpi, 96), SWP_NOZORDER);
    SetWindowPos(h->progress_bar, nullptr, pad, pad + MulDiv(44, dpi, 96), w - 2 * pad, MulDiv(14, dpi, 96), SWP_NOZORDER);
    HWND cancel = GetDlgItem(h->progress, ID_CANCEL);
    SetWindowPos(cancel, nullptr, w - pad - MulDiv(88, dpi, 96), ht - pad - MulDiv(28, dpi, 96), MulDiv(88, dpi, 96), MulDiv(26, dpi, 96), SWP_NOZORDER);
}

void refresh_progress(AsbRwDragHost *h)
{
    if (!h->progress) return;
    EnterCriticalSection(&h->lock);
    std::wstring text = h->status;
    bool final = h->status_final;
    uint64_t done = h->done, total = h->total;
    uint32_t phase = h->phase;
    h->stats.done_bytes = done;
    h->stats.total_bytes = total;
    h->stats.phase = phase;
    LeaveCriticalSection(&h->lock);
    if (!IsWindowVisible(h->progress)) {
        if (final && !text.empty() && !h->cfg.no_progress) {   /* errors are shown even without a running copy */
            place_progress(h);
            ShowWindow(h->progress, SW_SHOWNOACTIVATE);
        } else {
            return;
        }
    }
    if (!final && total) {
        wchar_t extra[96];
        swprintf_s(extra, L"\n%.1f of %.1f MB", done / 1048576.0, total / 1048576.0);
        text += extra;
    }
    SetWindowTextW(h->progress_text, text.c_str());
    SendMessageW(h->progress_bar, PBM_SETPOS, total ? (WPARAM)(done * 1000 / total) : 0, 0);
    EnableWindow(GetDlgItem(h->progress, ID_CANCEL), !final);
    if (final) {
        if (text.empty()) ShowWindow(h->progress, SW_HIDE);
        else SetTimer(h->msg, kHideTimer, 3500, nullptr);
    }
}

void show_progress(AsbRwDragHost *h)
{
    if (!h->progress || h->cfg.no_progress) return;
    KillTimer(h->msg, kHideTimer);
    place_progress(h);
    ShowWindow(h->progress, SW_SHOWNOACTIVATE);
    refresh_progress(h);
}

void cancel_from_ui(AsbRwDragHost *h)
{
    EnterCriticalSection(&h->lock);
    uint32_t phase = h->phase;
    uint64_t id = h->id;
    LeaveCriticalSection(&h->lock);
    dlog(h, "drag: cancelled by the user");
    if (phase == ASB_RW_DH_SENDING) {
        InterlockedExchange(&h->src_cancel, 1);
        send_status(h, ASB_RW_FILE_CANCEL, id, ASB_RW_ERR_CANCELLED);
        count_end(h, true, ASB_RW_ERR_CANCELLED);
        EnterCriticalSection(&h->lock);
        h->phase = ASB_RW_DH_IDLE;
        h->id = 0;
        LeaveCriticalSection(&h->lock);
    } else if (phase == ASB_RW_DH_RECEIVING) {
        fail_out(h, ASB_RW_ERR_CANCELLED, true);
    }
    set_status(h, L"", true);
}

LRESULT CALLBACK progress_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwDragHost *h = (AsbRwDragHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CANCEL && h) cancel_from_ui(h);
        return 0;
    case WM_CLOSE:
        if (h) cancel_from_ui(h);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

AsbRwDragHost *g_overlay_owner;   /* one broker per process */

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if ((msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) && g_overlay_owner) {
        InterlockedExchange(&g_overlay_owner->press_seen, 1);   /* hand-off: this thread owns the press */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Shows the overlay over the proxy surface under a host OLE drag, hides it otherwise. */
void poll_overlay(AsbRwDragHost *h)
{
    if (h->cfg.no_overlay || h->out_running) return;
    EnterCriticalSection(&h->lock);
    bool can = h->stats.connected && (h->peer_directions & ASB_RW_DIR_TO_GUEST) &&
               (h->phase == ASB_RW_DH_IDLE || h->phase == ASB_RW_DH_HOVER);
    LeaveCriticalSection(&h->lock);
    bool show = false;
    HWND target = nullptr;
    if (can && asb_rw_any_button_down()) {
        GUITHREADINFO gi = { sizeof(gi) };
        GetGUIThreadInfo(0, &gi);
        if (gi.hwndCapture && !our_window(h, gi.hwndCapture, nullptr) && gi.hwndCapture != h->overlay) {
            POINT pt;
            GetCursorPos(&pt);
            POINT origin;
            uint64_t surface = surface_at(h, pt, &origin, &target);
            show = surface != 0 && target;
        }
    }
    if (!show) {
        if (IsWindowVisible(h->overlay)) ShowWindow(h->overlay, SW_HIDE);
        h->overlay_target = nullptr;
        return;
    }
    RECT rc;
    POINT o = { 0, 0 };
    GetClientRect(target, &rc);
    ClientToScreen(target, &o);
    RECT cur;
    GetWindowRect(h->overlay, &cur);
    if (target != h->overlay_target || !IsWindowVisible(h->overlay) || cur.left != o.x || cur.top != o.y ||
        cur.right - cur.left != rc.right || cur.bottom - cur.top != rc.bottom) {
        SetWindowPos(h->overlay, HWND_TOPMOST, o.x, o.y, rc.right, rc.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (target != h->overlay_target) {
            EnterCriticalSection(&h->lock);
            h->stats.overlay_shown++;
            LeaveCriticalSection(&h->lock);
        }
        h->overlay_target = target;
    }
}

LRESULT CALLBACK msg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwDragHost *h = (AsbRwDragHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!h) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_TIMER:
        if (wp == kPollTimer) poll_overlay(h);
        else if (wp == kHideTimer) { KillTimer(hwnd, kHideTimer); if (h->phase == ASB_RW_DH_IDLE) ShowWindow(h->progress, SW_HIDE); }
        return 0;
    case WM_DH_OUT_READY:
        run_drag_out(h);
        return 0;
    case WM_DH_UPDATE:
        refresh_progress(h);
        return 0;
    case WM_DH_INVOKE:
        ((void (*)(void *))wp)((void *)lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI ui_thread(LPVOID arg)
{
    AsbRwDragHost *h = (AsbRwDragHost *)arg;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT init = OleInitialize(nullptr);
    h->ui_thread_id = GetCurrentThreadId();
    g_overlay_owner = h;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = msg_proc;
    wc.lpszClassName = kMsgClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = overlay_proc;
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = progress_proc;
    wc.lpszClassName = kProgressClass;
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassExW(&wc);
    h->msg = CreateWindowExW(0, kMsgClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    h->overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kOverlayClass,
                                 L"App Sandbox drop", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    h->progress = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kProgressClass, L"App Sandbox",
                                  WS_POPUP | WS_BORDER, 0, 0, 380, 118, nullptr, nullptr, inst, nullptr);
    if (h->msg) SetWindowLongPtrW(h->msg, GWLP_USERDATA, (LONG_PTR)h);
    if (h->progress) {
        SetWindowLongPtrW(h->progress, GWLP_USERDATA, (LONG_PTR)h);
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(h->progress)))
            font = CreateFontIndirectW(&ncm.lfMessageFont);
        h->progress_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 1, 1,
                                           h->progress, nullptr, inst, nullptr);
        h->progress_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, h->progress, nullptr, inst, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 1, 1,
                                      h->progress, (HMENU)(UINT_PTR)ID_CANCEL, inst, nullptr);
        SendMessageW(h->progress_bar, PBM_SETRANGE32, 0, 1000);
        SendMessageW(h->progress_text, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
    }
    if (h->overlay) {
        SetLayeredWindowAttributes(h->overlay, 0, 1, LWA_ALPHA);   /* invisible but hit-testable */
        AsbRwDropTargetOps ops = { h, target_enter, target_over, target_leave, target_drop };
        h->target = asb_rw_drop_target_create(&ops);
        if (!h->target || FAILED(RegisterDragDrop(h->overlay, h->target))) dlog(h, "drag: overlay registration failed");
    }
    h->ui_ok = h->msg && h->overlay && h->target;
    if (h->ui_ok) SetTimer(h->msg, kPollTimer, kPollMs, nullptr);
    SetEvent(h->ready);
    MSG m;
    while (h->ui_ok && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (h->overlay) { RevokeDragDrop(h->overlay); DestroyWindow(h->overlay); }
    if (h->target) h->target->Release();
    if (h->progress) DestroyWindow(h->progress);
    if (h->msg) DestroyWindow(h->msg);
    if (SUCCEEDED(init)) OleUninitialize();
    return 0;
}

} // namespace

/* ================================ public API ============================== */

extern "C" AsbRwDragHost *asb_rw_drag_host_start(const AsbRwDragHostConfig *config)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return nullptr;
    AsbRwDragHost *h = new (std::nothrow) AsbRwDragHost();
    if (!h) { WSACleanup(); return nullptr; }
    h->cfg = *config;
    h->stage_parent = config->stage_parent ? config->stage_parent : L"";
    h->guest_name = config->guest_name && *config->guest_name ? config->guest_name : L"the guest";
    h->cfg.stage_parent = nullptr;
    h->cfg.guest_name = nullptr;
    InitializeCriticalSection(&h->lock);
    InitializeCriticalSection(&h->send_lock);
    h->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    h->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    asb_rw_stage_collect(h->stage_parent.c_str());
    h->ui_thread = CreateThread(nullptr, 0, ui_thread, h, 0, nullptr);
    if (h->ui_thread) WaitForSingleObject(h->ready, 10000);
    if (!h->ui_ok) {
        if (h->ui_thread) { WaitForSingleObject(h->ui_thread, 5000); CloseHandle(h->ui_thread); }
        CloseHandle(h->ready);
        CloseHandle(h->stop_event);
        DeleteCriticalSection(&h->lock);
        DeleteCriticalSection(&h->send_lock);
        delete h;
        WSACleanup();
        return nullptr;
    }
    h->conn_thread = CreateThread(nullptr, 0, conn_thread, h, 0, nullptr);
    dlog(h, "drag: broker started");
    return h;
}

extern "C" void asb_rw_drag_host_stop(AsbRwDragHost *h)
{
    if (!h) return;
    InterlockedExchange(&h->stopping, 1);
    SetEvent(h->stop_event);
    EnterCriticalSection(&h->send_lock);
    if (h->sock != INVALID_SOCKET) shutdown(h->sock, SD_BOTH);
    LeaveCriticalSection(&h->send_lock);
    if (h->conn_thread) { WaitForSingleObject(h->conn_thread, INFINITE); CloseHandle(h->conn_thread); }
    if (h->msg) PostMessageW(h->msg, WM_QUIT, 0, 0);
    PostThreadMessageW(GetThreadId(h->ui_thread), WM_QUIT, 0, 0);
    WaitForSingleObject(h->ui_thread, INFINITE);
    CloseHandle(h->ui_thread);
    stop_to_guest(h);
    if (h->stage) { asb_rw_stage_close(h->stage, 0); h->stage = nullptr; }
    CloseHandle(h->ready);
    CloseHandle(h->stop_event);
    DeleteCriticalSection(&h->lock);
    DeleteCriticalSection(&h->send_lock);
    if (h->cfg.log) h->cfg.log(h->cfg.ctx, "drag: broker stopped");
    delete h;
    WSACleanup();
}

extern "C" void asb_rw_drag_host_stats(AsbRwDragHost *h, AsbRwDragHostStats *out)
{
    EnterCriticalSection(&h->lock);
    h->stats.phase = h->phase;
    h->stats.done_bytes = h->done;
    h->stats.total_bytes = h->total;
    *out = h->stats;
    LeaveCriticalSection(&h->lock);
}

IDropTarget *asb_rw_drag_host_target(AsbRwDragHost *h) { return h->target; }

void asb_rw_drag_host_invoke(AsbRwDragHost *h, void (*fn)(void *arg), void *arg)
{
    SendMessageW(h->msg, WM_DH_INVOKE, (WPARAM)fn, (LPARAM)arg);
}
