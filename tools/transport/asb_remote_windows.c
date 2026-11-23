/* Remote Windows internal protocol and session contracts (AppSandbox-eh6.2). See asb_remote_windows.h. */
#include "asb_remote_windows.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>

/* ================================ errors ================================== */

static const struct { const char *name, *text; } k_errors[ASB_RW_ERR_COUNT] = {
    { "ok", "Success." },
    { "need_more", "The message is incomplete." },
    { "malformed", "The peer sent a malformed Remote Windows message." },
    { "wire_version", "The peer uses an unknown Remote Windows message format." },
    { "unsupported_version", "The guest and host Remote Windows protocol versions are incompatible. Update the guest components." },
    { "upgrade_required", "The guest helper or display driver is too old for shared windows. Update the guest components; the normal desktop keeps working." },
    { "unknown_type", "The peer sent an unknown Remote Windows message." },
    { "wrong_channel", "A Remote Windows message arrived on the wrong channel." },
    { "too_large", "A Remote Windows message exceeded a protocol limit." },
    { "buffer", "The output buffer is too small." },
    { "stale_id", "That window has closed or changed. Refresh the window list." },
    { "not_eligible", "That window cannot be shared." },
    { "elevated", "Administrator (elevated) applications cannot be shared." },
    { "busy", "A change to that window is already in progress." },
    { "limit", "Too many windows or pending operations." },
    { "private_display", "The private sharing display is unavailable." },
    { "secure_desktop", "The guest is locked or showing a secure prompt; sharing is paused." },
    { "foreground_denied", "Windows did not allow the shared window to take focus." },
    { "file_rejected", "A file or folder was rejected (unsafe path, link or virtual item)." },
    { "cancelled", "The operation was cancelled." },
    { "internal", "An internal Remote Windows error occurred." },
    { "file_io", "A file could not be read or written (disk full, access denied, or it changed during the copy)." },
    { "unsupported_data", "Only ordinary files and folders can be dragged between the host and the guest." },
    { "disconnected", "The connection between the host and the guest ended." },
};

const char *asb_rw_error_name(uint32_t code)
{
    return code < ASB_RW_ERR_COUNT ? k_errors[code].name : "unknown";
}

const char *asb_rw_error_text(uint32_t code)
{
    return code < ASB_RW_ERR_COUNT ? k_errors[code].text : "An unknown Remote Windows error occurred.";
}

/* ================================ channels ================================ */

int asb_rw_type_channel(uint16_t type)
{
    switch (type) {
    case ASB_RW_HELLO: case ASB_RW_ERROR: case ASB_RW_INVENTORY_REQUEST: case ASB_RW_INVENTORY_SNAPSHOT:
    case ASB_RW_INVENTORY_EVENT: case ASB_RW_SELECT_REQUEST: case ASB_RW_SELECT_RESULT:
    case ASB_RW_GEOMETRY_COMMAND: case ASB_RW_GEOMETRY_ACK: case ASB_RW_ACTIVATE_REQUEST:
    case ASB_RW_ACTIVATE_ACK: case ASB_RW_CLOSE_REQUEST: case ASB_RW_CLOSE_RESULT: case ASB_RW_SURFACES:
    case ASB_RW_RESTORE_ALL: case ASB_RW_HIT_TEST_REQUEST: case ASB_RW_HIT_TEST_RESULT: case ASB_RW_WINDOW_STATE:
    case ASB_RW_WINDOW_ICON: case ASB_RW_DRAG_PROBE: case ASB_RW_DRAG_STATE: case ASB_RW_DRAG_ACK:
        return ASB_CH_RW_CONTROL;
    case ASB_RW_FRAME: case ASB_RW_FRAME_RELEASE: case ASB_RW_FRAMES_HELLO: case ASB_RW_POOL_REQUEST:
    case ASB_RW_POOL_SLOT: case ASB_RW_POOL_STATUS: case ASB_RW_SURFACE_END:
        return ASB_CH_RW_FRAMES;
    case ASB_RW_FILE_BEGIN: case ASB_RW_FILE_ENTRY: case ASB_RW_FILE_DATA: case ASB_RW_FILE_END:
    case ASB_RW_FILE_PROGRESS: case ASB_RW_FILE_CANCEL: case ASB_RW_FILE_RESULT: case ASB_RW_FILES_HELLO:
    case ASB_RW_FILE_POINTER: case ASB_RW_FILE_FEEDBACK: case ASB_RW_FILE_STREAM:
        return ASB_CH_RW_FILES;
    }
    return 0;
}

uint32_t asb_rw_channel_max_payload(int channel)
{
    switch (channel) {
    case ASB_CH_RW_CONTROL: return ASB_RW_MAX_CONTROL_PAYLOAD;
    case ASB_CH_RW_FRAMES: return ASB_RW_MAX_FRAMES_PAYLOAD;
    case ASB_CH_RW_FILES: return ASB_RW_MAX_FILES_PAYLOAD;
    }
    return 0;
}

/* ================================ primitives ============================== */

typedef struct { uint8_t *p; size_t cap, len; int fail; } Writer;
typedef struct { const uint8_t *p; size_t len, pos; int fail; } Reader;

#define W_INVALID  1
#define W_OVERFLOW 2

static void w_bytes(Writer *w, const void *src, size_t n)
{
    if (w->fail) return;
    if (n > w->cap - w->len) { w->fail = W_OVERFLOW; return; }
    memcpy(w->p + w->len, src, n);
    w->len += n;
}
static void w_u16(Writer *w, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; w_bytes(w, b, 2); }
static void w_u32(Writer *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    w_bytes(w, b, 4);
}
static void w_u64(Writer *w, uint64_t v) { w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)(v >> 32)); }
static void w_i32(Writer *w, int32_t v) { w_u32(w, (uint32_t)v); }
static void w_rect(Writer *w, const AsbRwRect *r) { w_i32(w, r->left); w_i32(w, r->top); w_i32(w, r->right); w_i32(w, r->bottom); }
static void w_str(Writer *w, const uint16_t *s, uint16_t n, uint16_t max)
{
    if (n > max) { w->fail = 1; return; }
    w_u16(w, n);
    for (uint16_t i = 0; i < n; ++i) w_u16(w, s[i]);
}

static const uint8_t *r_take(Reader *r, size_t n)
{
    if (r->fail || n > r->len - r->pos) { r->fail = 1; return NULL; }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}
static uint16_t r_u16(Reader *r) { const uint8_t *b = r_take(r, 2); return b ? (uint16_t)(b[0] | (b[1] << 8)) : 0; }
static uint32_t r_u32(Reader *r)
{
    const uint8_t *b = r_take(r, 4);
    return b ? (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24) : 0;
}
static uint64_t r_u64(Reader *r) { uint64_t lo = r_u32(r); return lo | ((uint64_t)r_u32(r) << 32); }
static int32_t r_i32(Reader *r) { return (int32_t)r_u32(r); }
static void r_rect(Reader *r, AsbRwRect *out)
{
    out->left = r_i32(r); out->top = r_i32(r); out->right = r_i32(r); out->bottom = r_i32(r);
    /* Virtual-desktop coordinates are bounded; inverted rectangles are invalid. */
    if (out->right < out->left || out->bottom < out->top ||
        out->left < -65536 || out->top < -65536 || out->right > 65536 || out->bottom > 65536)
        r->fail = 1;
}
static void r_str(Reader *r, uint16_t *out, uint16_t *n, uint16_t max)
{
    uint16_t count = r_u16(r);
    if (count > max) { r->fail = 1; *n = 0; return; }
    for (uint16_t i = 0; i < count; ++i) out[i] = r_u16(r);
    *n = r->fail ? 0 : count;
}

/* ================================ framing ================================= */

uint32_t asb_rw_frame_check(int channel, const uint8_t *buf, size_t len, size_t *total_len)
{
    if (total_len) *total_len = 0;
    if (!buf || len < ASB_RW_HEADER_BYTES) return ASB_RW_ERR_NEED_MORE;
    Reader r = { buf, ASB_RW_HEADER_BYTES, 0, 0 };
    uint32_t magic = r_u32(&r);
    uint16_t wire = r_u16(&r), type = r_u16(&r);
    (void)r_u32(&r);
    uint32_t payload = r_u32(&r);
    if (magic != ASB_RW_MAGIC) return ASB_RW_ERR_MALFORMED;
    if (wire != ASB_RW_WIRE_VERSION) return ASB_RW_ERR_WIRE_VERSION;
    int ch = asb_rw_type_channel(type);
    if (!ch) return ASB_RW_ERR_UNKNOWN_TYPE;
    if (ch != channel && !(type == ASB_RW_ERROR && asb_rw_channel_max_payload(channel)))   /* ERROR: any channel */
        return ASB_RW_ERR_WRONG_CHANNEL;
    if (payload > asb_rw_channel_max_payload(channel)) return ASB_RW_ERR_TOO_LARGE;
    if (total_len) *total_len = ASB_RW_HEADER_BYTES + (size_t)payload;
    return ASB_RW_OK;
}

/* ================================ payload codecs ========================== */

