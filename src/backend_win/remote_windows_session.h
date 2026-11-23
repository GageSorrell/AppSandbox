/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_session
 *
 * @file      remote_windows_session.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows host control session (AppSandbox-eh6.6): the platform-neutral half of the
 * per-VM host manager. One instance per control-channel (9) connection, driven from one thread
 * (the manager's UI thread). Messages come in through asb_rw_session_receive, native proxy
 * window events through the asb_rw_session_* calls below; everything leaves through
 * AsbRwSessionOps (control bytes, proxy window actions), so the rules are tested against a
 * model (tools/tests/remote_windows_proxy_test.c) and the Win32 layer stays thin.
 *
 * Identities and lifetimes
 *   A proxy exists exactly while the guest acknowledges its root as SHARED in this session.
 *   SELECT_RESULT RESTORING / NONE / FAILED, RESTORE_ALL and the end of the session destroy it;
 *   IDs from other sessions (wrong tag) are ignored. Unselecting never closes the application.
 *
 * Geometry (acknowledged)
 *   Host move/resize/minimize/maximize/Snap produce asb_rw_session_resize. At most one
 *   GEOMETRY_COMMAND is in flight per proxy; newer requests coalesce and are sent when the ack
 *   arrives (stale serials are ignored). When the guest applies a different size (application
 *   limits) and the host is not in a size loop, the proxy adopts the acknowledged size.
 *   Application-initiated size changes (SURFACES) are adopted the same way. Host minimization
 *   is reported as such; the guest keeps the source renderable.
 *
 * State feedback
 *   WINDOW_STATE drives the proxy's styles, enabled state (modal owned windows) and tracking
 *   limits; a new request_serial replays an application minimize/maximize on the proxy
 *   (maximize toggles, because the source itself never stays maximized).
 *
 * Close
 *   asb_rw_session_close sends CLOSE_REQUEST (one at a time per window). The proxy stays until
 *   the guest window really goes away, so prompts and cancellation work. A disabled root (modal
 *   owned window) is not asked: its owned window is activated instead (also on CLOSE_RESULT BUSY).
 *
 * Hit testing
 *   asb_rw_session_hit_test answers from a small cache of guest WM_NCHITTEST results and queries
 *   the guest asynchronously (one request in flight per surface, newest point queued), so custom
 *   title bars keep their own caption, button and edge regions without blocking the host.
 *
 * Input (AppSandbox-eh6.7; needs ASB_RW_CAP_ACTIVATION)
 *   Keyboard and mouse go through the VM's single input arbitration (input_router.h) as the
 *   ASB_INPUT_SOURCE_WINDOWS user source, never a separate injection path. When a proxy gets
 *   host activation the session claims the source (a fresh epoch: the router releases whatever
 *   the previous source or proxy held) and sends ACTIVATE_REQUEST for that root or owned window.
 *   Input is queued (bounded) until ACTIVATE_ACK confirms that window is the guest foreground
 *   for that epoch; a refused or unanswered activation discards the queue. An unsolicited ack
 *   (foreground lost in the guest), a newer router epoch (viewer or automation took over,
 *   channel reconnect) or host deactivation releases every key/button this source holds and
 *   requires a new activation before anything else is injected.
 *   Mouse points are relative to a surface's visible frame and are mapped through the last
 *   acknowledged SURFACES rectangle to guest virtual-screen pixels (INPUT_MOUSE_VIRTUAL; 1 host
 *   pixel = 1 guest pixel, so host and guest DPI do not enter the mapping). Presses, wheel and
 *   hover outside the acknowledged rectangle, on unknown surfaces or on an owned window whose
 *   host move is unacknowledged are stale geometry and dropped; while a button is held, moves
 *   and the release still go through so drags end cleanly.
 *   Foreground guard: while a proxy holds the guest foreground the session tells the router;
 *   when another user source takes over (or the automation agent), it asks the guest to move
 *   the foreground away (ACTIVATE_REQUEST id 0) and the router releases the held keyboard
 *   input only once that is acknowledged. Host Alt+Tab, Windows-key and other shell shortcuts
 *   stay local (asb_rw_input_key_is_local).
 *
 * Guest selector (AppSandbox-eh6.8; ASB_RW_CAP_GUEST_SELECTOR on both sides)
 *   The guest's own tray flyout sends SELECT_REQUEST to the host. The host performs it exactly
 *   like its own selector (asb_rw_session_select: SELECT_REQUEST back to the guest, proxy on
 *   SHARED), so both selectors always show the same acknowledged state; a refusal (upgrade,
 *   stale ID, proxy table full, ...) is answered with ERROR carrying the guest's request ID.
 *   WINDOW_ICON for an entry without a proxy is an inventory icon for the selector rows.
 *
 * Drag and drop (AppSandbox-eh6.9; ASB_RW_CAP_FILES_* on both sides, offered via ops.file_caps)
 *   Files themselves travel on channel 11 between the guest helper and the medium-integrity host
 *   drag broker; this session only arbitrates the guest pointer. When the pointer leaves a proxy
 *   with a button held, the manager calls asb_rw_session_drag_probe: DRAG_PROBE is sent and input
 *   forwarding is suspended. DRAG_STATE ACTIVE FROM_GUEST makes the session forget its held
 *   button (the helper releases it on its catcher) and asks the manager to drop mouse capture so
 *   the broker's host drag can run; NONE resumes forwarding; SINK keeps it suspended until the
 *   host button goes up, which is answered with DRAG_ACK RELEASE. For a host drag into a proxy
 *   the guest sends DRAG_STATE ACTIVE TO_GUEST: the session claims the input arbitration (a fresh
 *   epoch: the viewer's or a previous holder's keys are released) and answers DRAG_ACK ALLOW, or
 *   CANCEL when automation owns input. If another source or automation takes over during a drag,
 *   DRAG_ACK CANCEL ends it. While the helper owns the pointer no button-up is injected by the
 *   host (keys are still released). DRAG_STATE ENDED returns to normal forwarding.
 */
#ifndef REMOTE_WINDOWS_SESSION_H
#define REMOTE_WINDOWS_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include "../../tools/transport/asb_remote_windows.h"
#include "../core/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_HOST_MAX_PROXIES   32u
#define ASB_RW_HOST_MAX_OWNED_MOVES 8u
#define ASB_RW_HOST_HIT_CACHE     48u
#define ASB_RW_HOST_HIT_RADIUS    2      /* px: a cached result answers points this close */
#define ASB_RW_HOST_HIT_NEAR      8      /* px: fallback answer while the exact point is queried */
#define ASB_RW_HOST_HIT_TTL_MS    1500u
#define ASB_RW_HOST_RETRY_MS      400u   /* unanswered geometry/hit/close requests are retried after this */
#define ASB_RW_INPUT_QUEUE        64u    /* input events waiting for an activation ack */
#define ASB_RW_INPUT_ACTIVATE_MS  1000u  /* an unanswered activation fails after this */
#define ASB_RW_INPUT_RELEASE_MS   2000u  /* an unanswered foreground release is forgotten after this */
#define ASB_RW_INPUT_KEYS         768u   /* scan (+256 extended) or 512 + VK */

/* Capabilities this host offers, and the subset proxies need from the guest. */
#define ASB_RW_HOST_CAPS (ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | \
                          ASB_RW_CAP_FRAMES_CPU | ASB_RW_CAP_FRAMES_GPU | ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY | \
                          ASB_RW_CAP_ACTIVATION | ASB_RW_CAP_GUEST_SELECTOR)
