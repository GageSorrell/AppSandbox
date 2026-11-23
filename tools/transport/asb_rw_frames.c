/**
 *
 *
 * @module AppSandbox/tools/transport/asb_rw_frames
 *
 * @file      asb_rw_frames.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows frame channel flow control (AppSandbox-eh6.5). See asb_rw_frames.h. */
#include "asb_rw_frames.h"

#include <string.h>

static uint32_t align_up(uint32_t v)
{
    uint32_t a = (v + ASB_RW_POOL_ALIGN - 1u) / ASB_RW_POOL_ALIGN * ASB_RW_POOL_ALIGN;
    return a;
}

static uint32_t clamp_dim(uint32_t v, uint32_t max) { return v > max ? max : v; }

/* ================================ guest =================================== */

void asb_rw_tx_init(AsbRwTx *tx, const AsbRwFramesAgreement *agreement)
{
    memset(tx, 0, sizeof(*tx));
    if (agreement) tx->agreement = *agreement;
}

int asb_rw_tx_find(const AsbRwTx *tx, uint64_t surface_id)
{
    if (!surface_id) return -1;
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i)
        if (tx->surfaces[i].in_use && tx->surfaces[i].surface_id == surface_id) return (int)i;
    return -1;
}

int asb_rw_tx_add(AsbRwTx *tx, uint64_t surface_id, uint64_t root_id)
{
    int found = asb_rw_tx_find(tx, surface_id);
    if (found >= 0 || !surface_id || !root_id) return found;
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i) {
        AsbRwTxSurface *s = &tx->surfaces[i];
        if (s->in_use) continue;
        memset(s, 0, sizeof(*s));
        s->in_use = 1;
        s->surface_id = surface_id;
        s->root_id = root_id;
        s->next_seq = 1;
        tx->count++;
        return (int)i;
    }
    return -1;
}

static void tx_drop_in_flight(AsbRwTx *tx, AsbRwTxSurface *s)
{
    for (uint32_t k = 0; k < ASB_RW_MAX_FRAME_SLOTS; ++k) {
        if (s->in_flight & (1u << k)) tx->cpu_bytes -= s->slot_bytes[k];
        s->slot_bytes[k] = 0;
    }
    s->in_flight = 0;
}

int asb_rw_tx_remove(AsbRwTx *tx, int index, uint32_t reason, AsbRwSurfaceEnd *end)
{
    if (index < 0 || index >= (int)ASB_RW_MAX_FRAME_SURFACES || !tx->surfaces[index].in_use) return 0;
    AsbRwTxSurface *s = &tx->surfaces[index];
    int announced = s->generation != 0;
    if (end) {
        memset(end, 0, sizeof(*end));
        end->surface_id = s->surface_id;
        end->generation = s->generation;
        end->reason = reason;
    }
    tx_drop_in_flight(tx, s);
    memset(s, 0, sizeof(*s));
    tx->count--;
    return announced;
}

int asb_rw_tx_captured(AsbRwTx *tx, int index)
{
    AsbRwTxSurface *s = &tx->surfaces[index];
    int superseded = s->pending;
    if (superseded) s->superseded++;
    s->pending = 1;
    return superseded;
}

void asb_rw_tx_invalidate(AsbRwTx *tx, int index, int use_cpu)
{
    AsbRwTxSurface *s = &tx->surfaces[index];
    if (!s->generation) return;
    tx_drop_in_flight(tx, s);
    /* Retire it now: releases the host still sends for it are stale, not protocol errors. The
       next prepare announces a newer generation (numbers may skip; they only need to grow). */
    s->generation++;
    if (!s->generation) s->generation = 1;
    s->width = s->height = 0;              /* forces a new generation on the next prepare */
    s->pool_w = s->pool_h = s->pool_slots = 0;
    s->pool_state = use_cpu ? ASB_RW_POOL_FAILED : ASB_RW_POOL_NONE;
}