static void enc_entry(Writer *w, const AsbRwInventoryEntry *e)
{
    w_u64(w, e->id); w_u32(w, e->pid); w_u32(w, e->flags); w_rect(w, &e->rect); w_u32(w, e->dpi);
    w_str(w, e->title, e->title_len, ASB_RW_MAX_TITLE);
    w_str(w, e->app, e->app_len, ASB_RW_MAX_APP_NAME);
}
static void dec_entry(Reader *r, AsbRwInventoryEntry *e)
{
    e->id = r_u64(r); e->pid = r_u32(r); e->flags = r_u32(r); r_rect(r, &e->rect); e->dpi = r_u32(r);
    r_str(r, e->title, &e->title_len, ASB_RW_MAX_TITLE);
    r_str(r, e->app, &e->app_len, ASB_RW_MAX_APP_NAME);
    if (!e->id || (e->flags & ~ASB_RW_ENTRY_FLAGS_MASK) || e->dpi > 1536) r->fail = 1;
}

static int rect_valid(const AsbRwRect *r)
{
    return r->right >= r->left && r->bottom >= r->top && r->left >= -65536 && r->top >= -65536 &&
           r->right <= 65536 && r->bottom <= 65536;
}

static int frame_shape_valid(const AsbRwFrameHeader *f)
{
    if (!f->surface_id || !f->root_id || !f->generation || !f->width || !f->height ||
        f->width > ASB_RW_MAX_FRAME_WIDTH || f->height > ASB_RW_MAX_FRAME_HEIGHT || f->format != ASB_RW_FORMAT_BGRA8 ||
        f->kind < ASB_RW_SURFACE_ROOT || f->kind > ASB_RW_SURFACE_POPUP || f->z >= ASB_RW_MAX_SURFACES ||
        (f->kind == ASB_RW_SURFACE_ROOT) != (f->surface_id == f->root_id) ||
        f->slot >= ASB_RW_MAX_FRAME_SLOTS || (f->flags & ~ASB_RW_FRAME_FLAGS_MASK) || !rect_valid(&f->rect))
        return 0;
    if (f->transport == ASB_RW_TRANSPORT_CPU)
        return f->stride >= f->width * 4u && f->stride <= f->width * 4u + 256u && !(f->stride & 3u) &&
               (uint64_t)f->stride * f->height == f->data_bytes;
    if (f->transport == ASB_RW_TRANSPORT_GPU) return f->data_bytes == 0 && f->stride == 0;
    return 0;
}

static int pool_dims_valid(uint32_t w, uint32_t h)
{
    return w && h && w <= ASB_RW_MAX_FRAME_WIDTH && h <= ASB_RW_MAX_FRAME_HEIGHT;
}

static int frames_hello_valid(const AsbRwFramesHello *m)
{
    return (m->role == ASB_RW_ROLE_HOST || m->role == ASB_RW_ROLE_GUEST) && m->session_tag &&
           !(m->transports & ~ASB_RW_TRANSPORTS_MASK) && (m->transports & ASB_RW_TRANSPORT_CPU) &&
           m->slots >= 1 && m->slots <= ASB_RW_MAX_FRAME_SLOTS && m->max_cpu_bytes >= 4096u &&
           (m->role == ASB_RW_ROLE_HOST ? m->process_id == 0 : m->process_id != 0);
}

static int pool_request_valid(const AsbRwPoolRequest *p)
{
    return p->surface_id && p->generation && p->slots <= ASB_RW_MAX_FRAME_SLOTS &&
           (!p->slots || pool_dims_valid(p->width, p->height));
}

static int pool_slot_valid(const AsbRwPoolSlot *p)
{
    return p->surface_id && p->generation && p->slots && p->slots <= ASB_RW_MAX_FRAME_SLOTS && p->slot < p->slots &&
           pool_dims_valid(p->width, p->height) && (!p->status) == (p->handle != 0);
}

static int surface_end_valid(const AsbRwSurfaceEnd *e)
{
    return e->surface_id && e->reason >= ASB_RW_END_CLOSED && e->reason <= ASB_RW_END_SESSION;
}

/* Hit-test points stay within one maximal frame of the surface (they may lie on its edge). */
static int hit_test_valid(const AsbRwHitTest *h)
{
    return h->id && h->x >= -(int32_t)ASB_RW_MAX_FRAME_WIDTH && h->x <= (int32_t)ASB_RW_MAX_FRAME_WIDTH &&
           h->y >= -(int32_t)ASB_RW_MAX_FRAME_HEIGHT && h->y <= (int32_t)ASB_RW_MAX_FRAME_HEIGHT && h->code <= 0xFFFFu;
}

static int track_valid(int32_t v, uint32_t limit) { return v >= 0 && v <= (int32_t)limit; }

static int window_state_valid(const AsbRwWindowState *s)
{
    return s->id && !(s->flags & ~ASB_RW_WS_FLAGS_MASK) && s->request <= ASB_RW_SHOW_MAXIMIZED && s->dpi <= 1536 &&
           track_valid(s->min_width, ASB_RW_MAX_FRAME_WIDTH) && track_valid(s->min_height, ASB_RW_MAX_FRAME_HEIGHT) &&
           track_valid(s->max_width, ASB_RW_MAX_FRAME_WIDTH) && track_valid(s->max_height, ASB_RW_MAX_FRAME_HEIGHT) &&
           (!s->max_width || s->max_width >= s->min_width) && (!s->max_height || s->max_height >= s->min_height);
}

static int icon_dims_valid(uint32_t w, uint32_t h)
{
    return (!w && !h) || (w && h && w <= ASB_RW_MAX_ICON && h <= ASB_RW_MAX_ICON);
}

/* eh6.9 drag/file validators (shared by encode and decode). */
static int drag_valid(uint16_t type, const AsbRwDrag *d)
{
    if (d->error >= ASB_RW_ERR_COUNT || d->button > 2) return 0;
    if (d->direction && d->direction != ASB_RW_DIR_TO_GUEST && d->direction != ASB_RW_DIR_FROM_GUEST) return 0;
    switch (type) {
    case ASB_RW_DRAG_PROBE: return d->surface_id && d->probe && !d->state && !d->session_id;
    case ASB_RW_DRAG_STATE:
        if (d->state < ASB_RW_DRAG_ACTIVE || d->state > ASB_RW_DRAG_SINK) return 0;
        return d->state != ASB_RW_DRAG_ACTIVE || (d->direction && d->session_id);
    case ASB_RW_DRAG_ACK: return d->state >= ASB_RW_DRAG_ALLOW && d->state <= ASB_RW_DRAG_RELEASE;
    }
    return 0;
}

static int files_hello_valid(const AsbRwFilesHello *h)
{
    return (h->role == ASB_RW_ROLE_HOST || h->role == ASB_RW_ROLE_GUEST) && h->version >= 1 &&
           !(h->directions & ~(ASB_RW_DIR_TO_GUEST | ASB_RW_DIR_FROM_GUEST)) && h->max_chunk >= 4096 &&
           h->max_chunk <= ASB_RW_MAX_FILE_CHUNK;
}

static int file_pointer_valid(const AsbRwFilePointer *p)
{
    return p->session_id && p->surface_id && p->x > -65536 && p->x < 65536 && p->y > -65536 && p->y < 65536 &&
           (p->action == ASB_RW_POINTER_OVER || p->action == ASB_RW_POINTER_DROP);
}

