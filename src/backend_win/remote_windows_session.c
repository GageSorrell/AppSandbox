/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_session
 *
 * @file      remote_windows_session.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows host control session (AppSandbox-eh6.6, input eh6.7). See remote_windows_session.h. */
#include "remote_windows_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HT_NOWHERE 0u
#define HT_CLIENT  1u

static void slog(AsbRwSession *s, const char *fmt, ...)
{
    char text[512];
    va_list args;
    if (!s->ops.log) return;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    s->ops.log(s->ops.ctx, text);
}

static void changed(AsbRwSession *s)
{
    if (s->ops.changed) s->ops.changed(s->ops.ctx);
}

static int send_tx(AsbRwSession *s, uint16_t type)
{
    size_t written = 0;
    uint32_t request_id = ++s->next_request;
    uint32_t result = asb_rw_encode(type, request_id, &s->tx, s->out, sizeof(s->out), &written);
    if (result) {
        slog(s, "session: encode type %u failed (%s)", type, asb_rw_error_name(result));
        return 0;
    }
    if (s->send_failed) return 0;
    if (!s->ops.send(s->ops.ctx, s->out, written)) {
        s->send_failed = 1;
        return 0;
    }
    return 1;
}

static int proxies_supported(const AsbRwSession *s)
{
    return s->phase == ASB_RW_SESSION_READY;
}

static int id_current(const AsbRwSession *s, uint64_t id)
{
    return id && s->phase != ASB_RW_SESSION_HELLO && asb_rw_id_session_tag(id) == s->negotiated.session_tag;
}

/* ================================ lookups ================================= */

AsbRwHostProxy *asb_rw_session_proxy(AsbRwSession *s, uint64_t id)
{
    if (!s || !id) return NULL;
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i)
        if (s->proxies[i].id == id) return &s->proxies[i];
    return NULL;
}

static AsbRwHostProxy *proxy_slot(AsbRwSession *s, uint64_t id)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    if (p) return p;
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i) {
        if (s->proxies[i].id) continue;
        memset(&s->proxies[i], 0, sizeof(s->proxies[i]));
        s->proxies[i].id = id;
        return &s->proxies[i];
    }
    return NULL;
}

const AsbRwInventoryEntry *asb_rw_session_entry(const AsbRwSession *s, uint64_t id)
{
    for (uint32_t i = 0; s && i < s->inventory.count; ++i)
        if (s->inventory.entries[i].id == id) return &s->inventory.entries[i];
    return NULL;
}

uint64_t asb_rw_session_root_of(const AsbRwSession *s, uint64_t surface_id)
{
    for (uint32_t i = 0; s && surface_id && i < ASB_RW_HOST_MAX_PROXIES; ++i) {
        const AsbRwHostProxy *p = &s->proxies[i];
        if (!p->id) continue;
        if (p->id == surface_id) return p->id;
        for (uint32_t k = 0; p->have_surfaces && k < p->surfaces.count; ++k)
            if (p->surfaces.surfaces[k].surface_id == surface_id) return p->id;
    }
    return 0;
}

static const AsbRwSurface *find_surface(const AsbRwHostProxy *p, uint64_t surface_id)
{
    for (uint32_t k = 0; p->have_surfaces && k < p->surfaces.count; ++k)
        if (p->surfaces.surfaces[k].surface_id == surface_id) return &p->surfaces.surfaces[k];
    return NULL;
}

static int32_t rw(const AsbRwRect *r) { return r->right - r->left; }
static int32_t rh(const AsbRwRect *r) { return r->bottom - r->top; }

/* ================================ hit cache =============================== */

static void hits_forget(AsbRwSession *s, uint64_t surface_id)
{
    for (uint32_t i = 0; i < ASB_RW_HOST_HIT_CACHE; ++i)
        if (s->hits[i].surface_id == surface_id) memset(&s->hits[i], 0, sizeof(s->hits[i]));
}

static AsbRwHitQuery *query_for(AsbRwSession *s, uint64_t root_id)
{
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i)
        if (s->proxies[i].id == root_id) return &s->queries[i];
    return NULL;
}

static void send_hit(AsbRwSession *s, AsbRwHitQuery *q, uint64_t surface_id, int32_t x, int32_t y)
{
    memset(&s->tx.hit_test, 0, sizeof(s->tx.hit_test));
    q->surface_id = surface_id;
    q->serial = ++s->next_serial;
    q->x = x;
    q->y = y;
    q->tick = s->now_ms;
    q->queued = 0;
    s->tx.hit_test.id = surface_id;
    s->tx.hit_test.serial = q->serial;
    s->tx.hit_test.x = x;
    s->tx.hit_test.y = y;
    send_tx(s, ASB_RW_HIT_TEST_REQUEST);
}

/* ================================ proxies ================================= */

static void input_forget_root(AsbRwSession *s, uint64_t root);
static void handle_drag_state(AsbRwSession *s, const AsbRwDrag *d);
static void drag_tick(AsbRwSession *s);
static void handle_activate_ack(AsbRwSession *s, const AsbRwActivate *a);
static void input_tick(AsbRwSession *s);
static void input_end(AsbRwSession *s);

static void destroy_proxy(AsbRwSession *s, AsbRwHostProxy *p, uint32_t state, uint32_t error)
{
    uint64_t id = p->id;
    int was = p->created;
    input_forget_root(s, id);
    for (uint32_t k = 0; p->have_surfaces && k < p->surfaces.count; ++k) hits_forget(s, p->surfaces.surfaces[k].surface_id);
    hits_forget(s, id);
    AsbRwHitQuery *q = query_for(s, id);
    if (q) memset(q, 0, sizeof(*q));
    if (was && s->ops.proxy_destroy) s->ops.proxy_destroy(s->ops.ctx, id);
    if (state == ASB_RW_STATE_FAILED || state == ASB_RW_STATE_PENDING || state == ASB_RW_STATE_RESTORING) {
        /* Keep the entry (no proxy) so selectors can show the transition or the error. */
        memset(p, 0, sizeof(*p));
        p->id = id;
        p->state = state;
        p->error = error;
    } else {
        memset(p, 0, sizeof(*p));
    }
    if (was) slog(s, "proxy %016llx destroyed (%s)", (unsigned long long)id, error ? asb_rw_error_name(error) : "ok");
}

static void create_proxy(AsbRwSession *s, AsbRwHostProxy *p)
{
    AsbRwProxyInit init;
    const AsbRwInventoryEntry *e = asb_rw_session_entry(s, p->id);
    memset(&init, 0, sizeof(init));
    if (e) {
        init.guest_rect = e->rect;
        init.dpi = e->dpi;
        init.title = e->title;
        init.title_len = e->title_len;
        init.app = e->app;
        init.app_len = e->app_len;
    }
    if (!s->ops.proxy_create || !s->ops.proxy_create(s->ops.ctx, p->id, &init)) {
        slog(s, "proxy %016llx could not be created; unselecting", (unsigned long long)p->id);
        memset(&s->tx.select, 0, sizeof(s->tx.select));
        s->tx.select.id = p->id;
        s->tx.select.select = 0;
        send_tx(s, ASB_RW_SELECT_REQUEST);
        p->state = ASB_RW_STATE_RESTORING;
        p->error = ASB_RW_ERR_INTERNAL;
        return;
    }
    p->created = 1;
    p->want_w = rw(&init.guest_rect);
    p->want_h = rh(&init.guest_rect);
    p->want_show = ASB_RW_SHOW_NORMAL;
    p->host_show = ASB_RW_SHOW_NORMAL;
    slog(s, "proxy %016llx created (%dx%d)", (unsigned long long)p->id, p->want_w, p->want_h);
    if (p->have_icon && s->ops.proxy_icon) s->ops.proxy_icon(s->ops.ctx, p->id, &p->icon);
    if (p->have_window && s->ops.proxy_state) s->ops.proxy_state(s->ops.ctx, p->id, p->id, &p->window);
    if (p->have_surfaces && s->ops.proxy_surfaces) s->ops.proxy_surfaces(s->ops.ctx, p->id, &p->surfaces);
}

