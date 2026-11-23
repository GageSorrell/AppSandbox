/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_helper
 *
 * @file      remote_windows_helper.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows guest helper control session (AppSandbox-eh6.4). */
#include "remote_windows_helper.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void helper_log(AsbRwHelper *h, const char *fmt, ...)
{
    char text[512];
    va_list args;
    if (!h->ops.log) return;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    h->ops.log(h->ops.ctx, text);
}

static void send_tx(AsbRwHelper *h, uint16_t type, uint32_t request_id)
{
    size_t written = 0;
    uint32_t result = asb_rw_encode(type, request_id, h->tx, h->out,
                                    ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD, &written);
    if (result) {
        helper_log(h, "encode type %u failed (%s)", type, asb_rw_error_name(result));
        return;
    }
    if (!h->send_failed && !h->ops.send(h->ops.ctx, h->out, written)) h->send_failed = 1;
}

static void send_error(AsbRwHelper *h, uint32_t request_id, uint32_t code, uint32_t detail)
{
    memset(&h->tx->error, 0, sizeof(h->tx->error));
    h->tx->error.code = code;
    h->tx->error.detail = detail;
    send_tx(h, ASB_RW_ERROR, request_id);
}

static void send_result(AsbRwHelper *h, uint32_t request_id, uint64_t id, uint32_t state, uint32_t error)
{
    memset(&h->tx->select_result, 0, sizeof(h->tx->select_result));
    h->tx->select_result.id = id;
    h->tx->select_result.state = state;
    h->tx->select_result.error = error;
    send_tx(h, ASB_RW_SELECT_RESULT, request_id);
}

static void send_snapshot(AsbRwHelper *h, uint32_t request_id)
{
    asb_rw_inventory_snapshot(&h->inventory, &h->tx->snapshot);
    send_tx(h, ASB_RW_INVENTORY_SNAPSHOT, request_id);
}

/* Rebuilds the inventory; shared roots stay listed (selected) and transitions are pending. */
static void refresh_inventory(AsbRwHelper *h, int send_events)
{
    uint32_t count = 0, n_events = 0, dropped = 0;
    h->ops.enumerate(h->ops.ctx, h->facts, ASB_RW_MAX_WINDOWS, &count);
    for (uint32_t i = 0; i < count; ++i) {
        AsbRwWindowFacts *f = &h->facts[i];
        int k = asb_rw_share_find(h->share, (HWND)(ULONG_PTR)f->identity.hwnd);
        if (k < 0) continue;
        const AsbRwIdentity *bound = &h->share->shared[k].j.identity;
        if (bound->pid != f->identity.pid || bound->thread_id != f->identity.thread_id ||
            bound->process_start != f->identity.process_start) continue;   /* reused handle */
        f->selected = 1;
        f->pending = h->share->shared[k].j.state != ASB_RW_ENTRY_PARKED;
    }
    asb_rw_inventory_update(&h->inventory, &h->ids, h->facts, count, h->events, ASB_RW_HELPER_MAX_EVENTS,
                            &n_events, &dropped);
    if (!send_events || !h->hello_done) return;
    if (dropped) {
        send_snapshot(h, 0);
        return;
    }
    for (uint32_t i = 0; i < n_events; ++i) {
        h->tx->event = h->events[i];
        send_tx(h, ASB_RW_INVENTORY_EVENT, 0);
    }
}

/* ---- surfaces (eh6.5) ----------------------------------------------------------------- */

static int same_surfaces(const AsbRwSurfaces *a, const AsbRwSurfaces *b)
{
    if (a->root_id != b->root_id || a->count != b->count || memcmp(&a->root_rect, &b->root_rect, sizeof(a->root_rect)))
        return 0;
    for (uint32_t i = 0; i < a->count; ++i) {
        const AsbRwSurface *x = &a->surfaces[i], *y = &b->surfaces[i];
        if (x->surface_id != y->surface_id || x->kind != y->kind || x->z != y->z || memcmp(&x->rect, &y->rect, sizeof(x->rect)))
            return 0;
    }
    return 1;
}

static int surface_listed(const AsbRwSurfaces *s, uint64_t id)
{
    for (uint32_t i = 0; i < s->count; ++i) if (s->surfaces[i].surface_id == id) return 1;
    return 0;
}

/* Releases popup IDs of `old` that are not in `now` (NULL: all of them). */
static void release_popup_ids(AsbRwHelper *h, const AsbRwSurfaces *old, const AsbRwSurfaces *now)
{
    for (uint32_t i = 0; i < old->count; ++i) {
        const AsbRwSurface *s = &old->surfaces[i];
        if (s->kind == ASB_RW_SURFACE_ROOT || (now && surface_listed(now, s->surface_id))) continue;
        asb_rw_ids_release(&h->ids, s->surface_id);
    }
}

static void end_root(AsbRwHelper *h, AsbRwHelperRoot *r)
{
    if (h->ops.capture) h->ops.capture(h->ops.ctx, r->root_id, NULL, 0);
    release_popup_ids(h, &r->last, NULL);
    if (h->hello_done && r->last.count) {
        memset(&h->tx->surfaces, 0, sizeof(h->tx->surfaces));
        h->tx->surfaces.root_id = r->root_id;
        send_tx(h, ASB_RW_SURFACES, 0);
    }
    memset(r, 0, sizeof(*r));
}

void asb_rw_helper_surfaces(AsbRwHelper *h)
{
    uint32_t count = 0, idx[ASB_RW_MAX_SURFACES], kinds[ASB_RW_MAX_SURFACES];
    if (!h->ops.scan || !h->roots) return;
    /* Roots that stopped being shared (or lost their session ID) end first. */
    for (uint32_t r = 0; r < ASB_RW_SHARE_MAX; ++r) {
        AsbRwHelperRoot *root = &h->roots[r];
        if (!root->root_id) continue;
        int k = asb_rw_share_find(h->share, (HWND)(ULONG_PTR)root->hwnd);
        if (k < 0 || h->share->shared[k].wire_id != root->root_id || h->share->shared[k].j.state != ASB_RW_ENTRY_PARKED)
            end_root(h, root);
    }
    int any = 0;
    for (uint32_t k = 0; k < h->share->count; ++k)
        any |= h->share->shared[k].wire_id && h->share->shared[k].j.state == ASB_RW_ENTRY_PARKED;
    if (!any) return;
    h->ops.scan(h->ops.ctx, h->facts, ASB_RW_MAX_WINDOWS, &count);
    memset(h->claimed, 0, ASB_RW_MAX_WINDOWS);
    AsbRwRect priv = { h->share->private_rect.left, h->share->private_rect.top, h->share->private_rect.right,
                       h->share->private_rect.bottom };
    const AsbRwRect *private_rect = h->share->display_active ? &priv : NULL;
    for (uint32_t k = 0; k < h->share->count; ++k) {
        const AsbRwShared *sh = &h->share->shared[k];
        if (!sh->wire_id || sh->j.state != ASB_RW_ENTRY_PARKED) continue;
        AsbRwHelperRoot *root = NULL;
        for (uint32_t r = 0; r < ASB_RW_SHARE_MAX && !root; ++r)
            if (h->roots[r].root_id == sh->wire_id) root = &h->roots[r];
        for (uint32_t r = 0; r < ASB_RW_SHARE_MAX && !root; ++r)
            if (!h->roots[r].root_id) {
                root = &h->roots[r];
                root->root_id = sh->wire_id;
                root->hwnd = sh->j.identity.hwnd;
            }
        if (!root) continue;
        uint32_t n = asb_rw_collect_surfaces(h->facts, count, sh->j.identity.hwnd, private_rect, h->claimed, idx, kinds,
                                             ASB_RW_MAX_SURFACES);
        if (!n) continue;   /* root not visible in this scan (e.g. mid-move); keep the last set */
        const AsbRwWindowFacts *rf = &h->facts[idx[0]];
        if (rf->identity.pid != sh->j.identity.pid || rf->identity.thread_id != sh->j.identity.thread_id) continue;
        AsbRwSurfaces *now = &h->tx->surfaces;
        memset(now, 0, sizeof(*now));
        now->root_id = sh->wire_id;
        now->root_rect = rf->rect;
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const AsbRwWindowFacts *w = &h->facts[idx[i]];
            uint64_t id = sh->wire_id;
            if (i && asb_rw_ids_assign(&h->ids, &w->identity, &id) != ASB_RW_OK) continue;   /* table full */
            AsbRwSurface *s = &now->surfaces[m];
            s->surface_id = id;
            s->kind = kinds[i];
            s->z = m;
            s->rect = w->rect;
            AsbRwCaptureTarget *t = &h->targets[m];
            memset(t, 0, sizeof(*t));
            t->surface_id = id;
            t->root_id = sh->wire_id;
            t->hwnd = w->identity.hwnd;
            t->kind = kinds[i];
            t->z = m;
            t->rect = w->rect;
            ++m;
        }
        now->count = m;
        if (same_surfaces(now, &root->last)) continue;
        release_popup_ids(h, &root->last, now);
        root->last = *now;
        for (uint32_t i = 0; i < m; ++i) root->hwnds[i] = h->targets[i].hwnd;
        if (h->hello_done) send_tx(h, ASB_RW_SURFACES, 0);
        if (h->ops.capture) h->ops.capture(h->ops.ctx, sh->wire_id, h->targets, m);
    }
}

