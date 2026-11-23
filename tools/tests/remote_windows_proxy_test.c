/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_proxy_test
 *
 * @file      remote_windows_proxy_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows native host proxies, model suite (AppSandbox-eh6.6).
 *
 *   codec     HIT_TEST / WINDOW_STATE / WINDOW_ICON round trips and malformed inputs.
 *   session   the host session (remote_windows_session.c) against recorded proxy operations:
 *             negotiation, proxy identities and lifetimes, acknowledged geometry (coalescing,
 *             stale acks, application limits, size loops, busy/unanswered retries, host
 *             minimization, guest-initiated sizes), state feedback (styles, modal-disabled,
 *             application min/max replay), modal ownership and close (refusal, BUSY, cancellation),
 *             owned-window moves, the hit-test cache, protocol violations.
 *   helper    the guest helper's eh6.6 handlers (remote_windows_helper.c) against a window model:
 *             in-place root resize on the private display, host minimization leaving the source
 *             alone, busy/stale/ineligible IDs, owned moves, close (modal BUSY), hit tests in
 *             visible-frame coordinates, WINDOW_STATE/limits/icon change detection and
 *             application min/max normalization.
 *   drag      eh6.9 pointer arbitration for drag and drop: probe / hand-off / sink / timeout,
 *             no host button-up while the helper owns the pointer, input claim for host drags,
 *             takeover cancellation, automation refusal, capability gating, helper routing.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../transport/asb_remote_windows.h"
#include "../../src/backend_win/remote_windows_session.h"
#include "../agent/remote_windows_helper.h"
#include "../../src/core/protocol.h"

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static uint8_t g_buf[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD];
static AsbRwPayload g_msg, g_out;

static size_t encode(uint16_t type, const AsbRwPayload *m)
{
    size_t n = 0;
    return asb_rw_encode(type, 7, m, g_buf, sizeof(g_buf), &n) ? 0 : n;
}

static uint32_t decode(size_t n, AsbRwHeader *h)
{
    return asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, h, &g_out, NULL, NULL);
}

/* ======================================================================== codec */

static void test_codec(void)
{
    AsbRwHeader h;
    size_t n;
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hit_test.id = 0x1234; g_msg.hit_test.serial = 9; g_msg.hit_test.x = -3; g_msg.hit_test.y = 40;
    n = encode(ASB_RW_HIT_TEST_REQUEST, &g_msg);
    CHECK(n && !decode(n, &h) && h.type == ASB_RW_HIT_TEST_REQUEST && g_out.hit_test.x == -3 && g_out.hit_test.y == 40);
    g_msg.hit_test.code = 2;
    n = encode(ASB_RW_HIT_TEST_REQUEST, &g_msg);   /* a request carries no code */
    CHECK(n && decode(n, &h) == ASB_RW_ERR_MALFORMED);
    n = encode(ASB_RW_HIT_TEST_RESULT, &g_msg);
    CHECK(n && !decode(n, &h) && g_out.hit_test.code == 2 && g_out.hit_test.serial == 9);
    g_msg.hit_test.x = 9000;
    CHECK(!encode(ASB_RW_HIT_TEST_RESULT, &g_msg));
    CHECK_EQ(asb_rw_type_channel(ASB_RW_HIT_TEST_REQUEST), ASB_CH_RW_CONTROL);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_WINDOW_ICON), ASB_CH_RW_CONTROL);

    memset(&g_msg, 0, sizeof(g_msg));
    AsbRwWindowState *st = &g_msg.window_state;
    st->id = 5; st->flags = ASB_RW_WS_RESIZABLE | ASB_RW_WS_DISABLED; st->request = ASB_RW_SHOW_MAXIMIZED;
    st->request_serial = 3; st->dpi = 144; st->min_width = 400; st->min_height = 300; st->max_width = 0; st->max_height = 900;
    n = encode(ASB_RW_WINDOW_STATE, &g_msg);
    CHECK(n && !decode(n, &h) && !memcmp(&g_out.window_state, st, sizeof(*st)));
    st->flags = 0x40;
    CHECK(!encode(ASB_RW_WINDOW_STATE, &g_msg));
    st->flags = 0; st->request = 3;
    CHECK(!encode(ASB_RW_WINDOW_STATE, &g_msg));
    st->request = 0; st->max_height = 200;   /* max below min */
    CHECK(!encode(ASB_RW_WINDOW_STATE, &g_msg));
    st->max_height = 0; st->min_width = -1;
    CHECK(!encode(ASB_RW_WINDOW_STATE, &g_msg));

    memset(&g_msg, 0, sizeof(g_msg));
    AsbRwWindowIcon *ic = &g_msg.window_icon;
    ic->id = 6; ic->width = 2; ic->height = 3;
    for (int i = 0; i < 24; ++i) ic->pixels[i] = (uint8_t)(i * 7);
    n = encode(ASB_RW_WINDOW_ICON, &g_msg);
    CHECK_EQ(n, ASB_RW_HEADER_BYTES + 16 + 24);
    CHECK(n && !decode(n, &h) && g_out.window_icon.width == 2 && !memcmp(g_out.window_icon.pixels, ic->pixels, 24));
    /* Declared size and payload must agree: a short or long payload is malformed. */
    CHECK(decode(n - 1, &h) == ASB_RW_ERR_NEED_MORE);
    g_buf[12] = (uint8_t)(g_buf[12] + 1);   /* payload_len + 1 with the same bytes */
    CHECK(decode(n + 1, &h) != ASB_RW_OK);
    ic->width = 65; ic->height = 65;
    CHECK(!encode(ASB_RW_WINDOW_ICON, &g_msg));
    ic->width = 0; ic->height = 5;
    CHECK(!encode(ASB_RW_WINDOW_ICON, &g_msg));
    ic->width = 0; ic->height = 0;
    n = encode(ASB_RW_WINDOW_ICON, &g_msg);   /* "no icon" */
    CHECK(n && !decode(n, &h) && !g_out.window_icon.width);
    /* New types are control-channel only. */
    CHECK(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, n, &h, &g_out, NULL, NULL) == ASB_RW_ERR_WRONG_CHANNEL);
}

/* ======================================================================== session */

typedef struct Event {
    char what[16];
    uint64_t id, surface;
    int32_t a, b;
    uint32_t flags;
} Event;

static Event g_ev[256];
static int g_nev;
static uint8_t g_sent[64][4096];
static size_t g_sent_len[64];
static int g_nsent;
static int g_fail_create;

static void ev(const char *what, uint64_t id, uint64_t surface, int32_t a, int32_t b, uint32_t flags)
{
    if (g_nev >= 256) return;
    Event *e = &g_ev[g_nev++];
    strcpy_s(e->what, sizeof(e->what), what);
    e->id = id; e->surface = surface; e->a = a; e->b = b; e->flags = flags;
}

static int count_ev(const char *what)
{
    int n = 0;
    for (int i = 0; i < g_nev; ++i) n += !strcmp(g_ev[i].what, what);
    return n;
}

static const Event *last_ev(const char *what)
{
    for (int i = g_nev - 1; i >= 0; --i) if (!strcmp(g_ev[i].what, what)) return &g_ev[i];
    return NULL;
}

static void reset_log(void) { g_nev = 0; g_nsent = 0; }

static int s_send(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (g_nsent < 64 && len <= sizeof(g_sent[0])) { memcpy(g_sent[g_nsent], bytes, len); g_sent_len[g_nsent] = len; }
    g_nsent++;
    return 1;
}
static void s_log(void *ctx, const char *m) { (void)ctx; (void)m; }
static int s_create(void *ctx, uint64_t id, const AsbRwProxyInit *init)
{
    (void)ctx;
    ev("create", id, 0, init->guest_rect.right - init->guest_rect.left, init->guest_rect.bottom - init->guest_rect.top, init->title_len);
    return !g_fail_create;
}
static void s_destroy(void *ctx, uint64_t id) { (void)ctx; ev("destroy", id, 0, 0, 0, 0); }
static void s_title(void *ctx, uint64_t id, const uint16_t *t, uint32_t len) { (void)ctx; (void)t; ev("title", id, 0, (int32_t)len, 0, 0); }
static void s_icon(void *ctx, uint64_t id, const AsbRwWindowIcon *icon) { (void)ctx; ev("icon", id, 0, (int32_t)icon->width, (int32_t)icon->height, 0); }
static void s_state(void *ctx, uint64_t id, uint64_t surface, const AsbRwWindowState *st) { (void)ctx; ev("state", id, surface, st->min_width, st->min_height, st->flags); }
static void s_size(void *ctx, uint64_t id, int32_t w, int32_t h) { (void)ctx; ev("size", id, 0, w, h, 0); }
static void s_show(void *ctx, uint64_t id, uint32_t show) { (void)ctx; ev("show", id, 0, (int32_t)show, 0, 0); }
static AsbRwSurfaces g_last_surfaces;
static void s_surfaces(void *ctx, uint64_t id, const AsbRwSurfaces *s) { (void)ctx; g_last_surfaces = *s; ev("surfaces", id, 0, (int32_t)s->count, 0, 0); }
static void s_modal(void *ctx, uint64_t id) { (void)ctx; ev("modal", id, 0, 0, 0, 0); }
static void s_hit(void *ctx, uint64_t id, uint64_t surface) { (void)ctx; ev("hit", id, surface, 0, 0, 0); }
static void s_changed(void *ctx) { (void)ctx; }