static void send_geometry(AsbRwSession *s, AsbRwHostProxy *p)
{
    memset(&s->tx.geometry, 0, sizeof(s->tx.geometry));
    p->sent_serial = ++s->next_serial;
    p->sent_tick = s->now_ms;
    p->sent_w = p->want_w;
    p->sent_h = p->want_h;
    p->sent_show = p->want_show;
    s->tx.geometry.id = p->id;
    s->tx.geometry.serial = p->sent_serial;
    s->tx.geometry.show_state = p->want_show;
    /* The guest keeps the root's position; only the size matters. */
    AsbRwRect base = p->have_surfaces ? p->surfaces.root_rect : p->acked;
    s->tx.geometry.rect.left = base.left;
    s->tx.geometry.rect.top = base.top;
    s->tx.geometry.rect.right = base.left + p->want_w;
    s->tx.geometry.rect.bottom = base.top + p->want_h;
    send_tx(s, ASB_RW_GEOMETRY_COMMAND);
}

static int geometry_pending(const AsbRwHostProxy *p)
{
    return p->sent_serial != 0;
}

static void maybe_send_geometry(AsbRwSession *s, AsbRwHostProxy *p)
{
    if (!p->created || geometry_pending(p)) return;
    if (p->have_ack && p->sent_w == p->want_w && p->sent_h == p->want_h && p->sent_show == p->want_show) return;
    send_geometry(s, p);
}

/* The proxy follows a guest size it did not ask for (application limits or its own resize). */
static void adopt_guest_size(AsbRwSession *s, AsbRwHostProxy *p, int32_t w, int32_t h)
{
    if (!p->created || p->sizing || p->host_show != ASB_RW_SHOW_NORMAL || geometry_pending(p) || w <= 0 || h <= 0) return;
    if (w == p->want_w && h == p->want_h) return;
    p->want_w = w;
    p->want_h = h;
    p->sent_w = w;          /* already what the guest has: nothing to send for it */
    p->sent_h = h;
    p->sent_show = p->want_show;
    if (s->ops.proxy_size) s->ops.proxy_size(s->ops.ctx, p->id, w, h);
}

static AsbRwHostOwnedMove *owned_move(AsbRwHostProxy *p, uint64_t surface_id, int create)
{
    AsbRwHostOwnedMove *free_slot = NULL;
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_OWNED_MOVES; ++i) {
        if (p->moves[i].surface_id == surface_id) return &p->moves[i];
        if (!p->moves[i].surface_id && !free_slot) free_slot = &p->moves[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->surface_id = surface_id;
    return free_slot;
}

/* ================================ handlers ================================ */

static void handle_hello(AsbRwSession *s, const AsbRwHello *remote)
{
    AsbRwHello local;
    asb_rw_hello_init(&local, ASB_RW_ROLE_HOST, ASB_RW_HOST_CAPS | s->ops.file_caps, ASB_RW_HOST_VERSION, 0, 0);
    s->negotiated = asb_rw_negotiate(&local, remote);
    s->guest_helper_version = remote->helper_version;
    s->guest_driver_version = remote->driver_version;
    uint32_t missing = ASB_RW_PROXY_REQUIRED & ~s->negotiated.capabilities;
    int usable = s->negotiated.error == ASB_RW_OK || s->negotiated.error == ASB_RW_ERR_UPGRADE_REQUIRED;
    s->phase = usable && !missing ? ASB_RW_SESSION_READY : ASB_RW_SESSION_LIMITED;
    slog(s, "session: guest helper %08x caps %08x -> %s, proxies %s (missing %08x)", remote->helper_version,
         remote->capabilities, asb_rw_error_name(s->negotiated.error), s->phase == ASB_RW_SESSION_READY ? "on" : "off",
         missing);
    if (usable) send_tx(s, ASB_RW_INVENTORY_REQUEST);
    changed(s);
}

static void inventory_apply(AsbRwSession *s, const AsbRwInventoryEntry *e, uint32_t kind)
{
    AsbRwInventory *inv = &s->inventory;
    uint32_t i;
    for (i = 0; i < inv->count; ++i) if (inv->entries[i].id == e->id) break;
    if (kind == ASB_RW_EVENT_REMOVED) {
        if (i < inv->count) {
            memmove(&inv->entries[i], &inv->entries[i + 1], (inv->count - i - 1) * sizeof(inv->entries[0]));
            inv->count--;
        }
        return;
    }
    if (i == inv->count) {
        if (inv->count >= ASB_RW_MAX_INVENTORY) return;
        inv->count++;
    }
    int title_changed = i == inv->count - 1 || inv->entries[i].title_len != e->title_len ||
                        memcmp(inv->entries[i].title, e->title, e->title_len * sizeof(uint16_t));
    inv->entries[i] = *e;
    AsbRwHostProxy *p = asb_rw_session_proxy(s, e->id);
    if (p && p->created && title_changed && s->ops.proxy_title) s->ops.proxy_title(s->ops.ctx, p->id, e->title, e->title_len);
}

static void handle_snapshot(AsbRwSession *s, const AsbRwInventorySnapshot *snap)
{
    s->inventory.generation = snap->generation;
    s->inventory.count = snap->count;
    memcpy(s->inventory.entries, snap->entries, snap->count * sizeof(snap->entries[0]));
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i) {
        AsbRwHostProxy *p = &s->proxies[i];
        const AsbRwInventoryEntry *e = p->created ? asb_rw_session_entry(s, p->id) : NULL;
        if (e && s->ops.proxy_title) s->ops.proxy_title(s->ops.ctx, p->id, e->title, e->title_len);
    }
}

static void handle_select_result(AsbRwSession *s, const AsbRwSelectResult *r)
{
    if (!id_current(s, r->id)) { slog(s, "session: result for a foreign id ignored"); return; }
    AsbRwHostProxy *p = asb_rw_session_proxy(s, r->id);
    switch (r->state) {
    case ASB_RW_STATE_PENDING:
        if (!p) p = proxy_slot(s, r->id);
        if (p && !p->created) { p->state = ASB_RW_STATE_PENDING; p->error = 0; }
        break;
    case ASB_RW_STATE_SHARED:
        if (!p) p = proxy_slot(s, r->id);
        if (!p) {
            slog(s, "session: proxy table full; unselecting %016llx", (unsigned long long)r->id);
            memset(&s->tx.select, 0, sizeof(s->tx.select));
            s->tx.select.id = r->id;
            send_tx(s, ASB_RW_SELECT_REQUEST);
            break;
        }
        p->state = ASB_RW_STATE_SHARED;
        p->error = 0;
        if (!p->created && proxies_supported(s)) create_proxy(s, p);
        break;
    case ASB_RW_STATE_RESTORING:
        if (p) destroy_proxy(s, p, ASB_RW_STATE_RESTORING, r->error);
        break;
    case ASB_RW_STATE_FAILED:
        if (!p) p = proxy_slot(s, r->id);
        if (p) destroy_proxy(s, p, ASB_RW_STATE_FAILED, r->error);
        break;
    default:   /* NONE: unselected, or the window is gone (STALE_ID) */
        if (p) destroy_proxy(s, p, ASB_RW_STATE_NONE, r->error);
        break;
    }
    changed(s);
}

static void handle_surfaces(AsbRwSession *s, const AsbRwSurfaces *in)
{
    if (!id_current(s, in->root_id)) return;
    AsbRwHostProxy *p = asb_rw_session_proxy(s, in->root_id);
    if (!p || p->state != ASB_RW_STATE_SHARED) return;
    AsbRwSurfaces copy = *in;   /* `in` is the receive buffer; edit a copy */
    /* Forget hit results of surfaces that changed size or disappeared. */
    for (uint32_t k = 0; p->have_surfaces && k < p->surfaces.count; ++k) {
        const AsbRwSurface *old = &p->surfaces.surfaces[k];
        int keep = 0;
        for (uint32_t j = 0; j < copy.count; ++j)
            if (copy.surfaces[j].surface_id == old->surface_id && rw(&copy.surfaces[j].rect) == rw(&old->rect) &&
                rh(&copy.surfaces[j].rect) == rh(&old->rect))
                keep = 1;
        if (!keep) hits_forget(s, old->surface_id);
    }
    /* An owned window being moved by the host keeps its requested place until acknowledged. */
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_OWNED_MOVES; ++i) {
        AsbRwHostOwnedMove *m = &p->moves[i];
        if (!m->surface_id) continue;
        int present = 0;
        for (uint32_t j = 0; j < copy.count; ++j)
            if (copy.surfaces[j].surface_id == m->surface_id) { present = 1; if (m->serial) copy.surfaces[j].rect = m->rect; }
        if (!present) memset(m, 0, sizeof(*m));
    }
    p->surfaces = copy;
    p->have_surfaces = copy.count != 0;
    if (!copy.count) return;   /* the root stopped being shared; SELECT_RESULT follows */
    if (p->created && s->ops.proxy_surfaces) s->ops.proxy_surfaces(s->ops.ctx, p->id, &p->surfaces);
    adopt_guest_size(s, p, rw(&copy.root_rect), rh(&copy.root_rect));
}

