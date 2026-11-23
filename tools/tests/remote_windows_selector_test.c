/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_selector_test
 *
 * @file      remote_windows_selector_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows selectors (AppSandbox-eh6.8), model level: flyout placement on every taskbar
 * edge, tray click sequences, row/status texts, duplicate-title labels, the host session's
 * handling of the guest selector (performed through the host's own selection path, refusals
 * answered with ERROR), inventory icons, and the helper's guest selector requests (pending,
 * rejected, timed out, upgrade required) and selector model.
 */
#include "../../src/backend_win/remote_windows_session.h"
#include "../agent/remote_windows_helper.h"
#include "../agent/remote_windows_flyout.h"
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static uint8_t g_buf[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD];
static AsbRwPayload g_msg, g_rx;
static uint8_t g_sent[128][4096];
static size_t g_sent_len[128];
static int g_nsent;

static int capture(const uint8_t *bytes, size_t len)
{
    if (g_nsent < 128 && len <= sizeof(g_sent[0])) { memcpy(g_sent[g_nsent], bytes, len); g_sent_len[g_nsent] = len; }
    ++g_nsent;
    return 1;
}
static void reset(void) { g_nsent = 0; }
/* Last sent message of `type` decoded into g_rx (header into *hd); 0 when none. */
static int sent(uint16_t type, AsbRwHeader *hd)
{
    for (int i = (g_nsent < 128 ? g_nsent : 128) - 1; i >= 0; --i) {
        AsbRwHeader h;
        if (!asb_rw_decode(ASB_CH_RW_CONTROL, g_sent[i], g_sent_len[i], &h, &g_rx, NULL, NULL) && h.type == type) {
            if (hd) *hd = h;
            return 1;
        }
    }
    return 0;
}
static int count_sent(uint16_t type)
{
    int n = 0;
    for (int i = 0; i < (g_nsent < 128 ? g_nsent : 128); ++i) {
        AsbRwHeader h;
        AsbRwPayload p;
        if (!asb_rw_decode(ASB_CH_RW_CONTROL, g_sent[i], g_sent_len[i], &h, &p, NULL, NULL) && h.type == type) ++n;
    }
    return n;
}
static size_t encode(uint16_t type, uint32_t request_id, const AsbRwPayload *m)
{
    size_t n = 0;
    return asb_rw_encode(type, request_id, m, g_buf, sizeof(g_buf), &n) ? 0 : n;
}

/* ================================ placement / clicks / texts ============================ */

static void test_placement(void)
{
    RECT mon = { 0, 0, 1920, 1080 }, out;
    RECT bottom_work = { 0, 0, 1920, 1032 }, top_work = { 0, 48, 1920, 1080 };
    RECT left_work = { 60, 0, 1920, 1080 }, right_work = { 0, 0, 1860, 1080 };
    RECT icon_bottom = { 1700, 1040, 1724, 1064 }, icon_top = { 1700, 12, 1724, 36 };
    RECT icon_left = { 18, 900, 42, 924 }, icon_right = { 1878, 900, 1902, 924 };
    asb_rw_flyout_place(&icon_bottom, &bottom_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.bottom, 1032 - 12);                 /* above the bottom taskbar */
    CHECK_EQ((out.left + out.right) / 2, 1712);      /* centred on the icon */
    asb_rw_flyout_place(&icon_top, &top_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.top, 48 + 12);
    asb_rw_flyout_place(&icon_left, &left_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.left, 60 + 12);
    CHECK_EQ((out.top + out.bottom) / 2, 912);
    asb_rw_flyout_place(&icon_right, &right_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.right, 1860 - 12);
    /* An icon at the far right edge: clamped inside the work area. */
    RECT corner = { 1900, 1040, 1920, 1064 };
    asb_rw_flyout_place(&corner, &bottom_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.right, 1920 - 12);
    /* A left-hand secondary monitor with negative coordinates. */
    RECT mon2 = { -2560, 0, 0, 1440 }, work2 = { -2560, 0, 0, 1392 }, icon2 = { -40, 1400, -16, 1424 };
    asb_rw_flyout_place(&icon2, &work2, &mon2, 570, 450, 18, &out);
    CHECK(out.right <= -18 && out.left >= -2560 + 18 && out.bottom == 1392 - 18);
    /* Overflow area / cursor anchor inside the work area: above it, or below without room. */
    RECT inside = { 800, 600, 801, 601 };
    asb_rw_flyout_place(&inside, &bottom_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.bottom, 600 - 12);
    RECT near_top = { 800, 100, 801, 101 };
    asb_rw_flyout_place(&near_top, &bottom_work, &mon, 380, 300, 12, &out);
    CHECK_EQ(out.top, 101 + 12);
    /* Taller than the work area: pinned to its top, never off screen. */
    asb_rw_flyout_place(&icon_bottom, &bottom_work, &mon, 380, 5000, 12, &out);
    CHECK_EQ(out.top, 12);
}