int asb_rw_tx_prepare(AsbRwTx *tx, int index, uint32_t width, uint32_t height, AsbRwPoolRequest *request)
{
    AsbRwTxSurface *s = &tx->surfaces[index];
    int gpu = tx->agreement.gpu && s->pool_state != ASB_RW_POOL_FAILED;
    int fresh = !s->generation || !s->width;
    if (!fresh) {
        if (s->pool_slots) {
            /* Keep the pool while the content fits and is not much smaller. */
            if (width <= s->pool_w && height <= s->pool_h && width * 2u > s->pool_w - ASB_RW_POOL_ALIGN &&
                height * 2u > s->pool_h - ASB_RW_POOL_ALIGN) {
                s->width = width;
                s->height = height;
                return 0;
            }
        } else if (width == s->width && height == s->height) {
            return 0;
        }
    }
    /* New generation: frames of the previous one are retired (late releases are stale). */
    tx_drop_in_flight(tx, s);
    s->generation++;
    if (!s->generation) s->generation = 1;
    s->width = width;
    s->height = height;
    if (gpu) {
        s->pool_w = clamp_dim(align_up(width), ASB_RW_MAX_FRAME_WIDTH);
        s->pool_h = clamp_dim(align_up(height), ASB_RW_MAX_FRAME_HEIGHT);
        s->pool_slots = tx->agreement.slots;
        s->pool_state = ASB_RW_POOL_REQUESTED;
    } else {
        s->pool_w = s->pool_h = s->pool_slots = 0;
        if (s->pool_state != ASB_RW_POOL_FAILED) s->pool_state = ASB_RW_POOL_NONE;
    }
    if (request) {
        memset(request, 0, sizeof(*request));
        request->surface_id = s->surface_id;
        request->generation = s->generation;
        request->width = s->pool_w;
        request->height = s->pool_h;
        request->slots = s->pool_slots;
    }
    return 1;
}

int asb_rw_tx_pool_result(AsbRwTx *tx, int index, uint32_t generation, int ok)
{
    AsbRwTxSurface *s = &tx->surfaces[index];
    if (!s->in_use || generation != s->generation || s->pool_state != ASB_RW_POOL_REQUESTED) return 0;
    if (ok) {
        s->pool_state = ASB_RW_POOL_READY;
    } else {
        /* CPU for the rest of this generation; later generations try the GPU again only after
           asb_rw_tx_invalidate(..., 0) (e.g. device recovery). */
        s->pool_state = ASB_RW_POOL_FAILED;
        s->pool_slots = 0;
    }
    return 1;
}

uint32_t asb_rw_tx_transport(const AsbRwTx *tx, int index)
{
    return tx->surfaces[index].pool_state == ASB_RW_POOL_READY ? ASB_RW_TRANSPORT_GPU : ASB_RW_TRANSPORT_CPU;
}

int asb_rw_tx_slot(const AsbRwTx *tx, int index, uint32_t bytes)
{
    const AsbRwTxSurface *s = &tx->surfaces[index];
    if (!s->in_use || !s->generation || s->pool_state == ASB_RW_POOL_REQUESTED) return -1;
    uint32_t slots = s->pool_state == ASB_RW_POOL_READY ? s->pool_slots : tx->agreement.slots;
    if (s->pool_state != ASB_RW_POOL_READY && tx->cpu_bytes + bytes > tx->agreement.max_cpu_bytes) return -1;
    for (uint32_t k = 0; k < slots && k < ASB_RW_MAX_FRAME_SLOTS; ++k)
        if (!(s->in_flight & (1u << k))) return (int)k;
    return -1;
}

void asb_rw_tx_commit(AsbRwTx *tx, int index, int slot, uint32_t bytes, AsbRwFrameHeader *header)
{
    AsbRwTxSurface *s = &tx->surfaces[index];
    uint32_t transport = asb_rw_tx_transport(tx, index);
    if (transport == ASB_RW_TRANSPORT_GPU) bytes = 0;
    s->in_flight |= 1u << slot;
    s->slot_bytes[slot] = bytes;
    tx->cpu_bytes += bytes;
    s->pending = 0;
    s->sent++;
    header->surface_id = s->surface_id;
    header->root_id = s->root_id;
    header->generation = s->generation;
    header->frame_seq = s->next_seq++;
    header->slot = (uint32_t)slot;
    header->transport = transport;
    header->format = ASB_RW_FORMAT_BGRA8;
    header->width = s->width;
    header->height = s->height;
    if (transport == ASB_RW_TRANSPORT_GPU) {
        header->stride = 0;
        header->data_bytes = 0;
    }
}

uint32_t asb_rw_tx_release(AsbRwTx *tx, const AsbRwFrameRelease *release)
{
    int index = asb_rw_tx_find(tx, release->surface_id);
    if (index < 0) return ASB_RW_ERR_STALE_ID;            /* surface already ended */
    AsbRwTxSurface *s = &tx->surfaces[index];
    if (release->generation != s->generation) {
        if (release->generation > s->generation) return ASB_RW_ERR_MALFORMED;
        s->stale_releases++;
        return ASB_RW_ERR_STALE_ID;
    }
    if (release->slot >= ASB_RW_MAX_FRAME_SLOTS || !(s->in_flight & (1u << release->slot)))
        return ASB_RW_ERR_MALFORMED;
    s->in_flight &= ~(1u << release->slot);
    tx->cpu_bytes -= s->slot_bytes[release->slot];
    s->slot_bytes[release->slot] = 0;
    return ASB_RW_OK;
}