/* Decodes sent message i (from the end when negative). */
static uint16_t sent_type(int i, AsbRwPayload *out)
{
    AsbRwHeader h;
    if (i < 0) i += g_nsent;
    if (i < 0 || i >= g_nsent || i >= 64) return 0;
    if (asb_rw_decode(ASB_CH_RW_CONTROL, g_sent[i], g_sent_len[i], &h, out, NULL, NULL)) return 0xFFFF;
    return h.type;
}

static int count_sent(uint16_t type)
{
    int n = 0;
    for (int i = 0; i < g_nsent && i < 64; ++i) n += sent_type(i, &g_out) == type;
    return n;
}

static uint32_t feed(AsbRwSession *s, uint16_t type, const AsbRwPayload *m)
{
    size_t n = 0;
    if (asb_rw_encode(type, 0, m, g_buf, sizeof(g_buf), &n)) return 0xFFFF;
    return asb_rw_session_receive(s, g_buf, n);
}

static AsbRwSession *new_session(uint32_t guest_caps, uint32_t tag)
{
    AsbRwSessionOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.send = s_send; ops.log = s_log; ops.proxy_create = s_create; ops.proxy_destroy = s_destroy; ops.proxy_title = s_title;
    ops.proxy_icon = s_icon; ops.proxy_state = s_state; ops.proxy_size = s_size; ops.proxy_show = s_show;
    ops.proxy_surfaces = s_surfaces; ops.proxy_activate_modal = s_modal; ops.hit_result = s_hit; ops.changed = s_changed;
    reset_log();
    AsbRwSession *s = asb_rw_session_create(&ops);
    AsbRwPayload *m = (AsbRwPayload *)calloc(1, sizeof(*m));
    asb_rw_hello_init(&m->hello, ASB_RW_ROLE_GUEST, guest_caps, ASB_RW_HELPER_VERSION, 1, tag);
    feed(s, ASB_RW_HELLO, m);
    free(m);
    return s;
}

static AsbRwIdTable g_ids;
static uint64_t mkid(uint32_t n)
{
    AsbRwIdentity id;
    uint64_t out = 0;
    memset(&id, 0, sizeof(id));
    id.hwnd = 0x1000 + n; id.pid = 77; id.thread_id = 5; id.process_start = 99;
    asb_rw_ids_assign(&g_ids, &id, &out);
    return out;
}

static void give_inventory(AsbRwSession *s, const uint64_t *ids, int n)
{
    AsbRwPayload *m = (AsbRwPayload *)calloc(1, sizeof(*m));
    m->snapshot.generation = 1;
    m->snapshot.count = (uint32_t)n;
    for (int i = 0; i < n; ++i) {
        AsbRwInventoryEntry *e = &m->snapshot.entries[i];
        e->id = ids[i]; e->pid = 77; e->dpi = 96;
        e->rect.left = 100; e->rect.top = 100; e->rect.right = 740; e->rect.bottom = 520;
        e->title_len = 5; memcpy(e->title, L"Title", 10);
        e->app_len = 3; memcpy(e->app, L"App", 6);
    }
    feed(s, ASB_RW_INVENTORY_SNAPSHOT, m);
    free(m);
}

static void result(AsbRwSession *s, uint64_t id, uint32_t state, uint32_t error)
{
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select_result.id = id; g_msg.select_result.state = state; g_msg.select_result.error = error;
    feed(s, ASB_RW_SELECT_RESULT, &g_msg);
}

static void surfaces(AsbRwSession *s, uint64_t root, int w, int h, uint64_t owned, AsbRwRect owned_rect)
{
    memset(&g_msg, 0, sizeof(g_msg));
    AsbRwSurfaces *x = &g_msg.surfaces;
    x->root_id = root;
    x->root_rect.left = 3000; x->root_rect.top = 100; x->root_rect.right = 3000 + w; x->root_rect.bottom = 100 + h;
    x->count = 1;
    x->surfaces[0].surface_id = root; x->surfaces[0].kind = ASB_RW_SURFACE_ROOT; x->surfaces[0].rect = x->root_rect;
    if (owned) {
        x->count = 2;
        x->surfaces[1].surface_id = owned; x->surfaces[1].kind = ASB_RW_SURFACE_OWNED; x->surfaces[1].z = 1;
        x->surfaces[1].rect = owned_rect;
    }
    feed(s, ASB_RW_SURFACES, &g_msg);
}

static void ack(AsbRwSession *s, uint64_t id, uint32_t serial, uint32_t show, int w, int h, uint32_t error)
{
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.geometry.id = id; g_msg.geometry.serial = serial; g_msg.geometry.show_state = show;
    g_msg.geometry.rect.left = 3000; g_msg.geometry.rect.top = 100; g_msg.geometry.rect.right = 3000 + w;
    g_msg.geometry.rect.bottom = 100 + h; g_msg.geometry.error = error;
    feed(s, ASB_RW_GEOMETRY_ACK, &g_msg);
}

static void wstate(AsbRwSession *s, uint64_t id, uint32_t flags, uint32_t request, uint32_t serial, int minw, int minh)
{
    memset(&g_msg, 0, sizeof(g_msg));
    AsbRwWindowState *st = &g_msg.window_state;
    st->id = id; st->flags = flags; st->request = request; st->request_serial = serial; st->dpi = 96;
    st->min_width = minw; st->min_height = minh;
    feed(s, ASB_RW_WINDOW_STATE, &g_msg);
}

/* Last GEOMETRY_COMMAND sent (0 serial when none). */
static AsbRwGeometry last_geometry(void)
{
    AsbRwGeometry g;
    memset(&g, 0, sizeof(g));
    for (int i = g_nsent - 1; i >= 0 && i < 64; --i)
        if (sent_type(i, &g_out) == ASB_RW_GEOMETRY_COMMAND) return g_out.geometry;
    return g;
}

#define FULL_CAPS (ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | ASB_RW_CAP_FRAMES_CPU | \
                   ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY)
#define TAG 0x00ABCDEFu

static AsbRwSession *shared_session(uint64_t *root)
{
    asb_rw_ids_init(&g_ids, TAG);
    AsbRwSession *s = new_session(FULL_CAPS, TAG);
    *root = mkid(1);
    give_inventory(s, root, 1);
    asb_rw_session_select(s, *root, 1);
    result(s, *root, ASB_RW_STATE_PENDING, 0);
    result(s, *root, ASB_RW_STATE_SHARED, 0);
    AsbRwRect none = { 0, 0, 0, 0 };
    surfaces(s, *root, 640, 420, 0, none);
    reset_log();
    return s;
}

static void test_negotiation(void)
{
    asb_rw_ids_init(&g_ids, TAG);
    AsbRwSession *s = new_session(FULL_CAPS & ~ASB_RW_CAP_GEOMETRY, TAG);   /* eh6.5 helper */
    CHECK_EQ(s->phase, ASB_RW_SESSION_LIMITED);
    uint64_t id = mkid(1);
    give_inventory(s, &id, 1);
    int before = g_nsent;
    CHECK_EQ(asb_rw_session_select(s, id, 1), ASB_RW_ERR_UPGRADE_REQUIRED);
    CHECK_EQ(g_nsent, before);   /* nothing asked of an older guest */
    CHECK_EQ(s->inventory.count, 1);   /* the inventory still works (selectors show the upgrade need) */
    asb_rw_session_destroy(s);

    s = new_session(FULL_CAPS, TAG);
    CHECK_EQ(s->phase, ASB_RW_SESSION_READY);
    CHECK(sent_type(0, &g_out) == ASB_RW_HELLO && (g_out.hello.capabilities & ASB_RW_CAP_GEOMETRY));
    CHECK_EQ(sent_type(-1, &g_out), ASB_RW_INVENTORY_REQUEST);
    /* A HELLO from the wrong role, or anything before HELLO, drops the connection. */
    asb_rw_session_destroy(s);
    AsbRwSessionOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.send = s_send;
    s = asb_rw_session_create(&ops);
    memset(&g_msg, 0, sizeof(g_msg));
    CHECK(feed(s, ASB_RW_INVENTORY_REQUEST, &g_msg) != ASB_RW_OK);
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_HOST, FULL_CAPS, 1, 0, 0);
    CHECK(feed(s, ASB_RW_HELLO, &g_msg) != ASB_RW_OK);
    asb_rw_session_destroy(s);
}