static void handle_geometry_ack(AsbRwSession *s, const AsbRwGeometry *a)
{
    uint64_t root = asb_rw_session_root_of(s, a->id);
    AsbRwHostProxy *p = root ? asb_rw_session_proxy(s, root) : NULL;
    if (!p) return;
    if (a->id != p->id) {
        AsbRwHostOwnedMove *m = owned_move(p, a->id, 0);
        if (!m || m->serial != a->serial) return;   /* superseded */
        memset(m, 0, sizeof(*m));   /* the next SURFACES carries the applied place */
        return;
    }
    if (!p->sent_serial || a->serial != p->sent_serial) return;   /* stale ack (or none pending) */
    p->sent_serial = 0;
    if (a->error == ASB_RW_ERR_BUSY) {
        p->sent_w = p->sent_h = -1;                 /* not applied: retried by tick/next request */
        p->sent_tick = s->now_ms;
        return;
    }
    if (a->error) { slog(s, "geometry %016llx refused (%s)", (unsigned long long)p->id, asb_rw_error_name(a->error)); return; }
    p->acked = a->rect;
    p->have_ack = 1;
    hits_forget(s, p->id);
    if (p->sent_w != p->want_w || p->sent_h != p->want_h || p->sent_show != p->want_show) {
        send_geometry(s, p);                        /* newer request coalesced meanwhile */
        return;
    }
    if (a->show_state != ASB_RW_SHOW_MINIMIZED && (rw(&a->rect) != p->want_w || rh(&a->rect) != p->want_h))
        adopt_guest_size(s, p, rw(&a->rect), rh(&a->rect));
}

static void handle_window_state(AsbRwSession *s, const AsbRwWindowState *st)
{
    uint64_t root = asb_rw_session_root_of(s, st->id);
    AsbRwHostProxy *p = root ? asb_rw_session_proxy(s, root) : asb_rw_session_proxy(s, st->id);
    if (!p || !id_current(s, st->id)) return;
    if (st->id == p->id) {
        int first = !p->have_window;
        p->window = *st;
        p->have_window = 1;
        if (first) p->request_serial_seen = st->request_serial;
        if (p->created && s->ops.proxy_state) s->ops.proxy_state(s->ops.ctx, p->id, p->id, st);
        if (!first && st->request_serial != p->request_serial_seen) {
            p->request_serial_seen = st->request_serial;
            if (p->created && s->ops.proxy_show) {
                uint32_t show = st->request;
                if (show == ASB_RW_SHOW_MAXIMIZED && p->host_show == ASB_RW_SHOW_MAXIMIZED) show = ASB_RW_SHOW_NORMAL;
                slog(s, "proxy %016llx: application requested show %u -> %u", (unsigned long long)p->id, st->request, show);
                s->ops.proxy_show(s->ops.ctx, p->id, show);
            }
        }
    } else if (p->created && s->ops.proxy_state) {
        s->ops.proxy_state(s->ops.ctx, p->id, st->id, st);
    }
}

static void handle_window_icon(AsbRwSession *s, const AsbRwWindowIcon *icon)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, icon->id);
    if (!id_current(s, icon->id)) return;
    if (!p || !p->created) {
        /* eh6.8: selector icon of an inventory entry */
        if (asb_rw_session_entry(s, icon->id) && s->ops.inventory_icon) s->ops.inventory_icon(s->ops.ctx, icon);
        if (!p) return;
    }
    p->icon = *icon;
    p->have_icon = 1;
    if (p->created && s->ops.proxy_icon) s->ops.proxy_icon(s->ops.ctx, p->id, &p->icon);
}

static void handle_close_result(AsbRwSession *s, const AsbRwClose *c)
{
    uint64_t root = asb_rw_session_root_of(s, c->id);
    AsbRwHostProxy *p = root ? asb_rw_session_proxy(s, root) : asb_rw_session_proxy(s, c->id);
    if (!p) return;
    if (p->close_id == c->id) p->close_id = 0;
    slog(s, "close %016llx -> %s", (unsigned long long)c->id, asb_rw_error_name(c->error));
    if (c->error == ASB_RW_ERR_BUSY && p->created && s->ops.proxy_activate_modal)
        s->ops.proxy_activate_modal(s->ops.ctx, p->id);
}

static void handle_hit_result(AsbRwSession *s, const AsbRwHitTest *h)
{
    uint64_t root = asb_rw_session_root_of(s, h->id);
    AsbRwHitQuery *q = root ? query_for(s, root) : NULL;
    if (!q || q->surface_id != h->id || q->serial != h->serial) return;
    AsbRwHitEntry *e = &s->hits[s->hit_next++ % ASB_RW_HOST_HIT_CACHE];
    e->surface_id = h->id;
    e->x = h->x;
    e->y = h->y;
    e->code = h->code ? h->code : HT_CLIENT;
    e->tick = s->now_ms ? s->now_ms : 1;
    int queued = q->queued;
    int32_t qx = q->qx, qy = q->qy;
    uint64_t surface = q->surface_id;
    memset(q, 0, sizeof(*q));
    if (queued) send_hit(s, q, surface, qx, qy);
    if (s->ops.hit_result) s->ops.hit_result(s->ops.ctx, root, h->id);
}

/* eh6.8: the guest selector's request runs through the host's own selection path. */
static void handle_guest_select(AsbRwSession *s, uint32_t request_id, const AsbRwSelect *req)
{
    AsbRwSelect copy = *req;   /* rx is reused by nested sends' callers */
    uint32_t result = asb_rw_session_select(s, copy.id, copy.select ? 1 : 0);
    slog(s, "session: guest selector %s %016llx -> %s", copy.select ? "select" : "unselect", (unsigned long long)copy.id,
         asb_rw_error_name(result));
    if (result == ASB_RW_OK) return;
    size_t written = 0;
    memset(&s->tx.error, 0, sizeof(s->tx.error));
    s->tx.error.code = result;
    s->tx.error.detail = ASB_RW_SELECT_REQUEST;
    if (!asb_rw_encode(ASB_RW_ERROR, request_id, &s->tx, s->out, sizeof(s->out), &written) && !s->send_failed &&
        !s->ops.send(s->ops.ctx, s->out, written))
        s->send_failed = 1;
}