#define ASB_RW_PROXY_REQUIRED (ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | \
                               ASB_RW_CAP_FRAMES_CPU | ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY)
#define ASB_RW_HOST_VERSION ((1u << 16) | (3u << 8) | 0u)   /* 1.3.0: drag and drop (eh6.9) */

/* Initial facts for a new proxy (from the inventory entry). */
typedef struct AsbRwProxyInit {
    AsbRwRect guest_rect;           /* visible frame on the guest when selected */
    uint32_t dpi;
    uint16_t title_len, app_len;
    const uint16_t *title, *app;
} AsbRwProxyInit;

typedef struct AsbRwSessionOps {
    void *ctx;
    int  (*send)(void *ctx, const uint8_t *bytes, size_t len);           /* 0: send failed */
    void (*log)(void *ctx, const char *message);
    /* Root proxies. create returns 0 on failure (the selection is then undone). */
    int  (*proxy_create)(void *ctx, uint64_t id, const AsbRwProxyInit *init);
    void (*proxy_destroy)(void *ctx, uint64_t id);
    void (*proxy_title)(void *ctx, uint64_t id, const uint16_t *title, uint32_t len);
    void (*proxy_icon)(void *ctx, uint64_t id, const AsbRwWindowIcon *icon);
    /* State of the root (surface_id == id) or of one of its owned windows. */
    void (*proxy_state)(void *ctx, uint64_t id, uint64_t surface_id, const AsbRwWindowState *state);
    void (*proxy_size)(void *ctx, uint64_t id, int32_t width, int32_t height);   /* client size */
    void (*proxy_show)(void *ctx, uint64_t id, uint32_t show);                   /* ASB_RW_SHOW_* */
    /* The root and its surfaces (owned windows/menus/tooltips/popups), bottom-to-top. */
    void (*proxy_surfaces)(void *ctx, uint64_t id, const AsbRwSurfaces *surfaces);
    void (*proxy_activate_modal)(void *ctx, uint64_t id);
    void (*hit_result)(void *ctx, uint64_t id, uint64_t surface_id);
    void (*changed)(void *ctx);     /* inventory / selection state changed (selectors) */
    /* eh6.7 input arbitration (all optional; without input_claim no input is forwarded).
       claim: makes Remote Windows the current user source (renew: always a fresh epoch) and
       returns the epoch, 0 when user input is unavailable (automation owns it, no channel). */
    uint32_t (*input_claim)(void *ctx, int renew);
    uint32_t (*input_send)(void *ctx, uint32_t epoch, const InputPacket *packet);   /* 0: injected */
    void (*input_guard)(void *ctx, int on);                    /* a proxy window is the guest foreground */
    uint32_t (*input_hold)(void *ctx);                         /* hold other sources' keys now; hold id */
    void (*input_unhold)(void *ctx, uint32_t hold, int ok);    /* release (flush) or discard held keys */
    /* eh6.8 (optional): icon of an inventory entry that has no proxy (selector rows). */
    void (*inventory_icon)(void *ctx, const AsbRwWindowIcon *icon);
    /* eh6.9 (optional): ASB_RW_CAP_FILES_* this host offers (a drag broker is available), and
       "the guest took the pointer for a guest -> host drag: release the proxy's mouse capture". */
    uint32_t file_caps;
    void (*drag_capture)(void *ctx);
} AsbRwSessionOps;