static void test_lifetimes(void)
{
    asb_rw_ids_init(&g_ids, TAG);
    AsbRwSession *s = new_session(FULL_CAPS, TAG);
    uint64_t a = mkid(1), b = mkid(2), ids[2] = { a, b };
    give_inventory(s, ids, 2);
    reset_log();
    CHECK_EQ(asb_rw_session_select(s, a, 1), ASB_RW_OK);
    CHECK(sent_type(-1, &g_out) == ASB_RW_SELECT_REQUEST && g_out.select.id == a && g_out.select.select == 1);
    CHECK(asb_rw_session_proxy(s, a) && asb_rw_session_proxy(s, a)->state == ASB_RW_STATE_PENDING);
    CHECK_EQ(asb_rw_session_select(s, a, 1), ASB_RW_OK);   /* pending: not asked twice */
    CHECK_EQ(count_sent(ASB_RW_SELECT_REQUEST), 1);
    result(s, a, ASB_RW_STATE_PENDING, 0);
    CHECK_EQ(count_ev("create"), 0);
    result(s, a, ASB_RW_STATE_SHARED, 0);
    CHECK_EQ(count_ev("create"), 1);
    CHECK(last_ev("create")->a == 640 && last_ev("create")->b == 420 && last_ev("create")->flags == 5);
    result(s, a, ASB_RW_STATE_SHARED, 0);                 /* duplicate: one proxy per root */
    CHECK_EQ(count_ev("create"), 1);
    /* IDs of another session (different tag) never touch proxies. */
    AsbRwIdTable other;
    asb_rw_ids_init(&other, TAG + 1);
    AsbRwIdentity fid;
    uint64_t foreign = 0;
    memset(&fid, 0, sizeof(fid));
    fid.hwnd = 0x1001; fid.pid = 77;
    asb_rw_ids_assign(&other, &fid, &foreign);
    result(s, foreign, ASB_RW_STATE_SHARED, 0);
    CHECK_EQ(count_ev("create"), 1);
    CHECK_EQ(asb_rw_session_select(s, foreign, 1), ASB_RW_ERR_STALE_ID);
    /* Second root: a separate proxy. Failure keeps an error entry without a proxy. */
    asb_rw_session_select(s, b, 1);
    result(s, b, ASB_RW_STATE_FAILED, ASB_RW_ERR_ELEVATED);
    CHECK_EQ(count_ev("create"), 1);
    CHECK(asb_rw_session_proxy(s, b) && asb_rw_session_proxy(s, b)->error == ASB_RW_ERR_ELEVATED);
    asb_rw_session_select(s, b, 1);
    result(s, b, ASB_RW_STATE_SHARED, 0);
    CHECK_EQ(count_ev("create"), 2);
    /* Host unselect destroys at once and only asks for restoration (never a close). */
    reset_log();
    CHECK_EQ(asb_rw_session_select(s, a, 0), ASB_RW_OK);
    CHECK(count_ev("destroy") == 1 && last_ev("destroy")->id == a);
    CHECK(sent_type(-1, &g_out) == ASB_RW_SELECT_REQUEST && g_out.select.select == 0);
    CHECK_EQ(count_sent(ASB_RW_CLOSE_REQUEST), 0);
    result(s, a, ASB_RW_STATE_RESTORING, 0);
    result(s, a, ASB_RW_STATE_NONE, 0);
    CHECK_EQ(count_ev("destroy"), 1);
    CHECK(asb_rw_session_proxy(s, a) == NULL);
    /* The guest window closes (NONE + STALE_ID): its proxy goes. */
    result(s, b, ASB_RW_STATE_NONE, ASB_RW_ERR_STALE_ID);
    CHECK(count_ev("destroy") == 2 && last_ev("destroy")->id == b);
    /* RESTORE_ALL and the end of the session destroy every proxy. */
    asb_rw_session_select(s, a, 1);
    result(s, a, ASB_RW_STATE_SHARED, 0);
    reset_log();
    CHECK_EQ(asb_rw_session_restore_all(s), ASB_RW_OK);
    CHECK_EQ(count_ev("destroy"), 1);
    CHECK_EQ(sent_type(-1, &g_out), ASB_RW_RESTORE_ALL);
    asb_rw_session_select(s, b, 1);
    result(s, b, ASB_RW_STATE_SHARED, 0);
    reset_log();
    asb_rw_session_destroy(s);
    CHECK_EQ(count_ev("destroy"), 1);
    /* A proxy that cannot be created undoes the selection. */
    s = new_session(FULL_CAPS, TAG);
    give_inventory(s, ids, 2);
    g_fail_create = 1;
    asb_rw_session_select(s, a, 1);
    result(s, a, ASB_RW_STATE_SHARED, 0);
    g_fail_create = 0;
    CHECK(sent_type(-1, &g_out) == ASB_RW_SELECT_REQUEST && g_out.select.select == 0);
    asb_rw_session_destroy(s);
}

static void test_geometry(void)
{
    uint64_t r;
    AsbRwSession *s = shared_session(&r);
    s->now_ms = 1000;
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 800, 600);
    AsbRwGeometry g1 = last_geometry();
    CHECK(g1.serial && g1.id == r && g1.rect.right - g1.rect.left == 800 && g1.rect.bottom - g1.rect.top == 600);
    CHECK(g1.rect.left == 3000 && g1.show_state == ASB_RW_SHOW_NORMAL);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 850, 650);   /* in flight: coalesced */
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 900, 700);
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), 1);
    ack(s, r, g1.serial + 100, 0, 1, 1, 0);                       /* unknown serial: ignored */
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), 1);
    ack(s, r, g1.serial, 0, 800, 600, 0);                          /* newest request goes out */
    AsbRwGeometry g2 = last_geometry();
    CHECK(count_sent(ASB_RW_GEOMETRY_COMMAND) == 2 && g2.serial > g1.serial && g2.rect.right - g2.rect.left == 900);
    ack(s, r, g1.serial, 0, 800, 600, 0);                          /* stale ack: no effect */
    CHECK_EQ(count_ev("size"), 0);
    ack(s, r, g2.serial, 0, 900, 700, 0);
    CHECK_EQ(count_ev("size"), 0);
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), 2);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 900, 700);   /* unchanged: nothing sent */
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), 2);
    /* Below the application's minimum: the proxy adopts the acknowledged size. */
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 100, 80);
    AsbRwGeometry g3 = last_geometry();
    ack(s, r, g3.serial, 0, 420, 320, 0);
    CHECK(count_ev("size") == 1 && last_ev("size")->a == 420 && last_ev("size")->b == 320);
    CHECK(s->proxies[0].want_w == 420 && s->proxies[0].want_h == 320);
    /* ... but not in the middle of a host size loop; it is applied when the loop ends. */
    asb_rw_session_sizing(s, r, 1);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 120, 90);
    AsbRwGeometry g4 = last_geometry();
    ack(s, r, g4.serial, 0, 420, 320, 0);
    CHECK_EQ(count_ev("size"), 1);
    asb_rw_session_sizing(s, r, 0);
    CHECK(count_ev("size") == 2 && last_ev("size")->a == 420);
    /* Busy guest: retried by the timer, not dropped. */
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 700, 500);
    AsbRwGeometry g5 = last_geometry();
    ack(s, r, g5.serial, 0, 420, 320, ASB_RW_ERR_BUSY);
    int sent = count_sent(ASB_RW_GEOMETRY_COMMAND);
    asb_rw_session_tick(s, s->now_ms + 10);
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), sent);
    asb_rw_session_tick(s, s->now_ms + ASB_RW_HOST_RETRY_MS + 10);
    AsbRwGeometry g6 = last_geometry();
    CHECK(count_sent(ASB_RW_GEOMETRY_COMMAND) == sent + 1 && g6.rect.right - g6.rect.left == 700);
    /* Unanswered: resent after a while. */
    asb_rw_session_tick(s, s->now_ms + 5 * ASB_RW_HOST_RETRY_MS);
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), sent + 2);
    AsbRwGeometry g7 = last_geometry();
    ack(s, r, g7.serial, 0, 700, 500, 0);
    /* Host minimize: reported as such, size unchanged, no proxy resize from the ack. */
    reset_log();
    asb_rw_session_resize(s, r, ASB_RW_SHOW_MINIMIZED, 0, 0);
    AsbRwGeometry gm = last_geometry();
    CHECK(gm.show_state == ASB_RW_SHOW_MINIMIZED && gm.rect.right - gm.rect.left == 700);
    ack(s, r, gm.serial, ASB_RW_SHOW_MINIMIZED, 640, 420, 0);
    CHECK_EQ(count_ev("size"), 0);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 700, 500);
    AsbRwGeometry gn = last_geometry();
    CHECK(gn.show_state == ASB_RW_SHOW_NORMAL && gn.serial > gm.serial);
    ack(s, r, gn.serial, 0, 700, 500, 0);
    /* Maximized: the host size is sent; the guest content is never adopted back. */
    asb_rw_session_resize(s, r, ASB_RW_SHOW_MAXIMIZED, 1920, 1040);
    AsbRwGeometry gx = last_geometry();
    CHECK(gx.show_state == ASB_RW_SHOW_MAXIMIZED && gx.rect.right - gx.rect.left == 1920);
    ack(s, r, gx.serial, ASB_RW_SHOW_MAXIMIZED, 1900, 1040, 0);
    CHECK_EQ(count_ev("size"), 0);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 700, 500);
    ack(s, r, last_geometry().serial, 0, 700, 500, 0);
    /* The application resized itself (SURFACES): the proxy follows when nothing is pending. */
    reset_log();
    AsbRwRect none = { 0, 0, 0, 0 };
    surfaces(s, r, 760, 520, 0, none);
    CHECK(count_ev("size") == 1 && last_ev("size")->a == 760 && last_ev("size")->b == 520);
    CHECK_EQ(count_sent(ASB_RW_GEOMETRY_COMMAND), 0);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 800, 520);
    surfaces(s, r, 760, 520, 0, none);                              /* stale while pending: ignored */
    CHECK_EQ(count_ev("size"), 1);
    asb_rw_session_destroy(s);
}

