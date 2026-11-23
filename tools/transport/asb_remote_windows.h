/**
 *
 *
 * @module AppSandbox/tools/transport/asb_remote_windows
 *
 * @file      asb_remote_windows.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows: internal host <-> Windows-guest protocol and session contracts (AppSandbox-eh6.2).
 *
 * Shared, platform-neutral C used by BOTH endpoints (host core and the standard-user guest helper)
 * so they agree byte-for-byte on framing, capability negotiation, error codes and limits.
 * Internal only: nothing here is exposed through HTTP, MCP or the Python SDK.
 *
 * ---- Channels (Hyper-V socket service a5b0cafe-000N-4000-8000-000000000001, Windows guests only) ----
 *   9  ASB_CH_RW_CONTROL  HELLO, inventory, selection/state/geometry/activation/close, surfaces, errors
 *   10 ASB_CH_RW_FRAMES   per-surface frames (CPU BGRA bytes or shared GPU resources) and releases
 *   11 ASB_CH_RW_FILES    copy-only file sessions (bulk data never shares the control/input path)
 *   Channels 1..8 (agent, display, input, audio, clipboard x2, ssh, screenshot) are unchanged.
 *   Keyboard/mouse keep using the existing per-VM input channel (3); this protocol only carries
 *   activation/input-target acknowledgements.
 *
 * ---- Wire format (little-endian) ----
 *   Every message = 16-byte header + payload.
 *     u32 magic 'ASRW' | u16 wire_version (=1) | u16 type | u32 request_id | u32 payload_len
 *   Payload fields are encoded explicitly (no struct casts). Strings are u16 count + UTF-16LE code
 *   units, each with a per-field bound. Every decode is bounds-checked; trailing bytes are rejected.
 *   Per-channel payload limits are enforced from the header BEFORE any allocation.
 *
 * ---- Session and identity ----
 *   Each connection begins with HELLO in both directions. The guest's HELLO carries a fresh
 *   session tag; every window/surface ID the guest issues embeds that tag, a table slot and a
 *   per-slot serial. IDs are opaque to the host. The guest resolves an ID only if tag, slot and
 *   serial match AND the live window still has the same process ID, process start time and thread
 *   ID; anything else is ASB_RW_ERR_STALE_ID (destroyed window, HWND/process reuse, or an ID from a
 *   previous connection). Selections end with the session.
 */
#ifndef ASB_REMOTE_WINDOWS_H
#define ASB_REMOTE_WINDOWS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================ channels ================================ */
#define ASB_CH_RW_CONTROL 9
#define ASB_CH_RW_FRAMES  10
#define ASB_CH_RW_FILES   11

/* ================================ framing ================================= */
#define ASB_RW_MAGIC            0x57525341u   /* "ASRW" */
#define ASB_RW_WIRE_VERSION     1u
#define ASB_RW_HEADER_BYTES     16u

/* Protocol (semantic) versions negotiated in HELLO. */
#define ASB_RW_PROTOCOL_MIN     1u
#define ASB_RW_PROTOCOL_MAX     1u

/* Limits (bytes / counts). Shared so both sides reject the same inputs. */
#define ASB_RW_MAX_CONTROL_PAYLOAD (256u * 1024u)
#define ASB_RW_MAX_FILE_CHUNK      (1024u * 1024u)
#define ASB_RW_MAX_FILES_PAYLOAD   (ASB_RW_MAX_FILE_CHUNK + 64u)
#define ASB_RW_MAX_FRAME_WIDTH     7680u
#define ASB_RW_MAX_FRAME_HEIGHT    4320u
#define ASB_RW_MAX_FRAME_BYTES     (ASB_RW_MAX_FRAME_WIDTH * ASB_RW_MAX_FRAME_HEIGHT * 4u)
#define ASB_RW_MAX_FRAMES_PAYLOAD  (ASB_RW_MAX_FRAME_BYTES + 128u)
#define ASB_RW_MAX_INVENTORY       256u     /* roots in one snapshot */
#define ASB_RW_MAX_SURFACES        64u      /* associated surfaces per root */
#define ASB_RW_MAX_TITLE           256u     /* UTF-16 code units */
#define ASB_RW_MAX_APP_NAME        128u
#define ASB_RW_MAX_CLASS_NAME      64u
#define ASB_RW_MAX_REL_PATH        512u
#define ASB_RW_MAX_FILE_ENTRIES    100000u
#define ASB_RW_MAX_OWNER_DEPTH     8u
#define ASB_RW_MAX_WINDOWS         1024u    /* ID table slots */