static void note_selection(AsbRwHelper *h, uint64_t id, uint32_t error);

static uint32_t eligibility_error(AsbRwEligibility e)
{
    if (e == ASB_RW_INELIGIBLE_ELEVATED || e == ASB_RW_INELIGIBLE_ELEVATION_UNKNOWN) return ASB_RW_ERR_ELEVATED;
    return ASB_RW_ERR_NOT_ELIGIBLE;
}

static void handle_select(AsbRwHelper *h, uint32_t request_id, const AsbRwSelect *request)
{
    AsbRwIdentity identity;
    uint32_t needed = ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY;
    if ((h->negotiated.capabilities & needed) != needed) {
        send_result(h, request_id, request->id, request->select ? ASB_RW_STATE_FAILED : ASB_RW_STATE_NONE,
                    ASB_RW_ERR_UPGRADE_REQUIRED);
        return;
    }
    memset(&identity, 0, sizeof(identity));
    uint32_t result = asb_rw_ids_resolve(&h->ids, request->id, h->ops.probe, h->ops.ctx, &identity);
    if (result) {
        send_result(h, request_id, request->id, request->select ? ASB_RW_STATE_FAILED : ASB_RW_STATE_NONE, result);
        note_selection(h, request->id, result);
        return;
    }
    HWND hwnd = (HWND)(ULONG_PTR)identity.hwnd;
    if (request->select) {
        if (asb_rw_share_find(h->share, hwnd) < 0) {
            AsbRwWindowFacts *facts = &h->facts[0];
            AsbRwEligibility e;
            if (!h->ops.facts(h->ops.ctx, hwnd, facts)) {
                send_result(h, request_id, request->id, ASB_RW_STATE_FAILED, ASB_RW_ERR_STALE_ID);
                return;
            }
            facts->selected = 0;
            e = asb_rw_root_eligibility(facts);
            if (e != ASB_RW_ELIGIBLE) {
                send_result(h, request_id, request->id, ASB_RW_STATE_FAILED, eligibility_error(e));
                note_selection(h, request->id, eligibility_error(e));
                return;
            }
        }
        send_result(h, request_id, request->id, ASB_RW_STATE_PENDING, ASB_RW_OK);
        result = asb_rw_share_select(h->share, &identity, request->id);
        note_selection(h, request->id, result);
        helper_log(h, "select id=%016llx hwnd=%p -> %s", (unsigned long long)request->id, (void *)hwnd,
                   asb_rw_error_name(result));
        send_result(h, request_id, request->id, result ? ASB_RW_STATE_FAILED : ASB_RW_STATE_SHARED, result);
    } else {
        if (asb_rw_share_find(h->share, hwnd) < 0) {
            send_result(h, request_id, request->id, ASB_RW_STATE_NONE, ASB_RW_OK);
            note_selection(h, request->id, ASB_RW_OK);
            return;
        }
        send_result(h, request_id, request->id, ASB_RW_STATE_RESTORING, ASB_RW_OK);
        result = asb_rw_share_unselect(h->share, hwnd);
        note_selection(h, request->id, result == ASB_RW_ERR_BUSY ? ASB_RW_OK : result);
        helper_log(h, "unselect id=%016llx hwnd=%p -> %s", (unsigned long long)request->id, (void *)hwnd,
                   asb_rw_error_name(result));
        if (result != ASB_RW_ERR_BUSY)
            send_result(h, request_id, request->id, ASB_RW_STATE_NONE, result == ASB_RW_ERR_STALE_ID ? result : ASB_RW_OK);
    }
    refresh_inventory(h, 1);
    asb_rw_helper_surfaces(h);
}

static void handle_restore_all(AsbRwHelper *h, uint32_t request_id)
{
    uint64_t ids[ASB_RW_SHARE_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; i < h->share->count; ++i) ids[n++] = h->share->shared[i].wire_id;
    asb_rw_share_restore_all(h->share);
    asb_rw_helper_surfaces(h);
    for (uint32_t i = 0; i < n; ++i) {
        if (!ids[i]) continue;
        int pending = 0;
        for (uint32_t k = 0; k < h->share->count; ++k) pending |= h->share->shared[k].wire_id == ids[i];
        send_result(h, 0, ids[i], pending ? ASB_RW_STATE_RESTORING : ASB_RW_STATE_NONE, ASB_RW_OK);
    }
    refresh_inventory(h, 1);
    send_tx(h, ASB_RW_RESTORE_ALL, request_id);
}

/* ---- proxy support (eh6.6) --------------------------------------------------------------- */

uint32_t asb_rw_helper_capabilities(const AsbRwHelperOps *ops, uint32_t base)
{
    if (ops->frames && ops->set_rect && ops->responsive && ops->post_close && ops->hit_test && ops->window_state &&
        ops->normalize && ops->icon) {
        base |= ASB_RW_CAP_GEOMETRY;
        if (ops->foreground && ops->activate && ops->desktop) base |= ASB_RW_CAP_ACTIVATION;
    }
    return base;
}

static int geometry_enabled(const AsbRwHelper *h)
{
    return (h->negotiated.capabilities & ASB_RW_CAP_GEOMETRY) && (h->capabilities & ASB_RW_CAP_GEOMETRY);
}

/* Window of a current surface (the root or one of its owned/popup windows) of a shared root.
   The ID must still resolve to the identity the surface was collected with. */
