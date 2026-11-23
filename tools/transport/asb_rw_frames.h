/**
 *
 *
 * @module AppSandbox/tools/transport/asb_rw_frames
 *
 * @file      asb_rw_frames.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows frame channel (10) flow control (AppSandbox-eh6.5).
 *
 * Platform-neutral state machines for both ends of one frame channel connection, so the guest
 * capture engine and the host receiver agree on the same rules and the rules are tested without
 * D3D or sockets (tools/tests/remote_windows_frames_test.c). Message semantics are described in
 * asb_remote_windows.h ("Frame channel").
 *
 *   Guest (AsbRwTx): one entry per captured surface. A captured frame first marks the surface
 *   pending (a newer capture supersedes an unsent one). Before sending, asb_rw_tx_prepare
 *   starts a new generation when the size class changes (any size change on CPU; outgrowing the
 *   pool, or shrinking below half of it, on GPU) and returns the POOL_REQUEST to send. A frame is
 *   sent only into a free slot of the current generation and within the CPU byte budget.
 *
 *   Host (AsbRwRx): validates every frame against the announced generation, pool and slot state,
 *   keeps one pending and one held (displayed) frame per surface and produces the FRAME_RELEASE
 *   for every frame exactly once: superseded pending frames, stale generations and consumer
 *   releases. Pools are retired by a newer generation, SURFACE_END or reset.
 *
 * Any violation (unknown surface, unannounced generation, slot reuse before release, CPU budget
 * overrun, GPU frame without a ready pool) is ASB_RW_ERR_MALFORMED: drop the connection.
 */
#ifndef ASB_RW_FRAMES_H
#define ASB_RW_FRAMES_H

#include "asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_POOL_NONE      0u   /* CPU generation (no pool requested) */
#define ASB_RW_POOL_REQUESTED 1u   /* guest: waiting for POOL_SLOT; host: textures offered */
#define ASB_RW_POOL_READY     2u
#define ASB_RW_POOL_FAILED    3u   /* this generation falls back to CPU */

#define ASB_RW_POOL_ALIGN     64u  /* GPU pool dimensions are rounded up to this */

/* ================================ guest =================================== */
typedef struct AsbRwTxSurface {
    uint64_t surface_id, root_id;
    uint32_t generation;            /* 0 until the first frame is prepared */
    uint32_t width, height;         /* content size of the current generation */
    uint32_t pool_w, pool_h, pool_slots, pool_state;
    uint32_t in_flight;             /* bitmask of slots awaiting FRAME_RELEASE */
    uint32_t slot_bytes[ASB_RW_MAX_FRAME_SLOTS];
    uint64_t next_seq;
    uint8_t in_use, pending;
    uint32_t sent, superseded, stale_releases;
} AsbRwTxSurface;

typedef struct AsbRwTx {
    AsbRwFramesAgreement agreement;
    uint64_t cpu_bytes;             /* unreleased CPU pixel bytes */
    uint32_t count;
    AsbRwTxSurface surfaces[ASB_RW_MAX_FRAME_SURFACES];
} AsbRwTx;

void asb_rw_tx_init(AsbRwTx *tx, const AsbRwFramesAgreement *agreement);
/* Index of a new surface (or the existing one); -1 when ASB_RW_MAX_FRAME_SURFACES are tracked. */
int  asb_rw_tx_add(AsbRwTx *tx, uint64_t surface_id, uint64_t root_id);
int  asb_rw_tx_find(const AsbRwTx *tx, uint64_t surface_id);
/* Forgets a surface (its in-flight slots stop counting; later releases are stale). Returns the
   SURFACE_END to send in *end and 1 if the host had been told about the surface, else 0. */
int  asb_rw_tx_remove(AsbRwTx *tx, int index, uint32_t reason, AsbRwSurfaceEnd *end);
/* A new capture is available: returns 1 if it superseded an unsent one. */
int  asb_rw_tx_captured(AsbRwTx *tx, int index);
/* Retires the current generation (device lost, pool failure, reconnect): the next prepare
   starts a new one. use_cpu forces the next generation to CPU. */
void asb_rw_tx_invalidate(AsbRwTx *tx, int index, int use_cpu);
/* Before sending a frame of width x height. Returns 1 when a new generation started and
   *request must be sent first (slots 0 = CPU), 0 otherwise. */
int  asb_rw_tx_prepare(AsbRwTx *tx, int index, uint32_t width, uint32_t height, AsbRwPoolRequest *request);
/* Pool result for a generation (all POOL_SLOTs opened, or a failure). Stale generations are
   ignored (returns 0); otherwise returns 1. A failure makes this generation CPU. */
int  asb_rw_tx_pool_result(AsbRwTx *tx, int index, uint32_t generation, int ok);
/* Free slot for the next frame (bytes: CPU payload, 0 for GPU), or -1 to keep it pending
   (all slots in flight, pool not ready yet, or the CPU budget is exhausted). */