uint32_t asb_rw_session_receive(AsbRwSession *s, const uint8_t *bytes, size_t len)
{
    AsbRwHeader header;
    memset(&header, 0, sizeof(header));
    uint32_t result = asb_rw_decode(ASB_CH_RW_CONTROL, bytes, len, &header, &s->rx, NULL, NULL);
    if (result) { slog(s, "session: bad message (%s)", asb_rw_error_name(result)); return result; }
    if (s->phase == ASB_RW_SESSION_HELLO) {
        if (header.type == ASB_RW_ERROR) {
            slog(s, "session: guest refused (%s)", asb_rw_error_name(s->rx.error.code));
            return s->rx.error.code ? s->rx.error.code : ASB_RW_ERR_MALFORMED;
        }
        if (header.type != ASB_RW_HELLO || s->rx.hello.role != ASB_RW_ROLE_GUEST || !s->rx.hello.session_tag)
            return ASB_RW_ERR_MALFORMED;
        handle_hello(s, &s->rx.hello);
        return s->phase == ASB_RW_SESSION_READY || s->negotiated.error == ASB_RW_ERR_UPGRADE_REQUIRED ||
               s->negotiated.error == ASB_RW_OK ? ASB_RW_OK : s->negotiated.error;
    }
    switch (header.type) {
    case ASB_RW_HELLO:
        return ASB_RW_ERR_MALFORMED;
    case ASB_RW_ERROR:
        slog(s, "session: guest error %s (detail %u)", asb_rw_error_name(s->rx.error.code), s->rx.error.detail);
        return ASB_RW_OK;
    case ASB_RW_INVENTORY_SNAPSHOT:
        handle_snapshot(s, &s->rx.snapshot);
        changed(s);
        return ASB_RW_OK;
    case ASB_RW_INVENTORY_EVENT:
        s->inventory.generation = s->rx.event.generation;
        inventory_apply(s, &s->rx.event.entry, s->rx.event.kind);
        changed(s);
        return ASB_RW_OK;
    case ASB_RW_SELECT_RESULT:
        handle_select_result(s, &s->rx.select_result);
        return ASB_RW_OK;
    case ASB_RW_SURFACES:
        handle_surfaces(s, &s->rx.surfaces);
        return ASB_RW_OK;
    case ASB_RW_GEOMETRY_ACK:
        handle_geometry_ack(s, &s->rx.geometry);
        return ASB_RW_OK;
    case ASB_RW_WINDOW_STATE:
        handle_window_state(s, &s->rx.window_state);
        return ASB_RW_OK;
    case ASB_RW_WINDOW_ICON:
        handle_window_icon(s, &s->rx.window_icon);
        return ASB_RW_OK;
    case ASB_RW_CLOSE_RESULT:
        handle_close_result(s, &s->rx.close);
        return ASB_RW_OK;
    case ASB_RW_HIT_TEST_RESULT:
        handle_hit_result(s, &s->rx.hit_test);
        return ASB_RW_OK;
    case ASB_RW_RESTORE_ALL:
        for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i)
            if (s->proxies[i].id) destroy_proxy(s, &s->proxies[i], ASB_RW_STATE_NONE, 0);
        changed(s);
        return ASB_RW_OK;
    case ASB_RW_ACTIVATE_ACK:
        handle_activate_ack(s, &s->rx.activate);
        return ASB_RW_OK;
    case ASB_RW_DRAG_STATE:   /* eh6.9 */
        if (!asb_rw_session_files_enabled(s, ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST)) return ASB_RW_ERR_UNKNOWN_TYPE;
        handle_drag_state(s, &s->rx.drag);
        return ASB_RW_OK;
    case ASB_RW_SELECT_REQUEST:   /* eh6.8: the guest's own selector */
        if (!(s->negotiated.capabilities & ASB_RW_CAP_GUEST_SELECTOR)) return ASB_RW_ERR_UNKNOWN_TYPE;
        handle_guest_select(s, header.request_id, &s->rx.select);
        return ASB_RW_OK;
    default:   /* host -> guest types arriving here are protocol violations */
        return ASB_RW_ERR_UNKNOWN_TYPE;
    }
}

/* ================================ lifecycle =============================== */

AsbRwSession *asb_rw_session_create(const AsbRwSessionOps *ops)
{
    AsbRwSession *s = (AsbRwSession *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ops = *ops;
    s->phase = ASB_RW_SESSION_HELLO;
    asb_rw_inventory_init(&s->inventory);
    s->ops.file_caps &= ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST;
    asb_rw_hello_init(&s->tx.hello, ASB_RW_ROLE_HOST, ASB_RW_HOST_CAPS | s->ops.file_caps, ASB_RW_HOST_VERSION, 0, 0);
    send_tx(s, ASB_RW_HELLO);
    return s;
}

void asb_rw_session_destroy(AsbRwSession *s)
{
    if (!s) return;
    input_end(s);
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i)
        if (s->proxies[i].id) destroy_proxy(s, &s->proxies[i], ASB_RW_STATE_NONE, 0);
    free(s);
}

void asb_rw_session_tick(AsbRwSession *s, uint64_t now_ms)
{
    s->now_ms = now_ms;
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i) {
        AsbRwHostProxy *p = &s->proxies[i];
        AsbRwHitQuery *q = &s->queries[i];
        if (!p->id) continue;
        if (p->created && geometry_pending(p) && now_ms - p->sent_tick > 4 * ASB_RW_HOST_RETRY_MS) {
            slog(s, "geometry %016llx unanswered; resending", (unsigned long long)p->id);
            send_geometry(s, p);
        } else if (p->created && !geometry_pending(p) && p->sent_w < 0 && now_ms - p->sent_tick > ASB_RW_HOST_RETRY_MS) {
            send_geometry(s, p);   /* guest was busy */
        }
        if (p->close_id && now_ms - p->close_tick > 10 * ASB_RW_HOST_RETRY_MS) p->close_id = 0;
        if (q->surface_id && now_ms - q->tick > ASB_RW_HOST_RETRY_MS) {
            uint64_t surface = q->surface_id;
            int32_t x = q->queued ? q->qx : q->x, y = q->queued ? q->qy : q->y;
            send_hit(s, q, surface, x, y);
        }
    }
    input_tick(s);
    drag_tick(s);
}

/* ================================ requests ================================ */

uint32_t asb_rw_session_select(AsbRwSession *s, uint64_t id, int select)
{
    if (s->phase == ASB_RW_SESSION_HELLO) return ASB_RW_ERR_BUSY;
    if (!proxies_supported(s) && select) return ASB_RW_ERR_UPGRADE_REQUIRED;
    if (!id_current(s, id)) return ASB_RW_ERR_STALE_ID;
    if (select && !asb_rw_session_entry(s, id)) return ASB_RW_ERR_STALE_ID;
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    if (select) {
        if (p && (p->state == ASB_RW_STATE_PENDING || p->state == ASB_RW_STATE_SHARED)) return ASB_RW_OK;
        if (!p) p = proxy_slot(s, id);
        if (!p) return ASB_RW_ERR_LIMIT;
        p->state = ASB_RW_STATE_PENDING;
        p->error = 0;
    } else if (p && p->created) {
        destroy_proxy(s, p, ASB_RW_STATE_RESTORING, 0);   /* the guest restores; nothing is closed */
    }
    memset(&s->tx.select, 0, sizeof(s->tx.select));
    s->tx.select.id = id;
    s->tx.select.select = select ? 1u : 0u;
    send_tx(s, ASB_RW_SELECT_REQUEST);
    changed(s);
    return s->send_failed ? ASB_RW_ERR_INTERNAL : ASB_RW_OK;
}

uint32_t asb_rw_session_restore_all(AsbRwSession *s)
{
    if (s->phase == ASB_RW_SESSION_HELLO) return ASB_RW_ERR_BUSY;
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_PROXIES; ++i)
        if (s->proxies[i].created) destroy_proxy(s, &s->proxies[i], ASB_RW_STATE_RESTORING, 0);
    send_tx(s, ASB_RW_RESTORE_ALL);
    changed(s);
    return s->send_failed ? ASB_RW_ERR_INTERNAL : ASB_RW_OK;
}

void asb_rw_session_resize(AsbRwSession *s, uint64_t id, uint32_t show, int32_t width, int32_t height)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    if (!p || !p->created || show > ASB_RW_SHOW_MAXIMIZED) return;
    p->host_show = show;
    if (show != ASB_RW_SHOW_MINIMIZED) {
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        if (width > (int32_t)ASB_RW_MAX_FRAME_WIDTH) width = (int32_t)ASB_RW_MAX_FRAME_WIDTH;
        if (height > (int32_t)ASB_RW_MAX_FRAME_HEIGHT) height = (int32_t)ASB_RW_MAX_FRAME_HEIGHT;
        p->want_w = width;
        p->want_h = height;
    }
    p->want_show = show;
    maybe_send_geometry(s, p);
}