static uint32_t resolve_surface(AsbRwHelper *h, uint64_t id, HWND *hwnd, uint32_t *kind)
{
    AsbRwIdentity identity;
    memset(&identity, 0, sizeof(identity));
    for (uint32_t r = 0; r < ASB_RW_SHARE_MAX; ++r) {
        AsbRwHelperRoot *root = &h->roots[r];
        if (!root->root_id) continue;
        for (uint32_t i = 0; i < root->last.count; ++i) {
            if (root->last.surfaces[i].surface_id != id) continue;
            uint32_t result = asb_rw_ids_resolve(&h->ids, id, h->ops.probe, h->ops.ctx, &identity);
            if (result) return result;
            if (identity.hwnd != root->hwnds[i]) return ASB_RW_ERR_STALE_ID;
            *hwnd = (HWND)(ULONG_PTR)identity.hwnd;
            *kind = root->last.surfaces[i].kind;
            return ASB_RW_OK;
        }
    }
    /* A parked root whose surfaces were not collected yet. */
    for (uint32_t k = 0; k < h->share->count; ++k) {
        const AsbRwShared *sh = &h->share->shared[k];
        if (sh->wire_id != id || sh->j.state != ASB_RW_ENTRY_PARKED) continue;
        uint32_t result = asb_rw_ids_resolve(&h->ids, id, h->ops.probe, h->ops.ctx, &identity);
        if (result) return result;
        if (identity.hwnd != sh->j.identity.hwnd) return ASB_RW_ERR_STALE_ID;
        *hwnd = (HWND)(ULONG_PTR)identity.hwnd;
        *kind = ASB_RW_SURFACE_ROOT;
        return ASB_RW_OK;
    }
    return ASB_RW_ERR_STALE_ID;
}

static AsbRwRect to_wire(const RECT *r)
{
    AsbRwRect w;
    w.left = r->left; w.top = r->top; w.right = r->right; w.bottom = r->bottom;
    return w;
}

static LONG clamp_long(LONG v, LONG lo, LONG hi) { return v < lo ? lo : v > hi ? hi : v; }

static void send_geometry_ack(AsbRwHelper *h, uint32_t request_id, const AsbRwGeometry *cmd, const RECT *applied,
                              uint32_t error)
{
    memset(&h->tx->geometry, 0, sizeof(h->tx->geometry));
    h->tx->geometry.id = cmd->id;
    h->tx->geometry.serial = cmd->serial;
    h->tx->geometry.show_state = cmd->show_state;
    if (applied) h->tx->geometry.rect = to_wire(applied);
    h->tx->geometry.error = error;
    send_tx(h, ASB_RW_GEOMETRY_ACK, request_id);
}

/* Window rectangle whose visible frame is (x, y, w, hgt), keeping the current invisible margins. */
static RECT window_for_visible(const RECT *win, const RECT *now_vis, LONG x, LONG y, LONG w, LONG hgt)
{
    RECT out;
    out.left = x - (now_vis->left - win->left);
    out.top = y - (now_vis->top - win->top);
    out.right = x + w + (win->right - now_vis->right);
    out.bottom = y + hgt + (win->bottom - now_vis->bottom);
    return out;
}

static void handle_geometry(AsbRwHelper *h, uint32_t request_id, const AsbRwGeometry *cmd)
{
    HWND hwnd = NULL;
    uint32_t kind = 0;
    RECT win = { 0, 0, 0, 0 }, vis = { 0, 0, 0, 0 };
    if (!geometry_enabled(h)) {
        send_error(h, request_id, ASB_RW_ERR_UPGRADE_REQUIRED, ASB_RW_GEOMETRY_COMMAND);
        return;
    }
    uint32_t result = resolve_surface(h, cmd->id, &hwnd, &kind);
    if (!result && kind != ASB_RW_SURFACE_ROOT && kind != ASB_RW_SURFACE_OWNED) result = ASB_RW_ERR_NOT_ELIGIBLE;
    if (!result && !h->ops.frames(h->ops.ctx, hwnd, &win, &vis)) result = ASB_RW_ERR_STALE_ID;
    if (result) { send_geometry_ack(h, request_id, cmd, NULL, result); return; }
    /* Host minimization only hides the proxy: the source keeps its size and keeps rendering. */
    if (kind == ASB_RW_SURFACE_ROOT && cmd->show_state == ASB_RW_SHOW_MINIMIZED) {
        send_geometry_ack(h, request_id, cmd, &vis, ASB_RW_OK);
        return;
    }
    if (!h->ops.responsive(h->ops.ctx, hwnd)) { send_geometry_ack(h, request_id, cmd, &vis, ASB_RW_ERR_BUSY); return; }
    LONG w = clamp_long(cmd->rect.right - cmd->rect.left, 1, (LONG)ASB_RW_MAX_FRAME_WIDTH);
    LONG hgt = clamp_long(cmd->rect.bottom - cmd->rect.top, 1, (LONG)ASB_RW_MAX_FRAME_HEIGHT);
    LONG x = vis.left, y = vis.top;
    BOOL size = TRUE;
    if (kind == ASB_RW_SURFACE_ROOT) {
        /* Resize in place; stay on the private display when the new size fits there. */
        const RECT *p = &h->share->private_rect;
        if (h->share->display_active && !IsRectEmpty(p)) {
            if (x + w > p->right) x = p->right - w;
            if (y + hgt > p->bottom) y = p->bottom - hgt;
            if (x < p->left) x = p->left;
            if (y < p->top) y = p->top;
        }
    } else {
        AsbRwWindowState st;
        UINT show = 0;
        memset(&st, 0, sizeof(st));
        x = cmd->rect.left;
        y = cmd->rect.top;
        size = h->ops.window_state(h->ops.ctx, hwnd, FALSE, &st, &show) && (st.flags & ASB_RW_WS_RESIZABLE);
        if (!size) { w = vis.right - vis.left; hgt = vis.bottom - vis.top; }
    }
    RECT target = window_for_visible(&win, &vis, x, y, w, hgt);
    if (!h->ops.set_rect(h->ops.ctx, hwnd, &target, size) || !h->ops.frames(h->ops.ctx, hwnd, &win, &vis)) {
        send_geometry_ack(h, request_id, cmd, NULL, ASB_RW_ERR_STALE_ID);
        return;
    }
    helper_log(h, "geometry id=%016llx serial=%u show=%u want %ldx%ld -> (%ld,%ld)-(%ld,%ld)",
               (unsigned long long)cmd->id, cmd->serial, cmd->show_state, w, hgt, vis.left, vis.top, vis.right, vis.bottom);
    send_geometry_ack(h, request_id, cmd, &vis, ASB_RW_OK);
    asb_rw_helper_surfaces(h);   /* SURFACES follow the new rectangles at once */
}

