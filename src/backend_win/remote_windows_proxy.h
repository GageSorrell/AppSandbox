/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_proxy
 *
 * @file      remote_windows_proxy.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows per-VM host manager and native proxy windows (AppSandbox-eh6.6).
 *
 * One manager per running Windows guest, owned by the host app and independent of the desktop
 * viewer: closing the viewer leaves sharing active, stopping the manager (VM stop or app exit)
 * ends every selection (the guest helper restores its windows when the control channel closes).
 *
 * Threads
 *   UI thread    owns the D3D11 device, DirectComposition, every proxy window and the session
 *                model (remote_windows_session.c); renders frames and runs native move/resize.
 *   connection   connects control channel 9 (AF_HYPERV, retried with backoff), reads complete
 *                messages and posts them to the UI thread; frame channel 10 is connected after
 *                the guest HELLO and served by remote_windows_frames_host.c.
 *
 * Proxy windows
 *   Root: an unowned, framed (WS_OVERLAPPEDWINDOW) window with taskbar/Alt+Tab entries, the
 *   guest title and icon. WM_NCCALCSIZE removes the host caption, so the client area is exactly
 *   the guest root's visible frame and the guest's own title bar (standard or custom) is the
 *   only one drawn - no double non-client frame. The invisible left/right/bottom borders, DWM
 *   shadow, rounded corners, minimize/maximize animations and Snap stay native. Inside the
 *   client, WM_NCHITTEST asks the guest application (cached) so its caption, caption buttons
 *   (Snap Layouts on the maximize button) and top resize edge work as on the guest.
 *   Owned windows (dialogs) are framed proxies owned by the root proxy; menus, tooltips and other
 *   popups are non-activating popups owned by it, placed relative to the root even outside its
 *   bounds and stacked bottom-to-top as on the guest. Everything is drawn with premultiplied
 *   alpha through composition swap chains, 1 guest pixel = 1 host pixel.
 *   A modal owned window disables the root proxy, so the system's own modal rules apply. Close
 *   forwards SC_CLOSE to the guest; the proxy stays until the guest window really closes.
 *
 * Input and clipboard (eh6.7)
 *   Keyboard and client-area mouse input of the proxy with host activation go through the VM's
 *   input arbitration (config.input) as the Remote Windows user source: host activation claims
 *   it and asks the guest to activate the root/owned window; nothing is injected before the
 *   guest confirms (see remote_windows_session.h "Input"). Windows-key, Alt+Tab and the other
 *   shell shortcuts stay on the host. config.focus reports whether any proxy has host activation,
 *   which makes the proxies one focus client of the VM's shared clipboard coordinator.
 *
 * Internal only (no HTTP/MCP/Python API).
 */
#ifndef REMOTE_WINDOWS_PROXY_H
#define REMOTE_WINDOWS_PROXY_H

#include <windows.h>   /* no winsock: includable after windows.h (ui.c) */
#include "../../tools/transport/asb_remote_windows.h"
#include "../core/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwManager AsbRwManager;

/* The VM's input arbitration as seen by Remote Windows (eh6.7). The host app implements it with
   the per-VM input session (headless_vm.h asb_input_*), tests with a recorder. */
typedef struct AsbRwInputPortState {
    uint32_t epoch, windows_current, user_owns, hold;
} AsbRwInputPortState;
typedef struct AsbRwInputPort {
    void *ctx;
    uint32_t (*claim)(void *ctx, int renew);                          /* epoch, 0 = unavailable */
    uint32_t (*send)(void *ctx, uint32_t epoch, const InputPacket *packet);   /* 0 = injected */
    void (*guard)(void *ctx, int on);
    uint32_t (*hold)(void *ctx);
    void (*unhold)(void *ctx, uint32_t hold, int ok);
    void (*state)(void *ctx, AsbRwInputPortState *out);
} AsbRwInputPort;

typedef struct AsbRwManagerConfig {
    GUID vm_id;                     /* HCS runtime id (AF_HYPERV) */
    /* Optional transport override (native tests: loopback TCP): a connected SOCKET, or
       INVALID_SOCKET (~0) on failure. */
    UINT_PTR (*connect)(void *ctx, int channel, DWORD timeout_ms);
    void *ctx;
    void (*log)(void *ctx, const char *message);
    void (*changed)(void *ctx);     /* any thread; must not block (post a message) */
    uint32_t allow_gpu;             /* 0 forces BGRA CPU frames */
    uint32_t local_frames;          /* 1: GPU pools for a local guest process (tests) */
    uint32_t no_activate;           /* 1: new proxies are shown without activation (tests) */
    const wchar_t *group;           /* taskbar grouping prefix (e.g. the VM name); optional */
    AsbRwInputPort input;           /* eh6.7; no input forwarding when input.claim is NULL */
    void (*focus)(void *ctx, int focused);   /* UI thread: a proxy gained / all proxies lost host activation */
    /* eh6.9 drag and drop (optional): start the drag broker for this manager (proxy windows carry
       `cookie` in ASB_RW_DRAG_COOKIE_PROP); nonzero = running, file capabilities are offered.
       drag_stop is called once at manager stop. */
    int (*drag_start)(void *ctx, uint64_t cookie);
    void (*drag_stop)(void *ctx);
} AsbRwManagerConfig;