void asb_rw_session_sizing(AsbRwSession *s, uint64_t id, int active)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    if (!p || !p->created) return;
    p->sizing = active != 0;
    if (!p->sizing && p->have_ack && !geometry_pending(p) &&
        (rw(&p->acked) != p->want_w || rh(&p->acked) != p->want_h) && p->host_show == ASB_RW_SHOW_NORMAL)
        adopt_guest_size(s, p, rw(&p->acked), rh(&p->acked));   /* the loop ended below the app's minimum */
}

void asb_rw_session_move_owned(AsbRwSession *s, uint64_t id, uint64_t surface_id, int32_t dx, int32_t dy,
                               int32_t width, int32_t height)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    if (!p || !p->created || !p->have_surfaces) return;
    const AsbRwSurface *surface = find_surface(p, surface_id);
    if (!surface || surface->kind != ASB_RW_SURFACE_OWNED) return;
    AsbRwRect r;
    r.left = p->surfaces.root_rect.left + dx;
    r.top = p->surfaces.root_rect.top + dy;
    r.right = r.left + (width > 0 ? width : rw(&surface->rect));
    r.bottom = r.top + (height > 0 ? height : rh(&surface->rect));
    if (!memcmp(&r, &surface->rect, sizeof(r))) return;
    AsbRwHostOwnedMove *m = owned_move(p, surface_id, 1);
    if (!m) return;
    m->serial = ++s->next_serial;
    m->rect = r;
    memset(&s->tx.geometry, 0, sizeof(s->tx.geometry));
    s->tx.geometry.id = surface_id;
    s->tx.geometry.serial = m->serial;
    s->tx.geometry.show_state = ASB_RW_SHOW_NORMAL;
    s->tx.geometry.rect = r;
    for (uint32_t k = 0; k < p->surfaces.count; ++k)
        if (p->surfaces.surfaces[k].surface_id == surface_id) p->surfaces.surfaces[k].rect = r;
    send_tx(s, ASB_RW_GEOMETRY_COMMAND);
}

void asb_rw_session_close(AsbRwSession *s, uint64_t id, uint64_t surface_id)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    if (!p || !p->created) return;
    if (!surface_id) surface_id = id;
    if (surface_id != id) {
        const AsbRwSurface *surface = find_surface(p, surface_id);
        if (!surface || surface->kind != ASB_RW_SURFACE_OWNED) return;
    } else if (p->have_window && (p->window.flags & ASB_RW_WS_DISABLED)) {
        /* A modal owned window owns the decision: bring it forward instead of asking the root. */
        if (s->ops.proxy_activate_modal) s->ops.proxy_activate_modal(s->ops.ctx, p->id);
        return;
    }
    if (p->close_id == surface_id) return;   /* already asked; the application is deciding */
    p->close_id = surface_id;
    p->close_tick = s->now_ms;
    memset(&s->tx.close, 0, sizeof(s->tx.close));
    s->tx.close.id = surface_id;
    send_tx(s, ASB_RW_CLOSE_REQUEST);
}

uint32_t asb_rw_session_hit_test(AsbRwSession *s, uint64_t id, uint64_t surface_id, int32_t x, int32_t y)
{
    AsbRwHostProxy *p = asb_rw_session_proxy(s, id);
    AsbRwHitQuery *q = query_for(s, id);
    const AsbRwHitEntry *near_entry = NULL;
    int32_t near_d = 0x7FFFFFFF;
    if (!p || !p->created || !q) return HT_CLIENT;
    if (!surface_id) surface_id = id;
    if (x < -(int32_t)ASB_RW_MAX_FRAME_WIDTH || x > (int32_t)ASB_RW_MAX_FRAME_WIDTH ||
        y < -(int32_t)ASB_RW_MAX_FRAME_HEIGHT || y > (int32_t)ASB_RW_MAX_FRAME_HEIGHT)
        return HT_NOWHERE;
    for (uint32_t i = 0; i < ASB_RW_HOST_HIT_CACHE; ++i) {
        const AsbRwHitEntry *e = &s->hits[i];
        if (e->surface_id != surface_id || s->now_ms - e->tick > ASB_RW_HOST_HIT_TTL_MS) continue;
        int32_t dx = e->x - x, dy = e->y - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        int32_t d = dx > dy ? dx : dy;
        if (d <= ASB_RW_HOST_HIT_RADIUS) return e->code;
        if (d < near_d) { near_d = d; near_entry = e; }
    }
    if (!q->surface_id) send_hit(s, q, surface_id, x, y);
    else if (q->surface_id == surface_id && (q->x != x || q->y != y)) { q->queued = 1; q->qx = x; q->qy = y; }
    return near_entry && near_d <= ASB_RW_HOST_HIT_NEAR ? near_entry->code : HT_CLIENT;
}

/* ================================ input (eh6.7) =========================== */

#define VK_CANCEL_  0x03u
#define VK_TAB_     0x09u
#define VK_PAUSE_   0x13u
#define VK_ESCAPE_  0x1Bu
#define VK_SNAPSHOT_ 0x2Cu
#define VK_LWIN_    0x5Bu
#define VK_RWIN_    0x5Cu
#define VK_SLEEP_   0x5Fu
#define VK_F4_      0x73u

int asb_rw_input_key_is_local(uint32_t vk, int alt_down, int ctrl_down, int win_down)
{
    if (vk == VK_LWIN_ || vk == VK_RWIN_ || win_down) return 1;   /* Start, Win+X shortcuts, Snap keys */
    if (vk == VK_TAB_ && alt_down) return 1;                      /* Alt+Tab */
    if (vk == VK_ESCAPE_ && (alt_down || ctrl_down)) return 1;    /* Alt+Esc, Ctrl+Esc */
    if (vk == VK_F4_ && alt_down) return 1;                       /* host close -> CLOSE_REQUEST */
    if (vk == VK_SNAPSHOT_) return 1;                             /* host screenshot */
    return 0;
}

static int input_enabled(const AsbRwSession *s)
{
    return proxies_supported(s) && (s->negotiated.capabilities & ASB_RW_CAP_ACTIVATION) && s->ops.input_claim &&
           s->ops.input_send;
}

static const AsbRwHostProxy *proxy_const(const AsbRwSession *s, uint64_t id)
{
    for (uint32_t i = 0; id && i < ASB_RW_HOST_MAX_PROXIES; ++i)
        if (s->proxies[i].id == id) return &s->proxies[i];
    return NULL;
}

int asb_rw_session_map_point(const AsbRwSession *s, uint64_t root, uint64_t surface, int32_t x, int32_t y,
                             int require_inside, int32_t *gx, int32_t *gy)
{
    const AsbRwHostProxy *p = proxy_const(s, root);
    if (!p || p->state != ASB_RW_STATE_SHARED || !p->have_surfaces) return 0;
    if (!surface) surface = root;
    const AsbRwSurface *sf = find_surface(p, surface);
    if (!sf) return 0;                                   /* surface gone or never acknowledged */
    for (uint32_t i = 0; i < ASB_RW_HOST_MAX_OWNED_MOVES; ++i)
        if (p->moves[i].surface_id == surface && p->moves[i].serial) return 0;   /* host move unacknowledged */
    if (require_inside && (x < 0 || y < 0 || x >= rw(&sf->rect) || y >= rh(&sf->rect))) return 0;
    *gx = sf->rect.left + x;
    *gy = sf->rect.top + y;
    return 1;
}

/* Key table index (scan, +256 extended, or 512 + VK for scanless function keys); -1 = never sent.
   Mirrors the desktop viewer's INPUT_KEY_PHYSICAL rules. */
static int key_index(uint32_t vk, uint32_t *scan, uint32_t *ext)
{
    if (vk == VK_PAUSE_ || vk == VK_CANCEL_) { *scan = 0; *ext = 0; }
    if (*scan == 0x36 || *scan == 0x45) *ext = 0;   /* right Shift / Num Lock carry no E0 prefix */
    if (vk > 255 || *scan > 255) return -1;
    if (!*scan) {
        if (vk != VK_CANCEL_ && vk != VK_PAUSE_ && vk != VK_SNAPSHOT_ && vk != VK_SLEEP_ && !(vk >= 0xA6 && vk <= 0xB7))
            return -1;
        return (int)(512 + vk);
    }
    return (int)(*scan + (*ext ? 256u : 0u));
}