typedef struct AsbRwHostOwnedMove {
    uint64_t surface_id;
    uint32_t serial;
    AsbRwRect rect;                 /* desired guest rectangle while unacknowledged */
} AsbRwHostOwnedMove;

typedef struct AsbRwHostProxy {
    uint64_t id;
    uint32_t state, error;          /* acknowledged selection state (ASB_RW_STATE_*) and last error */
    int created;
    AsbRwSurfaces surfaces;         /* last SURFACES (owned moves overlaid) */
    int have_surfaces;
    AsbRwWindowState window;        /* root WINDOW_STATE */
    int have_window;
    uint32_t request_serial_seen;
    uint32_t host_show;             /* proxy show state as last reported by the host */
    int sizing;                     /* host move/size loop active */
    int32_t want_w, want_h;         /* latest desired client size */
    uint32_t want_show;
    uint32_t sent_serial;           /* in flight when nonzero */
    uint64_t sent_tick;
    int32_t sent_w, sent_h;
    uint32_t sent_show;
    AsbRwRect acked;
    int have_ack;
    uint64_t close_id;              /* window with a CLOSE_REQUEST in flight (0 = none) */
    uint64_t close_tick;
    AsbRwHostOwnedMove moves[ASB_RW_HOST_MAX_OWNED_MOVES];
    AsbRwWindowIcon icon;
    int have_icon;
} AsbRwHostProxy;

typedef struct AsbRwHitEntry {
    uint64_t surface_id;
    int32_t x, y;
    uint32_t code;
    uint64_t tick;
} AsbRwHitEntry;

typedef struct AsbRwHitQuery {
    uint64_t surface_id;            /* in flight when nonzero */
    uint32_t serial;
    int32_t x, y;
    uint64_t tick;
    int queued;                     /* newer point waiting */
    int32_t qx, qy;
} AsbRwHitQuery;

/* Input target state (eh6.7). */
#define ASB_RW_INPUT_IDLE       0u   /* no proxy has host activation */
#define ASB_RW_INPUT_ACTIVATING 1u   /* ACTIVATE_REQUEST in flight; input queued */
#define ASB_RW_INPUT_ACTIVE     2u   /* guest foreground confirmed for `epoch` */
#define ASB_RW_INPUT_LOST       3u   /* needs a new activation before injecting */

/* Mouse event kinds for asb_rw_session_input_mouse. */
#define ASB_RW_MOUSE_MOVE  1u
#define ASB_RW_MOUSE_DOWN  2u
#define ASB_RW_MOUSE_UP    3u
#define ASB_RW_MOUSE_WHEEL 4u