static void test_state_feedback(void)
{
    uint64_t r;
    AsbRwSession *s = shared_session(&r);
    wstate(s, r, ASB_RW_WS_RESIZABLE | ASB_RW_WS_MAXIMIZABLE, ASB_RW_SHOW_MAXIMIZED, 7, 400, 300);
    CHECK(count_ev("state") == 1 && last_ev("state")->a == 400 && last_ev("state")->surface == r);
    CHECK_EQ(count_ev("show"), 0);                                  /* the first state is a baseline */
    wstate(s, r, ASB_RW_WS_RESIZABLE | ASB_RW_WS_MAXIMIZABLE, ASB_RW_SHOW_MAXIMIZED, 8, 400, 300);
    CHECK(count_ev("show") == 1 && last_ev("show")->a == ASB_RW_SHOW_MAXIMIZED);
    asb_rw_session_resize(s, r, ASB_RW_SHOW_MAXIMIZED, 1920, 1040);
    wstate(s, r, ASB_RW_WS_RESIZABLE | ASB_RW_WS_MAXIMIZABLE, ASB_RW_SHOW_MAXIMIZED, 9, 400, 300);
    CHECK(count_ev("show") == 2 && last_ev("show")->a == ASB_RW_SHOW_NORMAL);   /* maximize toggles */
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 640, 420);
    wstate(s, r, ASB_RW_WS_RESIZABLE, ASB_RW_SHOW_MINIMIZED, 10, 400, 300);
    CHECK(count_ev("show") == 3 && last_ev("show")->a == ASB_RW_SHOW_MINIMIZED);
    wstate(s, r, ASB_RW_WS_RESIZABLE | ASB_RW_WS_DISABLED, ASB_RW_SHOW_MINIMIZED, 10, 400, 300);   /* same serial */
    CHECK_EQ(count_ev("show"), 3);
    CHECK(last_ev("state")->flags & ASB_RW_WS_DISABLED);
    /* Icons and titles reach the proxy. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.window_icon.id = r; g_msg.window_icon.width = 32; g_msg.window_icon.height = 32;
    feed(s, ASB_RW_WINDOW_ICON, &g_msg);
    CHECK(count_ev("icon") == 1 && last_ev("icon")->a == 32);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.event.generation = 2; g_msg.event.kind = ASB_RW_EVENT_CHANGED;
    g_msg.event.entry.id = r; g_msg.event.entry.pid = 77; g_msg.event.entry.title_len = 9; memcpy(g_msg.event.entry.title, L"New title", 18);
    feed(s, ASB_RW_INVENTORY_EVENT, &g_msg);
    CHECK(count_ev("title") >= 1 && last_ev("title")->a == 9);
    asb_rw_session_destroy(s);
}

static void test_modal_and_close(void)
{
    uint64_t r;
    AsbRwSession *s = shared_session(&r);
    uint64_t dlg = mkid(9);
    AsbRwRect dr = { 3060, 170, 3380, 350 };
    surfaces(s, r, 640, 420, dlg, dr);
    CHECK(count_ev("surfaces") == 1 && g_last_surfaces.count == 2 && g_last_surfaces.surfaces[1].kind == ASB_RW_SURFACE_OWNED);
    CHECK_EQ(asb_rw_session_root_of(s, dlg), r);
    wstate(s, dlg, ASB_RW_WS_SYSMENU, 0, 0, 0, 0);
    CHECK(last_ev("state")->surface == dlg && last_ev("state")->id == r);
    /* Root disabled by the modal dialog: host Close is not forwarded; the dialog comes forward. */
    wstate(s, r, ASB_RW_WS_RESIZABLE | ASB_RW_WS_DISABLED, 0, 0, 0, 0);
    reset_log();
    asb_rw_session_close(s, r, 0);
    CHECK_EQ(count_sent(ASB_RW_CLOSE_REQUEST), 0);
    CHECK(count_ev("modal") == 1 && last_ev("modal")->id == r);
    /* Closing the dialog itself is forwarded with its own ID. */
    asb_rw_session_close(s, r, dlg);
    CHECK(sent_type(-1, &g_out) == ASB_RW_CLOSE_REQUEST && g_out.close.id == dlg);
    /* A guest BUSY (raced with the dialog) also brings it forward. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.close.id = r; g_msg.close.error = ASB_RW_ERR_BUSY;
    feed(s, ASB_RW_CLOSE_RESULT, &g_msg);
    CHECK_EQ(count_ev("modal"), 2);
    /* Owned move from the host: offset from the root origin, kept while unacknowledged. */
    reset_log();
    asb_rw_session_move_owned(s, r, dlg, 100, 90, 320, 180);
    AsbRwGeometry g = last_geometry();
    CHECK(g.id == dlg && g.rect.left == 3100 && g.rect.top == 190 && g.rect.right == 3420);
    surfaces(s, r, 640, 420, dlg, dr);   /* older guest place arrives: overlaid */
    CHECK(g_last_surfaces.surfaces[1].rect.left == 3100 && g_last_surfaces.surfaces[1].rect.top == 190);
    ack(s, dlg, g.serial, 0, 320, 180, 0);
    AsbRwRect moved = { 3100, 190, 3420, 370 };
    surfaces(s, r, 640, 420, dlg, moved);
    CHECK(g_last_surfaces.surfaces[1].rect.left == 3100);
    asb_rw_session_move_owned(s, r, r, 5, 5, 0, 0);   /* the root is not an owned window */
    CHECK(last_geometry().id == dlg);
    /* Dialog closed, root enabled: Close is forwarded once; cancellation keeps the proxy. */
    AsbRwRect none = { 0, 0, 0, 0 };
    surfaces(s, r, 640, 420, 0, none);
    wstate(s, r, ASB_RW_WS_RESIZABLE, 0, 0, 0, 0);
    reset_log();
    asb_rw_session_close(s, r, 0);
    asb_rw_session_close(s, r, 0);
    CHECK_EQ(count_sent(ASB_RW_CLOSE_REQUEST), 1);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.close.id = r; g_msg.close.error = ASB_RW_OK;
    feed(s, ASB_RW_CLOSE_RESULT, &g_msg);
    surfaces(s, r, 640, 420, dlg, dr);                            /* "save changes?" prompt */
    surfaces(s, r, 640, 420, 0, none);                            /* the user cancelled */
    CHECK_EQ(count_ev("destroy"), 0);
    CHECK(asb_rw_session_proxy(s, r) && asb_rw_session_proxy(s, r)->created);
    asb_rw_session_close(s, r, 0);                                 /* asked again: forwarded again */
    CHECK_EQ(count_sent(ASB_RW_CLOSE_REQUEST), 2);
    result(s, r, ASB_RW_STATE_NONE, ASB_RW_ERR_STALE_ID);         /* this time it closed */
    CHECK_EQ(count_ev("destroy"), 1);
    asb_rw_session_destroy(s);
}

static void test_hit_cache(void)
{
    uint64_t r;
    AsbRwSession *s = shared_session(&r);
    s->now_ms = 5000;
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 100, 10), HTCLIENT);
    AsbRwPayload sent;
    CHECK(sent_type(-1, &sent) == ASB_RW_HIT_TEST_REQUEST && sent.hit_test.x == 100 && sent.hit_test.y == 10);
    asb_rw_session_hit_test(s, r, 0, 300, 12);                     /* queued, not sent */
    CHECK_EQ(count_sent(ASB_RW_HIT_TEST_REQUEST), 1);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hit_test = sent.hit_test;
    g_msg.hit_test.serial += 50;                                    /* stale serial: ignored */
    g_msg.hit_test.code = HTCLOSE;
    feed(s, ASB_RW_HIT_TEST_RESULT, &g_msg);
    CHECK_EQ(count_ev("hit"), 0);
    g_msg.hit_test.serial = sent.hit_test.serial;
    g_msg.hit_test.code = HTCAPTION;
    feed(s, ASB_RW_HIT_TEST_RESULT, &g_msg);
    CHECK(count_ev("hit") == 1 && last_ev("hit")->surface == r);
    CHECK(sent_type(-1, &sent) == ASB_RW_HIT_TEST_REQUEST && sent.hit_test.x == 300);   /* queued point follows */
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 101, 11), HTCAPTION);   /* cached (radius) */
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 106, 10), HTCAPTION);   /* near: fallback while querying */
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 160, 300), HTCLIENT);   /* unknown area */
    s->now_ms += ASB_RW_HOST_HIT_TTL_MS + 1;
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hit_test = sent.hit_test;
    g_msg.hit_test.code = HTMAXBUTTON;
    int n = count_sent(ASB_RW_HIT_TEST_REQUEST);
    feed(s, ASB_RW_HIT_TEST_RESULT, &g_msg);
    CHECK(count_sent(ASB_RW_HIT_TEST_REQUEST) == n + 1 && sent_type(-1, &sent) == ASB_RW_HIT_TEST_REQUEST &&
          sent.hit_test.x == 160);                                   /* only the newest queued point */
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 300, 12), HTMAXBUTTON);
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 100, 10), HTCLIENT);   /* the old entry expired */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hit_test = sent.hit_test;
    g_msg.hit_test.code = HTCLIENT;
    n = count_sent(ASB_RW_HIT_TEST_REQUEST);
    feed(s, ASB_RW_HIT_TEST_RESULT, &g_msg);
    CHECK(count_sent(ASB_RW_HIT_TEST_REQUEST) == n + 1 && sent_type(-1, &sent) == ASB_RW_HIT_TEST_REQUEST &&
          sent.hit_test.x == 100);                                   /* expired point asked again */
    /* A resize forgets the root's cached answers. */
    asb_rw_session_resize(s, r, ASB_RW_SHOW_NORMAL, 900, 600);
    ack(s, r, last_geometry().serial, 0, 900, 600, 0);
    memset(&g_msg, 0, sizeof(g_msg));
    CHECK(asb_rw_session_hit_test(s, r, 0, 300, 12) == HTCLIENT);
    /* Unanswered queries are retried; points far outside are HTNOWHERE without a query. */
    n = count_sent(ASB_RW_HIT_TEST_REQUEST);
    asb_rw_session_tick(s, s->now_ms + ASB_RW_HOST_RETRY_MS + 5);
    CHECK_EQ(count_sent(ASB_RW_HIT_TEST_REQUEST), n + 1);
    CHECK_EQ(asb_rw_session_hit_test(s, r, 0, 90000, 5), HTNOWHERE);
    asb_rw_session_destroy(s);
}