static void test_clicks(void)
{
    AsbRwTrayClicks c;
    memset(&c, 0, sizeof(c));
    /* single click: deferred, then opens */
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONUP, 1000, 500), ASB_RW_TRAY_ARM);
    CHECK_EQ(asb_rw_tray_timer(&c, 1500), ASB_RW_TRAY_OPEN);
    CHECK_EQ(asb_rw_tray_timer(&c, 1600), ASB_RW_TRAY_NONE);
    /* double click: only the double action; the second button-up and the timer do nothing */
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONUP, 2000, 500), ASB_RW_TRAY_ARM);
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONDBLCLK, 2100, 500), ASB_RW_TRAY_DOUBLE);
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONUP, 2150, 500), ASB_RW_TRAY_NONE);
    CHECK_EQ(asb_rw_tray_timer(&c, 2500), ASB_RW_TRAY_NONE);
    /* right click: menu, cancels a pending single click */
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONUP, 3000, 500), ASB_RW_TRAY_ARM);
    CHECK_EQ(asb_rw_tray_event(&c, WM_RBUTTONUP, 3100, 500), ASB_RW_TRAY_MENU);
    CHECK_EQ(asb_rw_tray_timer(&c, 3500), ASB_RW_TRAY_NONE);
    /* clicking the icon while the flyout is open only closes it */
    asb_rw_tray_closed(&c, 4000);
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONUP, 4100, 500), ASB_RW_TRAY_NONE);
    CHECK_EQ(asb_rw_tray_event(&c, WM_LBUTTONUP, 5000, 500), ASB_RW_TRAY_ARM);
    /* other notifications are ignored */
    CHECK_EQ(asb_rw_tray_event(&c, WM_MOUSEMOVE, 5100, 500), ASB_RW_TRAY_NONE);
}

static void test_texts(void)
{
    wchar_t t[400];
    static AsbRwFlyoutModel m;
    asb_rw_flyout_row_status(ASB_RW_STATE_NONE, 0, 0, t, 400);
    CHECK(!t[0]);
    asb_rw_flyout_row_status(ASB_RW_STATE_NONE, 0, 1, t, 400);
    CHECK(!wcscmp(t, L"Requesting…"));
    asb_rw_flyout_row_status(ASB_RW_STATE_PENDING, 0, 0, t, 400);
    CHECK(!wcscmp(t, L"Sharing…"));
    asb_rw_flyout_row_status(ASB_RW_STATE_SHARED, 0, 0, t, 400);
    CHECK(!wcscmp(t, L"Shared"));
    asb_rw_flyout_row_status(ASB_RW_STATE_RESTORING, 0, 0, t, 400);
    CHECK(!wcscmp(t, L"Restoring…"));
    asb_rw_flyout_row_status(ASB_RW_STATE_FAILED, ASB_RW_ERR_ELEVATED, 0, t, 400);
    CHECK(!wcsncmp(t, L"Failed: ", 8) && wcslen(t) > 10);
    asb_rw_flyout_row_status(ASB_RW_STATE_NONE, ASB_RW_ERR_STALE_ID, 0, t, 400);
    CHECK(!t[0]);   /* the window went away: nothing to report */
    CHECK(asb_rw_flyout_row_on(ASB_RW_STATE_SHARED) && asb_rw_flyout_row_on(ASB_RW_STATE_PENDING));
    CHECK(!asb_rw_flyout_row_on(ASB_RW_STATE_NONE) && !asb_rw_flyout_row_on(ASB_RW_STATE_RESTORING));
    memset(&m, 0, sizeof(m));
    m.status = ASB_RW_FLYOUT_READY;
    asb_rw_flyout_status_text(&m, t, 400);
    CHECK(!wcscmp(t, L"No application windows are open."));
    m.count = 1;
    wcscpy_s(m.message, 320, L"hint");
    asb_rw_flyout_status_text(&m, t, 400);
    CHECK(!wcscmp(t, L"hint"));
    uint32_t statuses[] = { ASB_RW_FLYOUT_CONNECTING, ASB_RW_FLYOUT_DISCONNECTED, ASB_RW_FLYOUT_UPGRADE,
                            ASB_RW_FLYOUT_NO_GUEST, ASB_RW_FLYOUT_MULTIPLE, ASB_RW_FLYOUT_ERROR };
    m.message[0] = 0;
    for (int i = 0; i < 6; ++i) {
        m.status = statuses[i];
        asb_rw_flyout_status_text(&m, t, 400);
        CHECK(t[0] != 0);
        CHECK(!asb_rw_flyout_interactive(statuses[i]));
    }
    CHECK(asb_rw_flyout_interactive(ASB_RW_FLYOUT_READY));
    m.status = ASB_RW_FLYOUT_UPGRADE;
    wcscpy_s(m.message, 320, L"custom upgrade");
    asb_rw_flyout_status_text(&m, t, 400);
    CHECK(!wcscmp(t, L"custom upgrade"));
}