typedef struct AsbRwInputEvent {
    uint32_t kind;                  /* ASB_RW_MOUSE_* or 0 = key */
    uint64_t root, surface;
    int32_t x, y, wheel;
    uint32_t button;                /* INPUT_BTN_* */
    uint32_t vk, scan, extended, up;
} AsbRwInputEvent;

/* Router state as seen by Remote Windows (asb_rw_session_input_changed). */
typedef struct AsbRwInputRouterState {
    uint32_t epoch;                 /* current user epoch */
    uint32_t windows_current;       /* Remote Windows is the current user source */
    uint32_t user_owns;             /* the user (not automation) owns input and the channel is up */
    uint32_t hold;                  /* keyboard of another source held for our guard (0 none) */
} AsbRwInputRouterState;

typedef struct AsbRwHostInput {
    uint32_t state;                 /* ASB_RW_INPUT_* */
    uint64_t host_root, host_surface;   /* proxy with host activation (0 = none) */
    uint64_t target;                /* surface the pending/confirmed activation names */
    uint32_t epoch;                 /* epoch authorizing injection (ACTIVE) */
    uint32_t request_epoch;         /* epoch sent with the pending activation */
    uint64_t request_tick;
    int guard;                      /* router told: a proxy window holds the guest foreground */
    uint32_t release_hold;          /* hold id of the ACTIVATE_REQUEST id 0 in flight */
    int release_pending;
    uint64_t release_tick;
    AsbRwInputRouterState router;   /* last router state */
    int have_router;
    uint32_t buttons;               /* held buttons (bit per INPUT_BTN_*) */
    uint8_t keys[ASB_RW_INPUT_KEYS];
    uint16_t key_vk[ASB_RW_INPUT_KEYS];
    AsbRwInputEvent queue[ASB_RW_INPUT_QUEUE];
    uint32_t queued;
    /* counters (diagnostics / tests) */
    uint64_t injected, dropped_stale, dropped_refused, dropped_overflow, activations, failures, released;
    uint32_t last_error;
} AsbRwHostInput;

/* Drag-and-drop pointer ownership (eh6.9). */
#define ASB_RW_HDRAG_IDLE       0u   /* the host forwards input normally */
#define ASB_RW_HDRAG_PROBING    1u   /* DRAG_PROBE sent; input suspended until the answer */
#define ASB_RW_HDRAG_TO_GUEST   2u   /* the guest helper drives the guest pointer (host drag) */
#define ASB_RW_HDRAG_FROM_GUEST 3u   /* guest drag handed to the host broker */
#define ASB_RW_HDRAG_SINK       4u   /* unsupported guest drag parked until the host button goes up */
#define ASB_RW_DRAG_PROBE_MS    3000u

typedef struct AsbRwHostDrag {
    uint32_t state;                 /* ASB_RW_HDRAG_* */
    uint32_t probe;                 /* serial of the current / last probe */
    uint32_t next_probe;
    uint32_t button;                /* logical button held at the probe */
    uint64_t session_id;            /* file session (TO_GUEST / FROM_GUEST) */
    uint32_t epoch;                 /* router epoch claimed for the drag */
    uint64_t tick;
    uint64_t probes, to_guest, from_guest, refused, cancelled;
    uint32_t last_error;
} AsbRwHostDrag;

#define ASB_RW_SESSION_HELLO   0u   /* waiting for the guest HELLO */
#define ASB_RW_SESSION_READY   1u   /* proxies supported */
#define ASB_RW_SESSION_LIMITED 2u   /* connected, but the guest lacks proxy capabilities */

typedef struct AsbRwSession {
    AsbRwSessionOps ops;
    uint32_t phase;
    AsbRwNegotiated negotiated;
    uint32_t guest_helper_version, guest_driver_version;
    uint32_t next_request, next_serial;
    AsbRwInventory inventory;       /* mirror of the guest inventory */
    AsbRwHostProxy proxies[ASB_RW_HOST_MAX_PROXIES];
    AsbRwHitEntry hits[ASB_RW_HOST_HIT_CACHE];
    uint32_t hit_next;
    AsbRwHitQuery queries[ASB_RW_HOST_MAX_PROXIES];
    uint64_t now_ms;
    AsbRwPayload rx, tx;
    uint8_t out[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD];
    int send_failed;
    AsbRwHostInput input;           /* eh6.7 */
    AsbRwHostDrag drag;             /* eh6.9 */
} AsbRwSession;