/* ================================ message types =========================== */
typedef enum AsbRwType {
    /* control (channel 9) */
    ASB_RW_HELLO = 1,
    ASB_RW_ERROR = 2,                  /* reply to request_id: AsbRwError */
    ASB_RW_INVENTORY_REQUEST = 3,      /* host -> guest, empty: send a snapshot */
    ASB_RW_INVENTORY_SNAPSHOT = 4,     /* guest -> host */
    ASB_RW_INVENTORY_EVENT = 5,        /* guest -> host: added / changed / removed */
    ASB_RW_SELECT_REQUEST = 6,         /* host -> guest: AsbRwSelect (select or unselect) */
    ASB_RW_SELECT_RESULT = 7,          /* guest -> host: acknowledged state */
    ASB_RW_GEOMETRY_COMMAND = 8,       /* host -> guest */
    ASB_RW_GEOMETRY_ACK = 9,           /* guest -> host: applied geometry (may differ) */
    ASB_RW_ACTIVATE_REQUEST = 10,      /* host -> guest: focus + input target for an input epoch */
    ASB_RW_ACTIVATE_ACK = 11,          /* guest -> host */
    ASB_RW_CLOSE_REQUEST = 12,         /* host -> guest: SC_CLOSE (app keeps its prompts/cancel) */
    ASB_RW_CLOSE_RESULT = 13,
    ASB_RW_SURFACES = 14,              /* guest -> host: associated surfaces of a root */
    ASB_RW_RESTORE_ALL = 15,           /* either -> other, empty: end all selections */
    /* eh6.6 (only while ASB_RW_CAP_GEOMETRY is negotiated); see "Proxy windows" below */
    ASB_RW_HIT_TEST_REQUEST = 16,      /* host -> guest: AsbRwHitTest */
    ASB_RW_HIT_TEST_RESULT = 17,       /* guest -> host: AsbRwHitTest with the code */
    ASB_RW_WINDOW_STATE = 18,          /* guest -> host: AsbRwWindowState of a root / owned window */
    ASB_RW_WINDOW_ICON = 19,           /* guest -> host: AsbRwWindowIcon of a root */
    /* eh6.9 (only while ASB_RW_CAP_FILES_TO_GUEST or _FROM_GUEST is negotiated); see "Drag and drop" */
    ASB_RW_DRAG_PROBE = 20,            /* host -> guest: AsbRwDrag, the pointer left a proxy with a button held */
    ASB_RW_DRAG_STATE = 21,            /* guest -> host: AsbRwDrag, the helper drives / released the guest pointer */
    ASB_RW_DRAG_ACK = 22,              /* host -> guest: AsbRwDrag, allow / cancel / button released */
    /* frames (channel 10); see "Frame channel" below */
    ASB_RW_FRAME = 32,                 /* guest -> host: AsbRwFrameHeader + data_bytes */
    ASB_RW_FRAME_RELEASE = 33,         /* host -> guest: resource/slot may be reused */
    ASB_RW_FRAMES_HELLO = 34,          /* both, first message: binds the channel to the control session */
    ASB_RW_POOL_REQUEST = 35,          /* guest -> host: GPU slots for a surface generation (0 = none) */
    ASB_RW_POOL_SLOT = 36,             /* host -> guest: one shared texture (or a failure status) */
    ASB_RW_POOL_STATUS = 37,           /* guest -> host: pool opened (GPU) or not (CPU fallback) */
    ASB_RW_SURFACE_END = 38,           /* guest -> host: surface capture ended; free its resources */
    /* files (channel 11) */
    ASB_RW_FILE_BEGIN = 48,
    ASB_RW_FILE_ENTRY = 49,
    ASB_RW_FILE_DATA = 50,
    ASB_RW_FILE_END = 51,
    ASB_RW_FILE_PROGRESS = 52,
    ASB_RW_FILE_CANCEL = 53,
    ASB_RW_FILE_RESULT = 54,
    ASB_RW_FILES_HELLO = 55,           /* both, first message on channel 11 (eh6.9) */
    ASB_RW_FILE_POINTER = 56,          /* host -> guest: drag over / drop at a proxy surface point */
    ASB_RW_FILE_FEEDBACK = 57,         /* guest -> host: what the guest drop target accepts there */
    ASB_RW_FILE_STREAM = 58            /* sender -> receiver: the data phase begins (totals) */
} AsbRwType;

/* ================================ errors ================================== */
typedef enum AsbRwError {
    ASB_RW_OK = 0,
    ASB_RW_ERR_NEED_MORE = 1,          /* stream parser: incomplete message (not a failure) */
    ASB_RW_ERR_MALFORMED = 2,          /* bad magic/length/field/trailing bytes */
    ASB_RW_ERR_WIRE_VERSION = 3,       /* unknown header wire version */
    ASB_RW_ERR_UNSUPPORTED_VERSION = 4,/* no common protocol version */
    ASB_RW_ERR_UPGRADE_REQUIRED = 5,   /* peer lacks required capabilities (older helper/driver) */
    ASB_RW_ERR_UNKNOWN_TYPE = 6,
    ASB_RW_ERR_WRONG_CHANNEL = 7,
    ASB_RW_ERR_TOO_LARGE = 8,          /* exceeds a protocol limit */
    ASB_RW_ERR_BUFFER = 9,             /* caller buffer too small (encode) */
    ASB_RW_ERR_STALE_ID = 10,          /* destroyed, reused, or previous-session ID */
    ASB_RW_ERR_NOT_ELIGIBLE = 11,
    ASB_RW_ERR_ELEVATED = 12,
    ASB_RW_ERR_BUSY = 13,              /* transition already pending */
    ASB_RW_ERR_LIMIT = 14,             /* table/queue full */
    ASB_RW_ERR_PRIVATE_DISPLAY = 15,   /* private display unavailable */
    ASB_RW_ERR_SECURE_DESKTOP = 16,    /* lock/UAC: rendering and input paused */
    ASB_RW_ERR_FOREGROUND_DENIED = 17,
    ASB_RW_ERR_FILE_REJECTED = 18,     /* traversal, reparse point, virtual file, ... */
    ASB_RW_ERR_CANCELLED = 19,
    ASB_RW_ERR_INTERNAL = 20,
    ASB_RW_ERR_FILE_IO = 21,           /* eh6.9: read/write failed, disk full or a source changed */
    ASB_RW_ERR_UNSUPPORTED_DATA = 22,  /* eh6.9: the drag carries no ordinary files/folders */
    ASB_RW_ERR_DISCONNECTED = 23,      /* eh6.9: the guest/host connection ended */
    ASB_RW_ERR_COUNT
} AsbRwError;

const char *asb_rw_error_name(uint32_t code);   /* stable identifier, e.g. "stale_id" */
const char *asb_rw_error_text(uint32_t code);   /* actionable English message */

/* ================================ capabilities ============================ */
#define ASB_RW_CAP_INVENTORY        0x00000001u
#define ASB_RW_CAP_SELECTION        0x00000002u
#define ASB_RW_CAP_PRIVATE_DISPLAY  0x00000004u   /* guest driver provides the private display */
#define ASB_RW_CAP_FRAMES_CPU       0x00000008u
#define ASB_RW_CAP_FRAMES_GPU       0x00000010u
#define ASB_RW_CAP_POPUPS           0x00000020u
#define ASB_RW_CAP_GEOMETRY         0x00000040u
#define ASB_RW_CAP_ACTIVATION       0x00000080u
#define ASB_RW_CAP_FILES_TO_GUEST   0x00000100u
#define ASB_RW_CAP_FILES_FROM_GUEST 0x00000200u
#define ASB_RW_CAP_GUEST_SELECTOR   0x00000400u   /* guest tray flyout */
#define ASB_RW_CAP_KNOWN_MASK       0x000007FFu
/* Minimum set for sharing; anything less means "upgrade the guest components". */
#define ASB_RW_CAP_REQUIRED (ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | \
                             ASB_RW_CAP_FRAMES_CPU | ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY | \
                             ASB_RW_CAP_ACTIVATION)