static void handle_close(AsbRwHelper *h, uint32_t request_id, const AsbRwClose *cmd)
{
    HWND hwnd = NULL;
    uint32_t kind = 0;
    if (!geometry_enabled(h)) {
        send_error(h, request_id, ASB_RW_ERR_UPGRADE_REQUIRED, ASB_RW_CLOSE_REQUEST);
        return;
    }
    uint32_t result = resolve_surface(h, cmd->id, &hwnd, &kind);
    if (!result && kind != ASB_RW_SURFACE_ROOT && kind != ASB_RW_SURFACE_OWNED) result = ASB_RW_ERR_NOT_ELIGIBLE;
    if (!result) {
        AsbRwWindowState st;
        UINT show = 0;
        memset(&st, 0, sizeof(st));
        if (!h->ops.window_state(h->ops.ctx, hwnd, FALSE, &st, &show)) result = ASB_RW_ERR_STALE_ID;
        else if (st.flags & ASB_RW_WS_DISABLED) result = ASB_RW_ERR_BUSY;   /* a modal owned window decides */
        else if (!h->ops.post_close(h->ops.ctx, hwnd)) result = ASB_RW_ERR_STALE_ID;
    }
    helper_log(h, "close id=%016llx -> %s", (unsigned long long)cmd->id, asb_rw_error_name(result));
    memset(&h->tx->close, 0, sizeof(h->tx->close));
    h->tx->close.id = cmd->id;
    h->tx->close.error = result;
    send_tx(h, ASB_RW_CLOSE_RESULT, request_id);
}

static void handle_hit_test(AsbRwHelper *h, uint32_t request_id, const AsbRwHitTest *cmd)
{
    HWND hwnd = NULL;
    uint32_t kind = 0;
    RECT win = { 0, 0, 0, 0 }, vis = { 0, 0, 0, 0 };
    if (!geometry_enabled(h)) {
        send_error(h, request_id, ASB_RW_ERR_UPGRADE_REQUIRED, ASB_RW_HIT_TEST_REQUEST);
        return;
    }
    AsbRwHitTest reply = *cmd;
    reply.code = 0;
    if (!resolve_surface(h, cmd->id, &hwnd, &kind) && h->ops.frames(h->ops.ctx, hwnd, &win, &vis))
        reply.code = h->ops.hit_test(h->ops.ctx, hwnd, vis.left + cmd->x, vis.top + cmd->y) & 0xFFFFu;
    h->tx->hit_test = reply;
    send_tx(h, ASB_RW_HIT_TEST_RESULT, request_id);
}

static AsbRwHelperWindow *state_slot(AsbRwHelper *h, uint64_t id, uint64_t hwnd)
{
    AsbRwHelperWindow *free_slot = NULL;
    for (uint32_t i = 0; i < ASB_RW_HELPER_MAX_STATES; ++i) {
        AsbRwHelperWindow *w = &h->windows[i];
        if (w->id == id && w->hwnd == hwnd) return w;
        if (!w->id && !free_slot) free_slot = w;
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->id = id;
        free_slot->hwnd = hwnd;
    }
    return free_slot;
}

static void send_icon(AsbRwHelper *h, AsbRwHelperWindow *w, HWND hwnd)
{
    uint64_t key = 0;
    AsbRwWindowIcon *icon = &h->tx->window_icon;
    memset(icon, 0, offsetof(AsbRwWindowIcon, pixels));
    icon->id = w->id;
    if (!h->ops.icon(h->ops.ctx, hwnd, &key, icon)) { icon->width = icon->height = 0; key = 0; }
    if (icon->width > ASB_RW_MAX_ICON || icon->height > ASB_RW_MAX_ICON || !icon->width != !icon->height)
        icon->width = icon->height = 0;
    icon->id = w->id;
    w->icon_key = key;
    w->icon_sent = 1;
    w->icon_tick = h->now_ms;
    send_tx(h, ASB_RW_WINDOW_ICON, 0);
}

/* One root or owned window: re-normalize application min/max (roots), refresh limits and
   send WINDOW_STATE / WINDOW_ICON when something changed. */
static void track_window(AsbRwHelper *h, AsbRwHelperWindow *w)
{
    HWND hwnd = (HWND)(ULONG_PTR)w->hwnd;
    AsbRwWindowState st;
    UINT show = 0;
    memset(&st, 0, sizeof(st));
    if (!h->ops.window_state(h->ops.ctx, hwnd, FALSE, &st, &show)) return;
    int limits = !w->limits_tick || h->now_ms - w->limits_tick >= ASB_RW_HELPER_LIMITS_MS || st.dpi != w->limits_dpi;
    if (limits && !(st.flags & ASB_RW_WS_HUNG)) {
        AsbRwWindowState lim;
        UINT ignored = 0;
        memset(&lim, 0, sizeof(lim));
        if (h->ops.window_state(h->ops.ctx, hwnd, TRUE, &lim, &ignored)) {
            w->min_w = lim.min_width; w->min_h = lim.min_height; w->max_w = lim.max_width; w->max_h = lim.max_height;
        }
        w->limits_tick = h->now_ms ? h->now_ms : 1;
        w->limits_dpi = st.dpi;
    }
    if (w->is_root && (show == SW_SHOWMINIMIZED || show == SW_SHOWMAXIMIZED) && !(st.flags & ASB_RW_WS_HUNG)) {
        /* The application minimized/maximized itself: the proxy follows, the source stays renderable. */
        w->request = show == SW_SHOWMINIMIZED ? ASB_RW_SHOW_MINIMIZED : ASB_RW_SHOW_MAXIMIZED;
        w->request_serial++;
        h->ops.normalize(h->ops.ctx, hwnd);
        helper_log(h, "id=%016llx application %s itself; source kept normal", (unsigned long long)w->id,
                   show == SW_SHOWMINIMIZED ? "minimized" : "maximized");
    }
    st.id = w->id;
    st.request = w->request;
    st.request_serial = w->request_serial;
    st.min_width = w->min_w; st.min_height = w->min_h; st.max_width = w->max_w; st.max_height = w->max_h;
    if (st.max_width && st.max_width < st.min_width) st.max_width = st.min_width;
    if (st.max_height && st.max_height < st.min_height) st.max_height = st.min_height;
    if (!w->sent || memcmp(&st, &w->state, sizeof(st))) {
        w->state = st;
        w->sent = 1;
        h->tx->window_state = st;
        send_tx(h, ASB_RW_WINDOW_STATE, 0);
    }
    if (w->is_root) {
        uint64_t key = 0;
        if (!w->icon_sent) send_icon(h, w, hwnd);
        else if (h->now_ms - w->icon_tick >= ASB_RW_HELPER_LIMITS_MS) {
            w->icon_tick = h->now_ms;
            if (!h->ops.icon(h->ops.ctx, hwnd, &key, NULL)) key = 0;
            if (key != w->icon_key) send_icon(h, w, hwnd);
        }
    }
}

static void track_windows(AsbRwHelper *h)
{
    if (!geometry_enabled(h) || !h->hello_done) return;
    for (uint32_t i = 0; i < ASB_RW_HELPER_MAX_STATES; ++i) h->windows[i].seen = 0;
    for (uint32_t r = 0; r < ASB_RW_SHARE_MAX; ++r) {
        AsbRwHelperRoot *root = &h->roots[r];
        if (!root->root_id) continue;
        for (uint32_t i = 0; i < root->last.count; ++i) {
            const AsbRwSurface *s = &root->last.surfaces[i];
            if (s->kind != ASB_RW_SURFACE_ROOT && s->kind != ASB_RW_SURFACE_OWNED) continue;
            AsbRwHelperWindow *w = state_slot(h, s->surface_id, root->hwnds[i]);
            if (!w) continue;
            w->seen = 1;
            w->is_root = s->kind == ASB_RW_SURFACE_ROOT;
            track_window(h, w);
        }
    }
    for (uint32_t i = 0; i < ASB_RW_HELPER_MAX_STATES; ++i)
        if (h->windows[i].id && !h->windows[i].seen) memset(&h->windows[i], 0, sizeof(h->windows[i]));
}