static uint32_t input_send(AsbRwSession *s, uint32_t type, uint32_t p1, uint32_t p2, uint32_t p3)
{
    InputPacket packet;
    packet.magic = INPUT_MAGIC;
    packet.type = type;
    packet.param1 = p1;
    packet.param2 = p2;
    packet.param3 = p3;
    return s->ops.input_send(s->ops.ctx, s->input.epoch, &packet);
}

static void set_guard(AsbRwSession *s, int on)
{
    if (s->input.guard == on) return;
    s->input.guard = on;
    if (s->ops.input_guard) s->ops.input_guard(s->ops.ctx, on);
}

/* Every key/button this source holds goes up in the guest (no-op when the router already
   released them under a newer epoch: those sends are refused). */
static void release_held(AsbRwSession *s)
{
    AsbRwHostInput *in = &s->input;
    if (in->epoch && s->ops.input_send) {
        for (uint32_t i = 0; i < ASB_RW_INPUT_KEYS; ++i) {
            if (!in->keys[i]) continue;
            uint32_t scan = i < 512 ? (i & 0xFFu) : 0u, ext = i >= 256 && i < 512 ? INPUT_KEY_EXTENDED : 0u;
            input_send(s, INPUT_KEY_PHYSICAL, in->key_vk[i], scan, ext | INPUT_KEY_UP);
            in->released++;
        }
        for (uint32_t b = 0; b < 3 && !s->drag.state; ++b)   /* eh6.9: a drag's button belongs to the helper */
            if (in->buttons & (1u << b)) { input_send(s, INPUT_MOUSE_BUTTON, b, 0, 0); in->released++; }
    }
    memset(in->keys, 0, sizeof(in->keys));
    in->buttons = 0;
}

static void drop_queue(AsbRwSession *s)
{
    s->input.dropped_refused += s->input.queued;
    s->input.queued = 0;
}

static void enqueue(AsbRwSession *s, const AsbRwInputEvent *e)
{
    if (s->input.queued >= ASB_RW_INPUT_QUEUE) { s->input.dropped_overflow++; return; }
    s->input.queue[s->input.queued++] = *e;
}

static void send_activate(AsbRwSession *s, uint64_t id, uint32_t epoch)
{
    memset(&s->tx.activate, 0, sizeof(s->tx.activate));
    s->tx.activate.id = id;
    s->tx.activate.input_epoch = epoch;
    send_tx(s, ASB_RW_ACTIVATE_REQUEST);
}

/* Asks the guest to move its foreground off the shared windows (another source took over). */
static void request_release(AsbRwSession *s, uint32_t hold)
{
    AsbRwHostInput *in = &s->input;
    if (in->release_pending && in->release_hold == hold) return;
    in->release_pending = 1;
    in->release_hold = hold;
    in->release_tick = s->now_ms;
    send_activate(s, 0, hold);
}

static void input_activate(AsbRwSession *s)
{
    AsbRwHostInput *in = &s->input;
    if (!in->host_root || !input_enabled(s)) return;
    release_held(s);
    in->epoch = 0;
    uint32_t epoch = s->ops.input_claim(s->ops.ctx, 1);
    if (!epoch) {   /* automation owns input or the channel is down: nothing may be injected */
        in->state = ASB_RW_INPUT_LOST;
        drop_queue(s);
        return;
    }
    in->state = ASB_RW_INPUT_ACTIVATING;
    in->request_epoch = epoch;
    in->target = in->host_surface ? in->host_surface : in->host_root;
    in->request_tick = s->now_ms;
    in->activations++;
    send_activate(s, in->target, epoch);
}

static void input_failure(AsbRwSession *s, uint32_t error)
{
    AsbRwHostInput *in = &s->input;
    in->failures++;
    in->last_error = error;
    drop_queue(s);
    in->state = ASB_RW_INPUT_LOST;
    in->epoch = 0;
    slog(s, "input: activation of %016llx failed (%s); queued input discarded", (unsigned long long)in->target,
         asb_rw_error_name(error));
}

/* 0 injected, 1 the epoch went stale (re-activate), -1 dropped. */
static int inject(AsbRwSession *s, const AsbRwInputEvent *e)
{
    AsbRwHostInput *in = &s->input;
    uint32_t r;
    if (!e->kind) {
        uint32_t scan = e->scan, ext = e->extended ? 1u : 0u;
        int idx = key_index(e->vk, &scan, &ext);
        if (idx < 0 || (e->up && !in->keys[idx])) return -1;   /* never pressed through this source */
        r = input_send(s, INPUT_KEY_PHYSICAL, e->vk, scan, (ext ? INPUT_KEY_EXTENDED : 0u) | (e->up ? INPUT_KEY_UP : 0u));
        if (r == 5) return 1;
        if (r) { in->dropped_refused++; in->last_error = r; return -1; }
        in->keys[idx] = e->up ? 0u : 1u;
        in->key_vk[idx] = (uint16_t)e->vk;
        in->injected++;
        return 0;
    }
    uint32_t bit = e->button < 3 ? 1u << e->button : 0u;
    if ((e->kind == ASB_RW_MOUSE_DOWN || e->kind == ASB_RW_MOUSE_UP) && !bit) return -1;
    if (e->kind == ASB_RW_MOUSE_UP && !(in->buttons & bit)) return -1;
    int need_inside = e->kind == ASB_RW_MOUSE_DOWN || e->kind == ASB_RW_MOUSE_WHEEL ||
                      (e->kind == ASB_RW_MOUSE_MOVE && !in->buttons);
    int32_t gx = 0, gy = 0;
    if (!asb_rw_session_map_point(s, e->root, e->surface, e->x, e->y, need_inside, &gx, &gy)) {
        if (e->kind != ASB_RW_MOUSE_UP) { in->dropped_stale++; return -1; }
        /* The surface went away mid-drag: release where the pointer is. */
        r = input_send(s, INPUT_MOUSE_BUTTON, e->button, 0, 0);
        if (r == 5) return 1;
        in->buttons &= ~bit;
        in->injected++;
        return 0;
    }
    r = input_send(s, INPUT_MOUSE_MOVE, (uint32_t)gx, (uint32_t)gy, INPUT_MOUSE_VIRTUAL);
    if (r == 5) return 1;
    if (r) { in->dropped_refused++; in->last_error = r; return -1; }
    if (e->kind == ASB_RW_MOUSE_DOWN || e->kind == ASB_RW_MOUSE_UP) {
        r = input_send(s, INPUT_MOUSE_BUTTON, e->button, e->kind == ASB_RW_MOUSE_DOWN ? 1u : 0u, 0);
        if (r == 5) return 1;
        if (r) { in->dropped_refused++; in->last_error = r; return -1; }
        if (e->kind == ASB_RW_MOUSE_DOWN) in->buttons |= bit;
        else in->buttons &= ~bit;
    } else if (e->kind == ASB_RW_MOUSE_WHEEL) {
        r = input_send(s, INPUT_MOUSE_WHEEL, (uint32_t)e->wheel, 0, 0);
        if (r == 5) return 1;
        if (r) { in->dropped_refused++; in->last_error = r; return -1; }
    }
    in->injected++;
    return 0;
}

static void flush_queue(AsbRwSession *s)
{
    AsbRwHostInput *in = &s->input;
    uint32_t i = 0;
    for (; i < in->queued; ++i) {
        if (inject(s, &in->queue[i]) == 1) break;
    }
    if (i < in->queued) {
        /* The epoch went stale while flushing: keep the rest and activate again (once). */
        memmove(in->queue, in->queue + i, (in->queued - i) * sizeof(in->queue[0]));
        in->queued -= i;
        in->state = ASB_RW_INPUT_LOST;
        in->epoch = 0;
        memset(in->keys, 0, sizeof(in->keys));
        in->buttons = 0;
        input_activate(s);
        return;
    }
    in->queued = 0;
}