#define ASB_RW_ROLE_HOST  1u
#define ASB_RW_ROLE_GUEST 2u

/* ================================ payloads ================================ */
typedef struct AsbRwRect { int32_t left, top, right, bottom; } AsbRwRect;

typedef struct AsbRwHello {
    uint16_t min_version, max_version;
    uint32_t role;
    uint32_t capabilities;
    uint32_t helper_version;    /* (major << 16) | (minor << 8) | patch */
    uint32_t driver_version;    /* guest VDD; 0 on the host */
    uint32_t session_tag;       /* guest: fresh per connection; host: 0 */
    uint32_t max_control_payload, max_frame_bytes, max_file_chunk;
} AsbRwHello;

typedef struct AsbRwNegotiated {
    uint32_t error;             /* AsbRwError; ASB_RW_OK when sharing may proceed */
    uint16_t version;           /* agreed protocol version */
    uint32_t capabilities;      /* intersection of known bits */
    uint32_t missing_required;  /* required bits the guest lacks */
    uint32_t session_tag;       /* guest session tag */
    uint32_t max_control_payload, max_frame_bytes, max_file_chunk;   /* min of both sides */
} AsbRwNegotiated;

typedef struct AsbRwErrorMsg { uint32_t code; uint32_t detail; } AsbRwErrorMsg;

#define ASB_RW_ENTRY_MINIMIZED       0x01u
#define ASB_RW_ENTRY_SELECTED        0x02u
#define ASB_RW_ENTRY_PENDING         0x04u
#define ASB_RW_ENTRY_DUPLICATE_TITLE 0x08u
#define ASB_RW_ENTRY_FOREGROUND      0x10u
#define ASB_RW_ENTRY_FLAGS_MASK      0x1Fu

typedef struct AsbRwInventoryEntry {
    uint64_t id;
    uint32_t pid;
    uint32_t flags;
    AsbRwRect rect;
    uint32_t dpi;
    uint16_t title_len, app_len;
    uint16_t title[ASB_RW_MAX_TITLE];
    uint16_t app[ASB_RW_MAX_APP_NAME];
} AsbRwInventoryEntry;

typedef struct AsbRwInventorySnapshot {
    uint32_t generation;        /* increments on every inventory change */
    uint32_t count;
    AsbRwInventoryEntry entries[ASB_RW_MAX_INVENTORY];
} AsbRwInventorySnapshot;

#define ASB_RW_EVENT_ADDED   1u
#define ASB_RW_EVENT_CHANGED 2u
#define ASB_RW_EVENT_REMOVED 3u
typedef struct AsbRwInventoryEvent { uint32_t generation, kind; AsbRwInventoryEntry entry; } AsbRwInventoryEvent;

typedef struct AsbRwSelect { uint64_t id; uint32_t select; } AsbRwSelect;

#define ASB_RW_STATE_NONE      0u
#define ASB_RW_STATE_PENDING   1u
#define ASB_RW_STATE_SHARED    2u
#define ASB_RW_STATE_RESTORING 3u
#define ASB_RW_STATE_FAILED    4u
typedef struct AsbRwSelectResult { uint64_t id; uint32_t state; uint32_t error; } AsbRwSelectResult;

/*
 * ---- Proxy windows (eh6.6; requires ASB_RW_CAP_GEOMETRY on both sides) ----
 * The host renders each shared root as a native proxy whose client area is exactly the root's
 * visible frame (DWMWA_EXTENDED_FRAME_BOUNDS, the area Windows Graphics Capture delivers), one
 * guest physical pixel per host physical pixel. The guest's own title bar - standard or custom -
 * is part of the captured content, so the host draws no caption of its own.
 *
 *   GEOMETRY_COMMAND  id = a shared root or an OWNED surface of one. show_state is the HOST
 *                     proxy state; rect is the desired visible frame in guest coordinates. Roots
 *                     keep their parked position and take the size (clamped to frame limits).
 *                     MINIMIZED never minimizes the source: it stays renderable and unchanged.
 *                     Owned windows take the position (and the size when resizable). The ack
 *                     echoes serial/show_state with the visible frame actually applied (the
 *                     application may enforce its own limits); BUSY = not responding, unchanged.
 *   CLOSE_REQUEST     posts SC_CLOSE to the window: the application keeps its prompts and may
 *                     cancel. CLOSE_RESULT OK = requested (the window may stay); BUSY = the root
 *                     is disabled by a modal owned window (the host activates that window).
 *   HIT_TEST_*        x/y relative to the surface's visible frame; the guest answers with the
 *                     WM_NCHITTEST code the application reports there (HTNOWHERE when stale).
 *   WINDOW_STATE      sent for shared roots and owned windows when anything changes: styles,
 *                     modal-disabled, tracking limits (visible-frame pixels, 0 = none), DPI, and
 *                     application-initiated minimize/maximize (request + request_serial). The
 *                     guest re-normalizes such a source at once so it stays renderable.
 *   WINDOW_ICON       the root's icon (premultiplied BGRA, at most ASB_RW_MAX_ICON square;
 *                     0 x 0 = no icon), sent on selection and when the icon changes.
 */