static void put(AsbRwInventoryEntry *e, uint64_t id, const wchar_t *title, const wchar_t *app, uint32_t flags)
{
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->flags = flags;
    e->title_len = (uint16_t)wcslen(title);
    for (uint16_t i = 0; i < e->title_len; ++i) e->title[i] = title[i];
    e->app_len = (uint16_t)wcslen(app);
    for (uint16_t i = 0; i < e->app_len; ++i) e->app[i] = app[i];
}

static void test_fill(void)
{
    static AsbRwFlyoutModel m;
    AsbRwInventoryEntry e[4];
    put(&e[0], 1, L"Untitled", L"Notepad", 0);
    put(&e[1], 2, L"Untitled", L"Paint", ASB_RW_ENTRY_SELECTED);
    put(&e[2], 3, L"Report", L"Word", ASB_RW_ENTRY_MINIMIZED);
    put(&e[3], 4, L"Untitled", L"Notepad", 0);
    asb_rw_mark_duplicates(e, 4);
    asb_rw_flyout_fill(&m, e, NULL, NULL, 4);
    CHECK_EQ(m.count, 4);
    CHECK(wcscmp(m.items[0].label, m.items[1].label) && wcscmp(m.items[0].label, m.items[3].label));
    CHECK(wcsstr(m.items[1].label, L"Paint") != NULL);   /* duplicates distinguished by application */
    CHECK(!wcscmp(m.items[2].label, L"Report"));
    CHECK_EQ(m.items[1].state, ASB_RW_STATE_SHARED);
    CHECK(m.items[2].minimized);
    uint32_t states[4] = { ASB_RW_STATE_PENDING, ASB_RW_STATE_NONE, ASB_RW_STATE_FAILED, ASB_RW_STATE_NONE };
    uint32_t errors[4] = { 0, 0, ASB_RW_ERR_ELEVATED, 0 };
    asb_rw_flyout_fill(&m, e, states, errors, 4);
    CHECK_EQ(m.items[0].state, ASB_RW_STATE_PENDING);
    CHECK_EQ(m.items[2].error, ASB_RW_ERR_ELEVATED);
}

/* ================================ host session ============================================ */

static int g_creates, g_icons;
static uint64_t g_icon_id;
static int s_send(void *ctx, const uint8_t *b, size_t n) { (void)ctx; return capture(b, n); }
static void s_log(void *ctx, const char *m) { (void)ctx; (void)m; }
static int s_create(void *ctx, uint64_t id, const AsbRwProxyInit *init) { (void)ctx; (void)id; (void)init; ++g_creates; return 1; }
static void s_destroy(void *ctx, uint64_t id) { (void)ctx; (void)id; }
static void s_inventory_icon(void *ctx, const AsbRwWindowIcon *icon) { (void)ctx; ++g_icons; g_icon_id = icon->id; }

#define GUEST_FULL (ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | ASB_RW_CAP_FRAMES_CPU | \
                    ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY | ASB_RW_CAP_ACTIVATION)
#define TAG 0x00C0FFEEu