/* ---- activation (eh6.7) ------------------------------------------------------------------- */

static int activation_enabled(const AsbRwHelper *h)
{
    return geometry_enabled(h) && (h->negotiated.capabilities & ASB_RW_CAP_ACTIVATION) &&
           (h->capabilities & ASB_RW_CAP_ACTIVATION);
}

#define FOREGROUND_CHAIN (ASB_RW_MAX_OWNER_DEPTH + 1u)

/* Shared root (window handle) whose family contains chain[0] (its owner chain follows): the root
   itself, a window it owns directly or indirectly, or one of its current surfaces. */
static uint64_t family_root(const AsbRwHelper *h, const uint64_t *chain, uint32_t n)
{
    if (!n || !chain[0]) return 0;
    for (uint32_t k = 0; k < h->share->count; ++k) {
        const AsbRwShared *sh = &h->share->shared[k];
        if (!sh->wire_id || sh->j.state != ASB_RW_ENTRY_PARKED) continue;
        for (uint32_t i = 0; i < n; ++i)
            if (chain[i] == sh->j.identity.hwnd) return sh->j.identity.hwnd;
    }
    for (uint32_t r = 0; h->roots && r < ASB_RW_SHARE_MAX; ++r) {
        const AsbRwHelperRoot *root = &h->roots[r];
        if (!root->root_id) continue;
        for (uint32_t i = 0; i < root->last.count; ++i)
            if (root->hwnds[i] == chain[0]) return root->hwnd;
    }
    return 0;
}

static uint64_t root_wire_id(const AsbRwHelper *h, uint64_t root_hwnd)
{
    for (uint32_t k = 0; k < h->share->count; ++k)
        if (h->share->shared[k].j.identity.hwnd == root_hwnd) return h->share->shared[k].wire_id;
    return 0;
}

static void send_activate_ack(AsbRwHelper *h, uint32_t request_id, uint64_t id, uint32_t epoch, uint32_t foreground,
                              uint32_t error)
{
    memset(&h->tx->activate, 0, sizeof(h->tx->activate));
    h->tx->activate.id = id;
    h->tx->activate.input_epoch = epoch;
    h->tx->activate.foreground = foreground;
    h->tx->activate.error = error;
    send_tx(h, ASB_RW_ACTIVATE_ACK, request_id);
}

/* The window that takes the foreground for a host activation of `hwnd`: a root disabled by a
   modal owned window hands it to its topmost owned window. */
static HWND activation_target(AsbRwHelper *h, HWND hwnd, uint32_t kind, uint64_t *root_hwnd)
{
    *root_hwnd = (uint64_t)(ULONG_PTR)hwnd;
    for (uint32_t r = 0; r < ASB_RW_SHARE_MAX; ++r) {
        AsbRwHelperRoot *root = &h->roots[r];
        if (!root->root_id) continue;
        int member = root->hwnd == (uint64_t)(ULONG_PTR)hwnd;
        for (uint32_t i = 0; !member && i < root->last.count; ++i) member = root->hwnds[i] == (uint64_t)(ULONG_PTR)hwnd;
        if (!member) continue;
        *root_hwnd = root->hwnd;
        if (kind != ASB_RW_SURFACE_ROOT) return hwnd;
        AsbRwWindowState st;
        UINT show = 0;
        memset(&st, 0, sizeof(st));
        if (!h->ops.window_state(h->ops.ctx, hwnd, FALSE, &st, &show) || !(st.flags & ASB_RW_WS_DISABLED)) return hwnd;
        for (uint32_t i = root->last.count; i-- > 1;)
            if (root->last.surfaces[i].kind == ASB_RW_SURFACE_OWNED) return (HWND)(ULONG_PTR)root->hwnds[i];
        return hwnd;
    }
    return hwnd;
}

static void release_foreground(AsbRwHelper *h, uint32_t request_id, uint32_t epoch)
{
    uint64_t chain[FOREGROUND_CHAIN];
    uint32_t n = h->ops.foreground(h->ops.ctx, chain, FOREGROUND_CHAIN);
    h->active_id = h->active_root = 0;
    h->active_epoch = 0;
    if (family_root(h, chain, n)) {
        HWND target = NULL;
        AsbRwWindowFacts *f = &h->facts[0];
        if (h->plain_foreground && h->ops.facts(h->ops.ctx, (HWND)(ULONG_PTR)h->plain_foreground, f) && f->visible &&
            !f->minimized && !f->cloaked)
            target = (HWND)(ULONG_PTR)h->plain_foreground;
        if (!target || !h->ops.activate(h->ops.ctx, target)) {
            uint64_t desk = h->ops.desktop(h->ops.ctx);
            if (desk) h->ops.activate(h->ops.ctx, (HWND)(ULONG_PTR)desk);
        }
        n = h->ops.foreground(h->ops.ctx, chain, FOREGROUND_CHAIN);
    }
    int released = !family_root(h, chain, n);
    h->gain_reported = 0;
    helper_log(h, "activate release (epoch %u) -> %s", epoch, released ? "released" : "denied");
    send_activate_ack(h, request_id, 0, epoch, released ? 1u : 0u, released ? ASB_RW_OK : ASB_RW_ERR_FOREGROUND_DENIED);
}

static void handle_activate(AsbRwHelper *h, uint32_t request_id, const AsbRwActivate *a)
{
    HWND hwnd = NULL;
    uint32_t kind = 0;
    uint64_t chain[FOREGROUND_CHAIN];
    if (!activation_enabled(h)) {
        send_error(h, request_id, ASB_RW_ERR_UPGRADE_REQUIRED, ASB_RW_ACTIVATE_REQUEST);
        return;
    }
    if (!a->id) { release_foreground(h, request_id, a->input_epoch); return; }
    uint32_t result = resolve_surface(h, a->id, &hwnd, &kind);
    if (!result && kind != ASB_RW_SURFACE_ROOT && kind != ASB_RW_SURFACE_OWNED) result = ASB_RW_ERR_NOT_ELIGIBLE;
    uint64_t root_hwnd = 0;
    HWND target = result ? NULL : activation_target(h, hwnd, kind, &root_hwnd);
    if (!result && !h->ops.responsive(h->ops.ctx, target)) result = ASB_RW_ERR_BUSY;
    uint32_t foreground = 0;
    if (!result) {
        uint32_t n = h->ops.foreground(h->ops.ctx, chain, FOREGROUND_CHAIN);
        /* Already there: a root counts when anything of its family is foreground (an open menu,
           its own owned window); an owned window must be the foreground itself. */
        int whole = kind == ASB_RW_SURFACE_ROOT && target == hwnd;
        int there = whole ? family_root(h, chain, n) == root_hwnd : n && chain[0] == (uint64_t)(ULONG_PTR)target;
        if (!there) {
            h->ops.activate(h->ops.ctx, target);
            n = h->ops.foreground(h->ops.ctx, chain, FOREGROUND_CHAIN);
            there = n && (chain[0] == (uint64_t)(ULONG_PTR)target || (whole && family_root(h, chain, n) == root_hwnd));
        }
        foreground = there ? 1u : 0u;
        if (!there) result = ASB_RW_ERR_FOREGROUND_DENIED;
    }
    if (foreground) {
        h->active_id = a->id;
        h->active_root = root_hwnd;
        h->active_epoch = a->input_epoch;
        h->gain_reported = 0;
    } else if (h->active_id == a->id) {
        h->active_id = h->active_root = 0;
        h->active_epoch = 0;
    }
    helper_log(h, "activate id=%016llx epoch=%u -> %s", (unsigned long long)a->id, a->input_epoch,
               foreground ? "foreground" : asb_rw_error_name(result));
    send_activate_ack(h, request_id, a->id, a->input_epoch, foreground, result);
}

