/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_frames_host
 *
 * @file      remote_windows_frames_host.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows host frame receiver for one frame channel (10) connection (AppSandbox-eh6.5).
 *
 * Owns the connection after FRAMES_HELLO: a receive thread validates every message through
 * AsbRwRx (tools/transport/asb_rw_frames.h), creates GPU pools on request and keeps one pending
 * frame per surface. Consumers (the eh6.6 proxy renderer) pull the newest frame per surface:
 *
 *   GPU  frame.texture is the pool texture with its keyed mutex acquired (key 1) on the
 *        configured device; read/copy it, then asb_rw_frames_host_release (ReleaseSync(0) and
 *        FRAME_RELEASE). Pools reuse the validated Windows-guest path: BGRA keyed-mutex NT-shared
 *        textures duplicated into the guest helper with D3DKMTDuplicateHandle.
 *   CPU  frame.pixels/stride point at the received BGRA bytes, owned by the frame until release.
 *
 * Bounds: one pending + one consumer-held frame per surface, at most agreement.slots frames in
 * flight per surface, max_cpu_bytes of CPU pixels; superseded frames are released immediately.
 *
 * Failure: a protocol violation, socket error, guest SURFACE_END(SESSION) or host device loss
 * (texture creation/keyed mutex reports DXGI_ERROR_DEVICE_REMOVED/RESET, or the device's removed
 * reason is set) closes the connection and frees every pool and buffer; frames a consumer still
 * holds stay valid until it releases them. The owner reconnects (with a new device after a
 * device loss); the guest starts new generations on the new connection.
 *
 * vm_id GUID_NULL selects local mode for tests: pools are duplicated into guest_pid with
 * DuplicateHandle instead of D3DKMTDuplicateHandle.
 */
#ifndef REMOTE_WINDOWS_FRAMES_HOST_H
#define REMOTE_WINDOWS_FRAMES_HOST_H

#include <winsock2.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <d3d11.h>
#pragma warning(pop)
#include "../../tools/transport/asb_rw_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwFramesHost AsbRwFramesHost;

typedef struct AsbRwFramesHostConfig {
    GUID vm_id;                     /* HCS runtime id; GUID_NULL = local test mode */
    ID3D11Device *device;           /* render device for GPU pools; NULL = CPU only */
    uint32_t allow_gpu;             /* 0 forces BGRA CPU frames */
    uint32_t slots;                 /* frames in flight per surface (1..ASB_RW_MAX_FRAME_SLOTS) */
    uint32_t max_cpu_bytes;         /* unreleased CPU bytes over all surfaces */
    void *ctx;
    void (*log)(void *ctx, const char *message);
    void (*notify)(void *ctx, uint64_t surface_id);   /* receive thread: new frame/surface change */
} AsbRwFramesHostConfig;

#define ASB_RW_HOST_CONNECTING  0u
#define ASB_RW_HOST_RUNNING     1u
#define ASB_RW_HOST_CLOSED      2u  /* peer closed, protocol error or stop */
#define ASB_RW_HOST_DEVICE_LOST 3u  /* host render device removed: reconnect with a new device */

typedef struct AsbRwFramesHostStats {
    uint32_t state, error;          /* error: AsbRwError that closed the connection (0 = none) */
    AsbRwFramesAgreement agreement;
    uint32_t surfaces;
    uint64_t frames_gpu, frames_cpu, superseded, stale, releases_sent;
    uint64_t pools_created, pools_failed, pools_retired, surfaces_ended;
    uint64_t cpu_bytes;             /* unreleased CPU bytes right now */
    uint64_t textures_live;         /* pool textures alive right now (including consumer-held) */
} AsbRwFramesHostStats;

typedef struct AsbRwHostFrame {
    AsbRwFrameHeader header;
    ID3D11Texture2D *texture;       /* GPU: keyed mutex acquired; content at (0,0, width x height) */
    const uint8_t *pixels;          /* CPU: BGRA rows of header.stride bytes */
    /* private */
    void *buffer;
    size_t buffer_cap;
    IDXGIKeyedMutex *mutex;
} AsbRwHostFrame;

typedef struct AsbRwHostSurfaceInfo {
    uint64_t surface_id, root_id;
    uint32_t generation, transport, kind, z;
    AsbRwRect rect;                 /* from the newest frame */
    uint32_t width, height;
    uint64_t frames;
} AsbRwHostSurfaceInfo;

/* Connects to the guest's frame channel (AF_HYPERV, service of channel 10). INVALID_SOCKET on
   failure. WSAStartup must have been called. */
SOCKET asb_rw_frames_connect_vm(const GUID *vm_id, DWORD timeout_ms);

/* Takes ownership of `connected`, sends FRAMES_HELLO for `session_tag` (the control session's
   negotiated tag) and starts the receive thread. NULL only on allocation failure (the socket is
   closed). */
AsbRwFramesHost *asb_rw_frames_host_start(SOCKET connected, uint32_t session_tag, const AsbRwFramesHostConfig *config);

/* Newest frame of a surface: 1 = *frame filled (must be released), 0 = nothing new,
   -1 = the caller still holds this surface's previous frame or the surface is unknown. */
int  asb_rw_frames_host_acquire(AsbRwFramesHost *h, uint64_t surface_id, AsbRwHostFrame *frame);
void asb_rw_frames_host_release(AsbRwFramesHost *h, AsbRwHostFrame *frame);

uint32_t asb_rw_frames_host_surfaces(AsbRwFramesHost *h, AsbRwHostSurfaceInfo *out, uint32_t max);
void asb_rw_frames_host_stats(AsbRwFramesHost *h, AsbRwFramesHostStats *stats);
/* Test hook: behave exactly as if the render device had been removed. */
void asb_rw_frames_host_simulate_device_loss(AsbRwFramesHost *h);
/* Closes the connection, joins the thread and frees all pools/buffers not held by consumers.
   Frames still held by a consumer must be released before or after; release after stop only
   frees them. */
void asb_rw_frames_host_stop(AsbRwFramesHost *h);
/* Frees a frame acquired before asb_rw_frames_host_stop (no host involved). */
void asb_rw_frames_host_frame_free(AsbRwHostFrame *frame);

#ifdef __cplusplus
}
#endif
#endif