static uint32_t sfeed(AsbRwSession *s, uint16_t type, uint32_t request_id, const AsbRwPayload *m)
{
    size_t n = encode(type, request_id, m);
    return asb_rw_session_receive(s, g_buf, n);
}

static AsbRwSession *session(uint32_t guest_caps, uint64_t *ids, int n)
{
    AsbRwSessionOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.send = s_send; ops.log = s_log; ops.proxy_create = s_create; ops.proxy_destroy = s_destroy;
    ops.inventory_icon = s_inventory_icon;
    AsbRwSession *s = asb_rw_session_create(&ops);
    memset(&g_msg, 0, sizeof(g_msg));
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_GUEST, guest_caps, ASB_RW_HELPER_VERSION, 1, TAG);
    sfeed(s, ASB_RW_HELLO, 0, &g_msg);
    static AsbRwIdTable ids_table;
    asb_rw_ids_init(&ids_table, TAG);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.snapshot.generation = 1;
    g_msg.snapshot.count = (uint32_t)n;
    for (int i = 0; i < n; ++i) {
        AsbRwIdentity who;
        memset(&who, 0, sizeof(who));
        who.hwnd = 0x2000u + (uint64_t)i; who.pid = 10; who.thread_id = 11;
        asb_rw_ids_assign(&ids_table, &who, &ids[i]);
        g_msg.snapshot.entries[i].id = ids[i];
        g_msg.snapshot.entries[i].pid = 10;
        g_msg.snapshot.entries[i].title_len = 1;
        g_msg.snapshot.entries[i].title[0] = (uint16_t)('A' + i);
        g_msg.snapshot.entries[i].rect.right = 400;
        g_msg.snapshot.entries[i].rect.bottom = 300;
    }
    sfeed(s, ASB_RW_INVENTORY_SNAPSHOT, 0, &g_msg);
    return s;
}

static void test_session(void)
{
    uint64_t ids[2];
    AsbRwHeader hd;
    reset();
    AsbRwSession *s = session(GUEST_FULL | ASB_RW_CAP_GUEST_SELECTOR, ids, 2);
    CHECK(s->negotiated.capabilities & ASB_RW_CAP_GUEST_SELECTOR);
    /* The guest selector asks: the host performs it through its own path. */
    reset();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = ids[0];
    g_msg.select.select = 1;
    CHECK_EQ(sfeed(s, ASB_RW_SELECT_REQUEST, 0x80000007u, &g_msg), ASB_RW_OK);
    CHECK(sent(ASB_RW_SELECT_REQUEST, &hd) && g_rx.select.id == ids[0] && g_rx.select.select == 1);
    CHECK(!sent(ASB_RW_ERROR, NULL));
    AsbRwHostProxy *p = asb_rw_session_proxy(s, ids[0]);
    CHECK(p && p->state == ASB_RW_STATE_PENDING);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select_result.id = ids[0];
    g_msg.select_result.state = ASB_RW_STATE_SHARED;
    sfeed(s, ASB_RW_SELECT_RESULT, 0, &g_msg);
    CHECK(g_creates == 1 && p->created);   /* the host shows the proxy, exactly as for its own toggle */
    /* Guest unselect: the proxy goes, the guest is asked to restore. */
    reset();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = ids[0];
    sfeed(s, ASB_RW_SELECT_REQUEST, 0x80000008u, &g_msg);
    CHECK(sent(ASB_RW_SELECT_REQUEST, NULL) && g_rx.select.select == 0);
    /* Refused: stale ID -> ERROR carrying the guest's request ID. */
    reset();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = (uint64_t)0x12345678u << 32 | 5;
    g_msg.select.select = 1;
    sfeed(s, ASB_RW_SELECT_REQUEST, 0x80000009u, &g_msg);
    CHECK(sent(ASB_RW_ERROR, &hd) && hd.request_id == 0x80000009u && g_rx.error.code == ASB_RW_ERR_STALE_ID &&
          g_rx.error.detail == ASB_RW_SELECT_REQUEST);
    CHECK(!sent(ASB_RW_SELECT_REQUEST, NULL));
    /* Inventory icons: only for listed, current IDs. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.window_icon.id = ids[1];
    g_msg.window_icon.width = g_msg.window_icon.height = 2;
    sfeed(s, ASB_RW_WINDOW_ICON, 0, &g_msg);
    CHECK(g_icons == 1 && g_icon_id == ids[1]);
    g_msg.window_icon.id = (uint64_t)TAG << 32 | 0x77777;
    sfeed(s, ASB_RW_WINDOW_ICON, 0, &g_msg);
    CHECK_EQ(g_icons, 1);
    asb_rw_session_destroy(s);

    /* Guest components without proxy support: the request is refused with UPGRADE_REQUIRED. */
    reset();
    s = session((GUEST_FULL & ~ASB_RW_CAP_GEOMETRY) | ASB_RW_CAP_GUEST_SELECTOR, ids, 1);
    CHECK_EQ(s->phase, ASB_RW_SESSION_LIMITED);
    reset();
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = ids[0];
    g_msg.select.select = 1;
    sfeed(s, ASB_RW_SELECT_REQUEST, 0x8000000Au, &g_msg);
    CHECK(sent(ASB_RW_ERROR, &hd) && hd.request_id == 0x8000000Au && g_rx.error.code == ASB_RW_ERR_UPGRADE_REQUIRED);
    asb_rw_session_destroy(s);

    /* A guest that did not negotiate the selector must not send selection requests. */
    reset();
    s = session(GUEST_FULL, ids, 1);
    CHECK(!(s->negotiated.capabilities & ASB_RW_CAP_GUEST_SELECTOR));
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = ids[0];
    CHECK_EQ(sfeed(s, ASB_RW_SELECT_REQUEST, 1, &g_msg), ASB_RW_ERR_UNKNOWN_TYPE);
    asb_rw_session_destroy(s);
}