static void enc_payload(Writer *w, uint16_t type, const AsbRwPayload *m)
{
    switch (type) {
    case ASB_RW_HELLO:
        w_u16(w, m->hello.min_version); w_u16(w, m->hello.max_version); w_u32(w, m->hello.role);
        w_u32(w, m->hello.capabilities); w_u32(w, m->hello.helper_version); w_u32(w, m->hello.driver_version);
        w_u32(w, m->hello.session_tag); w_u32(w, m->hello.max_control_payload);
        w_u32(w, m->hello.max_frame_bytes); w_u32(w, m->hello.max_file_chunk);
        break;
    case ASB_RW_ERROR: w_u32(w, m->error.code); w_u32(w, m->error.detail); break;
    case ASB_RW_INVENTORY_REQUEST: case ASB_RW_RESTORE_ALL: break;
    case ASB_RW_INVENTORY_SNAPSHOT:
        if (m->snapshot.count > ASB_RW_MAX_INVENTORY) { w->fail = 1; break; }
        w_u32(w, m->snapshot.generation); w_u32(w, m->snapshot.count);
        for (uint32_t i = 0; i < m->snapshot.count; ++i) enc_entry(w, &m->snapshot.entries[i]);
        break;
    case ASB_RW_INVENTORY_EVENT:
        w_u32(w, m->event.generation); w_u32(w, m->event.kind); enc_entry(w, &m->event.entry);
        break;
    case ASB_RW_SELECT_REQUEST: w_u64(w, m->select.id); w_u32(w, m->select.select); break;
    case ASB_RW_SELECT_RESULT:
        w_u64(w, m->select_result.id); w_u32(w, m->select_result.state); w_u32(w, m->select_result.error);
        break;
    case ASB_RW_GEOMETRY_COMMAND: case ASB_RW_GEOMETRY_ACK:
        w_u64(w, m->geometry.id); w_u32(w, m->geometry.serial); w_u32(w, m->geometry.show_state);
        w_rect(w, &m->geometry.rect); w_u32(w, m->geometry.dpi); w_u32(w, m->geometry.error);
        break;
    case ASB_RW_ACTIVATE_REQUEST: case ASB_RW_ACTIVATE_ACK:
        w_u64(w, m->activate.id); w_u32(w, m->activate.input_epoch); w_u32(w, m->activate.foreground);
        w_u32(w, m->activate.error);
        break;
    case ASB_RW_CLOSE_REQUEST: case ASB_RW_CLOSE_RESULT: w_u64(w, m->close.id); w_u32(w, m->close.error); break;
    case ASB_RW_HIT_TEST_REQUEST: case ASB_RW_HIT_TEST_RESULT:
        if (!hit_test_valid(&m->hit_test)) { w->fail = 1; break; }
        w_u64(w, m->hit_test.id); w_u32(w, m->hit_test.serial); w_i32(w, m->hit_test.x); w_i32(w, m->hit_test.y);
        w_u32(w, m->hit_test.code);
        break;
    case ASB_RW_WINDOW_STATE: {
        const AsbRwWindowState *st = &m->window_state;
        if (!window_state_valid(st)) { w->fail = 1; break; }
        w_u64(w, st->id); w_u32(w, st->flags); w_u32(w, st->request); w_u32(w, st->request_serial); w_u32(w, st->dpi);
        w_i32(w, st->min_width); w_i32(w, st->min_height); w_i32(w, st->max_width); w_i32(w, st->max_height);
        break;
    }
    case ASB_RW_WINDOW_ICON: {
        const AsbRwWindowIcon *ic = &m->window_icon;
        if (!ic->id || !icon_dims_valid(ic->width, ic->height)) { w->fail = 1; break; }
        w_u64(w, ic->id); w_u32(w, ic->width); w_u32(w, ic->height);
        w_bytes(w, ic->pixels, (size_t)ic->width * ic->height * 4u);
        break;
    }
    case ASB_RW_SURFACES:
        if (m->surfaces.count > ASB_RW_MAX_SURFACES) { w->fail = 1; break; }
        w_u64(w, m->surfaces.root_id); w_rect(w, &m->surfaces.root_rect); w_u32(w, m->surfaces.count);
        for (uint32_t i = 0; i < m->surfaces.count; ++i) {
            const AsbRwSurface *s = &m->surfaces.surfaces[i];
            w_u64(w, s->surface_id); w_u32(w, s->kind); w_u32(w, s->z); w_rect(w, &s->rect);
        }
        break;
    case ASB_RW_FRAME: {
        const AsbRwFrameHeader *f = &m->frame;
        if (!frame_shape_valid(f)) { w->fail = 1; break; }
        w_u64(w, f->surface_id); w_u64(w, f->root_id); w_u64(w, f->frame_seq); w_u32(w, f->generation);
        w_u32(w, f->kind); w_u32(w, f->z); w_rect(w, &f->rect); w_u32(w, f->width); w_u32(w, f->height);
        w_u32(w, f->stride); w_u32(w, f->format); w_u32(w, f->transport); w_u32(w, f->slot); w_u32(w, f->flags);
        w_u32(w, f->data_bytes);
        break;
    }
    case ASB_RW_FRAME_RELEASE:
        if (!m->frame_release.surface_id || !m->frame_release.generation ||
            m->frame_release.slot >= ASB_RW_MAX_FRAME_SLOTS) { w->fail = 1; break; }
        w_u64(w, m->frame_release.surface_id); w_u64(w, m->frame_release.frame_seq);
        w_u32(w, m->frame_release.generation); w_u32(w, m->frame_release.slot);
        break;
    case ASB_RW_FRAMES_HELLO: {
        const AsbRwFramesHello *f = &m->frames_hello;
        if (!frames_hello_valid(f)) { w->fail = 1; break; }
        w_u32(w, f->role); w_u32(w, f->session_tag); w_u32(w, f->process_id); w_u32(w, f->vendor_id);
        w_u32(w, f->device_id); w_u32(w, f->transports); w_u32(w, f->slots); w_u32(w, f->max_cpu_bytes);
        break;
    }
    case ASB_RW_POOL_REQUEST: {
        const AsbRwPoolRequest *p = &m->pool_request;
        if (!pool_request_valid(p)) { w->fail = 1; break; }
        w_u64(w, p->surface_id); w_u32(w, p->generation); w_u32(w, p->width); w_u32(w, p->height); w_u32(w, p->slots);
        break;
    }
    case ASB_RW_POOL_SLOT: {
        const AsbRwPoolSlot *p = &m->pool_slot;
        if (!pool_slot_valid(p)) { w->fail = 1; break; }
        w_u64(w, p->surface_id); w_u32(w, p->generation); w_u32(w, p->slot); w_u32(w, p->slots);
        w_u32(w, p->width); w_u32(w, p->height); w_u64(w, p->handle); w_u32(w, p->status);
        break;
    }
    case ASB_RW_POOL_STATUS:
        if (!m->pool_status.surface_id || !m->pool_status.generation) { w->fail = 1; break; }
        w_u64(w, m->pool_status.surface_id); w_u32(w, m->pool_status.generation); w_u32(w, m->pool_status.status);
        break;
    case ASB_RW_SURFACE_END:
        if (!surface_end_valid(&m->surface_end)) { w->fail = 1; break; }
        w_u64(w, m->surface_end.surface_id); w_u32(w, m->surface_end.generation); w_u32(w, m->surface_end.reason);
        break;
    case ASB_RW_DRAG_PROBE: case ASB_RW_DRAG_STATE: case ASB_RW_DRAG_ACK:
        if (!drag_valid(type, &m->drag)) { w->fail = 1; break; }
        w_u64(w, m->drag.session_id); w_u64(w, m->drag.surface_id); w_u32(w, m->drag.probe);
        w_u32(w, m->drag.direction); w_u32(w, m->drag.state); w_u32(w, m->drag.button); w_u32(w, m->drag.error);
        break;
    case ASB_RW_FILES_HELLO:
        if (!files_hello_valid(&m->files_hello)) { w->fail = 1; break; }
        w_u32(w, m->files_hello.role); w_u32(w, m->files_hello.version); w_u32(w, m->files_hello.directions);
        w_u32(w, m->files_hello.max_chunk); w_u32(w, m->files_hello.session_tag);
        break;
    case ASB_RW_FILE_POINTER:
        if (!file_pointer_valid(&m->file_pointer)) { w->fail = 1; break; }
        w_u64(w, m->file_pointer.session_id); w_u64(w, m->file_pointer.surface_id); w_i32(w, m->file_pointer.x);
        w_i32(w, m->file_pointer.y); w_u32(w, m->file_pointer.action);
        break;
    case ASB_RW_FILE_FEEDBACK:
        if (!m->file_feedback.session_id || m->file_feedback.effect > ASB_RW_EFFECT_COPY ||
            m->file_feedback.status >= ASB_RW_ERR_COUNT) { w->fail = 1; break; }
        w_u64(w, m->file_feedback.session_id); w_u32(w, m->file_feedback.effect); w_u32(w, m->file_feedback.status);
        break;
    case ASB_RW_FILE_STREAM:
        if (!m->file_stream.session_id || !m->file_stream.entry_count ||
            m->file_stream.entry_count > ASB_RW_MAX_FILE_ENTRIES) { w->fail = 1; break; }
        w_u64(w, m->file_stream.session_id); w_u32(w, m->file_stream.entry_count); w_u64(w, m->file_stream.total_bytes);
        break;
    case ASB_RW_FILE_BEGIN:
        if (!m->file_begin.entry_count || m->file_begin.entry_count > ASB_RW_MAX_TOP_ENTRIES) { w->fail = 1; break; }
        w_u64(w, m->file_begin.session_id); w_u64(w, m->file_begin.target_id); w_u32(w, m->file_begin.direction);
        w_u32(w, m->file_begin.entry_count); w_i32(w, m->file_begin.x); w_i32(w, m->file_begin.y);
        w_u64(w, m->file_begin.total_bytes);
        break;
    case ASB_RW_FILE_ENTRY:
        if (!asb_rw_rel_path_safe(m->file_entry.rel, m->file_entry.rel_len)) { w->fail = 1; break; }
        w_u64(w, m->file_entry.session_id); w_u64(w, m->file_entry.size); w_u32(w, m->file_entry.index);
        w_u32(w, m->file_entry.kind); w_str(w, m->file_entry.rel, m->file_entry.rel_len, ASB_RW_MAX_REL_PATH);
        break;
    case ASB_RW_FILE_DATA:
        if (m->file_data.len > ASB_RW_MAX_FILE_CHUNK) { w->fail = 1; break; }
        w_u64(w, m->file_data.session_id); w_u64(w, m->file_data.offset); w_u32(w, m->file_data.index);
        w_u32(w, m->file_data.len);
        break;
    case ASB_RW_FILE_END: case ASB_RW_FILE_PROGRESS: case ASB_RW_FILE_CANCEL: case ASB_RW_FILE_RESULT:
        w_u64(w, m->file_status.session_id); w_u32(w, m->file_status.status);
        w_u64(w, m->file_status.done_bytes); w_u64(w, m->file_status.total_bytes);
        break;
    default:
        w->fail = 1;
    }
}

