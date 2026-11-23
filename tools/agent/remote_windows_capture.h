/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_capture
 *
 * @file      remote_windows_capture.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows guest capture engine (AppSandbox-eh6.5).
 *
 * Captures every surface the control thread asks for - each selected root and each associated
 * secondary window (owned dialogs, menus, tooltips, popups, including the ones outside the root's
 * bounds) - with its own Windows Graphics Capture session. The root session does not include
 * secondary windows (IncludeSecondaryWindows would only add them clipped to the root); popups are
 * separate surfaces with their own frames, alpha and z order. Sources are never minimized or
 * hidden: parked windows stay renderable on the private display, where they may overlap.
 *
 * One engine thread owns the WinRT capture objects, the frame channel (10) connection and the
 * send path. Frame-pool callbacks only copy the newest frame into the surface's mailbox texture
 * (a newer capture overwrites an unsent one). Frames go out through AsbRwTx: GPU pools
 * (keyed-mutex textures the host shares into this process) when agreed, otherwise BGRA CPU bytes.
 *
 * Device loss (DXGI_ERROR_DEVICE_REMOVED/RESET from any D3D call or the capture objects, or the
 * optional test event) stops every session, drops pools/mailboxes, recreates the D3D device and
 * restarts the sessions; every surface continues with a new generation. Window destruction
 * (capture item Closed) and removal from the requested set send SURFACE_END. A dropped frame
 * channel keeps the sessions; the newest frames are re-sent on the next connection.
 */
#ifndef REMOTE_WINDOWS_CAPTURE_H
#define REMOTE_WINDOWS_CAPTURE_H

#include <winsock2.h>
#include <windows.h>
#include "../transport/asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwCaptureConfig {
    void *ctx;
    /* A connected frame-channel socket, or INVALID_SOCKET after timeout_ms. */
    SOCKET (*accept)(void *ctx, DWORD timeout_ms);
    void (*close)(void *ctx, SOCKET s);
    void (*log)(void *ctx, const char *message);
    uint32_t allow_gpu;             /* 0: BGRA CPU frames only */
    uint32_t slots;                 /* frames in flight per surface offered to the host */
    uint32_t max_cpu_bytes;
    const wchar_t *device_loss_event; /* optional named auto-reset event: simulate device loss */
} AsbRwCaptureConfig;

typedef struct AsbRwCaptureStats {
    uint32_t connected, gpu_agreed, gpu_available, surfaces, capturing;
    uint32_t vendor_id, device_id;
    uint64_t captured, sent_gpu, sent_cpu, superseded, pools_opened, pool_failures;
    uint64_t capture_failures, surfaces_ended, device_losses, device_recoveries, connections;
} AsbRwCaptureStats;

typedef struct AsbRwCapture AsbRwCapture;

/* Starts the engine thread and creates the capture device (returns after that attempt).
   NULL when Windows Graphics Capture is unsupported or the thread could not start. */
AsbRwCapture *asb_rw_capture_start(const AsbRwCaptureConfig *config);
/* Control session the frame channel must belong to; 0 ends all surfaces and the connection. */
void asb_rw_capture_session(AsbRwCapture *c, uint32_t session_tag);
/* Complete set of surfaces for one root (n = 0 removes the root). Surfaces of other roots stay. */
void asb_rw_capture_set(AsbRwCapture *c, uint64_t root_id, const AsbRwCaptureTarget *targets, uint32_t n);
void asb_rw_capture_simulate_device_loss(AsbRwCapture *c);
void asb_rw_capture_stats(AsbRwCapture *c, AsbRwCaptureStats *stats);
void asb_rw_capture_stop(AsbRwCapture *c);

#ifdef __cplusplus
}
#endif
#endif