/* ================================ guest helper ============================================ */

static uint64_t g_icon_key[4] = { 0x10, 0x20, 0x30, 0x40 };
static BOOL h_send(void *ctx, const uint8_t *b, size_t n) { (void)ctx; return capture(b, n); }
static int h_probe(void *ctx, uint64_t hwnd, AsbRwIdentity *live)
{
    (void)ctx;
    if (hwnd < 0x3000 || hwnd > 0x3003) return 0;
    memset(live, 0, sizeof(*live));
    live->hwnd = hwnd; live->pid = 77; live->thread_id = 5; live->process_start = 99;
    return 1;
}
static uint32_t h_enumerate(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count) { (void)ctx; (void)out; (void)max; *count = 0; return 0; }
static BOOL h_frames(void *ctx, HWND hwnd, RECT *w, RECT *v) { (void)ctx; (void)hwnd; SetRect(w, 0, 0, 10, 10); *v = *w; return TRUE; }
static BOOL h_set_rect(void *ctx, HWND hwnd, const RECT *r, BOOL size) { (void)ctx; (void)hwnd; (void)r; (void)size; return TRUE; }
static BOOL h_true(void *ctx, HWND hwnd) { (void)ctx; (void)hwnd; return TRUE; }
static uint32_t h_hit(void *ctx, HWND hwnd, int x, int y) { (void)ctx; (void)hwnd; (void)x; (void)y; return 1; }
static BOOL h_state(void *ctx, HWND hwnd, BOOL limits, AsbRwWindowState *st, UINT *show) { (void)ctx; (void)hwnd; (void)limits; memset(st, 0, sizeof(*st)); *show = SW_SHOWNORMAL; return TRUE; }
static int g_icon_renders;
static BOOL h_icon(void *ctx, HWND hwnd, uint64_t *key, AsbRwWindowIcon *icon)
{
    (void)ctx;
    uint64_t i = (uint64_t)(ULONG_PTR)hwnd - 0x3000;
    if (i > 3) return FALSE;
    *key = g_icon_key[i];
    if (icon) { icon->width = icon->height = 2; memset(icon->pixels, 0x80, 16); ++g_icon_renders; }
    return TRUE;
}
static void h_log(void *ctx, const char *m) { (void)ctx; (void)m; }
static BOOL h_journal(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; (void)n; return TRUE; }
static DWORD h_release(void *ctx, uint64_t lease, DWORD (*restore)(void *), void *arg) { (void)ctx; (void)lease; return restore ? restore(arg) : 0; }

static uint32_t hfeed(AsbRwHelper *h, uint16_t type, uint32_t request_id, const AsbRwPayload *m)
{
    size_t n = encode(type, request_id, m);
    return asb_rw_helper_receive(h, g_buf, n);
}