uint32_t asb_rw_encode(uint16_t type, uint32_t request_id, const AsbRwPayload *msg,
                       uint8_t *out, size_t out_cap, size_t *written)
{
    if (written) *written = 0;
    int channel = asb_rw_type_channel(type);
    if (!channel) return ASB_RW_ERR_UNKNOWN_TYPE;
    if (!out || out_cap < ASB_RW_HEADER_BYTES) return ASB_RW_ERR_BUFFER;
    static const AsbRwPayload empty = { { 0 } };
    if (!msg) msg = &empty;
    Writer w = { out, out_cap, ASB_RW_HEADER_BYTES, 0 };
    enc_payload(&w, type, msg);
    if (w.fail) return w.fail == W_OVERFLOW ? ASB_RW_ERR_BUFFER : ASB_RW_ERR_MALFORMED;
    uint64_t payload = w.len - ASB_RW_HEADER_BYTES;
    if (type == ASB_RW_FRAME) payload += msg->frame.data_bytes;
    if (type == ASB_RW_FILE_DATA) payload += msg->file_data.len;
    if (payload > asb_rw_channel_max_payload(channel)) return ASB_RW_ERR_TOO_LARGE;
    Writer h = { out, ASB_RW_HEADER_BYTES, 0, 0 };
    w_u32(&h, ASB_RW_MAGIC); w_u16(&h, ASB_RW_WIRE_VERSION); w_u16(&h, type);
    w_u32(&h, request_id); w_u32(&h, (uint32_t)payload);
    if (written) *written = w.len;
    return ASB_RW_OK;
}

static uint32_t dec_payload(Reader *r, uint16_t type, AsbRwPayload *m, size_t *bulk_len)
{
    *bulk_len = 0;
    switch (type) {
    case ASB_RW_HELLO:
        m->hello.min_version = r_u16(r); m->hello.max_version = r_u16(r); m->hello.role = r_u32(r);
        m->hello.capabilities = r_u32(r); m->hello.helper_version = r_u32(r); m->hello.driver_version = r_u32(r);
        m->hello.session_tag = r_u32(r); m->hello.max_control_payload = r_u32(r);
        m->hello.max_frame_bytes = r_u32(r); m->hello.max_file_chunk = r_u32(r);
        if (m->hello.role != ASB_RW_ROLE_HOST && m->hello.role != ASB_RW_ROLE_GUEST) r->fail = 1;
        break;   /* version ranges are judged by negotiation, so old peers get a precise error */
    case ASB_RW_ERROR:
        m->error.code = r_u32(r); m->error.detail = r_u32(r);
        break;
    case ASB_RW_INVENTORY_REQUEST: case ASB_RW_RESTORE_ALL: break;
    case ASB_RW_INVENTORY_SNAPSHOT:
        m->snapshot.generation = r_u32(r); m->snapshot.count = r_u32(r);
        if (m->snapshot.count > ASB_RW_MAX_INVENTORY) return ASB_RW_ERR_TOO_LARGE;
        for (uint32_t i = 0; i < m->snapshot.count && !r->fail; ++i) dec_entry(r, &m->snapshot.entries[i]);
        break;
    case ASB_RW_INVENTORY_EVENT:
        m->event.generation = r_u32(r); m->event.kind = r_u32(r); dec_entry(r, &m->event.entry);
        if (m->event.kind < ASB_RW_EVENT_ADDED || m->event.kind > ASB_RW_EVENT_REMOVED) r->fail = 1;
        break;
    case ASB_RW_SELECT_REQUEST:
        m->select.id = r_u64(r); m->select.select = r_u32(r);
        if (!m->select.id || m->select.select > 1) r->fail = 1;
        break;
    case ASB_RW_SELECT_RESULT:
        m->select_result.id = r_u64(r); m->select_result.state = r_u32(r); m->select_result.error = r_u32(r);
        if (m->select_result.state > ASB_RW_STATE_FAILED || m->select_result.error >= ASB_RW_ERR_COUNT) r->fail = 1;
        break;
    case ASB_RW_GEOMETRY_COMMAND: case ASB_RW_GEOMETRY_ACK:
        m->geometry.id = r_u64(r); m->geometry.serial = r_u32(r); m->geometry.show_state = r_u32(r);
        r_rect(r, &m->geometry.rect); m->geometry.dpi = r_u32(r); m->geometry.error = r_u32(r);
        if (!m->geometry.id || m->geometry.show_state > ASB_RW_SHOW_MAXIMIZED || m->geometry.dpi > 1536 ||
            m->geometry.error >= ASB_RW_ERR_COUNT)
            r->fail = 1;
        break;
    case ASB_RW_ACTIVATE_REQUEST: case ASB_RW_ACTIVATE_ACK:
        m->activate.id = r_u64(r); m->activate.input_epoch = r_u32(r); m->activate.foreground = r_u32(r);
        m->activate.error = r_u32(r);
        /* id 0 is valid: "move the foreground off every shared window" (eh6.7) and its ack. */
        if (m->activate.foreground > 1 || m->activate.error >= ASB_RW_ERR_COUNT) r->fail = 1;
        break;
    case ASB_RW_CLOSE_REQUEST: case ASB_RW_CLOSE_RESULT:
        m->close.id = r_u64(r); m->close.error = r_u32(r);
        if (!m->close.id || m->close.error >= ASB_RW_ERR_COUNT) r->fail = 1;
        break;
    case ASB_RW_HIT_TEST_REQUEST: case ASB_RW_HIT_TEST_RESULT:
        m->hit_test.id = r_u64(r); m->hit_test.serial = r_u32(r); m->hit_test.x = r_i32(r); m->hit_test.y = r_i32(r);
        m->hit_test.code = r_u32(r);
        if (!hit_test_valid(&m->hit_test) || (type == ASB_RW_HIT_TEST_REQUEST && m->hit_test.code)) r->fail = 1;
        break;
    case ASB_RW_WINDOW_STATE: {
        AsbRwWindowState *st = &m->window_state;
        st->id = r_u64(r); st->flags = r_u32(r); st->request = r_u32(r); st->request_serial = r_u32(r); st->dpi = r_u32(r);
        st->min_width = r_i32(r); st->min_height = r_i32(r); st->max_width = r_i32(r); st->max_height = r_i32(r);
        if (!window_state_valid(st)) r->fail = 1;
        break;
    }
    case ASB_RW_WINDOW_ICON: {
        AsbRwWindowIcon *ic = &m->window_icon;
        ic->id = r_u64(r); ic->width = r_u32(r); ic->height = r_u32(r);
        if (r->fail || !ic->id || !icon_dims_valid(ic->width, ic->height)) return ASB_RW_ERR_MALFORMED;
        size_t n = (size_t)ic->width * ic->height * 4u;
        const uint8_t *px = r_take(r, n);
        if (n && px) memcpy(ic->pixels, px, n);
        break;
    }
    case ASB_RW_SURFACES:
        m->surfaces.root_id = r_u64(r); r_rect(r, &m->surfaces.root_rect); m->surfaces.count = r_u32(r);
        if (m->surfaces.count > ASB_RW_MAX_SURFACES) return ASB_RW_ERR_TOO_LARGE;
        for (uint32_t i = 0; i < m->surfaces.count && !r->fail; ++i) {
            AsbRwSurface *s = &m->surfaces.surfaces[i];
            s->surface_id = r_u64(r); s->kind = r_u32(r); s->z = r_u32(r); r_rect(r, &s->rect);
            if (!s->surface_id || s->kind < ASB_RW_SURFACE_ROOT || s->kind > ASB_RW_SURFACE_POPUP) r->fail = 1;
        }
        if (!m->surfaces.root_id) r->fail = 1;
        break;
    case ASB_RW_FRAME: {
        AsbRwFrameHeader *f = &m->frame;
        f->surface_id = r_u64(r); f->root_id = r_u64(r); f->frame_seq = r_u64(r); f->generation = r_u32(r);
        f->kind = r_u32(r); f->z = r_u32(r); r_rect(r, &f->rect); f->width = r_u32(r); f->height = r_u32(r);
        f->stride = r_u32(r); f->format = r_u32(r); f->transport = r_u32(r); f->slot = r_u32(r); f->flags = r_u32(r);
        f->data_bytes = r_u32(r);
        if (r->fail || !frame_shape_valid(f)) return ASB_RW_ERR_MALFORMED;
        *bulk_len = f->data_bytes;
        break;
    }
    case ASB_RW_FRAME_RELEASE:
        m->frame_release.surface_id = r_u64(r); m->frame_release.frame_seq = r_u64(r);
        m->frame_release.generation = r_u32(r); m->frame_release.slot = r_u32(r);
        if (!m->frame_release.surface_id || !m->frame_release.generation ||
            m->frame_release.slot >= ASB_RW_MAX_FRAME_SLOTS) r->fail = 1;
        break;
    case ASB_RW_FRAMES_HELLO: {
        AsbRwFramesHello *f = &m->frames_hello;
        f->role = r_u32(r); f->session_tag = r_u32(r); f->process_id = r_u32(r); f->vendor_id = r_u32(r);
        f->device_id = r_u32(r); f->transports = r_u32(r); f->slots = r_u32(r); f->max_cpu_bytes = r_u32(r);
        if (!frames_hello_valid(f)) r->fail = 1;
        break;
    }
    case ASB_RW_POOL_REQUEST: {
        AsbRwPoolRequest *p = &m->pool_request;
        p->surface_id = r_u64(r); p->generation = r_u32(r); p->width = r_u32(r); p->height = r_u32(r); p->slots = r_u32(r);
        if (!pool_request_valid(p)) r->fail = 1;
        break;
    }
    case ASB_RW_POOL_SLOT: {
        AsbRwPoolSlot *p = &m->pool_slot;
        p->surface_id = r_u64(r); p->generation = r_u32(r); p->slot = r_u32(r); p->slots = r_u32(r);
        p->width = r_u32(r); p->height = r_u32(r); p->handle = r_u64(r); p->status = r_u32(r);
        if (!pool_slot_valid(p)) r->fail = 1;
        break;
    }
    case ASB_RW_POOL_STATUS:
        m->pool_status.surface_id = r_u64(r); m->pool_status.generation = r_u32(r); m->pool_status.status = r_u32(r);
        if (!m->pool_status.surface_id || !m->pool_status.generation) r->fail = 1;
        break;
    case ASB_RW_SURFACE_END:
        m->surface_end.surface_id = r_u64(r); m->surface_end.generation = r_u32(r); m->surface_end.reason = r_u32(r);
        if (!surface_end_valid(&m->surface_end)) r->fail = 1;
        break;
    case ASB_RW_FILE_BEGIN:
        m->file_begin.session_id = r_u64(r); m->file_begin.target_id = r_u64(r); m->file_begin.direction = r_u32(r);
        m->file_begin.entry_count = r_u32(r); m->file_begin.x = r_i32(r); m->file_begin.y = r_i32(r);
        m->file_begin.total_bytes = r_u64(r);
        if (!m->file_begin.session_id || (m->file_begin.direction != ASB_RW_DIR_TO_GUEST &&
            m->file_begin.direction != ASB_RW_DIR_FROM_GUEST) || !m->file_begin.entry_count)
            r->fail = 1;
        if (m->file_begin.entry_count > ASB_RW_MAX_TOP_ENTRIES) return ASB_RW_ERR_TOO_LARGE;
        if (m->file_begin.direction == ASB_RW_DIR_TO_GUEST && !m->file_begin.target_id) r->fail = 1;
        break;
    case ASB_RW_DRAG_PROBE: case ASB_RW_DRAG_STATE: case ASB_RW_DRAG_ACK:
        m->drag.session_id = r_u64(r); m->drag.surface_id = r_u64(r); m->drag.probe = r_u32(r);
        m->drag.direction = r_u32(r); m->drag.state = r_u32(r); m->drag.button = r_u32(r); m->drag.error = r_u32(r);
        if (!r->fail && !drag_valid(type, &m->drag)) r->fail = 1;
        break;
    case ASB_RW_FILES_HELLO:
        m->files_hello.role = r_u32(r); m->files_hello.version = r_u32(r); m->files_hello.directions = r_u32(r);
        m->files_hello.max_chunk = r_u32(r); m->files_hello.session_tag = r_u32(r);
        if (!r->fail && !files_hello_valid(&m->files_hello)) r->fail = 1;
        break;
    case ASB_RW_FILE_POINTER:
        m->file_pointer.session_id = r_u64(r); m->file_pointer.surface_id = r_u64(r); m->file_pointer.x = r_i32(r);
        m->file_pointer.y = r_i32(r); m->file_pointer.action = r_u32(r);
        if (!r->fail && !file_pointer_valid(&m->file_pointer)) r->fail = 1;
        break;
    case ASB_RW_FILE_FEEDBACK:
        m->file_feedback.session_id = r_u64(r); m->file_feedback.effect = r_u32(r); m->file_feedback.status = r_u32(r);
        if (!m->file_feedback.session_id || m->file_feedback.effect > ASB_RW_EFFECT_COPY ||
            m->file_feedback.status >= ASB_RW_ERR_COUNT)
            r->fail = 1;
        break;
    case ASB_RW_FILE_STREAM:
        m->file_stream.session_id = r_u64(r); m->file_stream.entry_count = r_u32(r); m->file_stream.total_bytes = r_u64(r);
        if (r->fail || !m->file_stream.session_id || !m->file_stream.entry_count) return ASB_RW_ERR_MALFORMED;
        if (m->file_stream.entry_count > ASB_RW_MAX_FILE_ENTRIES) return ASB_RW_ERR_TOO_LARGE;
        break;
    case ASB_RW_FILE_ENTRY:
        m->file_entry.session_id = r_u64(r); m->file_entry.size = r_u64(r); m->file_entry.index = r_u32(r);
        m->file_entry.kind = r_u32(r); r_str(r, m->file_entry.rel, &m->file_entry.rel_len, ASB_RW_MAX_REL_PATH);
        if (r->fail || !m->file_entry.session_id ||
            (m->file_entry.kind != ASB_RW_ENTRY_FILE && m->file_entry.kind != ASB_RW_ENTRY_DIR) ||
            (m->file_entry.kind == ASB_RW_ENTRY_DIR && m->file_entry.size))
            return ASB_RW_ERR_MALFORMED;
        if (!asb_rw_rel_path_safe(m->file_entry.rel, m->file_entry.rel_len)) return ASB_RW_ERR_FILE_REJECTED;
        break;
    case ASB_RW_FILE_DATA:
        m->file_data.session_id = r_u64(r); m->file_data.offset = r_u64(r); m->file_data.index = r_u32(r);
        m->file_data.len = r_u32(r);
        if (r->fail || !m->file_data.session_id) return ASB_RW_ERR_MALFORMED;
        if (m->file_data.len > ASB_RW_MAX_FILE_CHUNK) return ASB_RW_ERR_TOO_LARGE;
        *bulk_len = m->file_data.len;
        break;
    case ASB_RW_FILE_END: case ASB_RW_FILE_PROGRESS: case ASB_RW_FILE_CANCEL: case ASB_RW_FILE_RESULT:
        m->file_status.session_id = r_u64(r); m->file_status.status = r_u32(r);
        m->file_status.done_bytes = r_u64(r); m->file_status.total_bytes = r_u64(r);
        if (!m->file_status.session_id || m->file_status.status >= ASB_RW_ERR_COUNT ||
            m->file_status.done_bytes > m->file_status.total_bytes)
            r->fail = 1;
        break;
    default:
        return ASB_RW_ERR_UNKNOWN_TYPE;
    }
    return r->fail ? ASB_RW_ERR_MALFORMED : ASB_RW_OK;
}

