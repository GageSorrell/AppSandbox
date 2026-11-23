/**
 *
 *
 * @module AppSandbox/tools/transport/asb_rw_files
 *
 * @file      asb_rw_files.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows file-session receive rules (AppSandbox-eh6.9). See asb_rw_files.h. */
#include "asb_rw_files.h"

#include <stdlib.h>
#include <string.h>


/* Locale-independent simple case folding for the common scripts (NTFS compares names through
   its upcase table; anything this misses is still refused by CREATE_NEW on disk). */
static uint16_t fold(uint16_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 32);
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint16_t)(c + 32);
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (uint16_t)(c | 1);
    if (c >= 0x139 && c <= 0x148) return (uint16_t)((c & 1) ? c + 1 : c);
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return (uint16_t)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (uint16_t)(c + 32);
    if (c >= 0x400 && c <= 0x40F) return (uint16_t)(c + 80);
    if (c >= 0xFF21 && c <= 0xFF3A) return (uint16_t)(c + 32);
    return c;
}

uint64_t asb_rw_path_key(const uint16_t *rel, uint32_t len)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < len; ++i) {
        uint16_t c = rel[i] == '/' ? (uint16_t)'\\' : rel[i];
        c = fold(c);
        h ^= (uint8_t)c;
        h *= 1099511628211ull;
        h ^= (uint8_t)(c >> 8);
        h *= 1099511628211ull;
    }
    return h ? h : 1;   /* 0 marks an empty slot */
}

void asb_rw_file_rx_init(AsbRwFileRx *rx)
{
    memset(rx, 0, sizeof(*rx));
    rx->current = -1;
}

void asb_rw_file_rx_free(AsbRwFileRx *rx)
{
    free(rx->entries);
    free(rx->keys);
    free(rx->key_vals);
    asb_rw_file_rx_init(rx);
}

static uint32_t fail(AsbRwFileRx *rx, uint32_t error)
{
    if (rx->phase != ASB_RW_RX_FAILED) {
        rx->phase = ASB_RW_RX_FAILED;
        rx->error = error;
    }
    return error;
}

/* ---- key set: key -> entry index (+1) ---- */

static int set_resize(AsbRwFileRx *rx, uint32_t want_entries)
{
    uint32_t slots = 64;
    while (slots < want_entries * 2u + 16u) slots <<= 1;
    if (slots <= rx->key_slots) return 1;
    uint64_t *keys = (uint64_t *)calloc(slots, sizeof(uint64_t));
    uint32_t *vals = (uint32_t *)calloc(slots, sizeof(uint32_t));
    if (!keys || !vals) { free(keys); free(vals); return 0; }
    for (uint32_t i = 0; i < rx->key_slots; ++i) {
        if (!rx->keys[i]) continue;
        uint32_t j = (uint32_t)rx->keys[i] & (slots - 1);
        while (keys[j]) j = (j + 1) & (slots - 1);
        keys[j] = rx->keys[i];
        vals[j] = rx->key_vals[i];
    }
    free(rx->keys);
    free(rx->key_vals);
    rx->keys = keys;
    rx->key_vals = vals;
    rx->key_slots = slots;
    return 1;
}

static uint32_t set_find(const AsbRwFileRx *rx, uint64_t key)   /* entry index + 1, 0 = absent */
{
    if (!rx->key_slots) return 0;
    uint32_t j = (uint32_t)key & (rx->key_slots - 1);
    while (rx->keys[j]) {
        if (rx->keys[j] == key) return rx->key_vals[j];
        j = (j + 1) & (rx->key_slots - 1);
    }
    return 0;
}

static void set_add(AsbRwFileRx *rx, uint64_t key, uint32_t index)
{
    uint32_t j = (uint32_t)key & (rx->key_slots - 1);
    while (rx->keys[j]) j = (j + 1) & (rx->key_slots - 1);
    rx->keys[j] = key;
    rx->key_vals[j] = index + 1;
}

static int reserve(AsbRwFileRx *rx, uint32_t count)
{
    if (count <= rx->capacity) return set_resize(rx, count);
    AsbRwRxEntry *e = (AsbRwRxEntry *)realloc(rx->entries, (size_t)count * sizeof(*e));
    if (!e) return 0;
    rx->entries = e;
    rx->capacity = count;
    return set_resize(rx, count);
}

/* ---- messages ---- */

uint32_t asb_rw_file_rx_begin(AsbRwFileRx *rx, const AsbRwFileBegin *b)
{
    if (rx->phase != ASB_RW_RX_IDLE) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (!b->session_id || !b->entry_count || (b->direction != ASB_RW_DIR_TO_GUEST && b->direction != ASB_RW_DIR_FROM_GUEST))
        return fail(rx, ASB_RW_ERR_MALFORMED);
    if (b->entry_count > ASB_RW_MAX_TOP_ENTRIES) return fail(rx, ASB_RW_ERR_TOO_LARGE);
    if (!reserve(rx, b->entry_count)) return fail(rx, ASB_RW_ERR_LIMIT);
    rx->session_id = b->session_id;
    rx->direction = b->direction;
    rx->top_count = b->entry_count;
    rx->phase = ASB_RW_RX_TOP;
    return ASB_RW_OK;
}

static uint32_t parent_len(const uint16_t *rel, uint32_t len)
{
    for (uint32_t i = len; i > 0; --i)
        if (rel[i - 1] == '\\' || rel[i - 1] == '/') return i - 1;
    return 0;
}