static void test_violations(void)
{
    uint64_t r;
    AsbRwSession *s = shared_session(&r);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.geometry.id = r; g_msg.geometry.serial = 1;
    CHECK(feed(s, ASB_RW_GEOMETRY_COMMAND, &g_msg) == ASB_RW_ERR_UNKNOWN_TYPE);   /* host-only type */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hit_test.id = r;
    CHECK(feed(s, ASB_RW_HIT_TEST_REQUEST, &g_msg) == ASB_RW_ERR_UNKNOWN_TYPE);
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_GUEST, FULL_CAPS, 1, 0, TAG);
    CHECK(feed(s, ASB_RW_HELLO, &g_msg) == ASB_RW_ERR_MALFORMED);
    uint8_t junk[ASB_RW_HEADER_BYTES] = { 0 };
    CHECK(asb_rw_session_receive(s, junk, sizeof(junk)) != ASB_RW_OK);
    /* State/icon/surfaces for unknown roots are ignored without side effects. */
    reset_log();
    uint64_t stranger = mkid(30);
    wstate(s, stranger, 0, 0, 0, 0, 0);
    AsbRwRect none = { 0, 0, 0, 0 };
    surfaces(s, stranger, 10, 10, 0, none);
    CHECK_EQ(g_nev, 0);
    asb_rw_session_destroy(s);
}

/* ======================================================================== helper */

typedef struct FakeWin {
    HWND hwnd;
    RECT window, visible;
    LONG min_w, min_h;              /* window minimum (WM_GETMINMAXINFO) */
    uint32_t flags;                 /* ASB_RW_WS_* */
    UINT show;
    BOOL responsive, alive;
    uint64_t icon_key;
    int closes, normalizes, set_rects, hit_x, hit_y, limit_queries, icon_renders;
} FakeWin;

static FakeWin g_wins[4];
static AsbRwPayload g_hrx;

static FakeWin *fw(HWND h)
{
    for (int i = 0; i < 4; ++i) if (g_wins[i].hwnd == h && g_wins[i].alive) return &g_wins[i];
    return NULL;
}
static BOOL h_send(void *ctx, const uint8_t *bytes, size_t len) { return s_send(ctx, bytes, len); }
static int h_probe(void *ctx, uint64_t hwnd, AsbRwIdentity *live)
{
    (void)ctx;
    FakeWin *w = fw((HWND)(ULONG_PTR)hwnd);
    if (!w) return 0;
    memset(live, 0, sizeof(*live));
    live->hwnd = hwnd; live->pid = 77; live->thread_id = 5; live->process_start = 99;
    return 1;
}
static BOOL h_frames(void *ctx, HWND hwnd, RECT *window, RECT *visible)
{
    (void)ctx;
    FakeWin *w = fw(hwnd);
    if (!w) return FALSE;
    *window = w->window;
    *visible = w->visible;
    return TRUE;
}
static BOOL h_set_rect(void *ctx, HWND hwnd, const RECT *r, BOOL size)
{
    (void)ctx;
    FakeWin *w = fw(hwnd);
    if (!w) return FALSE;
    LONG ml = w->visible.left - w->window.left, mt = w->visible.top - w->window.top;
    LONG mr = w->window.right - w->visible.right, mb = w->window.bottom - w->visible.bottom;
    LONG ww = size ? r->right - r->left : w->window.right - w->window.left;
    LONG wh = size ? r->bottom - r->top : w->window.bottom - w->window.top;
    if (ww < w->min_w) ww = w->min_w;   /* the application enforces its minimum */
    if (wh < w->min_h) wh = w->min_h;
    SetRect(&w->window, r->left, r->top, r->left + ww, r->top + wh);
    SetRect(&w->visible, w->window.left + ml, w->window.top + mt, w->window.right - mr, w->window.bottom - mb);
    w->set_rects++;
    return TRUE;
}
static BOOL h_responsive(void *ctx, HWND hwnd) { (void)ctx; FakeWin *w = fw(hwnd); return w && w->responsive; }
static BOOL h_close(void *ctx, HWND hwnd) { (void)ctx; FakeWin *w = fw(hwnd); if (!w) return FALSE; w->closes++; return TRUE; }
static uint32_t h_hit(void *ctx, HWND hwnd, int x, int y)
{
    (void)ctx;
    FakeWin *w = fw(hwnd);
    if (!w) return 0;
    w->hit_x = x; w->hit_y = y;
    return y - w->visible.top < 30 ? HTCAPTION : HTCLIENT;
}
static BOOL h_state(void *ctx, HWND hwnd, BOOL limits, AsbRwWindowState *st, UINT *show)
{
    (void)ctx;
    FakeWin *w = fw(hwnd);
    if (!w) return FALSE;
    memset(st, 0, sizeof(*st));
    st->flags = w->flags;
    st->dpi = 96;
    *show = w->show;
    if (limits) {
        w->limit_queries++;
        st->min_width = w->min_w - 14;
        st->min_height = w->min_h - 7;
    }
    return TRUE;
}
static BOOL h_normalize(void *ctx, HWND hwnd) { (void)ctx; FakeWin *w = fw(hwnd); if (!w) return FALSE; w->show = SW_SHOWNORMAL; w->normalizes++; return TRUE; }
static BOOL h_icon(void *ctx, HWND hwnd, uint64_t *key, AsbRwWindowIcon *icon)
{
    (void)ctx;
    FakeWin *w = fw(hwnd);
    if (!w) return FALSE;
    *key = w->icon_key;
    if (icon) { icon->width = icon->height = 2; memset(icon->pixels, 0x80, 16); w->icon_renders++; }
    return TRUE;
}
static void h_log(void *ctx, const char *m) { (void)ctx; (void)m; }
static uint32_t h_enumerate(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx; (void)out; (void)max;
    *count = 0;
    return ASB_RW_OK;
}
static BOOL h_journal(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; (void)n; return TRUE; }
static DWORD h_release(void *ctx, uint64_t lease, DWORD (*restore)(void *), void *arg)
{
    (void)ctx; (void)lease;
    return restore ? restore(arg) : 0;
}

static void make_win(int i, HWND h, int x, int y, int w, int hgt)
{
    FakeWin *f = &g_wins[i];
    memset(f, 0, sizeof(*f));
    f->hwnd = h;
    SetRect(&f->window, x, y, x + w, y + hgt);
    SetRect(&f->visible, x + 7, y, x + w - 7, y + hgt - 7);   /* invisible borders: 7 px sides/bottom */
    f->min_w = 300; f->min_h = 200;
    f->flags = ASB_RW_WS_RESIZABLE | ASB_RW_WS_SYSMENU;
    f->show = SW_SHOWNORMAL;
    f->responsive = TRUE; f->alive = TRUE;
    f->icon_key = 0x5000;
}

static uint32_t hfeed(AsbRwHelper *h, uint16_t type, const AsbRwPayload *m)
{
    size_t n = 0;
    if (asb_rw_encode(type, 3, m, g_buf, sizeof(g_buf), &n)) return 0xFFFF;
    return asb_rw_helper_receive(h, g_buf, n);
}

/* Last message of `type` the helper sent (decoded into g_hrx); 0 when none. */
static int hsent(uint16_t type)
{
    for (int i = (g_nsent < 64 ? g_nsent : 64) - 1; i >= 0; --i) {
        AsbRwHeader hd;
        if (!asb_rw_decode(ASB_CH_RW_CONTROL, g_sent[i], g_sent_len[i], &hd, &g_hrx, NULL, NULL) && hd.type == type) return 1;
    }
    return 0;
}