uint32_t asb_rw_decode(int channel, const uint8_t *buf, size_t len, AsbRwHeader *header,
                       AsbRwPayload *msg, const uint8_t **bulk, size_t *bulk_len)
{
    size_t total = 0, bulk_n = 0;
    if (bulk) *bulk = NULL;
    if (bulk_len) *bulk_len = 0;
    if (!msg || !header) return ASB_RW_ERR_BUFFER;
    uint32_t err = asb_rw_frame_check(channel, buf, len, &total);
    if (err == ASB_RW_ERR_NEED_MORE || (err == ASB_RW_OK && len < total)) return ASB_RW_ERR_NEED_MORE;
    if (err) return err;
    if (len != total) return ASB_RW_ERR_MALFORMED;
    Reader h = { buf, ASB_RW_HEADER_BYTES, 0, 0 };
    header->magic = r_u32(&h); header->wire_version = r_u16(&h); header->type = r_u16(&h);
    header->request_id = r_u32(&h); header->payload_len = r_u32(&h);
    memset(msg, 0, sizeof(*msg));
    Reader r = { buf + ASB_RW_HEADER_BYTES, header->payload_len, 0, 0 };
    err = dec_payload(&r, header->type, msg, &bulk_n);
    if (err) return err;
    /* Fixed part plus exactly the declared bulk bytes: no trailing data, no truncation. */
    if (r.pos + bulk_n != header->payload_len) return ASB_RW_ERR_MALFORMED;
    if (bulk_n) {
        const uint8_t *data = buf + ASB_RW_HEADER_BYTES + r.pos;
        if (header->type == ASB_RW_FILE_DATA) msg->file_data.data = data;
        if (bulk) *bulk = data;
        if (bulk_len) *bulk_len = bulk_n;
    }
    return ASB_RW_OK;
}

/* ================================ negotiation ============================= */