uint32_t asb_rw_file_rx_entry(AsbRwFileRx *rx, const AsbRwFileEntry *e)
{
    if (rx->phase != ASB_RW_RX_TOP && rx->phase != ASB_RW_RX_STREAM) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (e->session_id != rx->session_id || e->index != rx->seen) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (e->kind != ASB_RW_ENTRY_FILE && e->kind != ASB_RW_ENTRY_DIR) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (e->kind == ASB_RW_ENTRY_DIR && e->size) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (!asb_rw_rel_path_safe(e->rel, e->rel_len)) return fail(rx, ASB_RW_ERR_FILE_REJECTED);
    if (rx->current >= 0) return fail(rx, ASB_RW_ERR_MALFORMED);   /* previous file incomplete */
    uint32_t parent = parent_len(e->rel, e->rel_len), top;
    int toplevel = rx->phase == ASB_RW_RX_TOP;
    if (toplevel != !parent) return fail(rx, ASB_RW_ERR_FILE_REJECTED);   /* top-level: one segment only */
    if (!toplevel) {
        if (rx->seen >= rx->entry_count) return fail(rx, ASB_RW_ERR_MALFORMED);
        uint32_t p = set_find(rx, asb_rw_path_key(e->rel, parent));
        if (!p || rx->entries[p - 1].kind != ASB_RW_ENTRY_DIR) return fail(rx, ASB_RW_ERR_FILE_REJECTED);   /* orphan */
        top = rx->entries[p - 1].top;
    } else {
        top = rx->seen;
    }
    uint64_t key = asb_rw_path_key(e->rel, e->rel_len);
    if (set_find(rx, key)) return fail(rx, ASB_RW_ERR_FILE_REJECTED);   /* duplicate / case alias */
    if (e->kind == ASB_RW_ENTRY_FILE) {
        if (e->size > UINT64_MAX - rx->declared_bytes) return fail(rx, ASB_RW_ERR_TOO_LARGE);
        if (!toplevel && rx->declared_bytes + e->size > rx->total_bytes) return fail(rx, ASB_RW_ERR_MALFORMED);
        rx->declared_bytes += e->size;
    }
    AsbRwRxEntry *slot = &rx->entries[rx->seen];
    memset(slot, 0, sizeof(*slot));
    slot->size = e->size;
    slot->kind = e->kind;
    slot->key = key;
    slot->top = top;
    set_add(rx, key, rx->seen);
    /* A nested file's bytes follow its entry at once: nothing else is accepted until it is
       complete (the receiver holds it open). */
    if (!toplevel && e->kind == ASB_RW_ENTRY_FILE && e->size) rx->current = rx->seen;
    rx->seen++;
    if (toplevel && rx->seen == rx->top_count) rx->phase = ASB_RW_RX_READY;
    return ASB_RW_OK;
}

uint32_t asb_rw_file_rx_stream(AsbRwFileRx *rx, const AsbRwFileStream *s)
{
    if (rx->phase != ASB_RW_RX_READY || s->session_id != rx->session_id) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (s->entry_count < rx->top_count) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (s->entry_count > ASB_RW_MAX_FILE_ENTRIES) return fail(rx, ASB_RW_ERR_TOO_LARGE);
    if (s->total_bytes < rx->declared_bytes) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (!reserve(rx, s->entry_count)) return fail(rx, ASB_RW_ERR_LIMIT);
    rx->entry_count = s->entry_count;
    rx->total_bytes = s->total_bytes;
    rx->phase = ASB_RW_RX_STREAM;
    return ASB_RW_OK;
}

uint32_t asb_rw_file_rx_data(AsbRwFileRx *rx, const AsbRwFileData *d)
{
    if (rx->phase != ASB_RW_RX_STREAM || d->session_id != rx->session_id) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (d->index >= rx->seen || !d->len) return fail(rx, ASB_RW_ERR_MALFORMED);
    AsbRwRxEntry *e = &rx->entries[d->index];
    if (e->kind != ASB_RW_ENTRY_FILE) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (rx->current < 0) {
        /* Only a top-level file (declared before STREAM) starts its data without an entry. */
        if (e->done || d->index >= rx->top_count) return fail(rx, ASB_RW_ERR_MALFORMED);
        rx->current = d->index;
    } else if ((uint32_t)rx->current != d->index) {
        return fail(rx, ASB_RW_ERR_MALFORMED);
    }
    if (d->offset != e->done || d->len > e->size - e->done) return fail(rx, ASB_RW_ERR_MALFORMED);
    e->done += d->len;
    rx->done_bytes += d->len;
    if (e->done == e->size) rx->current = -1;
    return ASB_RW_OK;
}

uint32_t asb_rw_file_rx_end(AsbRwFileRx *rx, const AsbRwFileStatus *end)
{
    if (rx->phase != ASB_RW_RX_STREAM || end->session_id != rx->session_id) return fail(rx, ASB_RW_ERR_MALFORMED);
    if (end->status) return fail(rx, end->status);
    if (rx->current >= 0 || rx->seen != rx->entry_count || rx->done_bytes != rx->total_bytes ||
        rx->declared_bytes != rx->total_bytes || end->done_bytes != rx->total_bytes || end->total_bytes != rx->total_bytes)
        return fail(rx, ASB_RW_ERR_MALFORMED);
    for (uint32_t i = 0; i < rx->seen; ++i)
        if (rx->entries[i].kind == ASB_RW_ENTRY_FILE && rx->entries[i].done != rx->entries[i].size)
            return fail(rx, ASB_RW_ERR_MALFORMED);
    rx->phase = ASB_RW_RX_COMPLETE;
    return ASB_RW_OK;
}

const AsbRwRxEntry *asb_rw_file_rx_at(const AsbRwFileRx *rx, uint32_t index)
{
    return index < rx->seen ? &rx->entries[index] : NULL;
}