static void test_helper(void)
{
    static AsbRwShare share;
    static AsbRwHelper h;
    AsbRwHelperOps ops;
    memset(&share, 0, sizeof(share));
    share.ops.journal_write = h_journal;
    share.ops.display_release = h_release;
    memset(&ops, 0, sizeof(ops));
    ops.send = h_send; ops.probe = h_probe; ops.log = h_log; ops.enumerate = h_enumerate;
    ops.frames = h_frames; ops.set_rect = h_set_rect; ops.responsive = h_responsive; ops.post_close = h_close;
    ops.hit_test = h_hit; ops.window_state = h_state; ops.normalize = h_normalize; ops.icon = h_icon;
    CHECK_EQ(asb_rw_helper_capabilities(&ops, 0), ASB_RW_CAP_GEOMETRY);
    share.display_active = TRUE;
    SetRect(&share.private_rect, 2560, 0, 5120, 1440);
    make_win(0, (HWND)(ULONG_PTR)0x1001, 3000, 100, 654, 427);    /* root */
    make_win(1, (HWND)(ULONG_PTR)0x1009, 3060, 170, 334, 187);    /* owned dialog */
    make_win(2, (HWND)(ULONG_PTR)0x100A, 2900, 200, 200, 100);    /* menu */
    g_wins[1].flags = ASB_RW_WS_SYSMENU;
    reset_log();
    uint32_t caps = asb_rw_helper_capabilities(&ops, FULL_CAPS);
    CHECK_EQ(asb_rw_helper_open(&h, &ops, &share, caps, 1, TAG), ASB_RW_OK);
    CHECK(hsent(ASB_RW_HELLO) && (g_hrx.hello.capabilities & ASB_RW_CAP_GEOMETRY));
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_HOST, ASB_RW_HOST_CAPS, ASB_RW_HOST_VERSION, 0, 0);
    hfeed(&h, ASB_RW_HELLO, &g_msg);
    /* One shared root with an owned dialog and a menu (as asb_rw_helper_surfaces records them). */
    AsbRwIdentity idn;
    uint64_t ids[3];
    for (int i = 0; i < 3; ++i) {
        memset(&idn, 0, sizeof(idn));
        idn.hwnd = (uint64_t)(ULONG_PTR)g_wins[i].hwnd; idn.pid = 77; idn.thread_id = 5; idn.process_start = 99;
        asb_rw_ids_assign(&h.ids, &idn, &ids[i]);
    }
    AsbRwHelperRoot *root = &h.roots[0];
    root->root_id = ids[0];
    root->hwnd = (uint64_t)(ULONG_PTR)g_wins[0].hwnd;
    root->last.root_id = ids[0];
    root->last.count = 3;
    uint32_t kinds[3] = { ASB_RW_SURFACE_ROOT, ASB_RW_SURFACE_OWNED, ASB_RW_SURFACE_MENU };
    for (int i = 0; i < 3; ++i) {
        root->last.surfaces[i].surface_id = ids[i];
        root->last.surfaces[i].kind = kinds[i];
        root->last.surfaces[i].z = (uint32_t)i;
        root->hwnds[i] = (uint64_t)(ULONG_PTR)g_wins[i].hwnd;
    }

    /* Root resize: in place, visible frame size as asked, acked with the applied frame. */
    reset_log();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.geometry.id = ids[0]; g_msg.geometry.serial = 4; g_msg.geometry.show_state = ASB_RW_SHOW_NORMAL;
    g_msg.geometry.rect.left = 0; g_msg.geometry.rect.top = 0; g_msg.geometry.rect.right = 900; g_msg.geometry.rect.bottom = 600;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.serial == 4 && !g_hrx.geometry.error);
    CHECK(g_hrx.geometry.rect.right - g_hrx.geometry.rect.left == 900 && g_hrx.geometry.rect.bottom - g_hrx.geometry.rect.top == 600);
    CHECK(g_wins[0].visible.left == 3007 && g_wins[0].visible.top == 100);   /* kept its place */
    /* Too wide for the rest of the private display: moved left to stay on it. */
    g_msg.geometry.serial = 5; g_msg.geometry.rect.right = 2400;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.rect.left == 5120 - 2400 && g_hrx.geometry.rect.right == 5120);
    /* Below the application's minimum: the applied (larger) frame is acked. */
    g_msg.geometry.serial = 6; g_msg.geometry.rect.right = 50; g_msg.geometry.rect.bottom = 40;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.rect.right - g_hrx.geometry.rect.left == 300 - 14);
    /* Host minimization: the source is not touched. */
    int set_rects = g_wins[0].set_rects;
    g_msg.geometry.serial = 7; g_msg.geometry.show_state = ASB_RW_SHOW_MINIMIZED;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.show_state == ASB_RW_SHOW_MINIMIZED && !g_hrx.geometry.error);
    CHECK_EQ(g_wins[0].set_rects, set_rects);
    CHECK_EQ(g_wins[0].show, SW_SHOWNORMAL);
    /* Not responding: BUSY, unchanged. */
    g_wins[0].responsive = FALSE;
    g_msg.geometry.serial = 8; g_msg.geometry.show_state = ASB_RW_SHOW_NORMAL; g_msg.geometry.rect.right = 800;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.error == ASB_RW_ERR_BUSY);
    CHECK_EQ(g_wins[0].set_rects, set_rects);
    g_wins[0].responsive = TRUE;
    /* Owned dialog: moved to the requested place (not resizable: size kept). */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.geometry.id = ids[1]; g_msg.geometry.serial = 9;
    g_msg.geometry.rect.left = 3200; g_msg.geometry.rect.top = 300; g_msg.geometry.rect.right = 3900; g_msg.geometry.rect.bottom = 900;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.rect.left == 3200 && g_hrx.geometry.rect.top == 300);
    CHECK(g_hrx.geometry.rect.right - g_hrx.geometry.rect.left == 334 - 14);
    /* Menus are not geometry targets; unknown/stale IDs are refused. */
    g_msg.geometry.id = ids[2]; g_msg.geometry.serial = 10;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.error == ASB_RW_ERR_NOT_ELIGIBLE);
    g_wins[2].alive = FALSE;                                     /* the menu closed: its ID is stale */
    g_msg.geometry.serial = 11;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.error == ASB_RW_ERR_STALE_ID);
    g_msg.geometry.id = ids[0] + 0x10000; g_msg.geometry.serial = 12;
    hfeed(&h, ASB_RW_GEOMETRY_COMMAND, &g_msg);
    CHECK(hsent(ASB_RW_GEOMETRY_ACK) && g_hrx.geometry.error == ASB_RW_ERR_STALE_ID);
    root->last.count = 2;

    /* Close: forwarded as SC_CLOSE; refused (BUSY) while a modal dialog disables the root. */
    reset_log();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.close.id = ids[0];
    hfeed(&h, ASB_RW_CLOSE_REQUEST, &g_msg);
    CHECK(hsent(ASB_RW_CLOSE_RESULT) && !g_hrx.close.error && g_wins[0].closes == 1);
    g_wins[0].flags |= ASB_RW_WS_DISABLED;
    hfeed(&h, ASB_RW_CLOSE_REQUEST, &g_msg);
    CHECK(hsent(ASB_RW_CLOSE_RESULT) && g_hrx.close.error == ASB_RW_ERR_BUSY && g_wins[0].closes == 1);
    g_msg.close.id = ids[1];
    hfeed(&h, ASB_RW_CLOSE_REQUEST, &g_msg);
    CHECK(hsent(ASB_RW_CLOSE_RESULT) && !g_hrx.close.error && g_wins[1].closes == 1);
    g_wins[0].flags &= ~ASB_RW_WS_DISABLED;

    /* Hit test: visible-frame coordinates -> screen point; stale -> HTNOWHERE. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hit_test.id = ids[0]; g_msg.hit_test.serial = 3; g_msg.hit_test.x = 40; g_msg.hit_test.y = 12;
    hfeed(&h, ASB_RW_HIT_TEST_REQUEST, &g_msg);
    CHECK(hsent(ASB_RW_HIT_TEST_RESULT) && g_hrx.hit_test.code == HTCAPTION && g_hrx.hit_test.serial == 3);
    CHECK(g_wins[0].hit_x == g_wins[0].visible.left + 40 && g_wins[0].hit_y == g_wins[0].visible.top + 12);
    g_msg.hit_test.id = ids[2];
    hfeed(&h, ASB_RW_HIT_TEST_REQUEST, &g_msg);
    CHECK(hsent(ASB_RW_HIT_TEST_RESULT) && g_hrx.hit_test.code == 0);

    /* Window states: sent once per change; limits paced; icons on change; app min/max undone. */
    reset_log();
    asb_rw_helper_tick_at(&h, 0, 10000);
    CHECK(hsent(ASB_RW_WINDOW_STATE));
    int states = 0, icons = 0;
    for (int i = 0; i < g_nsent; ++i) {
        AsbRwHeader hd;
        if (asb_rw_decode(ASB_CH_RW_CONTROL, g_sent[i], g_sent_len[i], &hd, &g_hrx, NULL, NULL)) continue;
        states += hd.type == ASB_RW_WINDOW_STATE;
        icons += hd.type == ASB_RW_WINDOW_ICON;
    }
    CHECK_EQ(states, 2);   /* root + owned dialog */
    CHECK_EQ(icons, 1);    /* roots only */
    CHECK(hsent(ASB_RW_WINDOW_ICON) && g_hrx.window_icon.id == ids[0] && g_hrx.window_icon.width == 2);
    int queries = g_wins[0].limit_queries;
    reset_log();
    asb_rw_helper_tick_at(&h, 0, 10100);
    CHECK_EQ(g_nsent, 0);                                        /* nothing changed */
    CHECK_EQ(g_wins[0].limit_queries, queries);                  /* limits are not re-queried every tick */
    asb_rw_helper_tick_at(&h, 0, 10000 + ASB_RW_HELPER_LIMITS_MS + 1);
    CHECK(g_wins[0].limit_queries == queries + 1 && g_nsent == 0);
    g_wins[0].flags |= ASB_RW_WS_DISABLED;
    asb_rw_helper_tick_at(&h, 0, 12200);
    CHECK(hsent(ASB_RW_WINDOW_STATE) && (g_hrx.window_state.flags & ASB_RW_WS_DISABLED) && g_hrx.window_state.id == ids[0]);
    g_wins[0].flags &= ~ASB_RW_WS_DISABLED;
    reset_log();
    g_wins[0].show = SW_SHOWMAXIMIZED;                           /* the application maximized itself */
    asb_rw_helper_tick_at(&h, 0, 12300);
    CHECK(g_wins[0].normalizes == 1 && g_wins[0].show == SW_SHOWNORMAL);
    CHECK(hsent(ASB_RW_WINDOW_STATE) && g_hrx.window_state.request == ASB_RW_SHOW_MAXIMIZED);
    uint32_t serial = g_hrx.window_state.request_serial;
    g_wins[0].show = SW_SHOWMINIMIZED;
    asb_rw_helper_tick_at(&h, 0, 12400);
    CHECK(hsent(ASB_RW_WINDOW_STATE) && g_hrx.window_state.request == ASB_RW_SHOW_MINIMIZED &&
          g_hrx.window_state.request_serial == serial + 1);
    g_wins[1].show = SW_SHOWMAXIMIZED;                           /* owned windows are not normalized */
    asb_rw_helper_tick_at(&h, 0, 12500);
    CHECK_EQ(g_wins[1].normalizes, 0);
    reset_log();
    g_wins[0].icon_key = 0x6000;                                 /* new icon: sent after the pacing interval */
    asb_rw_helper_tick_at(&h, 0, 12600);
    CHECK(!hsent(ASB_RW_WINDOW_ICON));
    asb_rw_helper_tick_at(&h, 0, 10000 + 2 * ASB_RW_HELPER_LIMITS_MS + 5);
    CHECK(hsent(ASB_RW_WINDOW_ICON));
    /* Without the eh6.6 capability (older host) the new requests stay UPGRADE_REQUIRED. */
    asb_rw_helper_close(&h);
    reset_log();
    ops.icon = NULL;
    CHECK_EQ(asb_rw_helper_capabilities(&ops, 0), 0u);
    ops.icon = h_icon;
    CHECK_EQ(asb_rw_helper_open(&h, &ops, &share, caps, 1, TAG + 2), ASB_RW_OK);
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_HOST, ASB_RW_HOST_CAPS & ~ASB_RW_CAP_GEOMETRY, 1, 0, 0);
    hfeed(&h, ASB_RW_HELLO, &g_msg);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.close.id = ids[0];
    hfeed(&h, ASB_RW_CLOSE_REQUEST, &g_msg);
    CHECK(hsent(ASB_RW_ERROR) && g_hrx.error.code == ASB_RW_ERR_UPGRADE_REQUIRED);
    asb_rw_helper_close(&h);
}