void asb_rw_hello_init(AsbRwHello *hello, uint32_t role, uint32_t capabilities,
                       uint32_t helper_version, uint32_t driver_version, uint32_t session_tag)
{
    memset(hello, 0, sizeof(*hello));
    hello->min_version = ASB_RW_PROTOCOL_MIN;
    hello->max_version = ASB_RW_PROTOCOL_MAX;
    hello->role = role;
    hello->capabilities = capabilities;
    hello->helper_version = helper_version;
    hello->driver_version = driver_version;
    hello->session_tag = session_tag;
    hello->max_control_payload = ASB_RW_MAX_CONTROL_PAYLOAD;
    hello->max_frame_bytes = ASB_RW_MAX_FRAME_BYTES;
    hello->max_file_chunk = ASB_RW_MAX_FILE_CHUNK;
}

static uint32_t min3(uint32_t a, uint32_t b, uint32_t c) { uint32_t m = a < b ? a : b; return m < c ? m : c; }

AsbRwNegotiated asb_rw_negotiate(const AsbRwHello *local, const AsbRwHello *remote)
{
    AsbRwNegotiated n;
    memset(&n, 0, sizeof(n));
    if (!local || !remote || local->role == remote->role ||
        (local->role != ASB_RW_ROLE_HOST && local->role != ASB_RW_ROLE_GUEST) ||
        (remote->role != ASB_RW_ROLE_HOST && remote->role != ASB_RW_ROLE_GUEST)) {
        n.error = ASB_RW_ERR_MALFORMED;
        return n;
    }
    const AsbRwHello *guest = local->role == ASB_RW_ROLE_GUEST ? local : remote;
    if (!local->min_version || !remote->min_version || local->min_version > local->max_version ||
        remote->min_version > remote->max_version || local->max_version < remote->min_version ||
        remote->max_version < local->min_version) {
        n.error = ASB_RW_ERR_UNSUPPORTED_VERSION;
        return n;
    }
    n.version = local->max_version < remote->max_version ? local->max_version : remote->max_version;
    n.session_tag = guest->session_tag;
    n.capabilities = local->capabilities & remote->capabilities & ASB_RW_CAP_KNOWN_MASK;
    n.max_control_payload = min3(local->max_control_payload, remote->max_control_payload, ASB_RW_MAX_CONTROL_PAYLOAD);
    n.max_frame_bytes = min3(local->max_frame_bytes, remote->max_frame_bytes, ASB_RW_MAX_FRAME_BYTES);
    n.max_file_chunk = min3(local->max_file_chunk, remote->max_file_chunk, ASB_RW_MAX_FILE_CHUNK);
    if (!n.session_tag || n.max_control_payload < 4096u || !n.max_frame_bytes || !n.max_file_chunk) {
        n.error = ASB_RW_ERR_MALFORMED;
        return n;
    }
    n.missing_required = ASB_RW_CAP_REQUIRED & ~n.capabilities;
    n.error = n.missing_required ? ASB_RW_ERR_UPGRADE_REQUIRED : ASB_RW_OK;
    return n;
}

/* ================================ frame channel =========================== */

void asb_rw_frames_hello_init(AsbRwFramesHello *hello, uint32_t role, uint32_t session_tag,
                              uint32_t transports, uint32_t slots, uint32_t max_cpu_bytes)
{
    memset(hello, 0, sizeof(*hello));
    hello->role = role;
    hello->session_tag = session_tag;
    hello->transports = (transports & ASB_RW_TRANSPORTS_MASK) | ASB_RW_TRANSPORT_CPU;
    hello->slots = slots;
    hello->max_cpu_bytes = max_cpu_bytes;
}

AsbRwFramesAgreement asb_rw_frames_negotiate(const AsbRwFramesHello *local, const AsbRwFramesHello *remote,
                                             uint32_t session_tag)
{
    AsbRwFramesAgreement a;
    memset(&a, 0, sizeof(a));
    if (!local || !remote || !frames_hello_valid(local) || !frames_hello_valid(remote) || local->role == remote->role) {
        a.error = ASB_RW_ERR_MALFORMED;
        return a;
    }
    /* A frame channel from another (or a previous) control session is refused. */
    if (!session_tag || local->session_tag != session_tag || remote->session_tag != session_tag) {
        a.error = ASB_RW_ERR_STALE_ID;
        return a;
    }
    const AsbRwFramesHello *guest = local->role == ASB_RW_ROLE_GUEST ? local : remote;
    const AsbRwFramesHello *host = local->role == ASB_RW_ROLE_HOST ? local : remote;
    a.slots = local->slots < remote->slots ? local->slots : remote->slots;
    a.max_cpu_bytes = local->max_cpu_bytes < remote->max_cpu_bytes ? local->max_cpu_bytes : remote->max_cpu_bytes;
    a.guest_pid = guest->process_id;
    a.gpu = (guest->transports & host->transports & ASB_RW_TRANSPORT_GPU) && guest->vendor_id &&
            guest->vendor_id == host->vendor_id && guest->device_id == host->device_id &&
            !(guest->vendor_id == 0x1414u && guest->device_id == 0x008Cu);   /* Basic Render Driver */
    a.error = ASB_RW_OK;
    return a;
}


/* ================================ IDs ===================================== */

#define ID_SERIAL_BITS 20u
#define ID_SERIAL_MASK ((1u << ID_SERIAL_BITS) - 1u)

static uint64_t make_id(uint32_t tag, uint32_t slot, uint32_t serial)
{
    return ((uint64_t)tag << 32) | ((uint64_t)(slot + 1u) << ID_SERIAL_BITS) | (serial & ID_SERIAL_MASK);
}

static int same_identity(const AsbRwIdentity *a, const AsbRwIdentity *b)
{
    return a->hwnd == b->hwnd && a->pid == b->pid && a->thread_id == b->thread_id &&
           a->process_start == b->process_start;
}

uint32_t asb_rw_id_session_tag(uint64_t id) { return (uint32_t)(id >> 32); }

const AsbRwIdentity *asb_rw_ids_peek(const AsbRwIdTable *t, uint64_t id)
{
    uint32_t slot = (uint32_t)((id >> ID_SERIAL_BITS) & 0xFFFu);
    if (!slot || slot > ASB_RW_MAX_WINDOWS || asb_rw_id_session_tag(id) != t->session_tag) return NULL;
    const AsbRwIdSlot *s = &t->slots[slot - 1u];
    return s->in_use && (s->serial & ID_SERIAL_MASK) == (uint32_t)(id & ID_SERIAL_MASK) ? &s->identity : NULL;
}

void asb_rw_ids_init(AsbRwIdTable *t, uint32_t session_tag)
{
    /* Serials survive re-initialization so a reused slot never recreates an old ID even if a
       session tag repeats. */
    for (uint32_t i = 0; i < ASB_RW_MAX_WINDOWS; ++i) {
        t->slots[i].in_use = 0;
        memset(&t->slots[i].identity, 0, sizeof(t->slots[i].identity));
    }
    t->used = 0;
    t->session_tag = session_tag;
}

uint32_t asb_rw_ids_assign(AsbRwIdTable *t, const AsbRwIdentity *identity, uint64_t *id)
{
    uint32_t free_slot = ASB_RW_MAX_WINDOWS;
    *id = 0;
    if (!t->session_tag || !identity->hwnd || !identity->pid) return ASB_RW_ERR_NOT_ELIGIBLE;
    for (uint32_t i = 0; i < ASB_RW_MAX_WINDOWS; ++i) {
        AsbRwIdSlot *s = &t->slots[i];
        if (s->in_use && same_identity(&s->identity, identity)) {
            *id = make_id(t->session_tag, i, s->serial);
            return ASB_RW_OK;
        }
        if (!s->in_use && free_slot == ASB_RW_MAX_WINDOWS) free_slot = i;
    }
    if (free_slot == ASB_RW_MAX_WINDOWS) return ASB_RW_ERR_LIMIT;
    AsbRwIdSlot *s = &t->slots[free_slot];
    s->serial = (s->serial + 1u) & ID_SERIAL_MASK;
    if (!s->serial) s->serial = 1;
    s->identity = *identity;
    s->in_use = 1;
    t->used++;
    *id = make_id(t->session_tag, free_slot, s->serial);
    return ASB_RW_OK;
}

static AsbRwIdSlot *lookup(AsbRwIdTable *t, uint64_t id)
{
    uint32_t slot = (uint32_t)((id >> ID_SERIAL_BITS) & 0xFFFu);
    if (!t->session_tag || asb_rw_id_session_tag(id) != t->session_tag || !slot || slot > ASB_RW_MAX_WINDOWS)
        return NULL;
    AsbRwIdSlot *s = &t->slots[slot - 1u];
    return s->in_use && s->serial == (uint32_t)(id & ID_SERIAL_MASK) ? s : NULL;
}

uint32_t asb_rw_ids_release(AsbRwIdTable *t, uint64_t id)
{
    AsbRwIdSlot *s = lookup(t, id);
    if (!s) return ASB_RW_ERR_STALE_ID;
    s->in_use = 0;
    t->used--;
    return ASB_RW_OK;
}