/*
 * ---- Input (eh6.7; ASB_RW_CAP_ACTIVATION) ----
 *   ACTIVATE_REQUEST  id = shared root or owned surface: make it the guest foreground for the
 *                     host input epoch; the ack has foreground = 1 only when it is. id = 0:
 *                     move the foreground off every shared window (ack id 0, foreground = 1
 *                     when done). The guest also sends unsolicited ACTIVATE_ACKs (request 0):
 *                     the activated family lost the foreground (epoch, foreground 0) or a
 *                     shared window took it without a request (epoch 0, foreground 1).
 *
 * ---- Selectors (eh6.8; ASB_RW_CAP_GUEST_SELECTOR on both sides) ----
 *   SELECT_REQUEST    also guest -> host: the guest tray flyout asks the host to select or
 *                     unselect; the host performs it as its own request (SELECT_REQUEST to the
 *                     guest, SELECT_RESULT as usual) or answers ERROR with the same request ID.
 *   WINDOW_ICON       also for inventory entries that are not shared (selector rows).
 *   RESTORE_ALL       also guest -> host when the guest tray's "Restore all windows" ran.
 */
#define ASB_RW_SHOW_NORMAL    0u
#define ASB_RW_SHOW_MINIMIZED 1u
#define ASB_RW_SHOW_MAXIMIZED 2u
typedef struct AsbRwGeometry {
    uint64_t id;
    uint32_t serial;            /* echoed by the ack; newer serials supersede older ones */
    uint32_t show_state;
    AsbRwRect rect;             /* client-visible frame in guest physical pixels */
    uint32_t dpi;
    uint32_t error;             /* ack only */
} AsbRwGeometry;

typedef struct AsbRwHitTest {
    uint64_t id;                /* root or surface ID */
    uint32_t serial;
    int32_t x, y;               /* relative to the surface's visible frame */
    uint32_t code;              /* result only: HT* value (0 = HTNOWHERE) */
} AsbRwHitTest;

#define ASB_RW_WS_DISABLED    0x01u   /* disabled (a modal owned window is up) */
#define ASB_RW_WS_RESIZABLE   0x02u   /* WS_THICKFRAME */
#define ASB_RW_WS_MINIMIZABLE 0x04u   /* WS_MINIMIZEBOX */
#define ASB_RW_WS_MAXIMIZABLE 0x08u   /* WS_MAXIMIZEBOX */
#define ASB_RW_WS_SYSMENU     0x10u
#define ASB_RW_WS_HUNG        0x20u   /* not responding */
#define ASB_RW_WS_FLAGS_MASK  0x3Fu
typedef struct AsbRwWindowState {
    uint64_t id;
    uint32_t flags;             /* ASB_RW_WS_* */
    uint32_t request;           /* ASB_RW_SHOW_* the application last asked for itself */
    uint32_t request_serial;    /* increments on every application-initiated min/max */
    uint32_t dpi;
    int32_t min_width, min_height, max_width, max_height;   /* visible frame; 0 = unconstrained */
} AsbRwWindowState;

#define ASB_RW_MAX_ICON 64u
typedef struct AsbRwWindowIcon {
    uint64_t id;
    uint32_t width, height;     /* both 0 or both 1..ASB_RW_MAX_ICON */
    uint8_t pixels[ASB_RW_MAX_ICON * ASB_RW_MAX_ICON * 4];   /* rows of width*4 bytes */
} AsbRwWindowIcon;

typedef struct AsbRwActivate {
    uint64_t id;
    uint32_t input_epoch;       /* host input epoch this activation authorizes */
    uint32_t foreground;        /* ack: 1 when the root (or its active owned window) is foreground */
    uint32_t error;             /* ack only */
} AsbRwActivate;

typedef struct AsbRwClose { uint64_t id; uint32_t error; } AsbRwClose;

#define ASB_RW_SURFACE_ROOT    1u
#define ASB_RW_SURFACE_OWNED   2u   /* owned dialog/window */
#define ASB_RW_SURFACE_MENU    3u
#define ASB_RW_SURFACE_TOOLTIP 4u
#define ASB_RW_SURFACE_POPUP   5u   /* other owned popup (flyout, XAML island popup, ...) */
typedef struct AsbRwSurface { uint64_t surface_id; uint32_t kind; uint32_t z; AsbRwRect rect; } AsbRwSurface;
typedef struct AsbRwSurfaces {
    uint64_t root_id;
    AsbRwRect root_rect;
    uint32_t count;             /* bottom-to-top z order */
    AsbRwSurface surfaces[ASB_RW_MAX_SURFACES];
} AsbRwSurfaces;

/*
 * ---- Frame channel (10) ----
 * The host connects after the control HELLO and sends FRAMES_HELLO with the control session tag;
 * the guest answers with its own (helper PID and capture adapter) or ERROR stale_id and closes.
 * GPU frames are used only when both sides allow them and the adapters match; otherwise CPU.
 *
 * Every surface (a selected root or one of its popups) is captured separately. Its frames carry
 * a generation that changes whenever its size class, device or pool changes; the host drops
 * (and releases) frames of older generations, so resize and device loss never show stale sizes.
 *
 * Slots bound memory: a frame occupies a slot until the host sends FRAME_RELEASE for exactly
 * that surface/generation/slot. The guest never has more than the agreed slots in flight per
 * surface (and never more than max_cpu_bytes of CPU pixels in total), keeps only the newest
 * captured frame while all slots are busy (superseded frames are discarded) and the host keeps
 * one pending + one displayed frame per surface, releasing a superseded pending frame at once.
 *
 * GPU (the validated Windows-guest texture path): for each surface generation the guest sends
 * POOL_REQUEST; the host creates `slots` BGRA keyed-mutex shared textures of the requested size,
 * duplicates each into the helper process (D3DKMTDuplicateHandle) and sends POOL_SLOT with the
 * guest handle. The guest opens them, validates size/format and answers POOL_STATUS (OK, or an
 * error after which that generation is CPU). Publish: guest AcquireSync(0), copy, ReleaseSync(1),
 * FRAME; host AcquireSync(1), read, ReleaseSync(0), FRAME_RELEASE. A frame the host releases unread
 * (superseded/stale) keeps key 1, so the guest acquires a released slot with key 0 or 1. A newer POOL_REQUEST, a
 * SURFACE_END or a disconnect retires a pool; the host frees retired textures.
 */