/* ================================ host ==================================== */

void asb_rw_rx_init(AsbRwRx *rx, const AsbRwFramesAgreement *agreement)
{
    memset(rx, 0, sizeof(*rx));
    if (agreement) rx->agreement = *agreement;
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i) rx->surfaces[i].pending = rx->surfaces[i].held = -1;
}

int asb_rw_rx_find(const AsbRwRx *rx, uint64_t surface_id)
{
    if (!surface_id) return -1;
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i)
        if (rx->surfaces[i].in_use && rx->surfaces[i].surface_id == surface_id) return (int)i;
    return -1;
}

static void push_release(AsbRwRxReleases *out, const AsbRwFrameHeader *f)
{
    if (!out || out->count >= ASB_RW_RX_MAX_RELEASES) return;
    AsbRwFrameRelease *r = &out->items[out->count++];
    memset(r, 0, sizeof(*r));
    r->surface_id = f->surface_id;
    r->frame_seq = f->frame_seq;
    r->generation = f->generation;
    r->slot = f->slot;
}

/* Frees a slot the host had stored. Held/pending frames of retired generations are tracked by
   the frame header alone (their slot array belongs to the new generation). */
static void rx_free_slot(AsbRwRx *rx, AsbRwRxSurface *s, const AsbRwFrameHeader *f)
{
    if (f->transport == ASB_RW_TRANSPORT_CPU) rx->cpu_bytes -= f->data_bytes;
    if (f->generation == s->generation && f->slot < ASB_RW_MAX_FRAME_SLOTS) {
        s->slot_state[f->slot] = ASB_RW_RX_FREE;
        s->slot_bytes[f->slot] = 0;
    }
}

uint32_t asb_rw_rx_pool_request(AsbRwRx *rx, const AsbRwPoolRequest *request, int *index, int *allocate,
                                AsbRwRxReleases *out)
{
    *index = -1;
    *allocate = 0;
    if (request->slots && (!rx->agreement.gpu || request->slots > rx->agreement.slots)) return ASB_RW_ERR_MALFORMED;
    int i = asb_rw_rx_find(rx, request->surface_id);
    if (i < 0) {
        for (uint32_t k = 0; k < ASB_RW_MAX_FRAME_SURFACES && i < 0; ++k)
            if (!rx->surfaces[k].in_use) i = (int)k;
        if (i < 0) return ASB_RW_ERR_LIMIT;
        AsbRwRxSurface *s = &rx->surfaces[i];
        memset(s, 0, sizeof(*s));
        s->in_use = 1;
        s->surface_id = request->surface_id;
        s->pending = s->held = -1;
        rx->count++;
    }
    AsbRwRxSurface *s = &rx->surfaces[i];
    if (request->generation <= s->generation) return ASB_RW_ERR_MALFORMED;
    if (s->pending >= 0) {   /* superseded by the new generation */
        push_release(out, &s->pending_frame);
        rx_free_slot(rx, s, &s->pending_frame);
        s->pending = -1;
        s->stale++;
    }
    /* A held frame keeps its texture/buffer until the consumer is done; its bookkeeping now
       lives only in held_frame. */
    s->generation = request->generation;
    memset(s->slot_state, 0, sizeof(s->slot_state));
    memset(s->slot_bytes, 0, sizeof(s->slot_bytes));
    s->pool_slots = request->slots;
    s->pool_w = request->slots ? request->width : 0;
    s->pool_h = request->slots ? request->height : 0;
    s->pool_state = request->slots ? ASB_RW_POOL_REQUESTED : ASB_RW_POOL_NONE;
    *index = i;
    *allocate = request->slots != 0;
    return ASB_RW_OK;
}

void asb_rw_rx_pool_failed(AsbRwRx *rx, int index, uint32_t generation)
{
    AsbRwRxSurface *s = &rx->surfaces[index];
    if (!s->in_use || s->generation != generation) return;
    s->pool_state = ASB_RW_POOL_FAILED;
    s->pool_slots = 0;
}

uint32_t asb_rw_rx_pool_status(AsbRwRx *rx, const AsbRwPoolStatus *status, int *index, int *ready)
{
    *ready = 0;
    *index = asb_rw_rx_find(rx, status->surface_id);
    if (*index < 0) return ASB_RW_ERR_MALFORMED;
    AsbRwRxSurface *s = &rx->surfaces[*index];
    if (status->generation < s->generation) return ASB_RW_ERR_STALE_ID;
    if (status->generation > s->generation) return ASB_RW_ERR_MALFORMED;
    if (s->pool_state == ASB_RW_POOL_FAILED) return ASB_RW_OK;     /* host side already failed */
    if (s->pool_state != ASB_RW_POOL_REQUESTED) return ASB_RW_ERR_MALFORMED;
    if (status->status) {
        s->pool_state = ASB_RW_POOL_FAILED;
        s->pool_slots = 0;
    } else {
        s->pool_state = ASB_RW_POOL_READY;
        *ready = 1;
    }
    return ASB_RW_OK;
}