uint32_t asb_rw_ids_resolve(AsbRwIdTable *t, uint64_t id, AsbRwIdentityProbe probe, void *ctx,
                            AsbRwIdentity *identity)
{
    AsbRwIdSlot *s = lookup(t, id);
    AsbRwIdentity live;
    if (!s) return ASB_RW_ERR_STALE_ID;
    memset(&live, 0, sizeof(live));
    if (!probe || !probe(ctx, s->identity.hwnd, &live) || !same_identity(&live, &s->identity)) {
        s->in_use = 0;
        t->used--;
        return ASB_RW_ERR_STALE_ID;
    }
    if (identity) *identity = s->identity;
    return ASB_RW_OK;
}

/* ================================ eligibility ============================= */

static int str_eq_ascii(const uint16_t *s, uint16_t len, const char *lit)
{
    size_t n = strlen(lit);
    if (n != len) return 0;
    for (size_t i = 0; i < n; ++i) if (s[i] != (uint16_t)(unsigned char)lit[i]) return 0;
    return 1;
}

int asb_rw_is_shell_class(const uint16_t *cls, uint16_t len)
{
    static const char *const shell[] = {
        "Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd", "NotifyIconOverflowWindow",
        "XamlExplorerHostIslandWindow", "TopLevelWindowForOverflowXamlIsland", "MultitaskingViewFrame",
        "ForegroundStaging", "Windows.UI.Core.CoreWindow", "Shell_InputSwitchTopLevelWindow",
        "TaskListThumbnailWnd", "ApplicationManager_ImmersiveShellWindow", "EdgeUiInputTopWndClass",
        "Shell_LightDismissOverlay",
    };
    for (size_t i = 0; i < sizeof(shell) / sizeof(shell[0]); ++i)
        if (str_eq_ascii(cls, len, shell[i])) return 1;
    return 0;
}

AsbRwEligibility asb_rw_root_eligibility(const AsbRwWindowFacts *w)
{
    if (w->is_helper) return ASB_RW_INELIGIBLE_HELPER;
    if (w->session_id != w->active_session_id) return ASB_RW_INELIGIBLE_OTHER_SESSION;
    if (w->elevation == ASB_RW_ELEVATION_YES) return ASB_RW_INELIGIBLE_ELEVATED;
    if (w->elevation != ASB_RW_ELEVATION_NO) return ASB_RW_INELIGIBLE_ELEVATION_UNKNOWN;
    if (!w->visible) return ASB_RW_INELIGIBLE_HIDDEN;   /* minimized windows remain WS_VISIBLE */
    /* A shared root keeps its entry even though sharing re-owns it and removes its shell presence. */
    if (w->selected) return ASB_RW_ELIGIBLE;
    if (w->cloaked) return ASB_RW_INELIGIBLE_CLOAKED;
    if (w->owner) return ASB_RW_INELIGIBLE_OWNED;
    if (asb_rw_is_shell_class(w->class_name, w->class_len)) return ASB_RW_INELIGIBLE_SHELL;
    int app = (w->ex_style & ASB_RW_WS_EX_APPWINDOW) != 0;
    if ((w->ex_style & ASB_RW_WS_EX_TOOLWINDOW) && !app) return ASB_RW_INELIGIBLE_TOOL_WINDOW;
    if (!app && ((w->style & ASB_RW_WS_CAPTION) != ASB_RW_WS_CAPTION || (w->ex_style & ASB_RW_WS_EX_NOACTIVATE)))
        return ASB_RW_INELIGIBLE_NOT_APP_WINDOW;
    if (!w->title_len) return ASB_RW_INELIGIBLE_UNTITLED;
    return ASB_RW_ELIGIBLE;
}

uint32_t asb_rw_association(const AsbRwWindowFacts *w, uint64_t root_hwnd)
{
    if (!root_hwnd || w->identity.hwnd == root_hwnd) return 0;
    int menu = str_eq_ascii(w->class_name, w->class_len, "#32768");
    /* Ownership only: the window's owner chain, or for an unowned menu, its thread's menu owner chain
       (stored in owner_chain by the collector). Process membership alone never associates. */
    if (!w->owner && !(menu && w->menu_owner)) return 0;
    uint32_t depth = w->owner_depth < ASB_RW_MAX_OWNER_DEPTH ? w->owner_depth : ASB_RW_MAX_OWNER_DEPTH;
    for (uint32_t i = 0; i < depth; ++i) {
        if (w->owner_chain[i] != root_hwnd) continue;
        if (menu) return ASB_RW_SURFACE_MENU;
        if (str_eq_ascii(w->class_name, w->class_len, "tooltips_class32")) return ASB_RW_SURFACE_TOOLTIP;
        if ((w->style & ASB_RW_WS_CAPTION) == ASB_RW_WS_CAPTION) return ASB_RW_SURFACE_OWNED;
        return ASB_RW_SURFACE_POPUP;
    }
    return 0;
}

uint32_t asb_rw_surface_association(const AsbRwWindowFacts *w, const AsbRwWindowFacts *root,
                                    const AsbRwRect *private_rect)
{
    if (!w || !root || !w->visible || w->cloaked || w->minimized || w->identity.hwnd == root->identity.hwnd ||
        w->rect.right <= w->rect.left || w->rect.bottom <= w->rect.top)
        return 0;
    if (str_eq_ascii(w->class_name, w->class_len, "SysShadow")) return 0;   /* shadow only, no content */
    uint32_t kind = asb_rw_association(w, root->identity.hwnd);
    if (kind) return kind;
    /* Unowned popups (WinUI PopupWindowSiteBridge, some menus/tooltips): same process, not an
       application window, on the private display. Anything owned by another window is that
       window's business, and independent roots of the same process need their own selection. */
    if (w->owner || w->identity.pid != root->identity.pid || w->identity.process_start != root->identity.process_start)
        return 0;
    if ((w->style & ASB_RW_WS_CAPTION) == ASB_RW_WS_CAPTION || (w->ex_style & ASB_RW_WS_EX_APPWINDOW)) return 0;
    /* File Explorer windows share explorer.exe with the shell: its taskbars, desktop and other
       shell windows are never popups of a shared root (eh6.9). */
    if (asb_rw_is_shell_class(w->class_name, w->class_len)) return 0;
    /* A popup of a parked root lies entirely on the private display; windows reaching beyond it
       (desktop-spanning shell or overlay windows) are not the root's. */
    if (!private_rect || w->rect.left < private_rect->left || w->rect.right > private_rect->right ||
        w->rect.top < private_rect->top || w->rect.bottom > private_rect->bottom)
        return 0;
    if (str_eq_ascii(w->class_name, w->class_len, "#32768")) return ASB_RW_SURFACE_MENU;
    if (str_eq_ascii(w->class_name, w->class_len, "tooltips_class32")) return ASB_RW_SURFACE_TOOLTIP;
    return ASB_RW_SURFACE_POPUP;
}

uint32_t asb_rw_collect_surfaces(const AsbRwWindowFacts *windows, uint32_t count, uint64_t root_hwnd,
                                 const AsbRwRect *private_rect, uint8_t *claimed,
                                 uint32_t *indices, uint32_t *kinds, uint32_t max)
{
    uint32_t root = count, n = 0;
    if (max > ASB_RW_MAX_SURFACES) max = ASB_RW_MAX_SURFACES;
    for (uint32_t i = 0; i < count; ++i)
        if (windows[i].identity.hwnd == root_hwnd) { root = i; break; }
    if (root == count || !max) return 0;
    indices[n] = root;
    kinds[n++] = ASB_RW_SURFACE_ROOT;
    if (claimed) claimed[root] = 1;
    /* Ownership first (an owned popup belongs to its owner's root even when another shared
       root of the same process is nearer), then unowned same-process popups. */
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t k = count; k-- > 0 && n < max;) {   /* bottom-to-top */
            const AsbRwWindowFacts *w = &windows[k];
            if (k == root || (claimed && claimed[k]) || (pass == 1 && asb_rw_association(w, root_hwnd))) continue;
            uint32_t kind = pass == 0 ? asb_rw_association(w, root_hwnd)
                                      : asb_rw_surface_association(w, &windows[root], private_rect);
            if (!kind) continue;
            if (pass == 0 && (!w->visible || w->cloaked || w->minimized || w->rect.right <= w->rect.left ||
                              w->rect.bottom <= w->rect.top || str_eq_ascii(w->class_name, w->class_len, "SysShadow")))
                continue;
            indices[n] = k;
            kinds[n++] = kind;
            if (claimed) claimed[k] = 1;
        }
    }
    /* Restore bottom-to-top order across both passes (indices are top-first positions). */
    for (uint32_t a = 2; a < n; ++a)
        for (uint32_t b = a; b > 1 && indices[b - 1] < indices[b]; --b) {
            uint32_t ti = indices[b]; indices[b] = indices[b - 1]; indices[b - 1] = ti;
            uint32_t tk = kinds[b]; kinds[b] = kinds[b - 1]; kinds[b - 1] = tk;
        }
    return n;
}

/* ================================ inventory =============================== */

static uint16_t fold(uint16_t c) { return (uint16_t)towlower((wint_t)c); }