#define ASB_RW_FORMAT_BGRA8     1u
#define ASB_RW_TRANSPORT_CPU    1u   /* data_bytes of pixels follow */
#define ASB_RW_TRANSPORT_GPU    2u   /* frame is in pool texture `slot`; no pixel bytes */
#define ASB_RW_TRANSPORTS_MASK  3u
#define ASB_RW_FRAME_PREMULTIPLIED 0x1u  /* BGRA with premultiplied alpha (Windows Graphics Capture) */
#define ASB_RW_FRAME_FLAGS_MASK    0x1u
#define ASB_RW_MAX_FRAME_SLOTS  4u       /* per surface: CPU frames in flight / GPU pool textures */
#define ASB_RW_MAX_FRAME_SURFACES 128u   /* surfaces tracked by one frame channel */
typedef struct AsbRwFrameHeader {
    uint64_t surface_id, root_id, frame_seq;
    uint32_t generation;        /* nonzero; see above */
    uint32_t kind, z;           /* ASB_RW_SURFACE_*; z = bottom-to-top position among the root's surfaces */
    AsbRwRect rect;             /* surface frame on the guest (physical pixels, same space as SURFACES) */
    uint32_t width, height;     /* content size (GPU: <= pool size, top-left of the texture) */
    uint32_t stride, format, transport, slot, flags;
    uint32_t data_bytes;        /* CPU: stride * height; GPU: 0 */
} AsbRwFrameHeader;
typedef struct AsbRwFrameRelease { uint64_t surface_id, frame_seq; uint32_t generation, slot; } AsbRwFrameRelease;

typedef struct AsbRwFramesHello {
    uint32_t role, session_tag;   /* tag: the control session's (guest HELLO) tag */
    uint32_t process_id;          /* guest: helper PID (GPU pool target); host: 0 */
    uint32_t vendor_id, device_id;/* capture adapter (guest) / render adapter (host); 0 if none */
    uint32_t transports;          /* ASB_RW_TRANSPORT_* bits: guest available, host allowed */
    uint32_t slots;               /* frames in flight per surface, 1..ASB_RW_MAX_FRAME_SLOTS */
    uint32_t max_cpu_bytes;       /* unreleased CPU pixel bytes over all surfaces */
} AsbRwFramesHello;

typedef struct AsbRwFramesAgreement {
    uint32_t error;               /* ASB_RW_OK, STALE_ID (tag mismatch) or MALFORMED */
    uint32_t gpu;                 /* 1: GPU pools may be requested */
    uint32_t slots, max_cpu_bytes;/* minimum of both sides */
    uint32_t guest_pid;
} AsbRwFramesAgreement;

typedef struct AsbRwPoolRequest {
    uint64_t surface_id;
    uint32_t generation, width, height;
    uint32_t slots;               /* 0: no pool (this generation is CPU) */
} AsbRwPoolRequest;
typedef struct AsbRwPoolSlot {
    uint64_t surface_id;
    uint32_t generation, slot, slots, width, height;
    uint64_t handle;              /* handle valid in the helper process; 0 on failure */
    uint32_t status;              /* HRESULT; nonzero: no pool for this generation (use CPU) */
} AsbRwPoolSlot;
typedef struct AsbRwPoolStatus { uint64_t surface_id; uint32_t generation, status; } AsbRwPoolStatus;

#define ASB_RW_END_CLOSED      1u   /* window destroyed or capture item closed */
#define ASB_RW_END_UNSELECTED  2u   /* root unselected / popup gone */
#define ASB_RW_END_FAILED      3u   /* capture could not start or continue */
#define ASB_RW_END_SESSION     4u   /* control session ended */
typedef struct AsbRwSurfaceEnd { uint64_t surface_id; uint32_t generation, reason; } AsbRwSurfaceEnd;

/*
 * ---- Drag and drop (eh6.9; ASB_RW_CAP_FILES_TO_GUEST / _FROM_GUEST) ----
 * Copy only, both directions, real OLE drag loops on both sides. Only ordinary filesystem files
 * and folders (CF_HDROP) travel; virtual/generated items, links (name-surrogate reparse points),
 * offline/recall-on-access files, devices and unsafe names are rejected before anything moves.
 * Sources are never asked to delete: the guest catcher answers DROPEFFECT_NONE to the guest
 * source, and both drag sources offer DROPEFFECT_COPY only.
 *
 * Files travel on channel 11 into a private staging folder (asb_clipboard_temp) on the receiving
 * side; the destination application then copies from there with its own conflict handling.
 * A drop is only delivered once staging is complete and verified; incomplete staging is deleted,
 * completed staging is kept for the retention period (the target may copy asynchronously).
 *
 *   FILES_HELLO   both, first: role, version, directions offered, chunk limit, control session tag.
 *   FILE_BEGIN    sender: session, direction, target surface (to guest), entry_count = number of
 *                 TOP-LEVEL items (1..ASB_RW_MAX_TOP_ENTRIES), followed by exactly that many
 *                 FILE_ENTRY (index 0..n-1, a single path segment each). The receiver creates
 *                 placeholders for them so its drag can start before any data moved.
 *   FILE_POINTER  to guest, while the host drag hovers a proxy: surface + point relative to its
 *                 visible frame; action DROP ends the hover (the data phase follows).
 *   FILE_FEEDBACK to host: the effect the guest drop target reports at the latest point (COPY or
 *                 NONE with the reason), sent when it changes.
 *   FILE_STREAM   sender: the data phase begins: entry_count = ALL entries (top-level included),
 *                 total_bytes = sum of every file size. Then the remaining FILE_ENTRY records
 *                 (index n.., parents before children) and FILE_DATA: a file's bytes are sent in
 *                 order, completely, before the next entry or file; FILE_END closes the stream.
 *   FILE_PROGRESS receiver: bytes staged so far.
 *   FILE_CANCEL   either: the session ends now (leave, Escape, Cancel, error); staging is deleted.
 *   FILE_RESULT   whoever performed the final drop: OK (copied), FILE_REJECTED (the destination
 *                 refused), CANCELLED or an error.
 *
 * Host -> guest: the host drop target sends BEGIN + top-level entries when a host drag enters a
 * proxy; the guest asks for the pointer (DRAG_STATE ACTIVE TO_GUEST, host answers DRAG_ACK ALLOW
 * after claiming its input arbitration), presses on its private catcher window and runs a real
 * DoDragDrop, moving the guest pointer to each FILE_POINTER position. After DROP the host streams
 * the files; when staging is verified the guest releases the button over the target, which
 * receives a genuine Drop; FILE_RESULT reports what it did.
 * Guest -> host: when the pointer leaves a proxy with a button held, the host sends DRAG_PROBE;
 * the guest moves its pointer onto the catcher. If a guest OLE drag with files arrives there it
 * answers DRAG_STATE ACTIVE FROM_GUEST (the host forgets its held button) and streams BEGIN +
 * entries + data to the host broker, which runs the host DoDragDrop (its drop waits for complete
 * staging). FILE_RESULT ends it; the guest then releases its button on the catcher (the guest
 * source sees DROPEFFECT_NONE). A drag with nothing transferable is parked on the catcher
 * (DRAG_STATE SINK) until the host button goes up (DRAG_ACK RELEASE); no drag at all: NONE.
 */