/* Window properties of every proxy surface window (eh6.9): the owning manager's drag cookie and
   the guest surface ID (HANDLE-sized values), read by the drag broker in another process. */
#define ASB_RW_DRAG_COOKIE_PROP  L"AsbRwDragCookie"
#define ASB_RW_DRAG_SURFACE_PROP L"AsbRwDragSurface"
#define ASB_RW_PROXY_CLASS       L"AppSandbox.RemoteWindows.Proxy"
#define ASB_RW_POPUP_CLASS       L"AppSandbox.RemoteWindows.Popup"

/* NULL on failure (no thread started). */
AsbRwManager *asb_rw_manager_start(const AsbRwManagerConfig *config);
/* Ends sharing (proxies destroyed, connections closed -> the guest restores) and frees. */
void asb_rw_manager_stop(AsbRwManager *m);

#define ASB_RW_MGR_DISCONNECTED 0u
#define ASB_RW_MGR_CONNECTING   1u
#define ASB_RW_MGR_READY        2u   /* proxies supported */
#define ASB_RW_MGR_LIMITED      3u   /* connected; guest components too old for proxies */

typedef struct AsbRwManagerStatus {
    uint32_t state;                 /* ASB_RW_MGR_* */
    uint32_t negotiation_error;     /* AsbRwError of the HELLO negotiation */
    uint32_t guest_capabilities, missing;
    uint32_t guest_helper_version;
    uint32_t frames_state;          /* ASB_RW_HOST_* of the frame channel (CLOSED when none) */
    uint32_t frames_gpu;            /* frame channel agreed on GPU pools */
    uint32_t inventory_count, proxies;
    uint64_t connections, frames_presented;
    /* eh6.7 input (AsbRwHostInput of the session) */
    uint32_t input_state, input_guard, input_last_error;
    uint64_t input_root, input_injected, input_activations, input_failures, input_dropped_stale, input_released;
} AsbRwManagerStatus;

typedef struct AsbRwManagerEntry {
    AsbRwInventoryEntry entry;
    uint32_t state, error;          /* selection state (ASB_RW_STATE_*) and last error */
} AsbRwManagerEntry;

typedef struct AsbRwProxySurfaceInfo {
    uint64_t surface_id;
    uint32_t kind;
    HWND hwnd;
    uint64_t presented;             /* frames drawn into this window */
} AsbRwProxySurfaceInfo;

typedef struct AsbRwProxyInfo {
    uint64_t id;
    HWND hwnd;                      /* root proxy */
    uint64_t presented;
    uint32_t content_w, content_h;  /* size of the newest root frame */
    uint32_t window_flags;          /* last WINDOW_STATE flags (ASB_RW_WS_*) */
    uint32_t count;
    AsbRwProxySurfaceInfo surfaces[ASB_RW_MAX_SURFACES];
} AsbRwProxyInfo;

void asb_rw_manager_status(AsbRwManager *m, AsbRwManagerStatus *out);
/* Inventory with selection states (copy). Returns the number written. */
uint32_t asb_rw_manager_inventory(AsbRwManager *m, AsbRwManagerEntry *out, uint32_t max);
/* Proxies and their native windows (copy). */
uint32_t asb_rw_manager_proxies(AsbRwManager *m, AsbRwProxyInfo *out, uint32_t max);
/* Asynchronous select/unselect; returns ASB_RW_OK when requested (results arrive as state). */
uint32_t asb_rw_manager_select(AsbRwManager *m, uint64_t id, int select);
uint32_t asb_rw_manager_restore_all(AsbRwManager *m);
/* eh6.8: copy of the selector icon of an inventory entry (caller destroys; NULL = none). */
HICON asb_rw_manager_icon(AsbRwManager *m, uint64_t id);
/* The VM's input arbitration changed (any thread; posts to the manager). */
void asb_rw_manager_input_changed(AsbRwManager *m);

#ifdef __cplusplus
}
#endif
#endif