/* Heap-allocated (large). Sends the host HELLO. NULL on allocation failure. */
AsbRwSession *asb_rw_session_create(const AsbRwSessionOps *ops);
/* Destroys every proxy (selections end with the session) and frees the session. */
void asb_rw_session_destroy(AsbRwSession *s);

/* One complete control message. Nonzero: protocol violation (drop the connection). */
uint32_t asb_rw_session_receive(AsbRwSession *s, const uint8_t *bytes, size_t len);
/* Timers: retries unanswered geometry/close/hit requests. */
void asb_rw_session_tick(AsbRwSession *s, uint64_t now_ms);

/* Selector / API side. ASB_RW_ERR_UPGRADE_REQUIRED when the guest cannot host proxies. */
uint32_t asb_rw_session_select(AsbRwSession *s, uint64_t id, int select);
uint32_t asb_rw_session_restore_all(AsbRwSession *s);

/* Native proxy events. */
void asb_rw_session_resize(AsbRwSession *s, uint64_t id, uint32_t show, int32_t width, int32_t height);
void asb_rw_session_sizing(AsbRwSession *s, uint64_t id, int active);
/* An owned window's proxy moved: (dx, dy) is its offset from the root proxy's client origin. */
void asb_rw_session_move_owned(AsbRwSession *s, uint64_t id, uint64_t surface_id, int32_t dx, int32_t dy,
                               int32_t width, int32_t height);
/* Host Close on a root or owned proxy. */
void asb_rw_session_close(AsbRwSession *s, uint64_t id, uint64_t surface_id);
/* Guest hit-test code at (x, y) of a surface's visible frame (cached; HTCLIENT while unknown). */
uint32_t asb_rw_session_hit_test(AsbRwSession *s, uint64_t id, uint64_t surface_id, int32_t x, int32_t y);

/* Input (eh6.7). Host activation moved to a proxy (root, plus the owned surface that is active;
   0 = the root) or away from every proxy (root 0). */
void asb_rw_session_input_focus(AsbRwSession *s, uint64_t root, uint64_t surface);
/* Mouse event at (x, y) of a surface's visible frame (surface 0 = the root). */
void asb_rw_session_input_mouse(AsbRwSession *s, uint64_t root, uint64_t surface, uint32_t kind, uint32_t button,
                                int32_t x, int32_t y, int32_t wheel);
/* Key event for the proxy with host activation. Returns 0 when the key stays on the host. */
int asb_rw_session_input_key(AsbRwSession *s, uint32_t vk, uint32_t scan, int extended, int up);
/* The VM's router state changed (another user source, automation, epoch, hold). */
void asb_rw_session_input_changed(AsbRwSession *s, const AsbRwInputRouterState *state);
/* Host shell shortcuts that never reach the guest: Windows keys and anything while one is down,
   Alt+Tab, Alt+Esc, Ctrl+Esc, Alt+F4 (forwarded as a host close) and Print Screen. */
int asb_rw_input_key_is_local(uint32_t vk, int alt_down, int ctrl_down, int win_down);
/* Proxy surface point -> guest virtual-screen point through the acknowledged geometry.
   require_inside: the point must lie in the acknowledged rectangle. 0 when stale. */
int asb_rw_session_map_point(const AsbRwSession *s, uint64_t root, uint64_t surface, int32_t x, int32_t y,
                             int require_inside, int32_t *gx, int32_t *gy);

/* Drag and drop (eh6.9). The pointer left a proxy surface with `button` held: ask the guest to
   catch a guest OLE drag. ASB_RW_OK when sent (input forwarding is suspended until the answer). */
uint32_t asb_rw_session_drag_probe(AsbRwSession *s, uint64_t root, uint64_t surface, uint32_t button);
/* Nonzero while the guest helper owns (or may own) the guest pointer. */
int asb_rw_session_drag_busy(const AsbRwSession *s);
int asb_rw_session_files_enabled(const AsbRwSession *s, uint32_t direction_cap);

/* Lookups (NULL when absent). */
AsbRwHostProxy *asb_rw_session_proxy(AsbRwSession *s, uint64_t id);
const AsbRwInventoryEntry *asb_rw_session_entry(const AsbRwSession *s, uint64_t id);
/* The root whose current surfaces contain surface_id (0 when none). */
uint64_t asb_rw_session_root_of(const AsbRwSession *s, uint64_t surface_id);

#ifdef __cplusplus
}
#endif
#endif