/* Per tick: tell the host when the activated family loses the foreground, or when a shared
   window takes it without a host request; remember the last ordinary foreground window. */
static void track_foreground(AsbRwHelper *h)
{
    uint64_t chain[FOREGROUND_CHAIN];
    if (!activation_enabled(h) || !h->hello_done) return;
    uint32_t n = h->ops.foreground(h->ops.ctx, chain, FOREGROUND_CHAIN);
    uint64_t root = family_root(h, chain, n);
    if (!root && n && chain[0]) h->plain_foreground = chain[0];
    if (h->active_id && root != h->active_root) {
        helper_log(h, "activate id=%016llx lost the foreground", (unsigned long long)h->active_id);
        send_activate_ack(h, 0, h->active_id, h->active_epoch, 0, ASB_RW_ERR_FOREGROUND_DENIED);
        h->active_id = h->active_root = 0;
        h->active_epoch = 0;
    }
    if (root && root != h->active_root) {
        if (h->gain_reported != chain[0]) {
            uint64_t id = root_wire_id(h, root);
            h->gain_reported = chain[0];
            if (id) send_activate_ack(h, 0, id, 0, 1, ASB_RW_OK);
        }
    } else if (!root) {
        h->gain_reported = 0;
    }
}

static int inventory_has(const AsbRwHelper *h, uint64_t id)
{
    for (uint32_t i = 0; i < h->inventory.count; ++i) if (h->inventory.entries[i].id == id) return 1;
    return 0;
}

/* ---- guest selector (eh6.8) ------------------------------------------------------------------ */

static int selector_enabled(const AsbRwHelper *h)
{
    return h->hello_done && (h->negotiated.capabilities & ASB_RW_CAP_GUEST_SELECTOR) &&
           (h->capabilities & ASB_RW_CAP_GUEST_SELECTOR);
}

static AsbRwHelperRequest *request_for(AsbRwHelper *h, uint64_t id, int create)
{
    AsbRwHelperRequest *free_slot = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < ASB_RW_HELPER_MAX_REQUESTS; ++i) {
        AsbRwHelperRequest *r = &h->requests[i];
        if (r->id == id) return r;
        if (!r->id && !free_slot) free_slot = r;
        if (!r->active && (!oldest || r->tick < oldest->tick)) oldest = r;
    }
    if (!create) return NULL;
    AsbRwHelperRequest *r = free_slot ? free_slot : oldest;
    if (r) { memset(r, 0, sizeof(*r)); r->id = id; }
    return r;
}

/* The host acted on `id` (its SELECT_REQUEST arrived) with this outcome. */
static void note_selection(AsbRwHelper *h, uint64_t id, uint32_t error)
{
    AsbRwHelperRequest *r = request_for(h, id, error != 0);
    if (!r) return;
    r->active = 0;
    r->error = error;
    r->tick = h->now_ms;
    if (!error) memset(r, 0, sizeof(*r));
}

uint32_t asb_rw_helper_guest_select(AsbRwHelper *h, uint64_t id, int select)
{
    if (!h->hello_done) return ASB_RW_ERR_INTERNAL;
    if (!inventory_has(h, id)) return ASB_RW_ERR_STALE_ID;
    AsbRwHelperRequest *r = request_for(h, id, 1);
    if (!r) return ASB_RW_ERR_LIMIT;
    if (r->active) return ASB_RW_ERR_BUSY;
    if (!selector_enabled(h)) {   /* shown on the row like a host refusal */
        r->error = ASB_RW_ERR_UPGRADE_REQUIRED;
        r->tick = h->now_ms;
        return r->error;
    }
    r->active = 1;
    r->select = select ? 1u : 0u;
    r->error = 0;
    r->tick = h->now_ms;
    r->request_id = 0x80000000u | (++h->next_request & 0x7FFFFFFFu);
    memset(&h->tx->select, 0, sizeof(h->tx->select));
    h->tx->select.id = id;
    h->tx->select.select = r->select;
    send_tx(h, ASB_RW_SELECT_REQUEST, r->request_id);
    helper_log(h, "guest selector: %s %016llx (request %08x)", select ? "select" : "unselect", (unsigned long long)id,
               r->request_id);
    return h->send_failed ? ASB_RW_ERR_INTERNAL : ASB_RW_OK;
}

static void handle_host_error(AsbRwHelper *h, uint32_t request_id, const AsbRwErrorMsg *e)
{
    for (uint32_t i = 0; i < ASB_RW_HELPER_MAX_REQUESTS; ++i) {
        AsbRwHelperRequest *r = &h->requests[i];
        if (!r->id || r->request_id != request_id) continue;
        r->active = 0;
        r->error = e->code ? e->code : ASB_RW_ERR_INTERNAL;
        r->tick = h->now_ms;
        helper_log(h, "guest selector: host refused %016llx (%s)", (unsigned long long)r->id, asb_rw_error_name(r->error));
        return;
    }
    helper_log(h, "host error %s (detail %u)", asb_rw_error_name(e->code), e->detail);
}

void asb_rw_helper_restore_all(AsbRwHelper *h)
{
    if (h->hello_done && h->tx) handle_restore_all(h, 0);
    else asb_rw_share_restore_all(h->share);
}

static void expire_requests(AsbRwHelper *h)
{
    for (uint32_t i = 0; i < ASB_RW_HELPER_MAX_REQUESTS; ++i) {
        AsbRwHelperRequest *r = &h->requests[i];
        if (!r->id) continue;
        if (r->active && h->now_ms - r->tick >= ASB_RW_HELPER_REQUEST_MS) {
            r->active = 0;
            r->error = ASB_RW_ERR_BUSY;
            r->tick = h->now_ms;
            helper_log(h, "guest selector: no answer for %016llx", (unsigned long long)r->id);
        } else if (!r->active && !inventory_has(h, r->id)) {
            memset(r, 0, sizeof(*r));   /* the window is gone */
        }
    }
}

static int is_shared_root(const AsbRwHelper *h, uint64_t id)
{
    for (uint32_t k = 0; k < h->share->count; ++k)
        if (h->share->shared[k].wire_id == id) return 1;
    return 0;
}

#define ICONS_PER_TICK 8u