int  asb_rw_tx_slot(const AsbRwTx *tx, int index, uint32_t bytes);
/* Records the frame as sent in `slot` and fills the generation/seq/slot/transport/size fields
   of *header (the caller sets kind, z, rect, flags, stride and data_bytes for CPU). */
void asb_rw_tx_commit(AsbRwTx *tx, int index, int slot, uint32_t bytes, AsbRwFrameHeader *header);
/* FRAME_RELEASE from the host. ASB_RW_OK (slot freed), ASB_RW_ERR_STALE_ID (old generation or
   removed surface; ignored) or ASB_RW_ERR_MALFORMED (current generation, slot not in flight). */
uint32_t asb_rw_tx_release(AsbRwTx *tx, const AsbRwFrameRelease *release);
/* GPU frames go through a ready pool; everything else is CPU. */
uint32_t asb_rw_tx_transport(const AsbRwTx *tx, int index);

/* ================================ host ==================================== */
#define ASB_RW_RX_FREE    0u
#define ASB_RW_RX_PENDING 1u
#define ASB_RW_RX_HELD    2u

typedef struct AsbRwRxSurface {
    uint64_t surface_id, root_id;
    uint32_t generation;
    uint32_t pool_w, pool_h, pool_slots, pool_state;
    uint8_t slot_state[ASB_RW_MAX_FRAME_SLOTS];
    uint32_t slot_bytes[ASB_RW_MAX_FRAME_SLOTS];
    int pending, held;              /* slot index or -1 */
    AsbRwFrameHeader pending_frame, held_frame;
    uint8_t in_use;
    uint32_t received, superseded, stale;
} AsbRwRxSurface;

typedef struct AsbRwRx {
    AsbRwFramesAgreement agreement;
    uint64_t cpu_bytes;             /* unreleased CPU pixel bytes held by the host */
    uint32_t count;
    AsbRwRxSurface surfaces[ASB_RW_MAX_FRAME_SURFACES];
} AsbRwRx;

/* Releases produced by one call (a frame can supersede one pending frame; acquire can retire
   one held frame). */
#define ASB_RW_RX_MAX_RELEASES 2u
typedef struct AsbRwRxReleases { uint32_t count; AsbRwFrameRelease items[ASB_RW_RX_MAX_RELEASES]; } AsbRwRxReleases;

void asb_rw_rx_init(AsbRwRx *rx, const AsbRwFramesAgreement *agreement);
int  asb_rw_rx_find(const AsbRwRx *rx, uint64_t surface_id);
/* POOL_REQUEST: creates the surface or starts its newer generation (older pending frames are
   released into *out; a held frame stays with the consumer). *index is the surface; *allocate
   is 1 when the host must create request->slots textures (GPU agreed and slots > 0).
   MALFORMED: generation not newer, GPU slots without a GPU agreement or more than agreed;
   LIMIT: too many surfaces. */
uint32_t asb_rw_rx_pool_request(AsbRwRx *rx, const AsbRwPoolRequest *request, int *index, int *allocate,
                                AsbRwRxReleases *out);
/* Host could not create/duplicate the pool: this generation is CPU. */
void asb_rw_rx_pool_failed(AsbRwRx *rx, int index, uint32_t generation);
/* POOL_STATUS from the guest. Stale generations return STALE_ID; *ready reports a usable pool. */
uint32_t asb_rw_rx_pool_status(AsbRwRx *rx, const AsbRwPoolStatus *status, int *index, int *ready);
/* FRAME header. ASB_RW_OK: stored as the surface's pending frame (*index set); STALE_ID: older
   generation, released into *out; MALFORMED/LIMIT: drop the connection. */
uint32_t asb_rw_rx_frame(AsbRwRx *rx, const AsbRwFrameHeader *frame, int *index, AsbRwRxReleases *out);
/* Consumer takes the pending frame (newest) as held; the previously held frame is released.
   Returns 1 and the frame when there was a pending frame. */
int  asb_rw_rx_acquire(AsbRwRx *rx, int index, AsbRwFrameHeader *frame, AsbRwRxReleases *out);
/* Consumer is done with the held frame (generation/slot must match it). */
int  asb_rw_rx_done(AsbRwRx *rx, int index, uint32_t generation, uint32_t slot, AsbRwRxReleases *out);
/* SURFACE_END: forgets the surface (no releases: the guest discarded it). Returns its index
   (the caller frees textures/buffers before the slot is reused) or -1 if unknown. */
int  asb_rw_rx_end(AsbRwRx *rx, const AsbRwSurfaceEnd *end);

#ifdef __cplusplus
}
#endif
#endif /* ASB_RW_FRAMES_H */