static void open_helper(AsbRwHelper *h, AsbRwShare *share, uint32_t host_caps, uint64_t *ids)
{
    AsbRwHelperOps ops;
    memset(share, 0, sizeof(*share));
    share->ops.journal_write = h_journal;
    share->ops.display_release = h_release;
    memset(&ops, 0, sizeof(ops));
    ops.send = h_send; ops.probe = h_probe; ops.log = h_log; ops.enumerate = h_enumerate;
    ops.frames = h_frames; ops.set_rect = h_set_rect; ops.responsive = h_true; ops.post_close = h_true;
    ops.hit_test = h_hit; ops.window_state = h_state; ops.normalize = h_true; ops.icon = h_icon;
    uint32_t caps = asb_rw_helper_capabilities(&ops, GUEST_FULL | ASB_RW_CAP_GUEST_SELECTOR);
    CHECK(caps & ASB_RW_CAP_GUEST_SELECTOR);
    CHECK_EQ(asb_rw_helper_open(h, &ops, share, caps, 1, TAG), ASB_RW_OK);
    memset(&g_msg, 0, sizeof(g_msg));
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_HOST, host_caps, ASB_RW_HOST_VERSION, 0, 0);
    hfeed(h, ASB_RW_HELLO, 0, &g_msg);
    /* Two inventory entries (as refresh_inventory would list them). */
    h->inventory.count = 2;
    for (int i = 0; i < 2; ++i) {
        AsbRwIdentity who;
        memset(&who, 0, sizeof(who));
        who.hwnd = 0x3000u + (uint64_t)i; who.pid = 77; who.thread_id = 5; who.process_start = 99;
        asb_rw_ids_assign(&h->ids, &who, &ids[i]);
        AsbRwInventoryEntry *e = &h->inventory.entries[i];
        memset(e, 0, sizeof(*e));
        e->id = ids[i];
        e->title_len = 4;
        e->title[0] = 'D'; e->title[1] = 'o'; e->title[2] = 'c'; e->title[3] = (uint16_t)('1' + i);
    }
}