/* Icons for selector rows: sent when an entry appears and when its icon changes (paced). */
static void track_inventory_icons(AsbRwHelper *h)
{
    if (!selector_enabled(h) || !geometry_enabled(h) || !h->icons) return;
    uint32_t sent = 0;
    for (uint32_t i = 0; i < ASB_RW_MAX_INVENTORY; ++i)
        if (h->icons[i].id && !inventory_has(h, h->icons[i].id)) memset(&h->icons[i], 0, sizeof(h->icons[i]));
    for (uint32_t e = 0; e < h->inventory.count && sent < ICONS_PER_TICK; ++e) {
        uint64_t id = h->inventory.entries[e].id;
        if (is_shared_root(h, id)) continue;   /* shared roots send theirs with WINDOW_STATE */
        AsbRwHelperIcon *slot = NULL, *free_slot = NULL;
        for (uint32_t i = 0; i < ASB_RW_MAX_INVENTORY && !slot; ++i) {
            if (h->icons[i].id == id) slot = &h->icons[i];
            else if (!h->icons[i].id && !free_slot) free_slot = &h->icons[i];
        }
        if (slot && h->now_ms - slot->tick < ASB_RW_HELPER_LIMITS_MS) continue;
        if (!slot) {
            if (!free_slot) break;
            slot = free_slot;
            memset(slot, 0, sizeof(*slot));
            slot->id = id;
            slot->key = ~0ull;
        }
        const AsbRwIdentity *who = asb_rw_ids_peek(&h->ids, id);
        slot->tick = h->now_ms ? h->now_ms : 1;
        if (!who) continue;
        HWND hwnd = (HWND)(ULONG_PTR)who->hwnd;
        uint64_t key = 0;
        if (!h->ops.icon(h->ops.ctx, hwnd, &key, NULL)) key = 0;
        if (key == slot->key) continue;
        AsbRwWindowIcon *icon = &h->tx->window_icon;
        memset(icon, 0, offsetof(AsbRwWindowIcon, pixels));
        if (!key || !h->ops.icon(h->ops.ctx, hwnd, &key, icon) || icon->width > ASB_RW_MAX_ICON ||
            icon->height > ASB_RW_MAX_ICON || !icon->width != !icon->height)
            icon->width = icon->height = 0;
        icon->id = id;
        slot->key = key;
        send_tx(h, ASB_RW_WINDOW_ICON, 0);
        ++sent;
    }
}

void asb_rw_helper_flyout(const AsbRwHelper *h, AsbRwFlyoutModel *m, uint64_t *hwnds)
{
    static uint32_t states[ASB_RW_MAX_INVENTORY], errors[ASB_RW_MAX_INVENTORY];
    uint32_t count = h->inventory.count;
    memset(m, 0, offsetof(AsbRwFlyoutModel, items));
    wcscpy_s(m->heading, ARRAYSIZE(m->heading), L"Show windows on the host");
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t id = h->inventory.entries[i].id;
        states[i] = ASB_RW_STATE_NONE;
        errors[i] = 0;
        for (uint32_t k = 0; k < h->share->count; ++k) {
            const AsbRwShared *sh = &h->share->shared[k];
            if (sh->wire_id != id) continue;
            states[i] = sh->j.state == ASB_RW_ENTRY_PARKED ? ASB_RW_STATE_SHARED :
                        sh->j.state == ASB_RW_ENTRY_RESTORING ? ASB_RW_STATE_RESTORING : ASB_RW_STATE_PENDING;
        }
        for (uint32_t r = 0; r < ASB_RW_HELPER_MAX_REQUESTS; ++r)
            if (h->requests[r].id == id && !h->requests[r].active) errors[i] = h->requests[r].error;
    }
    asb_rw_flyout_fill(m, h->inventory.entries, states, errors, count);
    for (uint32_t i = 0; i < m->count; ++i) {
        for (uint32_t r = 0; r < ASB_RW_HELPER_MAX_REQUESTS; ++r)
            if (h->requests[r].id == m->items[i].id && h->requests[r].active) m->items[i].pending = 1;
        if (hwnds) {
            const AsbRwIdentity *who = asb_rw_ids_peek(&h->ids, m->items[i].id);
            hwnds[i] = who ? who->hwnd : 0;
        }
    }
    uint32_t needed = ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | ASB_RW_CAP_GEOMETRY;
    if (!h->hello_done) {
        m->status = ASB_RW_FLYOUT_CONNECTING;
    } else if (!selector_enabled(h) || (h->negotiated.capabilities & needed) != needed) {
        m->status = ASB_RW_FLYOUT_UPGRADE;
        wcscpy_s(m->message, ARRAYSIZE(m->message),
                 (h->negotiated.capabilities & needed) != needed
                     ? L"This guest cannot share windows yet. Update the App Sandbox guest components."
                     : L"Update App Sandbox on the host to choose windows from the guest.");
    } else {
        m->status = ASB_RW_FLYOUT_READY;
        wcscpy_s(m->message, ARRAYSIZE(m->message), L"Turn a window on to show it on the host.");
    }
}

uint32_t asb_rw_helper_open(AsbRwHelper *h, const AsbRwHelperOps *ops, AsbRwShare *share,
                            uint32_t capabilities, uint32_t driver_version, uint32_t session_tag)
{
    memset(h, 0, sizeof(*h));
    h->ops = *ops;
    h->share = share;
    h->capabilities = capabilities;
    h->helper_version = ASB_RW_HELPER_VERSION;
    h->driver_version = driver_version;
    h->session_tag = session_tag;
    h->facts = (AsbRwWindowFacts *)calloc(ASB_RW_MAX_WINDOWS, sizeof(*h->facts));
    h->events = (AsbRwInventoryEvent *)calloc(ASB_RW_HELPER_MAX_EVENTS, sizeof(*h->events));
    h->rx = (AsbRwPayload *)calloc(1, sizeof(*h->rx));
    h->tx = (AsbRwPayload *)calloc(1, sizeof(*h->tx));
    h->out = (uint8_t *)malloc(ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD);
    h->roots = (AsbRwHelperRoot *)calloc(ASB_RW_SHARE_MAX, sizeof(*h->roots));
    h->targets = (AsbRwCaptureTarget *)calloc(ASB_RW_MAX_SURFACES, sizeof(*h->targets));
    h->claimed = (uint8_t *)calloc(ASB_RW_MAX_WINDOWS, 1);
    h->windows = (AsbRwHelperWindow *)calloc(ASB_RW_HELPER_MAX_STATES, sizeof(*h->windows));
    h->icons = (AsbRwHelperIcon *)calloc(ASB_RW_MAX_INVENTORY, sizeof(*h->icons));
    uint32_t supported = asb_rw_helper_capabilities(ops, 0);
    if (!(supported & ASB_RW_CAP_GEOMETRY))
        h->capabilities &= ~ASB_RW_CAP_GEOMETRY;   /* advertised only with every window op */
    if (!(supported & ASB_RW_CAP_ACTIVATION))
        h->capabilities &= ~ASB_RW_CAP_ACTIVATION;
    if (!(supported & ASB_RW_CAP_GEOMETRY))
        h->capabilities &= ~ASB_RW_CAP_GUEST_SELECTOR;   /* selector rows need the icon op */
    if (!session_tag || !h->facts || !h->events || !h->rx || !h->tx || !h->out || !h->roots || !h->targets || !h->claimed ||
        !h->windows || !h->icons) {
        asb_rw_helper_close(h);
        return ASB_RW_ERR_INTERNAL;
    }
    asb_rw_ids_init(&h->ids, session_tag);
    asb_rw_inventory_init(&h->inventory);
    asb_rw_hello_init(&h->tx->hello, ASB_RW_ROLE_GUEST, h->capabilities, h->helper_version, driver_version, session_tag);
    send_tx(h, ASB_RW_HELLO, 0);
    return h->send_failed ? ASB_RW_ERR_INTERNAL : ASB_RW_OK;
}