static int str_ieq(const uint16_t *a, uint16_t an, const uint16_t *b, uint16_t bn)
{
    if (an != bn) return 0;
    for (uint16_t i = 0; i < an; ++i) if (fold(a[i]) != fold(b[i])) return 0;
    return 1;
}

void asb_rw_mark_duplicates(AsbRwInventoryEntry *entries, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) entries[i].flags &= ~ASB_RW_ENTRY_DUPLICATE_TITLE;
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = i + 1; j < count; ++j)
            if (str_ieq(entries[i].title, entries[i].title_len, entries[j].title, entries[j].title_len)) {
                entries[i].flags |= ASB_RW_ENTRY_DUPLICATE_TITLE;
                entries[j].flags |= ASB_RW_ENTRY_DUPLICATE_TITLE;
            }
}

static void put(uint16_t *out, uint32_t cap, uint32_t *n, const uint16_t *s, uint32_t len)
{
    for (uint32_t i = 0; i < len && *n + 1 < cap; ++i) out[(*n)++] = s[i];
}

uint32_t asb_rw_entry_label(const AsbRwInventoryEntry *entries, uint32_t count, uint32_t index,
                            uint16_t *out, uint32_t out_cap)
{
    uint32_t n = 0;
    if (!out || !out_cap || index >= count) return 0;
    const AsbRwInventoryEntry *e = &entries[index];
    put(out, out_cap, &n, e->title, e->title_len);
    if (e->flags & ASB_RW_ENTRY_DUPLICATE_TITLE) {
        static const uint16_t sep[] = { ' ', 0x2014, ' ' };   /* " — " */
        put(out, out_cap, &n, sep, 3);
        put(out, out_cap, &n, e->app, e->app_len);
        uint32_t ordinal = 0, same = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const AsbRwInventoryEntry *o = &entries[i];
            if (!str_ieq(o->title, o->title_len, e->title, e->title_len) ||
                !str_ieq(o->app, o->app_len, e->app, e->app_len))
                continue;
            ++same;
            if (i <= index) ++ordinal;
        }
        if (same > 1) {
            uint16_t num[16];
            uint32_t k = 0, v = ordinal;
            num[k++] = ' '; num[k++] = '(';
            uint16_t digits[10]; uint32_t d = 0;
            do { digits[d++] = (uint16_t)('0' + v % 10); v /= 10; } while (v && d < 10);
            while (d) num[k++] = digits[--d];
            num[k++] = ')';
            put(out, out_cap, &n, num, k);
        }
    }
    out[n] = 0;
    return n;
}

void asb_rw_inventory_init(AsbRwInventory *inv)
{
    memset(inv, 0, sizeof(*inv));
}

static int entry_changed(const AsbRwInventoryEntry *a, const AsbRwInventoryEntry *b)
{
    /* Selector-visible state only; geometry is synchronized through geometry acks. */
    return a->flags != b->flags || a->pid != b->pid ||
           a->title_len != b->title_len || memcmp(a->title, b->title, a->title_len * sizeof(uint16_t)) ||
           a->app_len != b->app_len || memcmp(a->app, b->app, a->app_len * sizeof(uint16_t));
}

static void emit(AsbRwInventoryEvent *events, uint32_t max, uint32_t *n, uint32_t *dropped,
                 uint32_t kind, const AsbRwInventoryEntry *e)
{
    if (*n < max && events) {
        events[*n].kind = kind;
        events[*n].entry = *e;
        (*n)++;
    } else {
        (*dropped)++;
    }
}

uint32_t asb_rw_inventory_update(AsbRwInventory *inv, AsbRwIdTable *ids,
                                 const AsbRwWindowFacts *windows, uint32_t window_count,
                                 AsbRwInventoryEvent *events, uint32_t max_events,
                                 uint32_t *event_count, uint32_t *dropped)
{
    uint32_t result = ASB_RW_OK, n_events = 0, n_dropped = 0;
    if (event_count) *event_count = 0;
    if (dropped) *dropped = 0;
    AsbRwInventory *nx = (AsbRwInventory *)calloc(1, sizeof(*nx));
    if (!nx) return ASB_RW_ERR_INTERNAL;
    nx->generation = inv->generation;
    for (uint32_t i = 0; i < window_count; ++i) {
        const AsbRwWindowFacts *w = &windows[i];
        if (asb_rw_root_eligibility(w) != ASB_RW_ELIGIBLE) continue;
        if (nx->count == ASB_RW_MAX_INVENTORY) { result = ASB_RW_ERR_LIMIT; break; }
        AsbRwInventoryEntry *e = &nx->entries[nx->count];
        if (asb_rw_ids_assign(ids, &w->identity, &e->id) != ASB_RW_OK) { result = ASB_RW_ERR_LIMIT; continue; }
        e->pid = w->identity.pid;
        e->flags = (w->minimized ? ASB_RW_ENTRY_MINIMIZED : 0) | (w->selected ? ASB_RW_ENTRY_SELECTED : 0) |
                   (w->foreground ? ASB_RW_ENTRY_FOREGROUND : 0) | (w->pending ? ASB_RW_ENTRY_PENDING : 0);
        e->rect = w->rect;
        e->dpi = w->dpi;
        e->title_len = w->title_len > ASB_RW_MAX_TITLE ? ASB_RW_MAX_TITLE : w->title_len;
        memcpy(e->title, w->title, e->title_len * sizeof(uint16_t));
        e->app_len = w->app_len > ASB_RW_MAX_APP_NAME ? ASB_RW_MAX_APP_NAME : w->app_len;
        memcpy(e->app, w->app, e->app_len * sizeof(uint16_t));
        nx->count++;
    }
    asb_rw_mark_duplicates(nx->entries, nx->count);

    for (uint32_t i = 0; i < inv->count; ++i) {
        uint32_t j = 0;
        while (j < nx->count && nx->entries[j].id != inv->entries[i].id) ++j;
        if (j == nx->count) {
            emit(events, max_events, &n_events, &n_dropped, ASB_RW_EVENT_REMOVED, &inv->entries[i]);
            asb_rw_ids_release(ids, inv->entries[i].id);
        }
    }
    for (uint32_t j = 0; j < nx->count; ++j) {
        uint32_t i = 0;
        while (i < inv->count && inv->entries[i].id != nx->entries[j].id) ++i;
        if (i == inv->count) emit(events, max_events, &n_events, &n_dropped, ASB_RW_EVENT_ADDED, &nx->entries[j]);
        else if (entry_changed(&inv->entries[i], &nx->entries[j]))
            emit(events, max_events, &n_events, &n_dropped, ASB_RW_EVENT_CHANGED, &nx->entries[j]);
    }
    if (n_events || n_dropped) nx->generation = inv->generation + 1u;
    for (uint32_t k = 0; k < n_events; ++k) events[k].generation = nx->generation;
    *inv = *nx;
    free(nx);
    if (event_count) *event_count = n_events;
    if (dropped) *dropped = n_dropped;
    return result;
}

void asb_rw_inventory_snapshot(const AsbRwInventory *inv, AsbRwInventorySnapshot *out)
{
    out->generation = inv->generation;
    out->count = inv->count;
    memcpy(out->entries, inv->entries, inv->count * sizeof(inv->entries[0]));
}

/* ================================ file paths ============================== */

static int reserved_device(const uint16_t *seg, uint32_t len)
{
    static const char *const names[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
    uint32_t base = 0;
    while (base < len && seg[base] != '.') ++base;   /* "NUL.txt" is still the device */
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t n = strlen(names[i]);
        if (n != base) continue;
        uint32_t k = 0;
        while (k < base && fold(seg[k]) == fold((uint16_t)(unsigned char)names[i][k])) ++k;
        if (k == base) return 1;
    }
    if (base == 4 && (fold(seg[0]) == 'c' || fold(seg[0]) == 'l')) {
        static const char com[] = "com", lpt[] = "lpt";
        const char *p = fold(seg[0]) == 'c' ? com : lpt;
        if (fold(seg[1]) == (uint16_t)p[1] && fold(seg[2]) == (uint16_t)p[2] &&
            ((seg[3] >= '0' && seg[3] <= '9') || seg[3] == 0x00B9 || seg[3] == 0x00B2 || seg[3] == 0x00B3))
            return 1;
    }
    return 0;
}

int asb_rw_rel_path_safe(const uint16_t *rel, uint32_t len)
{
    if (!rel || !len || len > ASB_RW_MAX_REL_PATH) return 0;
    if (rel[0] == '\\' || rel[0] == '/') return 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; ++i) {
        if (i < len && rel[i] != '\\' && rel[i] != '/') {
            uint16_t c = rel[i];
            if (c < 32 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                return 0;
            continue;
        }
        uint32_t seg = i - start;
        if (!seg) return 0;                                         /* empty segment ("a\\\\b", trailing '\\') */
        if (seg == 1 && rel[start] == '.') return 0;
        if (seg == 2 && rel[start] == '.' && rel[start + 1] == '.') return 0;
        if (rel[i - 1] == '.' || rel[i - 1] == ' ') return 0;      /* Win32 strips these: aliasing */
        if (reserved_device(rel + start, seg)) return 0;
        start = i + 1;
    }
    return 1;
}