/* ======================================================================== drag (eh6.9) */

static uint32_t g_epoch, g_claims, g_user_owns, g_captures;
static InputPacket g_pk[128];
static int g_npk;
static uint32_t i_claim(void *ctx, int renew) { (void)ctx; if (!g_user_owns) return 0; g_claims++; if (renew) g_epoch++; return g_epoch; }
static uint32_t i_send(void *ctx, uint32_t epoch, const InputPacket *p) { (void)ctx; if (epoch != g_epoch) return 5; if (g_npk < 128) g_pk[g_npk++] = *p; return 0; }
static void d_capture(void *ctx) { (void)ctx; g_captures++; }

static int button_packets(int from, uint32_t down)
{
    int n = 0;
    for (int i = from; i < g_npk; ++i) n += g_pk[i].type == INPUT_MOUSE_BUTTON && g_pk[i].param2 == down;
    return n;
}

#define DRAG_CAPS (FULL_CAPS | ASB_RW_CAP_ACTIVATION | ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST)
#define HOST_FILES (ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST)

static AsbRwSession *drag_session(uint32_t guest_caps, uint32_t host_files, uint64_t *root)
{
    AsbRwSessionOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.send = s_send; ops.log = s_log; ops.proxy_create = s_create; ops.proxy_destroy = s_destroy; ops.proxy_title = s_title;
    ops.proxy_icon = s_icon; ops.proxy_state = s_state; ops.proxy_size = s_size; ops.proxy_show = s_show;
    ops.proxy_surfaces = s_surfaces; ops.proxy_activate_modal = s_modal; ops.hit_result = s_hit; ops.changed = s_changed;
    ops.input_claim = i_claim; ops.input_send = i_send; ops.drag_capture = d_capture; ops.file_caps = host_files;
    g_epoch = 10; g_claims = 0; g_user_owns = 1; g_captures = 0; g_npk = 0;
    asb_rw_ids_init(&g_ids, TAG);
    reset_log();
    AsbRwSession *s = asb_rw_session_create(&ops);
    CHECK(sent_type(0, &g_out) == ASB_RW_HELLO && (g_out.hello.capabilities & HOST_FILES) == host_files);
    AsbRwPayload *m = (AsbRwPayload *)calloc(1, sizeof(*m));
    asb_rw_hello_init(&m->hello, ASB_RW_ROLE_GUEST, guest_caps, ASB_RW_HELPER_VERSION, 1, TAG);
    feed(s, ASB_RW_HELLO, m);
    free(m);
    *root = mkid(1);
    give_inventory(s, root, 1);
    asb_rw_session_select(s, *root, 1);
    result(s, *root, ASB_RW_STATE_PENDING, 0);
    result(s, *root, ASB_RW_STATE_SHARED, 0);
    AsbRwRect none = { 0, 0, 0, 0 };
    surfaces(s, *root, 640, 420, 0, none);
    AsbRwInputRouterState rs = { g_epoch, 0, 1, 0 };
    asb_rw_session_input_changed(s, &rs);
    reset_log();
    return s;
}

/* Host activation confirmed by the guest, then a press inside the proxy. */
static void activate_and_press(AsbRwSession *s, uint64_t root)
{
    if (s->input.state != ASB_RW_INPUT_ACTIVE || s->input.host_root != root) {
        reset_log();
        asb_rw_session_input_focus(s, root, 0);
        AsbRwPayload req;
        CHECK(sent_type(-1, &req) == ASB_RW_ACTIVATE_REQUEST);
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.activate.id = root; g_msg.activate.input_epoch = req.activate.input_epoch; g_msg.activate.foreground = 1;
        feed(s, ASB_RW_ACTIVATE_ACK, &g_msg);
    }
    CHECK_EQ(s->input.state, ASB_RW_INPUT_ACTIVE);
    asb_rw_session_input_mouse(s, root, 0, ASB_RW_MOUSE_DOWN, INPUT_BTN_LEFT, 20, 20, 0);
    CHECK(s->input.buttons & 1u);
}

static void drag_state(AsbRwSession *s, uint64_t session, uint32_t probe, uint32_t direction, uint32_t state, uint32_t error)
{
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.session_id = session; g_msg.drag.probe = probe; g_msg.drag.direction = direction;
    g_msg.drag.state = state; g_msg.drag.error = error;
    CHECK_EQ(feed(s, ASB_RW_DRAG_STATE, &g_msg), ASB_RW_OK);
}

static int last_drag_ack(uint32_t state, uint32_t error)
{
    for (int i = (g_nsent < 64 ? g_nsent : 64) - 1; i >= 0; --i)
        if (sent_type(i, &g_out) == ASB_RW_DRAG_ACK) return g_out.drag.state == state && g_out.drag.error == error;
    return 0;
}