#define ASB_RW_DIR_TO_GUEST   1u
#define ASB_RW_DIR_FROM_GUEST 2u
#define ASB_RW_ENTRY_FILE     1u
#define ASB_RW_ENTRY_DIR      2u
#define ASB_RW_MAX_TOP_ENTRIES 4096u     /* top-level items of one drag */
#define ASB_RW_FILES_VERSION   1u

/* AsbRwDrag.state */
#define ASB_RW_DRAG_ACTIVE  1u   /* STATE: the helper drives the guest pointer for this session */
#define ASB_RW_DRAG_ENDED   2u   /* STATE: the helper released the guest pointer; resume input */
#define ASB_RW_DRAG_NONE    3u   /* STATE: probe found no transferable drag; resume input */
#define ASB_RW_DRAG_SINK    4u   /* STATE: unsupported drag parked on the catcher until RELEASE */
#define ASB_RW_DRAG_ALLOW   5u   /* ACK: input arbitration claimed; the helper may drive the pointer */
#define ASB_RW_DRAG_CANCEL  6u   /* ACK: refused or taken over (error says why); end the session */
#define ASB_RW_DRAG_RELEASE 7u   /* ACK: the host button went up during PROBE / SINK */
typedef struct AsbRwDrag {
    uint64_t session_id;        /* channel 11 file session (0 when none) */
    uint64_t surface_id;        /* PROBE: surface the pointer left; ACTIVE TO_GUEST: target surface */
    uint32_t probe;             /* host probe serial; echoed by the answers to that probe */
    uint32_t direction;         /* ASB_RW_DIR_* (ACTIVE) */
    uint32_t state;             /* ASB_RW_DRAG_* (0 for PROBE) */
    uint32_t button;            /* PROBE: logical button held (INPUT_BTN_LEFT / _RIGHT) */
    uint32_t error;             /* AsbRwError */
} AsbRwDrag;

typedef struct AsbRwFilesHello {
    uint32_t role, version;
    uint32_t directions;        /* ASB_RW_DIR_* bits offered */
    uint32_t max_chunk;         /* <= ASB_RW_MAX_FILE_CHUNK */
    uint32_t session_tag;       /* guest: current control session tag; host: 0 */
} AsbRwFilesHello;

#define ASB_RW_POINTER_OVER 1u
#define ASB_RW_POINTER_DROP 2u
typedef struct AsbRwFilePointer {
    uint64_t session_id, surface_id;
    int32_t x, y;               /* relative to the surface's visible frame (1 host px = 1 guest px) */
    uint32_t action;            /* ASB_RW_POINTER_* */
} AsbRwFilePointer;

#define ASB_RW_EFFECT_NONE 0u
#define ASB_RW_EFFECT_COPY 1u
typedef struct AsbRwFileFeedback { uint64_t session_id; uint32_t effect, status; } AsbRwFileFeedback;

typedef struct AsbRwFileStream { uint64_t session_id; uint32_t entry_count; uint64_t total_bytes; } AsbRwFileStream;

typedef struct AsbRwFileBegin {
    uint64_t session_id, target_id;
    uint32_t direction, entry_count;   /* entry_count: top-level items */
    int32_t x, y;               /* first pointer position for ASB_RW_DIR_TO_GUEST */
    uint64_t total_bytes;       /* top-level file bytes (informational) */
} AsbRwFileBegin;
typedef struct AsbRwFileEntry {
    uint64_t session_id, size;
    uint32_t index, kind;
    uint16_t rel_len;
    uint16_t rel[ASB_RW_MAX_REL_PATH];   /* relative, '\\'-separated, validated by asb_rw_rel_path_safe */
} AsbRwFileEntry;
typedef struct AsbRwFileData {
    uint64_t session_id, offset;
    uint32_t index, len;
    const uint8_t *data;        /* decode: points into the caller's buffer */
} AsbRwFileData;
typedef struct AsbRwFileStatus {
    uint64_t session_id;
    uint32_t status;            /* AsbRwError */
    uint64_t done_bytes, total_bytes;
} AsbRwFileStatus;              /* FILE_END, FILE_PROGRESS, FILE_CANCEL, FILE_RESULT */

typedef struct AsbRwHeader { uint32_t magic; uint16_t wire_version, type; uint32_t request_id, payload_len; } AsbRwHeader;