uint32_t asb_rw_helper_receive(AsbRwHelper *h, const uint8_t *bytes, size_t len)
{
    AsbRwHeader header;
    const uint8_t *bulk = NULL;
    size_t bulk_len = 0;
    memset(&header, 0, sizeof(header));
    uint32_t result = asb_rw_decode(ASB_CH_RW_CONTROL, bytes, len, &header, h->rx, &bulk, &bulk_len);
    if (result) {
        send_error(h, header.request_id, result, header.type);
        return result;
    }
    if (!h->hello_done) {
        AsbRwHello local;
        if (header.type != ASB_RW_HELLO) {
            send_error(h, header.request_id, ASB_RW_ERR_MALFORMED, header.type);
            return ASB_RW_ERR_MALFORMED;
        }
        asb_rw_hello_init(&local, ASB_RW_ROLE_GUEST, h->capabilities, h->helper_version, h->driver_version,
                          h->session_tag);
        h->negotiated = asb_rw_negotiate(&local, &h->rx->hello);
        helper_log(h, "hello: host helper=%08x caps=%08x -> %s version=%u caps=%08x missing=%08x",
                   h->rx->hello.helper_version, h->rx->hello.capabilities, asb_rw_error_name(h->negotiated.error),
                   h->negotiated.version, h->negotiated.capabilities, h->negotiated.missing_required);
        if (h->negotiated.error != ASB_RW_OK && h->negotiated.error != ASB_RW_ERR_UPGRADE_REQUIRED) {
            send_error(h, header.request_id, h->negotiated.error, 0);
            return h->negotiated.error;
        }
        /* UPGRADE_REQUIRED: both sides computed it; the host shows the upgrade requirement. */
        h->hello_done = 1;
        refresh_inventory(h, 0);
        return ASB_RW_OK;
    }
    switch (header.type) {
    case ASB_RW_HELLO:
        send_error(h, header.request_id, ASB_RW_ERR_MALFORMED, header.type);
        return ASB_RW_ERR_MALFORMED;
    case ASB_RW_INVENTORY_REQUEST:
        refresh_inventory(h, 1);
        send_snapshot(h, header.request_id);
        return ASB_RW_OK;
    case ASB_RW_SELECT_REQUEST:
        handle_select(h, header.request_id, &h->rx->select);
        return ASB_RW_OK;
    case ASB_RW_RESTORE_ALL:
        handle_restore_all(h, header.request_id);
        return ASB_RW_OK;
    case ASB_RW_GEOMETRY_COMMAND:
        handle_geometry(h, header.request_id, &h->rx->geometry);
        return ASB_RW_OK;
    case ASB_RW_CLOSE_REQUEST:
        handle_close(h, header.request_id, &h->rx->close);
        return ASB_RW_OK;
    case ASB_RW_HIT_TEST_REQUEST:
        handle_hit_test(h, header.request_id, &h->rx->hit_test);
        return ASB_RW_OK;
    case ASB_RW_ACTIVATE_REQUEST:
        handle_activate(h, header.request_id, &h->rx->activate);
        return ASB_RW_OK;
    case ASB_RW_ERROR:   /* eh6.8: the host refused a guest selector request */
        handle_host_error(h, header.request_id, &h->rx->error);
        return ASB_RW_OK;
    case ASB_RW_DRAG_PROBE: case ASB_RW_DRAG_ACK:   /* eh6.9: handed to the file bridge */
        if (!asb_rw_helper_files_enabled(h) || !h->ops.drag) {
            send_error(h, header.request_id, ASB_RW_ERR_UNKNOWN_TYPE, header.type);
            return ASB_RW_OK;
        }
        h->ops.drag(h->ops.ctx, header.type, &h->rx->drag);
        return ASB_RW_OK;
    default:   /* guest -> host messages are not requests */
        send_error(h, header.request_id, ASB_RW_ERR_UNKNOWN_TYPE, header.type);
        return ASB_RW_OK;
    }
}

/* ---- drag and drop (eh6.9) --------------------------------------------------------------- */

int asb_rw_helper_files_enabled(const AsbRwHelper *h)
{
    uint32_t bits = ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST;
    return h->hello_done && (h->negotiated.capabilities & bits) && (h->capabilities & bits);
}

void asb_rw_helper_drag_state(AsbRwHelper *h, const AsbRwDrag *state)
{
    if (!asb_rw_helper_files_enabled(h)) return;
    h->tx->drag = *state;
    send_tx(h, ASB_RW_DRAG_STATE, 0);
}

uint32_t asb_rw_helper_drag_surfaces(const AsbRwHelper *h, uint64_t *ids, AsbRwRect *rects, uint32_t max)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ASB_RW_SHARE_MAX && n < max; ++i) {
        const AsbRwHelperRoot *r = &h->roots[i];
        if (!r->root_id) continue;
        for (uint32_t k = 0; k < r->last.count && n < max; ++k) {
            ids[n] = r->last.surfaces[k].surface_id;
            rects[n] = r->last.surfaces[k].rect;
            ++n;
        }
    }
    return n;
}

void asb_rw_helper_tick(AsbRwHelper *h, int inventory)
{
    asb_rw_helper_tick_at(h, inventory, GetTickCount64());
}

void asb_rw_helper_tick_at(AsbRwHelper *h, int inventory, uint64_t now_ms)
{
    uint64_t gone[ASB_RW_SHARE_MAX], restored[ASB_RW_SHARE_MAX];
    h->now_ms = now_ms;
    uint32_t n_gone = 0, n_restored = 0;
    asb_rw_share_maintain(h->share, gone, ASB_RW_SHARE_MAX, &n_gone, restored, ASB_RW_SHARE_MAX, &n_restored);
    for (uint32_t i = 0; i < n_gone; ++i)
        if (gone[i]) send_result(h, 0, gone[i], ASB_RW_STATE_NONE, ASB_RW_ERR_STALE_ID);
    for (uint32_t i = 0; i < n_restored; ++i)
        if (restored[i]) send_result(h, 0, restored[i], ASB_RW_STATE_NONE, ASB_RW_OK);
    if (inventory && h->hello_done) refresh_inventory(h, 1);
    /* Window states first: an application minimize/maximize is undone before surfaces are
       collected, so SURFACES never report the transient size. */
    track_windows(h);
    asb_rw_helper_surfaces(h);
    track_foreground(h);
    expire_requests(h);
    track_inventory_icons(h);
}

void asb_rw_helper_close(AsbRwHelper *h)
{
    if (h->roots && h->tx) {
        /* Capture ends with the session (the engine's frame channel goes with it). */
        for (uint32_t r = 0; r < ASB_RW_SHARE_MAX; ++r)
            if (h->roots[r].root_id && h->ops.capture) h->ops.capture(h->ops.ctx, h->roots[r].root_id, NULL, 0);
    }
    if (h->share && h->facts) {
        /* Selections end with the session. Wire IDs die with it. */
        asb_rw_share_restore_all(h->share);
        for (uint32_t i = 0; i < h->share->count; ++i) h->share->shared[i].wire_id = 0;
    }
    free(h->facts);
    free(h->events);
    free(h->rx);
    free(h->tx);
    free(h->out);
    free(h->roots);
    free(h->targets);
    free(h->claimed);
    free(h->windows);
    free(h->icons);
    h->icons = NULL;
    h->facts = NULL; h->events = NULL; h->rx = NULL; h->tx = NULL; h->out = NULL;
    h->roots = NULL; h->targets = NULL; h->claimed = NULL; h->windows = NULL;
}
