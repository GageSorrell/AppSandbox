/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_helper
 *
 * @file      remote_windows_helper.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows guest helper: one control-channel (9) session (AppSandbox-eh6.4).
 *
 * Transport-independent: complete messages go in through asb_rw_helper_receive, replies and
 * events leave through ops.send. One control thread drives receive/tick/close.
 *
 *   guest -> host  HELLO (on open), INVENTORY_SNAPSHOT / INVENTORY_EVENT, SELECT_RESULT,
 *                  RESTORE_ALL (acknowledgement, same request id), ERROR
 *   host -> guest  HELLO, INVENTORY_REQUEST, SELECT_REQUEST, RESTORE_ALL
 *
 * Select: SELECT_RESULT PENDING, then SHARED or FAILED (with the AsbRwError).
 * Unselect: RESTORING, then NONE (error OK, or STALE_ID when the window is gone). A hung window
 * stays RESTORING until a later tick restores it. A shared root that is destroyed reports NONE
 * with STALE_ID. Selections end with the session: closing restores every window.
 * Selection needs only SELECTION + PRIVATE_DISPLAY.
 *
 * Proxy support (eh6.6, only while ASB_RW_CAP_GEOMETRY is negotiated and the window ops are
 * set): GEOMETRY_COMMAND resizes a parked root in place (or moves an owned window) and acks the
 * visible frame actually applied; host minimization leaves the source untouched and renderable.
 * CLOSE_REQUEST posts SC_CLOSE (BUSY while a modal owned window disables the root). HIT_TEST
 * answers the application's own WM_NCHITTEST, so custom title bars keep their regions. Every
 * tick, WINDOW_STATE is sent for shared roots and owned windows whose styles, modal state, DPI
 * or tracking limits changed; a root the application minimizes or maximizes itself is put back
 * to normal at once (it must keep rendering) and reported as a request. WINDOW_ICON is sent on
 * selection and whenever the root's icon changes. IDs are resolved only against the current
 * surfaces of shared roots, with identity validation (STALE_ID otherwise).
 *
 * Surfaces (eh6.5): every tick, each parked root's surfaces are collected from a lightweight
 * window scan (asb_rw_collect_surfaces: the root, owned dialogs/menus/tooltips/popups, and
 * unowned same-process popups on the private display, bottom-to-top). When the set or any
 * rectangle changes the helper sends SURFACES (root first, z 0) and hands the complete set to
 * the capture engine; a root that stops being shared gets SURFACES with count 0 and an empty
 * capture set. Popup surface IDs are session IDs bound to the popup's identity and released
 * when the popup disappears; the root surface uses the root's ID.
 *
 * Activation (eh6.7, ASB_RW_CAP_ACTIVATION with the foreground/activate/desktop ops):
 * ACTIVATE_REQUEST for a shared root or owned window makes it (for a root disabled by a modal
 * owned window: that window) the guest foreground and acks foreground = 1 with the host's input
 * epoch, or foreground = 0 with FOREGROUND_DENIED / BUSY (hung) / STALE_ID / NOT_ELIGIBLE. The
 * host injects only after foreground = 1. ACTIVATE_REQUEST with id 0 moves the foreground off
 * every shared window (to the last ordinary foreground window, else the desktop) and acks id 0
 * with foreground = 1 once no shared window has it. Every tick the helper watches the
 * foreground: when the activated window's family (root, its owned windows and popups) loses
 * it, an unsolicited ACTIVATE_ACK (request 0, the epoch, foreground = 0) tells the host to
 * release held input; when a shared window gains it without a host request, an unsolicited ack
 * with epoch 0 and foreground = 1 lets the host protect other input sources.
 *
 * Guest selector (eh6.8, ASB_RW_CAP_GUEST_SELECTOR): asb_rw_helper_guest_select sends the
 * guest tray's toggle to the host as SELECT_REQUEST; the host performs it through its own
 * selection path, so the result arrives as an ordinary host SELECT_REQUEST and SELECT_RESULT.
 * The row shows "pending" until then; an ERROR with the request's ID, or no answer within
 * ASB_RW_HELPER_REQUEST_MS, shows the error. Every inventory entry's icon is sent (on add and
 * when it changes) so the host selector shows the same icons. asb_rw_helper_flyout builds the
 * shared flyout model from the acknowledged state.
 *
 * Drag and drop (eh6.9, ASB_RW_CAP_FILES_*): DRAG_PROBE / DRAG_ACK go to ops.drag (the file
 * bridge's control mailbox); the bridge's DRAG_STATE leaves through asb_rw_helper_drag_state.
 */
#ifndef REMOTE_WINDOWS_HELPER_H
#define REMOTE_WINDOWS_HELPER_H

#include "remote_windows_share.h"
#include "remote_windows_flyout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_HELPER_VERSION ((1u << 16) | (4u << 8) | 0u)   /* 1.4.0 (eh6.9 drag and drop) */
#define ASB_RW_HELPER_MAX_REQUESTS 16u
#define ASB_RW_HELPER_REQUEST_MS   5000u   /* a guest selector request without an answer fails */
#define ASB_RW_HELPER_MAX_EVENTS 64u
#define ASB_RW_HELPER_MAX_STATES 128u   /* roots + owned windows with WINDOW_STATE tracking */
#define ASB_RW_HELPER_LIMITS_MS  2000u  /* re-query tracking limits / icons at most this often */

typedef struct AsbRwHelperOps {
    void *ctx;
    BOOL (*send)(void *ctx, const uint8_t *bytes, size_t len);
    uint32_t (*enumerate)(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count);
    BOOL (*facts)(void *ctx, HWND hwnd, AsbRwWindowFacts *facts);
    int (*probe)(void *ctx, uint64_t hwnd, AsbRwIdentity *live);
    void (*log)(void *ctx, const char *message);
    /* eh6.5 (optional): visible-window scan (top first) and capture requests. */
    uint32_t (*scan)(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count);
    void (*capture)(void *ctx, uint64_t root_id, const AsbRwCaptureTarget *targets, uint32_t n);
    /* eh6.6 (optional; ASB_RW_CAP_GEOMETRY needs all of them): window control. */
    BOOL (*frames)(void *ctx, HWND hwnd, RECT *window, RECT *visible);   /* window rect + visible frame */
    BOOL (*set_rect)(void *ctx, HWND hwnd, const RECT *window, BOOL size); /* no activation / z change */
    BOOL (*responsive)(void *ctx, HWND hwnd);
    BOOL (*post_close)(void *ctx, HWND hwnd);                              /* WM_SYSCOMMAND SC_CLOSE */
    uint32_t (*hit_test)(void *ctx, HWND hwnd, int x, int y);              /* screen point -> HT* */
    /* Styles, DPI, placement show command and (when limits) the tracking limits as visible-frame
       sizes. FALSE when the window is gone. */
    BOOL (*window_state)(void *ctx, HWND hwnd, BOOL limits, AsbRwWindowState *state, UINT *show_cmd);
    BOOL (*normalize)(void *ctx, HWND hwnd);                               /* SW_SHOWNOACTIVATE */
    /* Current icon key (handle); with icon != NULL also renders it. FALSE when none. */
    BOOL (*icon)(void *ctx, HWND hwnd, uint64_t *key, AsbRwWindowIcon *icon);
    /* eh6.7 (optional; ASB_RW_CAP_ACTIVATION needs all three): the foreground window followed by
       its owner chain, foreground activation, and the desktop window used to release it. */
    uint32_t (*foreground)(void *ctx, uint64_t *chain, uint32_t max);
    BOOL (*activate)(void *ctx, HWND hwnd);
    uint64_t (*desktop)(void *ctx);
    /* eh6.9 (optional): DRAG_PROBE / DRAG_ACK for the file bridge (remote_windows_files.h). */
    void (*drag)(void *ctx, uint16_t type, const AsbRwDrag *msg);
} AsbRwHelperOps;

typedef struct AsbRwHelperRoot {
    uint64_t root_id, hwnd;
    AsbRwSurfaces last;               /* last SURFACES sent */
    uint64_t hwnds[ASB_RW_MAX_SURFACES];   /* window of each surface in `last` */
} AsbRwHelperRoot;

/* WINDOW_STATE / icon bookkeeping for one root or owned window (eh6.6). */
typedef struct AsbRwHelperWindow {
    uint64_t id, hwnd;
    int is_root, sent, seen;
    AsbRwWindowState state;           /* last sent */
    uint32_t request, request_serial;
    int32_t min_w, min_h, max_w, max_h;
    uint32_t limits_dpi;
    uint64_t limits_tick, icon_tick, icon_key;
    int icon_sent;
} AsbRwHelperWindow;

/* A guest selector request (eh6.8). */
typedef struct AsbRwHelperRequest {
    uint64_t id;
    uint32_t request_id, select, active;
    uint32_t error;                   /* last refusal for this id (shown until the next change) */
    uint64_t tick;
} AsbRwHelperRequest;

/* Inventory icon bookkeeping (eh6.8). */
typedef struct AsbRwHelperIcon { uint64_t id, key, tick; } AsbRwHelperIcon;

typedef struct AsbRwHelper {
    AsbRwHelperOps ops;
    AsbRwShare *share;
    uint32_t capabilities, helper_version, driver_version, session_tag;
    int hello_done, send_failed;
    AsbRwNegotiated negotiated;
    AsbRwIdTable ids;
    AsbRwInventory inventory;
    AsbRwWindowFacts *facts;          /* ASB_RW_MAX_WINDOWS */
    AsbRwInventoryEvent *events;      /* ASB_RW_HELPER_MAX_EVENTS */
    AsbRwPayload *rx, *tx;
    uint8_t *out;                     /* header + max control payload */
    AsbRwHelperRoot *roots;           /* ASB_RW_SHARE_MAX */
    AsbRwCaptureTarget *targets;      /* ASB_RW_MAX_SURFACES */
    uint8_t *claimed;                 /* ASB_RW_MAX_WINDOWS */
    AsbRwHelperWindow *windows;       /* ASB_RW_HELPER_MAX_STATES */
    uint64_t now_ms;                  /* tick clock (ops-independent; set by asb_rw_helper_tick) */
    /* eh6.7 activation */
    uint64_t active_id, active_root;  /* host-activated surface and its root window (0 = none) */
    uint32_t active_epoch;
    uint64_t plain_foreground;        /* last foreground window that was not a shared window */
    uint64_t gain_reported;           /* shared foreground window last reported as an unsolicited gain */
    /* eh6.8 guest selector */
    AsbRwHelperRequest requests[ASB_RW_HELPER_MAX_REQUESTS];
    uint32_t next_request;
    AsbRwHelperIcon *icons;           /* ASB_RW_MAX_INVENTORY */
} AsbRwHelper;

/* Capabilities the ops support (adds ASB_RW_CAP_GEOMETRY when every window op is set, and
   ASB_RW_CAP_ACTIVATION when the activation ops are set too). */
uint32_t asb_rw_helper_capabilities(const AsbRwHelperOps *ops, uint32_t base);

/* Allocates session state, binds IDs to session_tag (nonzero, fresh per connection) and sends
   HELLO. ASB_RW_ERR_INTERNAL on allocation/send failure. */
uint32_t asb_rw_helper_open(AsbRwHelper *h, const AsbRwHelperOps *ops, AsbRwShare *share,
                            uint32_t capabilities, uint32_t driver_version, uint32_t session_tag);
/* One complete control message. Nonzero: protocol violation, drop the connection. */
uint32_t asb_rw_helper_receive(AsbRwHelper *h, const uint8_t *bytes, size_t len);
/* Maintains shared windows; with inventory, also diffs the inventory and sends events.
   Surfaces are refreshed on every tick when ops.scan is set, then window states/icons (eh6.6).
   now_ms is a monotonic clock used to pace limit/icon queries. */
void asb_rw_helper_tick(AsbRwHelper *h, int inventory);
void asb_rw_helper_tick_at(AsbRwHelper *h, int inventory, uint64_t now_ms);
/* Re-collects the surfaces of every parked root now (sends SURFACES / capture requests). */
void asb_rw_helper_surfaces(AsbRwHelper *h);
/* eh6.8 guest selector. Sends the toggle to the host: ASB_RW_OK when sent, UPGRADE_REQUIRED
   when the host has no guest selector support, BUSY while a request for the id is in flight. */
uint32_t asb_rw_helper_guest_select(AsbRwHelper *h, uint64_t id, int select);
/* The guest tray's "Restore all windows": restores locally and tells the host. */
void asb_rw_helper_restore_all(AsbRwHelper *h);
/* Selector rows from the acknowledged state; hwnds[i] (optional) receives each row's window. */
void asb_rw_helper_flyout(const AsbRwHelper *h, AsbRwFlyoutModel *m, uint64_t *hwnds);

/* eh6.9 drag and drop: FILES capabilities negotiated on both sides; DRAG_STATE to the host;
   the surface table of the shared roots (last SURFACES) for pointer mapping. */
int asb_rw_helper_files_enabled(const AsbRwHelper *h);
void asb_rw_helper_drag_state(AsbRwHelper *h, const AsbRwDrag *state);
uint32_t asb_rw_helper_drag_surfaces(const AsbRwHelper *h, uint64_t *ids, AsbRwRect *rects, uint32_t max);

/* Ends the session: restores every window (pending ones keep being retried by the caller via
   asb_rw_share_maintain) and frees session state. */
void asb_rw_helper_close(AsbRwHelper *h);

#ifdef __cplusplus
}
#endif
#endif