typedef union AsbRwPayload {
    AsbRwHello hello;
    AsbRwErrorMsg error;
    AsbRwInventorySnapshot snapshot;
    AsbRwInventoryEvent event;
    AsbRwSelect select;
    AsbRwSelectResult select_result;
    AsbRwGeometry geometry;
    AsbRwActivate activate;
    AsbRwClose close;
    AsbRwHitTest hit_test;
    AsbRwWindowState window_state;
    AsbRwWindowIcon window_icon;
    AsbRwSurfaces surfaces;
    AsbRwFrameHeader frame;
    AsbRwFrameRelease frame_release;
    AsbRwFramesHello frames_hello;
    AsbRwPoolRequest pool_request;
    AsbRwPoolSlot pool_slot;
    AsbRwPoolStatus pool_status;
    AsbRwSurfaceEnd surface_end;
    AsbRwFileBegin file_begin;
    AsbRwFileEntry file_entry;
    AsbRwFileData file_data;
    AsbRwFileStatus file_status;
    AsbRwDrag drag;
    AsbRwFilesHello files_hello;
    AsbRwFilePointer file_pointer;
    AsbRwFileFeedback file_feedback;
    AsbRwFileStream file_stream;
} AsbRwPayload;

/* ================================ codec =================================== */
/* Channel a message type belongs to (0 for unknown types). */
int asb_rw_type_channel(uint16_t type);
/* Maximum payload accepted on a channel. */
uint32_t asb_rw_channel_max_payload(int channel);

/* Stream framing: inspect the first `len` received bytes of a message on `channel`.
   ASB_RW_OK: *total_len is the full message size (header + payload) and the header is valid.
   ASB_RW_ERR_NEED_MORE: fewer than 16 bytes; read more. Any other code: drop the connection.
   Never requires more than the header, so oversize lengths are rejected before allocation. */
uint32_t asb_rw_frame_check(int channel, const uint8_t *buf, size_t len, size_t *total_len);

/* Encodes one message (header + payload). For ASB_RW_FRAME and ASB_RW_FILE_DATA, the bulk bytes
   (frame pixels / file chunk) are NOT copied: the header's payload_len accounts for them and the
   caller sends them immediately after `out`. For FILE_DATA, msg->file_data.len gives that length. */
uint32_t asb_rw_encode(uint16_t type, uint32_t request_id, const AsbRwPayload *msg,
                       uint8_t *out, size_t out_cap, size_t *written);

/* Decodes a complete message received on `channel`. For FRAME, *bulk/*bulk_len give the pixel bytes;
   for FILE_DATA, msg->file_data.data points into `buf`. Payload must be fully consumed. */
uint32_t asb_rw_decode(int channel, const uint8_t *buf, size_t len, AsbRwHeader *header,
                       AsbRwPayload *msg, const uint8_t **bulk, size_t *bulk_len);

/* ================================ negotiation ============================= */
/* Each side builds its own HELLO with this, then calls asb_rw_negotiate(local, remote).
   Both endpoints compute the identical result from the two HELLOs. */
void asb_rw_hello_init(AsbRwHello *hello, uint32_t role, uint32_t capabilities,
                       uint32_t helper_version, uint32_t driver_version, uint32_t session_tag);
AsbRwNegotiated asb_rw_negotiate(const AsbRwHello *local, const AsbRwHello *remote);

/* Frame channel: both sides compute the same agreement from the two FRAMES_HELLOs.
   `session_tag` is the tag the caller's control session negotiated. GPU needs both sides to
   offer it, matching nonzero adapter IDs that are not the Microsoft Basic Render Driver. */
void asb_rw_frames_hello_init(AsbRwFramesHello *hello, uint32_t role, uint32_t session_tag,
                              uint32_t transports, uint32_t slots, uint32_t max_cpu_bytes);
AsbRwFramesAgreement asb_rw_frames_negotiate(const AsbRwFramesHello *local, const AsbRwFramesHello *remote,
                                             uint32_t session_tag);

/* ================================ identity / IDs ========================== */
typedef struct AsbRwIdentity {
    uint64_t hwnd;
    uint32_t pid, thread_id;
    uint64_t process_start;     /* FILETIME of process creation */
} AsbRwIdentity;

typedef struct AsbRwIdSlot { AsbRwIdentity identity; uint32_t serial; uint8_t in_use; } AsbRwIdSlot;
typedef struct AsbRwIdTable { uint32_t session_tag; uint32_t used; AsbRwIdSlot slots[ASB_RW_MAX_WINDOWS]; } AsbRwIdTable;

/* Re-reads the live identity of `hwnd`; returns 0 if the window no longer exists. */
typedef int (*AsbRwIdentityProbe)(void *ctx, uint64_t hwnd, AsbRwIdentity *live);

void asb_rw_ids_init(AsbRwIdTable *t, uint32_t session_tag);   /* new connection => new tag */
uint32_t asb_rw_ids_assign(AsbRwIdTable *t, const AsbRwIdentity *identity, uint64_t *id);
uint32_t asb_rw_ids_release(AsbRwIdTable *t, uint64_t id);
/* Validates an ID from the peer. On success *identity is the bound identity. Stale IDs whose
   window died or whose HWND now belongs to another process/thread are released. */
uint32_t asb_rw_ids_resolve(AsbRwIdTable *t, uint64_t id, AsbRwIdentityProbe probe, void *ctx,
                            AsbRwIdentity *identity);
uint32_t asb_rw_id_session_tag(uint64_t id);
/* The identity an ID is bound to in this table, without probing the live window (UI lookups
   only; requests must use asb_rw_ids_resolve). NULL when unknown or stale. */
const AsbRwIdentity *asb_rw_ids_peek(const AsbRwIdTable *t, uint64_t id);

/* ================================ eligibility ============================= */
/* Platform-neutral facts about one guest top-level window (filled by asb_remote_windows_win.c). */
#define ASB_RW_ELEVATION_NO      0u
#define ASB_RW_ELEVATION_YES     1u
#define ASB_RW_ELEVATION_UNKNOWN 2u
typedef struct AsbRwWindowFacts {
    AsbRwIdentity identity;
    uint32_t session_id, active_session_id;
    uint32_t style, ex_style;
    uint8_t visible, minimized, cloaked, elevation, is_helper, selected, foreground;
    uint8_t pending;                                 /* select/unselect transition in progress */
    uint64_t owner;                                  /* GW_OWNER, 0 if none */
    uint64_t owner_chain[ASB_RW_MAX_OWNER_DEPTH];    /* owner, owner's owner, ... */
    uint32_t owner_depth;
    uint64_t menu_owner;                             /* for "#32768": its thread's menu owner window */
    AsbRwRect rect;
    uint32_t dpi;
    uint16_t title_len, class_len, app_len;
    uint16_t title[ASB_RW_MAX_TITLE];
    uint16_t class_name[ASB_RW_MAX_CLASS_NAME];
    uint16_t app[ASB_RW_MAX_APP_NAME];
} AsbRwWindowFacts;