static void test_helper(void)
{
    static AsbRwShare share;
    static AsbRwHelper h;
    static AsbRwFlyoutModel m;
    uint64_t ids[2], hwnds[ASB_RW_FLYOUT_MAX_ITEMS];
    AsbRwHeader hd;
    reset();
    open_helper(&h, &share, ASB_RW_HOST_CAPS, ids);
    CHECK(h.negotiated.capabilities & ASB_RW_CAP_GUEST_SELECTOR);
    asb_rw_helper_flyout(&h, &m, hwnds);
    CHECK_EQ(m.status, ASB_RW_FLYOUT_READY);
    CHECK_EQ(m.count, 2);
    CHECK(hwnds[0] == 0x3000 && hwnds[1] == 0x3001);
    CHECK(!wcscmp(m.items[1].label, L"Doc2"));
    CHECK(!m.items[0].pending && m.items[0].state == ASB_RW_STATE_NONE);

    /* Toggle in the guest: a SELECT_REQUEST to the host, the row pending. */
    reset();
    CHECK_EQ(asb_rw_helper_guest_select(&h, ids[0], 1), ASB_RW_OK);
    CHECK(sent(ASB_RW_SELECT_REQUEST, &hd) && g_rx.select.id == ids[0] && g_rx.select.select == 1 && (hd.request_id & 0x80000000u));
    uint32_t request = hd.request_id;
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK(m.items[0].pending && !m.items[1].pending);
    CHECK_EQ(asb_rw_helper_guest_select(&h, ids[0], 1), ASB_RW_ERR_BUSY);   /* one request per row at a time */

    /* The host refuses: the row shows the error and is no longer pending. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.error.code = ASB_RW_ERR_LIMIT;
    g_msg.error.detail = ASB_RW_SELECT_REQUEST;
    CHECK_EQ(hfeed(&h, ASB_RW_ERROR, request, &g_msg), ASB_RW_OK);
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK(!m.items[0].pending && m.items[0].error == ASB_RW_ERR_LIMIT);

    /* The host acts on a request (its own SELECT_REQUEST arrives): pending and error clear. */
    CHECK_EQ(asb_rw_helper_guest_select(&h, ids[1], 0), ASB_RW_OK);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = ids[1];
    g_msg.select.select = 0;
    hfeed(&h, ASB_RW_SELECT_REQUEST, 9, &g_msg);
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK(!m.items[1].pending && !m.items[1].error);

    /* Acknowledged state comes from the share: a parked root shows as shared. */
    share.count = 1;
    share.shared[0].wire_id = ids[1];
    share.shared[0].j.state = ASB_RW_ENTRY_PARKED;
    share.shared[0].j.identity.hwnd = 0x3001;
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK_EQ(m.items[1].state, ASB_RW_STATE_SHARED);
    share.shared[0].j.state = ASB_RW_ENTRY_PARKING;
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK_EQ(m.items[1].state, ASB_RW_STATE_PENDING);
    share.shared[0].j.state = ASB_RW_ENTRY_RESTORING;
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK_EQ(m.items[1].state, ASB_RW_STATE_RESTORING);
    share.count = 0;

    /* No answer at all: the request fails after ASB_RW_HELPER_REQUEST_MS. */
    reset();
    asb_rw_helper_tick_at(&h, 0, 100000);
    CHECK_EQ(asb_rw_helper_guest_select(&h, ids[1], 1), ASB_RW_OK);
    asb_rw_helper_tick_at(&h, 0, 100000 + ASB_RW_HELPER_REQUEST_MS - 1);
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK(m.items[1].pending);
    asb_rw_helper_tick_at(&h, 0, 100000 + ASB_RW_HELPER_REQUEST_MS + 1);
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK(!m.items[1].pending && m.items[1].error == ASB_RW_ERR_BUSY);

    /* Inventory icons: each entry once, again only when its icon changes (paced). */
    CHECK_EQ(count_sent(ASB_RW_WINDOW_ICON), 2);
    reset();
    asb_rw_helper_tick_at(&h, 0, 100000 + ASB_RW_HELPER_REQUEST_MS + 100);
    CHECK_EQ(count_sent(ASB_RW_WINDOW_ICON), 0);
    g_icon_key[0] = 0x11;
    asb_rw_helper_tick_at(&h, 0, 100000 + ASB_RW_HELPER_REQUEST_MS + 1 + ASB_RW_HELPER_LIMITS_MS);
    CHECK_EQ(count_sent(ASB_RW_WINDOW_ICON), 1);
    CHECK(sent(ASB_RW_WINDOW_ICON, NULL) && g_rx.window_icon.id == ids[0] && g_rx.window_icon.width == 2);

    /* Stale request: the window is gone. */
    CHECK_EQ(asb_rw_helper_guest_select(&h, (uint64_t)TAG << 32 | 0x12345, 1), ASB_RW_ERR_STALE_ID);

    /* Restore all from the guest tray: the host hears RESTORE_ALL. */
    reset();
    asb_rw_helper_restore_all(&h);
    CHECK(sent(ASB_RW_RESTORE_ALL, NULL));
    asb_rw_helper_close(&h);

    /* A host without the guest selector: upgrade state, toggles refused locally with the reason. */
    reset();
    open_helper(&h, &share, ASB_RW_HOST_CAPS & ~ASB_RW_CAP_GUEST_SELECTOR, ids);
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK_EQ(m.status, ASB_RW_FLYOUT_UPGRADE);
    CHECK(wcsstr(m.message, L"host") != NULL);
    CHECK_EQ(asb_rw_helper_guest_select(&h, ids[0], 1), ASB_RW_ERR_UPGRADE_REQUIRED);
    CHECK(!sent(ASB_RW_SELECT_REQUEST, NULL));
    asb_rw_helper_flyout(&h, &m, NULL);
    CHECK_EQ(m.items[0].error, ASB_RW_ERR_UPGRADE_REQUIRED);
    asb_rw_helper_tick_at(&h, 0, 5000);
    CHECK_EQ(count_sent(ASB_RW_WINDOW_ICON), 0);   /* icons only when the host has a selector */
    asb_rw_helper_close(&h);
}

int main(void)
{
    test_placement();
    printf("placement        %s\n", g_failures ? "FAIL" : "ok");
    int f = g_failures;
    test_clicks();
    printf("click sequences  %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_texts();
    test_fill();
    printf("texts and labels %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_session();
    printf("host session     %s\n", g_failures > f ? "FAIL" : "ok");
    f = g_failures;
    test_helper();
    printf("guest helper     %s\n", g_failures > f ? "FAIL" : "ok");
    printf("RESULT %s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