uint32_t asb_rw_rx_frame(AsbRwRx *rx, const AsbRwFrameHeader *f, int *index, AsbRwRxReleases *out)
{
    *index = asb_rw_rx_find(rx, f->surface_id);
    if (*index < 0) return ASB_RW_ERR_MALFORMED;          /* never announced, or already ended */
    AsbRwRxSurface *s = &rx->surfaces[*index];
    if (s->root_id && s->root_id != f->root_id) return ASB_RW_ERR_MALFORMED;
    if (f->generation > s->generation) return ASB_RW_ERR_MALFORMED;
    if (f->generation < s->generation) {
        /* Sent before the guest's newer POOL_REQUEST was processed... cannot happen on one ordered
           stream, but a stale frame is harmless: release it without storing. */
        push_release(out, f);
        s->stale++;
        return ASB_RW_ERR_STALE_ID;
    }
    if (f->transport == ASB_RW_TRANSPORT_GPU) {
        if (s->pool_state != ASB_RW_POOL_READY || f->slot >= s->pool_slots || f->width > s->pool_w ||
            f->height > s->pool_h)
            return ASB_RW_ERR_MALFORMED;
    } else {
        if (s->pool_state == ASB_RW_POOL_READY || s->pool_state == ASB_RW_POOL_REQUESTED ||
            f->slot >= rx->agreement.slots)
            return ASB_RW_ERR_MALFORMED;
        if (rx->cpu_bytes + f->data_bytes > rx->agreement.max_cpu_bytes) return ASB_RW_ERR_MALFORMED;
    }
    if (s->slot_state[f->slot] != ASB_RW_RX_FREE) return ASB_RW_ERR_MALFORMED;   /* reused before release */
    s->root_id = f->root_id;
    if (s->pending >= 0) {
        push_release(out, &s->pending_frame);
        rx_free_slot(rx, s, &s->pending_frame);
        s->superseded++;
    }
    s->slot_state[f->slot] = ASB_RW_RX_PENDING;
    if (f->transport == ASB_RW_TRANSPORT_CPU) {
        s->slot_bytes[f->slot] = f->data_bytes;
        rx->cpu_bytes += f->data_bytes;
    }
    s->pending = (int)f->slot;
    s->pending_frame = *f;
    s->received++;
    return ASB_RW_OK;
}

int asb_rw_rx_acquire(AsbRwRx *rx, int index, AsbRwFrameHeader *frame, AsbRwRxReleases *out)
{
    AsbRwRxSurface *s = &rx->surfaces[index];
    if (!s->in_use || s->pending < 0) return 0;
    if (s->held >= 0) {
        push_release(out, &s->held_frame);
        rx_free_slot(rx, s, &s->held_frame);
    }
    s->held = s->pending;
    s->held_frame = s->pending_frame;
    s->pending = -1;
    if (s->held_frame.generation == s->generation) s->slot_state[s->held] = ASB_RW_RX_HELD;
    if (frame) *frame = s->held_frame;
    return 1;
}

int asb_rw_rx_done(AsbRwRx *rx, int index, uint32_t generation, uint32_t slot, AsbRwRxReleases *out)
{
    AsbRwRxSurface *s = &rx->surfaces[index];
    if (!s->in_use || s->held < 0 || s->held_frame.generation != generation || s->held_frame.slot != slot) return 0;
    push_release(out, &s->held_frame);
    rx_free_slot(rx, s, &s->held_frame);
    s->held = -1;
    return 1;
}

int asb_rw_rx_end(AsbRwRx *rx, const AsbRwSurfaceEnd *end)
{
    int i = asb_rw_rx_find(rx, end->surface_id);
    if (i < 0) return -1;
    AsbRwRxSurface *s = &rx->surfaces[i];
    if (s->pending >= 0 && s->pending_frame.transport == ASB_RW_TRANSPORT_CPU) rx->cpu_bytes -= s->pending_frame.data_bytes;
    if (s->held >= 0 && s->held_frame.transport == ASB_RW_TRANSPORT_CPU) rx->cpu_bytes -= s->held_frame.data_bytes;
    memset(s, 0, sizeof(*s));
    s->pending = s->held = -1;
    rx->count--;
    return i;
}