static void dispatch(AsbRwSession *s, const AsbRwInputEvent *e)
{
    AsbRwHostInput *in = &s->input;
    switch (in->state) {
    case ASB_RW_INPUT_ACTIVE:
        if (inject(s, e) == 1) {   /* another source or automation took over meanwhile */
            in->state = ASB_RW_INPUT_LOST;
            in->epoch = 0;
            memset(in->keys, 0, sizeof(in->keys));
            in->buttons = 0;
            enqueue(s, e);
            input_activate(s);
        }
        break;
    case ASB_RW_INPUT_ACTIVATING:
        enqueue(s, e);
        break;
    case ASB_RW_INPUT_LOST:
        if (e->kind == ASB_RW_MOUSE_MOVE) { in->dropped_stale++; break; }   /* hover never re-activates */
        enqueue(s, e);
        input_activate(s);
        break;
    default:
        in->dropped_refused++;   /* no proxy has host activation */
        break;
    }
}

void asb_rw_session_input_focus(AsbRwSession *s, uint64_t root, uint64_t surface)
{
    AsbRwHostInput *in = &s->input;
    if (!root) {
        release_held(s);
        drop_queue(s);
        in->host_root = in->host_surface = 0;
        in->state = ASB_RW_INPUT_IDLE;
        in->epoch = 0;
        return;
    }
    if (!surface) surface = root;
    if (in->host_root == root && in->host_surface == surface &&
        (in->state == ASB_RW_INPUT_ACTIVE || in->state == ASB_RW_INPUT_ACTIVATING))
        return;
    drop_queue(s);
    in->host_root = root;
    in->host_surface = surface;
    input_activate(s);
}

void asb_rw_session_input_mouse(AsbRwSession *s, uint64_t root, uint64_t surface, uint32_t kind, uint32_t button,
                                int32_t x, int32_t y, int32_t wheel)
{
    AsbRwInputEvent e;
    if (!input_enabled(s) || kind < ASB_RW_MOUSE_MOVE || kind > ASB_RW_MOUSE_WHEEL) return;
    if (s->drag.state) {   /* eh6.9: the guest helper owns the pointer */
        if (kind == ASB_RW_MOUSE_UP && (s->drag.state == ASB_RW_HDRAG_PROBING || s->drag.state == ASB_RW_HDRAG_SINK)) {
            memset(&s->tx.drag, 0, sizeof(s->tx.drag));
            s->tx.drag.probe = s->drag.probe;
            s->tx.drag.state = ASB_RW_DRAG_RELEASE;
            send_tx(s, ASB_RW_DRAG_ACK);
            s->input.buttons = 0;
        }
        s->input.dropped_refused++;
        return;
    }
    if (!root || root != s->input.host_root) {
        s->input.dropped_refused++;   /* the pointer belongs to the current owner, not a background proxy */
        return;
    }
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.root = root;
    e.surface = surface ? surface : root;
    e.x = x;
    e.y = y;
    e.wheel = wheel;
    e.button = button;
    dispatch(s, &e);
}

int asb_rw_session_input_key(AsbRwSession *s, uint32_t vk, uint32_t scan, int extended, int up)
{
    AsbRwInputEvent e;
    if (!input_enabled(s) || !s->input.host_root) return 0;
    if (s->drag.state) { s->input.dropped_refused++; return 1; }   /* eh6.9: Escape etc. belong to the drag loops */
    memset(&e, 0, sizeof(e));
    e.root = s->input.host_root;
    e.vk = vk;
    e.scan = scan;
    e.extended = extended ? 1u : 0u;
    e.up = up ? 1u : 0u;
    dispatch(s, &e);
    return 1;
}

void asb_rw_session_input_changed(AsbRwSession *s, const AsbRwInputRouterState *st)
{
    AsbRwHostInput *in = &s->input;
    in->router = *st;
    in->have_router = 1;
    uint32_t mine = in->state == ASB_RW_INPUT_ACTIVE ? in->epoch : in->state == ASB_RW_INPUT_ACTIVATING ? in->request_epoch : 0;
    if (mine && st->epoch != mine) {
        /* The viewer, automation or a reconnect superseded our epoch; the router released what
           we held. Nothing more is injected until a new activation. */
        memset(in->keys, 0, sizeof(in->keys));
        in->buttons = 0;
        drop_queue(s);
        in->state = ASB_RW_INPUT_LOST;
        in->epoch = 0;
    }
    if ((s->drag.state == ASB_RW_HDRAG_TO_GUEST || s->drag.state == ASB_RW_HDRAG_FROM_GUEST) && s->drag.epoch &&
        (!st->user_owns || st->epoch != s->drag.epoch)) {
        /* eh6.9: the viewer or automation took the input over: end the drag. */
        memset(&s->tx.drag, 0, sizeof(s->tx.drag));
        s->tx.drag.session_id = s->drag.session_id;
        s->tx.drag.state = ASB_RW_DRAG_CANCEL;
        s->tx.drag.error = ASB_RW_ERR_BUSY;
        send_tx(s, ASB_RW_DRAG_ACK);
        s->drag.cancelled++;
        s->drag.epoch = 0;   /* answered once; ENDED returns to normal */
    }
    if (!input_enabled(s) || !in->guard) return;
    if (st->hold) {
        if (!(in->release_pending && in->release_hold == st->hold)) request_release(s, st->hold);
    } else if (!st->user_owns && !in->release_pending) {
        request_release(s, 0);   /* automation: never leave the foreground on a hidden window */
    }
}

static void handle_activate_ack(AsbRwSession *s, const AsbRwActivate *a)
{
    AsbRwHostInput *in = &s->input;
    if (!input_enabled(s)) return;   /* no input forwarding on this host: the guest notices are informational */
    if (!a->id) {   /* foreground release: foreground = 1 when no shared window has it any more */
        if (!in->release_pending || a->input_epoch != in->release_hold) return;
        int ok = a->foreground && !a->error;
        if (in->release_hold && s->ops.input_unhold) s->ops.input_unhold(s->ops.ctx, in->release_hold, ok);
        in->release_pending = 0;
        in->release_hold = 0;
        if (ok) set_guard(s, 0);
        else slog(s, "input: guest foreground could not be released (%s)", asb_rw_error_name(a->error));
        return;
    }
    if (!id_current(s, a->id)) return;
    if (in->state == ASB_RW_INPUT_ACTIVATING && a->id == in->target && a->input_epoch == in->request_epoch) {
        if (a->foreground && !a->error) {
            in->state = ASB_RW_INPUT_ACTIVE;
            in->epoch = in->request_epoch;
            set_guard(s, 1);
            flush_queue(s);
        } else {
            input_failure(s, a->error ? a->error : ASB_RW_ERR_FOREGROUND_DENIED);
        }
        return;
    }
    if (a->input_epoch && in->state == ASB_RW_INPUT_ACTIVE && a->input_epoch == in->epoch && !a->foreground) {
        /* The guest moved its foreground off our target (new window, dialog, lock screen). */
        release_held(s);
        in->state = ASB_RW_INPUT_LOST;
        in->epoch = 0;
        set_guard(s, 0);
        slog(s, "input: %016llx lost the guest foreground; held input released", (unsigned long long)a->id);
        return;
    }
    if (!a->input_epoch && a->foreground) {
        /* A shared window took the guest foreground on its own. */
        set_guard(s, 1);
        if (!(in->router.windows_current && in->router.user_owns)) {
            uint32_t hold = s->ops.input_hold ? s->ops.input_hold(s->ops.ctx) : 0;
            request_release(s, hold);
        } else if (in->state == ASB_RW_INPUT_ACTIVE && asb_rw_session_root_of(s, a->id) != in->host_root) {
            release_held(s);
            in->state = ASB_RW_INPUT_LOST;   /* re-verify our own target before the next input */
            in->epoch = 0;
        }
    }
}

static void input_forget_root(AsbRwSession *s, uint64_t root)
{
    AsbRwHostInput *in = &s->input;
    if (!root || in->host_root != root) return;
    release_held(s);
    drop_queue(s);
    in->host_root = in->host_surface = 0;
    in->state = ASB_RW_INPUT_IDLE;
    in->epoch = 0;
    set_guard(s, 0);
}

