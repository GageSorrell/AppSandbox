/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_files
 *
 * @file      remote_windows_files.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows guest drag-and-drop bridge (AppSandbox-eh6.9). Protocol: "Drag and drop" in
 * asb_remote_windows.h; file rules: remote_windows_files_win32.h.
 *
 * Threads
 *   I/O      serves file channel 11 (one host broker connection, only while a control session
 *            exists), validates and stages incoming files, maps FILE_POINTER positions through the
 *            published surface table and moves the guest pointer, and reports feedback/progress/
 *            results. Bulk data never touches the control thread or the input channel.
 *   OLE      (STA) owns the catcher, a tiny invisible topmost window on the private display that
 *            is both where the guest pointer is pressed to start a host -> guest DoDragDrop and
 *            the IDropTarget that catches a guest application's own drag for guest -> host.
 *   sender   per guest -> host session: walks the dragged folders and streams the files.
 *
 * Pointer ownership: the helper moves/presses the guest pointer only between DRAG_STATE ACTIVE
 * (after the host's DRAG_ACK ALLOW for host -> guest) or a DRAG_PROBE, and DRAG_STATE ENDED /
 * NONE; the host suspends its own forwarding meanwhile. Every exit path (drop, leave, Escape,
 * cancel, rejection, disconnect, control session end, stop) releases the button over the catcher
 * (or, for a delivered drop, over the target), so no button stays pressed.
 *
 * The clipboard is never read or written.
 */
#ifndef REMOTE_WINDOWS_FILES_H
#define REMOTE_WINDOWS_FILES_H

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include "../transport/asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwFiles AsbRwFiles;

typedef struct AsbRwFilesSurface {
    uint64_t surface_id;
    AsbRwRect rect;                 /* visible frame, guest virtual-screen pixels */
} AsbRwFilesSurface;

typedef struct AsbRwFilesConfig {
    void *ctx;
    void (*log)(void *ctx, const char *message);
    /* Queues a DRAG_STATE for the control thread to send (any thread; must not block). */
    void (*drag_state)(void *ctx, const AsbRwDrag *state);
    /* Channel 11: one connection (INVALID_SOCKET on timeout) and its close. */
    SOCKET (*accept)(void *ctx, DWORD timeout_ms);
    void (*close)(void *ctx, SOCKET s);
    /* Optional pointer hooks (tests); default SendInput in virtual-screen pixels. Buttons are
       logical INPUT_BTN_* (swapped mouse buttons honored by the default). */
    void (*inject_move)(void *ctx, int32_t x, int32_t y);
    void (*inject_button)(void *ctx, uint32_t button, int down);
    const wchar_t *stage_parent;    /* private staging parent folder */
    uint32_t directions;            /* ASB_RW_DIR_* bits served */
    uint32_t probe_ms;              /* catcher wait for a guest drag (default 1500) */
    /* Test hooks (optional; OLE thread). run_drag replaces DoDragDrop (IDataObject*, IDropSource*,
       DWORD* effect; returns an HRESULT); probe runs instead of moving the pointer onto the catcher
       and receives the catcher's IDropTarget*. */
    long (*run_drag)(void *ctx, void *data, void *source, unsigned long *effect);
    void (*probe)(void *ctx, void *catcher_target);
} AsbRwFilesConfig;

typedef struct AsbRwFilesStats {
    uint32_t connected;             /* a host broker is connected */
    uint32_t phase;                 /* ASB_RW_FILES_* of the current session */
    uint64_t session_id;
    uint64_t to_guest_ok, from_guest_ok, cancelled, rejected, failed, probes_none, probes_sink;
    uint32_t last_error;
    uint32_t last_effect;           /* DROPEFFECT of the last guest DoDragDrop */
    uint64_t staged_bytes;
} AsbRwFilesStats;

#define ASB_RW_FILES_IDLE      0u
#define ASB_RW_FILES_RECEIVING 1u   /* to guest: BEGIN .. last top-level entry */
#define ASB_RW_FILES_WAIT_ACK  2u   /* to guest: DRAG_STATE ACTIVE sent */
#define ASB_RW_FILES_HOVER     3u   /* to guest: guest DoDragDrop follows the host pointer */
#define ASB_RW_FILES_DROPPING  4u   /* to guest: host dropped; staging the data, then release */
#define ASB_RW_FILES_PROBING   5u   /* from guest: catcher waits for a guest drag */
#define ASB_RW_FILES_SENDING   6u   /* from guest: streaming to the host broker */
#define ASB_RW_FILES_SINK      7u   /* from guest: unsupported drag parked on the catcher */
#define ASB_RW_FILES_ENDING    8u   /* releasing the guest pointer */

AsbRwFiles *asb_rw_files_start(const AsbRwFilesConfig *config);
void asb_rw_files_stop(AsbRwFiles *f);
/* The control session started (its tag) or ended (0: every file session is cancelled and the
   channel 11 connection closed). */
void asb_rw_files_session(AsbRwFiles *f, uint32_t control_tag);
/* Surface table of the shared roots (control thread, when it changes) and the area where the
   catcher may be placed (the private display; empty = primary monitor). */
void asb_rw_files_surfaces(AsbRwFiles *f, const AsbRwFilesSurface *surfaces, uint32_t n, const RECT *catcher_area);
/* DRAG_PROBE or DRAG_ACK from the host (control thread). */
void asb_rw_files_control(AsbRwFiles *f, uint16_t type, const AsbRwDrag *msg);
void asb_rw_files_stats(AsbRwFiles *f, AsbRwFilesStats *out);

#ifdef __cplusplus
}
#endif
#endif