typedef enum AsbRwEligibility {
    ASB_RW_ELIGIBLE = 0,
    ASB_RW_INELIGIBLE_HIDDEN,
    ASB_RW_INELIGIBLE_CLOAKED,
    ASB_RW_INELIGIBLE_OWNED,          /* owned dialogs are surfaces of their root, never roots */
    ASB_RW_INELIGIBLE_TOOL_WINDOW,
    ASB_RW_INELIGIBLE_NOT_APP_WINDOW, /* neither caption nor WS_EX_APPWINDOW */
    ASB_RW_INELIGIBLE_UNTITLED,
    ASB_RW_INELIGIBLE_SHELL,
    ASB_RW_INELIGIBLE_HELPER,
    ASB_RW_INELIGIBLE_ELEVATED,
    ASB_RW_INELIGIBLE_ELEVATION_UNKNOWN,
    ASB_RW_INELIGIBLE_OTHER_SESSION
} AsbRwEligibility;

/* Win32 style bits used by the filter (kept here so the core stays platform-neutral). */
#define ASB_RW_WS_CAPTION        0x00C00000u
#define ASB_RW_WS_EX_TOOLWINDOW  0x00000080u
#define ASB_RW_WS_EX_APPWINDOW   0x00040000u
#define ASB_RW_WS_EX_NOACTIVATE  0x08000000u

AsbRwEligibility asb_rw_root_eligibility(const AsbRwWindowFacts *w);
int asb_rw_is_shell_class(const uint16_t *cls, uint16_t len);

/* Associated surface kind of `w` for a selected root, by OWNERSHIP only (owner chain, or the
   menu-owner chain for "#32768" menus). Returns 0 when not associated - including same-process
   windows with no ownership link. */
uint32_t asb_rw_association(const AsbRwWindowFacts *w, uint64_t root_hwnd);

/* Capture association (eh6.5): a visible, non-cloaked, non-empty window that is associated by
   ownership (asb_rw_association), or - because WinUI/XAML popup bridges, menus and tooltips are
   often unowned - a non-application popup of the root's own process lying on the private display.
   Captioned/APPWINDOW windows of the same process are independent roots and never associated;
   shadow-only windows ("SysShadow") are skipped. Returns the ASB_RW_SURFACE_* kind or 0. */
uint32_t asb_rw_surface_association(const AsbRwWindowFacts *w, const AsbRwWindowFacts *root,
                                    const AsbRwRect *private_rect);

/* Surfaces of one root from a top-first window list: the root itself (z 0) followed by its
   associated windows bottom-to-top, at most `max` (<= ASB_RW_MAX_SURFACES). Writes indices into
   `windows` and kinds; returns the count (0 when the root is not in the list). Windows already
   claimed by another shared root (`claimed[i]` nonzero) are skipped and newly claimed ones are
   marked, so an unowned popup belongs to exactly one root. */
/* One surface the guest helper asks its capture engine to capture (eh6.5). */
typedef struct AsbRwCaptureTarget {
    uint64_t surface_id, root_id;
    uint64_t hwnd;
    uint32_t kind, z;
    AsbRwRect rect;
} AsbRwCaptureTarget;

uint32_t asb_rw_collect_surfaces(const AsbRwWindowFacts *windows, uint32_t count, uint64_t root_hwnd,
                                 const AsbRwRect *private_rect, uint8_t *claimed,
                                 uint32_t *indices, uint32_t *kinds, uint32_t max);

/* ================================ inventory =============================== */
typedef struct AsbRwInventory {
    uint32_t generation;
    uint32_t count;
    AsbRwInventoryEntry entries[ASB_RW_MAX_INVENTORY];
} AsbRwInventory;

void asb_rw_inventory_init(AsbRwInventory *inv);
/* Rebuilds the inventory from the current top-level windows (in z order). Eligible roots and
   still-alive selected roots are kept; IDs are assigned through `ids` (stable per identity);
   ids of vanished windows are released. Emits ADDED/CHANGED/REMOVED events (bounded by
   max_events; *dropped counts events that did not fit, meaning a snapshot should be sent).
   Duplicate titles are flagged. Returns ASB_RW_ERR_LIMIT if more roots than ASB_RW_MAX_INVENTORY. */
uint32_t asb_rw_inventory_update(AsbRwInventory *inv, AsbRwIdTable *ids,
                                 const AsbRwWindowFacts *windows, uint32_t window_count,
                                 AsbRwInventoryEvent *events, uint32_t max_events,
                                 uint32_t *event_count, uint32_t *dropped);
void asb_rw_inventory_snapshot(const AsbRwInventory *inv, AsbRwInventorySnapshot *out);

/* Flags entries whose titles collide (case-insensitive). */
void asb_rw_mark_duplicates(AsbRwInventoryEntry *entries, uint32_t count);
/* Display label: "Title" or, for duplicates, "Title - App", plus " (n)" when title and app
   both collide (n is 1-based in z order). Returns code units written (NUL-terminated). */
uint32_t asb_rw_entry_label(const AsbRwInventoryEntry *entries, uint32_t count, uint32_t index,
                            uint16_t *out, uint32_t out_cap);

/* ================================ file paths ============================== */
/* Relative path from the peer: no root/drive/UNC/stream forms, no "." / ".." segments, no
   reserved characters or device names, no trailing dot/space segments, bounded length. */
int asb_rw_rel_path_safe(const uint16_t *rel, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* ASB_REMOTE_WINDOWS_H */