static void input_tick(AsbRwSession *s)
{
    AsbRwHostInput *in = &s->input;
    if (in->state == ASB_RW_INPUT_ACTIVATING && s->now_ms - in->request_tick > ASB_RW_INPUT_ACTIVATE_MS)
        input_failure(s, ASB_RW_ERR_BUSY);
    if (in->release_pending && s->now_ms - in->release_tick > ASB_RW_INPUT_RELEASE_MS) {
        in->release_pending = 0;   /* the router discards its own hold on timeout */
        in->release_hold = 0;
    }
}

/* The control session ends (disconnect / manager stop): nothing stays pressed, and keys held
   for our guard are discarded rather than typed into a window that may still be hidden. */
static void input_end(AsbRwSession *s)
{
    AsbRwHostInput *in = &s->input;
    s->drag.state = ASB_RW_HDRAG_IDLE;   /* the guest helper releases its pointer when the session ends */
    in->buttons = 0;
    release_held(s);
    in->queued = 0;
    if (in->release_pending && in->release_hold && s->ops.input_unhold) s->ops.input_unhold(s->ops.ctx, in->release_hold, 0);
    in->release_pending = 0;
    set_guard(s, 0);
    in->state = ASB_RW_INPUT_IDLE;
    in->host_root = in->host_surface = 0;
}

/* ================================ drag and drop (eh6.9) =================== */

int asb_rw_session_files_enabled(const AsbRwSession *s, uint32_t direction_cap)
{
    return s->phase == ASB_RW_SESSION_READY && (s->negotiated.capabilities & s->ops.file_caps & direction_cap) != 0;
}

int asb_rw_session_drag_busy(const AsbRwSession *s)
{
    return s->drag.state != ASB_RW_HDRAG_IDLE;
}

uint32_t asb_rw_session_drag_probe(AsbRwSession *s, uint64_t root, uint64_t surface, uint32_t button)
{
    if (!asb_rw_session_files_enabled(s, ASB_RW_CAP_FILES_FROM_GUEST)) return ASB_RW_ERR_UPGRADE_REQUIRED;
    if (s->drag.state) return ASB_RW_ERR_BUSY;
    if (button > INPUT_BTN_RIGHT || !root || root != s->input.host_root || s->input.state != ASB_RW_INPUT_ACTIVE ||
        !(s->input.buttons & (1u << button)))
        return ASB_RW_ERR_STALE_ID;   /* only a drag this source pressed in the active proxy */
    if (!surface) surface = root;
    if (!++s->drag.next_probe) s->drag.next_probe = 1;
    memset(&s->tx.drag, 0, sizeof(s->tx.drag));
    s->tx.drag.surface_id = surface;
    s->tx.drag.probe = s->drag.next_probe;
    s->tx.drag.button = button;
    if (!send_tx(s, ASB_RW_DRAG_PROBE)) return ASB_RW_ERR_INTERNAL;
    s->drag.state = ASB_RW_HDRAG_PROBING;
    s->drag.probe = s->drag.next_probe;
    s->drag.button = button;
    s->drag.session_id = 0;
    s->drag.tick = s->now_ms;
    s->drag.probes++;
    slog(s, "drag: pointer left %016llx with a button held; probing for a guest drag (%u)", (unsigned long long)surface,
         s->drag.probe);
    return ASB_RW_OK;
}

static void drag_ack(AsbRwSession *s, uint64_t session_id, uint32_t probe, uint32_t state, uint32_t error)
{
    memset(&s->tx.drag, 0, sizeof(s->tx.drag));
    s->tx.drag.session_id = session_id;
    s->tx.drag.probe = probe;
    s->tx.drag.state = state;
    s->tx.drag.error = error;
    send_tx(s, ASB_RW_DRAG_ACK);
}

static void handle_drag_state(AsbRwSession *s, const AsbRwDrag *d)
{
    AsbRwHostDrag *g = &s->drag;
    AsbRwHostInput *in = &s->input;
    switch (d->state) {
    case ASB_RW_DRAG_ACTIVE:
        if (d->direction == ASB_RW_DIR_TO_GUEST) {
            uint32_t epoch = 0;
            if (!asb_rw_session_files_enabled(s, ASB_RW_CAP_FILES_TO_GUEST) || g->state) {
                drag_ack(s, d->session_id, 0, ASB_RW_DRAG_CANCEL, ASB_RW_ERR_BUSY);
                g->refused++;
                return;
            }
            if (input_enabled(s)) {
                if (!in->have_router || in->router.user_owns) {
                    release_held(s);
                    drop_queue(s);
                    epoch = s->ops.input_claim(s->ops.ctx, 1);   /* fresh epoch: every other holder is released */
                }
            } else {
                epoch = 1;   /* no input arbitration on this host (tests): nothing else injects */
            }
            if (!epoch) {
                drag_ack(s, d->session_id, 0, ASB_RW_DRAG_CANCEL, ASB_RW_ERR_BUSY);
                g->refused++;
                g->last_error = ASB_RW_ERR_BUSY;
                slog(s, "drag: host -> guest drop refused (input owned by automation or unavailable)");
                return;
            }
            if (in->state == ASB_RW_INPUT_ACTIVE || in->state == ASB_RW_INPUT_ACTIVATING) {
                in->state = ASB_RW_INPUT_LOST;   /* the proxy re-activates after the drag */
                in->epoch = 0;
            }
            g->state = ASB_RW_HDRAG_TO_GUEST;
            g->session_id = d->session_id;
            g->epoch = input_enabled(s) ? epoch : 0;
            g->tick = s->now_ms;
            g->to_guest++;
            drag_ack(s, d->session_id, 0, ASB_RW_DRAG_ALLOW, ASB_RW_OK);
            slog(s, "drag: host -> guest drag %016llx may drive the guest pointer", (unsigned long long)d->session_id);
            return;
        }
        if (g->state != ASB_RW_HDRAG_PROBING || d->probe != g->probe) {
            drag_ack(s, d->session_id, d->probe, ASB_RW_DRAG_CANCEL, ASB_RW_ERR_STALE_ID);
            return;
        }
        g->state = ASB_RW_HDRAG_FROM_GUEST;
        g->session_id = d->session_id;
        g->epoch = in->state == ASB_RW_INPUT_ACTIVE ? in->epoch : 0;
        g->from_guest++;
        in->buttons = 0;   /* the helper releases the guest button on its catcher */
        if (s->ops.drag_capture) s->ops.drag_capture(s->ops.ctx);
        slog(s, "drag: guest drag %016llx handed to the host", (unsigned long long)d->session_id);
        return;
    case ASB_RW_DRAG_NONE:
        if (g->state == ASB_RW_HDRAG_PROBING && d->probe == g->probe) {
            g->state = ASB_RW_HDRAG_IDLE;   /* forwarding resumes; the next move puts the guest pointer back */
            if (d->error) g->last_error = d->error;
        }
        return;
    case ASB_RW_DRAG_SINK:
        if (g->state == ASB_RW_HDRAG_PROBING && d->probe == g->probe) {
            g->state = ASB_RW_HDRAG_SINK;
            g->last_error = d->error;
            if (!(in->buttons & (1u << g->button))) drag_ack(s, 0, g->probe, ASB_RW_DRAG_RELEASE, ASB_RW_OK);   /* already up */
            slog(s, "drag: guest drag cannot be copied (%s); parked until the button goes up", asb_rw_error_name(d->error));
        }
        return;
    case ASB_RW_DRAG_ENDED:
        if (g->state == ASB_RW_HDRAG_IDLE) return;
        if (d->session_id && g->session_id && d->session_id != g->session_id) return;
        if (!d->session_id && d->probe && d->probe != g->probe) return;
        g->state = ASB_RW_HDRAG_IDLE;
        g->session_id = 0;
        g->epoch = 0;
        if (d->error) g->last_error = d->error;
        in->buttons = 0;
        changed(s);
        return;
    }
}

static void drag_tick(AsbRwSession *s)
{
    if (s->drag.state == ASB_RW_HDRAG_PROBING && s->now_ms - s->drag.tick > ASB_RW_DRAG_PROBE_MS) {
        slog(s, "drag: probe %u unanswered; resuming input", s->drag.probe);
        s->drag.state = ASB_RW_HDRAG_IDLE;
    }
}