static void test_drag(void)
{
    uint64_t root;
    /* Guest -> host: probe, hand-off, no button-up from the host, end. */
    AsbRwSession *s = drag_session(DRAG_CAPS, HOST_FILES, &root);
    activate_and_press(s, root);
    CHECK_EQ(asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_RIGHT), ASB_RW_ERR_STALE_ID);   /* right was not pressed */
    reset_log();
    CHECK_EQ(asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT), ASB_RW_OK);
    CHECK(sent_type(-1, &g_out) == ASB_RW_DRAG_PROBE && g_out.drag.surface_id == root && g_out.drag.button == INPUT_BTN_LEFT);
    uint32_t probe = g_out.drag.probe;
    CHECK(probe != 0 && asb_rw_session_drag_busy(s));
    CHECK_EQ(asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT), ASB_RW_ERR_BUSY);
    int before = g_npk;
    asb_rw_session_input_mouse(s, root, 0, ASB_RW_MOUSE_MOVE, 0, -40, -40, 0);   /* the helper owns the pointer */
    CHECK_EQ(asb_rw_session_input_key(s, 0x1B, 0x01, 0, 0), 1);                  /* Escape is not typed into the guest */
    CHECK_EQ(g_npk, before);
    drag_state(s, 0x77, probe + 5, ASB_RW_DIR_FROM_GUEST, ASB_RW_DRAG_ACTIVE, 0);  /* stale probe answer */
    CHECK(last_drag_ack(ASB_RW_DRAG_CANCEL, ASB_RW_ERR_STALE_ID) && s->drag.state == ASB_RW_HDRAG_PROBING);
    drag_state(s, 0x77, probe, ASB_RW_DIR_FROM_GUEST, ASB_RW_DRAG_ACTIVE, 0);
    CHECK_EQ(s->drag.state, ASB_RW_HDRAG_FROM_GUEST);
    CHECK_EQ(g_captures, 1);
    CHECK_EQ(s->input.buttons, 0);
    asb_rw_session_input_focus(s, 0, 0);   /* the broker's drag takes the host foreground */
    CHECK_EQ(button_packets(before, 0), 0);
    drag_state(s, 0x12345, 0, 0, ASB_RW_DRAG_ENDED, 0);   /* another session's end */
    CHECK(asb_rw_session_drag_busy(s));
    drag_state(s, 0x77, 0, 0, ASB_RW_DRAG_ENDED, 0);
    CHECK(!asb_rw_session_drag_busy(s));
    CHECK_EQ(s->drag.from_guest, 1);
    asb_rw_session_destroy(s);

    /* No transferable drag: forwarding resumes and the host button-up is injected normally. */
    s = drag_session(DRAG_CAPS, HOST_FILES, &root);
    activate_and_press(s, root);
    asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT);
    probe = s->drag.probe;
    drag_state(s, 0, probe, 0, ASB_RW_DRAG_NONE, 0);
    CHECK(!asb_rw_session_drag_busy(s));
    before = g_npk;
    asb_rw_session_input_mouse(s, root, 0, ASB_RW_MOUSE_UP, INPUT_BTN_LEFT, -40, -40, 0);
    CHECK_EQ(button_packets(before, 0), 1);
    asb_rw_session_destroy(s);

    /* Unsupported guest drag parked on the catcher until the host button goes up. */
    s = drag_session(DRAG_CAPS, HOST_FILES, &root);
    activate_and_press(s, root);
    asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT);
    probe = s->drag.probe;
    drag_state(s, 0, probe, 0, ASB_RW_DRAG_SINK, ASB_RW_ERR_UNSUPPORTED_DATA);
    CHECK_EQ(s->drag.state, ASB_RW_HDRAG_SINK);
    reset_log();
    before = g_npk;
    asb_rw_session_input_mouse(s, root, 0, ASB_RW_MOUSE_UP, INPUT_BTN_LEFT, -40, -40, 0);
    CHECK(last_drag_ack(ASB_RW_DRAG_RELEASE, 0) && g_out.drag.probe == probe);
    CHECK_EQ(button_packets(before, 0), 0);   /* the helper releases on its catcher */
    drag_state(s, 0, probe, 0, ASB_RW_DRAG_ENDED, 0);
    CHECK(!asb_rw_session_drag_busy(s));
    /* Unanswered probe: input resumes after the timeout. */
    activate_and_press(s, root);
    asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT);
    asb_rw_session_tick(s, s->now_ms + ASB_RW_DRAG_PROBE_MS + 1);
    CHECK(!asb_rw_session_drag_busy(s));
    asb_rw_session_destroy(s);

    /* Host -> guest: the input arbitration is claimed (fresh epoch) before the guest may drive the
       pointer; a viewer/automation takeover cancels; automation ownership refuses. */
    s = drag_session(DRAG_CAPS, HOST_FILES, &root);
    uint32_t claims = g_claims, epoch = g_epoch;
    drag_state(s, 0x99, 0, ASB_RW_DIR_TO_GUEST, ASB_RW_DRAG_ACTIVE, 0);
    CHECK_EQ(g_claims, claims + 1);
    CHECK(g_epoch == epoch + 1);
    CHECK(last_drag_ack(ASB_RW_DRAG_ALLOW, 0) && g_out.drag.session_id == 0x99);
    CHECK_EQ(s->drag.state, ASB_RW_HDRAG_TO_GUEST);
    drag_state(s, 0x9A, 0, ASB_RW_DIR_TO_GUEST, ASB_RW_DRAG_ACTIVE, 0);   /* a second one while busy */
    CHECK(last_drag_ack(ASB_RW_DRAG_CANCEL, ASB_RW_ERR_BUSY));
    reset_log();
    AsbRwInputRouterState rs = { g_epoch, 1, 1, 0 };
    asb_rw_session_input_changed(s, &rs);   /* our own claim: nothing happens */
    CHECK_EQ(count_sent(ASB_RW_DRAG_ACK), 0);
    rs.epoch = g_epoch + 1; rs.windows_current = 0;   /* the viewer took over */
    asb_rw_session_input_changed(s, &rs);
    CHECK(last_drag_ack(ASB_RW_DRAG_CANCEL, ASB_RW_ERR_BUSY) && g_out.drag.session_id == 0x99);
    asb_rw_session_input_changed(s, &rs);   /* answered once */
    CHECK_EQ(count_sent(ASB_RW_DRAG_ACK), 1);
    drag_state(s, 0x99, 0, 0, ASB_RW_DRAG_ENDED, ASB_RW_ERR_BUSY);
    CHECK(!asb_rw_session_drag_busy(s));
    g_user_owns = 0;
    rs.user_owns = 0;
    asb_rw_session_input_changed(s, &rs);
    drag_state(s, 0x9B, 0, ASB_RW_DIR_TO_GUEST, ASB_RW_DRAG_ACTIVE, 0);
    CHECK(last_drag_ack(ASB_RW_DRAG_CANCEL, ASB_RW_ERR_BUSY) && !asb_rw_session_drag_busy(s));
    asb_rw_session_destroy(s);

    /* The session ends mid-drag: no button-up is injected by the host. */
    s = drag_session(DRAG_CAPS, HOST_FILES, &root);
    activate_and_press(s, root);
    asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT);
    drag_state(s, 0x55, s->drag.probe, ASB_RW_DIR_FROM_GUEST, ASB_RW_DRAG_ACTIVE, 0);
    before = g_npk;
    asb_rw_session_destroy(s);
    CHECK_EQ(button_packets(before, 0), 0);

    /* Capability gating: a host without the broker, a guest without the bridge. */
    s = drag_session(DRAG_CAPS, 0, &root);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.state = ASB_RW_DRAG_ENDED;
    CHECK_EQ(feed(s, ASB_RW_DRAG_STATE, &g_msg), ASB_RW_ERR_UNKNOWN_TYPE);
    activate_and_press(s, root);
    CHECK_EQ(asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT), ASB_RW_ERR_UPGRADE_REQUIRED);
    asb_rw_session_destroy(s);
    s = drag_session(FULL_CAPS | ASB_RW_CAP_ACTIVATION, HOST_FILES, &root);
    activate_and_press(s, root);
    CHECK_EQ(asb_rw_session_drag_probe(s, root, 0, INPUT_BTN_LEFT), ASB_RW_ERR_UPGRADE_REQUIRED);
    CHECK(!asb_rw_session_files_enabled(s, ASB_RW_CAP_FILES_TO_GUEST));
    asb_rw_session_destroy(s);
}

/* The guest helper routes DRAG_PROBE / DRAG_ACK to the bridge only when both sides offer files. */
static int g_drag_calls;
static uint16_t g_drag_type;
static AsbRwDrag g_drag_msg;
static void h_drag(void *ctx, uint16_t type, const AsbRwDrag *msg) { (void)ctx; g_drag_calls++; g_drag_type = type; g_drag_msg = *msg; }

static void helper_drag_case(uint32_t host_caps, int expect_routed)
{
    static AsbRwShare share;
    static AsbRwHelper h;
    AsbRwHelperOps ops;
    memset(&share, 0, sizeof(share));
    share.ops.journal_write = h_journal;
    share.ops.display_release = h_release;
    memset(&ops, 0, sizeof(ops));
    ops.send = h_send; ops.probe = h_probe; ops.log = h_log; ops.enumerate = h_enumerate; ops.drag = h_drag;
    reset_log();
    g_drag_calls = 0;
    uint32_t caps = FULL_CAPS | ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST;
    CHECK_EQ(asb_rw_helper_open(&h, &ops, &share, caps, 1, TAG), ASB_RW_OK);
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_HOST, host_caps, ASB_RW_HOST_VERSION, 0, 0);
    hfeed(&h, ASB_RW_HELLO, &g_msg);
    CHECK_EQ(asb_rw_helper_files_enabled(&h), expect_routed);
    reset_log();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.surface_id = 0x44; g_msg.drag.probe = 3; g_msg.drag.button = INPUT_BTN_LEFT;
    CHECK_EQ(hfeed(&h, ASB_RW_DRAG_PROBE, &g_msg), ASB_RW_OK);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.session_id = 0x99; g_msg.drag.state = ASB_RW_DRAG_ALLOW;
    CHECK_EQ(hfeed(&h, ASB_RW_DRAG_ACK, &g_msg), ASB_RW_OK);
    if (expect_routed) {
        CHECK(g_drag_calls == 2 && g_drag_type == ASB_RW_DRAG_ACK && g_drag_msg.session_id == 0x99);
        AsbRwDrag d;
        memset(&d, 0, sizeof(d));
        d.state = ASB_RW_DRAG_NONE; d.probe = 3;
        reset_log();
        asb_rw_helper_drag_state(&h, &d);
        CHECK(hsent(ASB_RW_DRAG_STATE) && g_hrx.drag.state == ASB_RW_DRAG_NONE && g_hrx.drag.probe == 3);
    } else {
        CHECK_EQ(g_drag_calls, 0);
        CHECK(hsent(ASB_RW_ERROR) && g_hrx.error.code == ASB_RW_ERR_UNKNOWN_TYPE);
        AsbRwDrag d;
        memset(&d, 0, sizeof(d));
        d.state = ASB_RW_DRAG_ENDED;
        reset_log();
        asb_rw_helper_drag_state(&h, &d);   /* never sent to a host without file support */
        CHECK(!hsent(ASB_RW_DRAG_STATE));
    }
    /* Surface table for pointer mapping: the shared roots' last SURFACES. */
    h.roots[0].root_id = 0x10;
    h.roots[0].last.count = 2;
    h.roots[0].last.surfaces[0].surface_id = 0x10;
    h.roots[0].last.surfaces[0].rect.left = 3000;
    h.roots[0].last.surfaces[1].surface_id = 0x11;
    h.roots[0].last.surfaces[1].rect.left = 3100;
    uint64_t ids[8];
    AsbRwRect rects[8];
    CHECK_EQ(asb_rw_helper_drag_surfaces(&h, ids, rects, 8), 2);
    CHECK(ids[1] == 0x11 && rects[1].left == 3100);
    memset(&h.roots[0], 0, sizeof(h.roots[0]));
    asb_rw_helper_close(&h);
}

static void test_helper_drag(void)
{
    helper_drag_case(ASB_RW_HOST_CAPS | ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST, 1);
    helper_drag_case(ASB_RW_HOST_CAPS, 0);   /* host without a drag broker */
}

int main(void)
{
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "codec", test_codec }, { "negotiation", test_negotiation }, { "lifetimes", test_lifetimes },
        { "geometry", test_geometry }, { "state feedback", test_state_feedback }, { "modal and close", test_modal_and_close },
        { "hit cache", test_hit_cache }, { "violations", test_violations }, { "helper", test_helper },
        { "drag", test_drag }, { "helper drag", test_helper_drag },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int before = g_failures;
        tests[i].fn();
        printf("%-16s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
        fflush(stdout);
    }
    printf("%s: %d checks, %d failures\n", g_failures ? "RESULT FAIL" : "RESULT PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
