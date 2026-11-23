/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_drag_host
 *
 * @file      remote_windows_drag_host.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows host drag broker (AppSandbox-eh6.9): the host half of two-way, copy-only OLE
 * file drag and drop. Protocol: "Drag and drop" in asb_remote_windows.h.
 *
 * Why a separate process: AppSandbox.exe runs elevated, and UIPI blocks OLE drag and drop between
 * a medium-integrity Explorer and elevated windows in both directions (eh6.1 finding 3). The
 * broker (AppSandboxDrag.exe, started by the host app with the shell's medium token) therefore
 * owns every host OLE object and file channel 11; the elevated proxies only carry window
 * properties (ASB_RW_DRAG_COOKIE_PROP / _SURFACE_PROP) and arbitrate the guest pointer over
 * the control channel.
 *
 * Host -> guest: while a button is held over a proxy surface of this broker's manager and the
 * mouse is captured by another application (an OLE drag), an invisible overlay window covers
 * that surface; its IDropTarget relays enter/over/leave/drop to the guest (FILE_BEGIN /
 * FILE_POINTER / FILE_CANCEL) and answers with the guest target's FILE_FEEDBACK. The dragged
 * CF_HDROP items are validated at enter and walked in the background; after the drop they are
 * streamed (copy only: the host source is never asked to delete) and a progress window with
 * Cancel shows the transfer and the guest's FILE_RESULT.
 * Guest -> host: a guest drag offered by the helper (FILE_BEGIN FROM_GUEST) is staged privately
 * and a real host DoDragDrop starts at once over the staged items; releasing the button delivers
 * the drop only when staging is complete and verified (the drag waits, Escape cancels), then
 * FILE_RESULT tells the guest what the destination did.
 * Threads: UI (STA: overlay, drag loop, progress window), connection (channel 11 receive and
 * staging), sender (host -> guest stream).
 */
#ifndef REMOTE_WINDOWS_DRAG_HOST_H
#define REMOTE_WINDOWS_DRAG_HOST_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwDragHost AsbRwDragHost;

typedef struct AsbRwDragHostConfig {
    GUID vm_id;                     /* HCS runtime id (AF_HYPERV) */
    /* Optional transport override (tests: loopback TCP): connected SOCKET or INVALID_SOCKET. */
    UINT_PTR (*connect)(void *ctx, int channel, DWORD timeout_ms);
    void *ctx;
    void (*log)(void *ctx, const char *message);
    uint64_t cookie;                /* proxy windows to serve (ASB_RW_DRAG_COOKIE_PROP) */
    const wchar_t *stage_parent;    /* private staging parent */
    const wchar_t *guest_name;      /* progress text ("Copying to <name>") */
    uint32_t no_overlay;            /* tests: no overlay polling */
    uint32_t no_progress;           /* tests: no progress window */
    /* Optional (tests): proxy surface under a screen point and the screen origin of its client
       area; 0 = none. Default: the topmost proxy window carrying the cookie. */
    uint64_t (*surface_at)(void *ctx, POINT screen, POINT *origin);
    /* Optional (tests): replaces DoDragDrop of a guest -> host drag (IDataObject*, IDropSource*,
       DWORD* effect; returns an HRESULT). */
    long (*run_drag)(void *ctx, void *data, void *source, unsigned long *effect);
} AsbRwDragHostConfig;

typedef struct AsbRwDragHostStats {
    uint32_t connected, peer_directions, phase;
    uint64_t to_guest_ok, to_guest_failed, from_guest_ok, from_guest_failed, cancelled, rejected;
    uint32_t last_error;            /* AsbRwError of the last failed / rejected session */
    uint32_t last_effect;           /* DROPEFFECT of the last host DoDragDrop */
    uint64_t done_bytes, total_bytes;
    uint32_t overlay_shown;         /* overlay placements */
} AsbRwDragHostStats;

#define ASB_RW_DH_IDLE        0u
#define ASB_RW_DH_HOVER       1u   /* host drag over a proxy (to guest) */
#define ASB_RW_DH_SENDING     2u   /* to guest: streaming after the drop */
#define ASB_RW_DH_RECEIVING   3u   /* from guest: staging; the host drag runs */

AsbRwDragHost *asb_rw_drag_host_start(const AsbRwDragHostConfig *config);
void asb_rw_drag_host_stop(AsbRwDragHost *h);
void asb_rw_drag_host_stats(AsbRwDragHost *h, AsbRwDragHostStats *out);

#ifdef __cplusplus
}
#include <ole2.h>
/* The overlay's drop target (tests drive it directly; UI thread only). */
IDropTarget *asb_rw_drag_host_target(AsbRwDragHost *h);
/* Runs fn on the broker's UI thread (synchronously). */
void asb_rw_drag_host_invoke(AsbRwDragHost *h, void (*fn)(void *arg), void *arg);
#endif

#endif
