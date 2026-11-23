/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_share_test
 *
 * @file      remote_windows_share_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* The actual guest parking/journal core (tools/agent/remote_windows_share.c) and helper
   control session (tools/agent/remote_windows_helper.c) against a stateful window model:
   placement/show state with mixed-DPI drift, styles, owners, properties, taskbar tabs,
   responsiveness, handle reuse, the private-display lease and the journal file. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../agent/remote_windows_helper.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* ---- window model ----------------------------------------------------------------- */
#define MAXW 40
#define H(i) ((HWND)(ULONG_PTR)(0x1000 + (i) * 4))
#define OWNER  ((HWND)(ULONG_PTR)0x9000)
#define OWNER2 ((HWND)(ULONG_PTR)0x9004)
static const RECT DESK = {0, 0, 1920, 1080}, PRIV = {1920, 0, 4480, 1440};
static const RECT MINR = {-32000, -32000, -31840, -31972};

typedef struct FakeWin {
    BOOL exists, responsive, visible, pmv2;
    DWORD pid, tid;
    uint64_t start;
    HWND owner;
    DWORD style, ex;
    UINT show, flags;
    RECT normal, rect;
    BOOL marked, tab;
    BOOL elevated;
} FakeWin;
static FakeWin g_w[MAXW];

static int widx(HWND h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v < 0x1000 || (v - 0x1000) % 4 || (v - 0x1000) / 4 >= MAXW) return -1;
    return (int)((v - 0x1000) / 4);
}
static FakeWin *win(HWND h) { int i = widx(h); return i >= 0 && g_w[i].exists ? &g_w[i] : NULL; }

static BOOL on_private(const RECT *r)
{
    LONG x = (r->left + r->right) / 2, y = (r->top + r->bottom) / 2;
    return x >= PRIV.left && x < PRIV.right && y >= PRIV.top && y < PRIV.bottom;
}
static UINT dpi_of(const RECT *r) { return on_private(r) ? 120u : 96u; }

/* failure injection and accounting */
static const char *g_fail_op;      /* op name failing on its g_fail_at-th call */
static int g_fail_at, g_calls;
static BOOL g_position_noop, g_owner_noop, g_show_noop;
static int g_activations, g_display_activations, g_display_removals, g_journal_writes;
static int g_mark_sets, g_owner_sets_nonnull, g_park_moves, g_modifications, g_tab_deletes, g_tab_adds;
static BOOL g_journal_fail, g_check_journaled = TRUE;
static uint8_t g_journal[ASB_RW_JOURNAL_MAX_BYTES];
static size_t g_journal_len;
static uint8_t g_first_journal[ASB_RW_JOURNAL_MAX_BYTES];
static size_t g_first_journal_len;
static BOOL g_capture_first;
/* display */
static BOOL g_display_active, g_display_restoring, g_display_fail, g_display_drift;
static uint64_t g_display_lease, g_release_lease;

static BOOL fail(const char *op)
{
    if (!g_fail_op || strcmp(op, g_fail_op)) return FALSE;
    return ++g_calls == g_fail_at;
}

static void model_reset(void)
{
    memset(g_w, 0, sizeof(g_w));
    g_fail_op = NULL; g_fail_at = 0; g_calls = 0;
    g_position_noop = g_owner_noop = g_show_noop = FALSE;
    g_activations = g_display_activations = g_display_removals = g_journal_writes = 0;
    g_mark_sets = g_owner_sets_nonnull = g_park_moves = g_modifications = g_tab_deletes = g_tab_adds = 0;
    g_journal_fail = FALSE; g_check_journaled = TRUE; g_journal_len = 0; g_capture_first = FALSE;
    g_first_journal_len = 0;
    g_display_active = g_display_restoring = g_display_fail = g_display_drift = FALSE;
    g_display_lease = g_release_lease = 0;
}

static HWND add_window(int i, DWORD pid, LONG x, LONG y, LONG w, LONG h, UINT show, DWORD ex)
{
    FakeWin *f = &g_w[i];
    memset(f, 0, sizeof(*f));
    f->exists = f->responsive = f->visible = f->pmv2 = TRUE;
    f->pid = pid; f->tid = pid * 10u; f->start = 0x1D0000000000000ull + pid;
    f->style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    f->ex = ex;
    SetRect(&f->normal, x, y, x + w, y + h);
    f->show = show;
    f->rect = show == SW_SHOWMINIMIZED ? MINR : show == SW_SHOWMAXIMIZED ? DESK : f->normal;
    f->tab = TRUE;
    return H(i);
}

static AsbRwIdentity ident(HWND h)
{
    AsbRwIdentity id = {0};
    FakeWin *f = win(h);
    id.hwnd = (uint64_t)(ULONG_PTR)h;
    if (f) { id.pid = f->pid; id.thread_id = f->tid; id.process_start = f->start; }
    return id;
}

/* Modifying a root must only happen while the journal holds its record. */
static void assert_journaled(HWND h)
{
    AsbRwJournal j;
    FakeWin *f = win(h);
    if (!g_check_journaled || !f) return;
    if (f->owner && f->owner != OWNER && f->owner != OWNER2) return;   /* owned dialog */
    CHECK(g_journal_len && asb_rw_journal_decode(g_journal, g_journal_len, &j) == ASB_RW_OK);
    BOOL found = FALSE;
    for (uint32_t k = 0; k < j.count; ++k) found |= j.entries[k].identity.hwnd == (uint64_t)(ULONG_PTR)h;
    CHECK(found);
}

static int f_probe(void *ctx, HWND h, AsbRwIdentity *live)
{
    (void)ctx;
    if (!win(h)) return 0;
    *live = ident(h);
    return 1;
}
static BOOL f_responsive(void *ctx, HWND h) { (void)ctx; return win(h) && win(h)->responsive; }
static BOOL f_get_placement(void *ctx, HWND h, WINDOWPLACEMENT *wp)
{
    FakeWin *f = win(h);
    (void)ctx;
    if (!f) return FALSE;
    memset(wp, 0, sizeof(*wp));
    wp->length = sizeof(*wp);
    wp->showCmd = f->show;
    wp->flags = f->flags;
    wp->ptMinPosition.x = wp->ptMinPosition.y = -1;
    wp->ptMaxPosition.x = wp->ptMaxPosition.y = -1;
    wp->rcNormalPosition = f->normal;
    return TRUE;
}
static void scale_for(FakeWin *f, const RECT *from, RECT *r)
{
    /* Per-monitor-aware windows rescale when they cross to a monitor with another DPI. */
    UINT a = dpi_of(from), b = dpi_of(r);
    if (!f->pmv2 || a == b) return;
    r->right = r->left + (r->right - r->left) * (LONG)b / (LONG)a;
    r->bottom = r->top + (r->bottom - r->top) * (LONG)b / (LONG)a;
}
static BOOL f_set_placement(void *ctx, HWND h, const WINDOWPLACEMENT *wp)
{
    FakeWin *f = win(h);
    (void)ctx;
    if (!f || fail("set_placement")) return FALSE;
    assert_journaled(h);
    g_modifications++;
    CHECK(wp->showCmd == SW_SHOWNOACTIVATE || wp->showCmd == SW_SHOWMINNOACTIVE || wp->showCmd == SW_SHOWMAXIMIZED);
    if (wp->showCmd == SW_SHOWMAXIMIZED) g_activations++;   /* only for maximized originals */
    RECT target = wp->rcNormalPosition;
    scale_for(f, &f->rect, &target);   /* first placement drifts across DPI, second is exact */
    f->normal = target;
    f->flags = wp->flags & WPF_RESTORETOMAXIMIZED;
    f->show = wp->showCmd == SW_SHOWMINNOACTIVE ? SW_SHOWMINIMIZED :
              wp->showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    f->rect = f->show == SW_SHOWMINIMIZED ? MINR : f->show == SW_SHOWMAXIMIZED ?
              (on_private(&target) ? PRIV : DESK) : target;
    return TRUE;
}
static BOOL f_get_rect(void *ctx, HWND h, RECT *r) { (void)ctx; if (!win(h)) return FALSE; *r = win(h)->rect; return TRUE; }
static BOOL f_show(void *ctx, HWND h, int cmd)
{
    FakeWin *f = win(h);
    (void)ctx;
    if (!f || fail("show")) return FALSE;
    assert_journaled(h);
    g_modifications++;
    CHECK(cmd == SW_SHOWNOACTIVATE);   /* never SW_RESTORE/SW_SHOWNORMAL: they activate */
    if (g_show_noop) return TRUE;
    f->flags &= ~(UINT)WPF_RESTORETOMAXIMIZED;
    f->show = SW_SHOWNORMAL;
    f->rect = f->normal;
    return TRUE;
}
static BOOL f_position(void *ctx, HWND h, int x, int y, UINT flags)
{
    FakeWin *f = win(h);
    (void)ctx;
    if (!f || fail("position")) return FALSE;
    assert_journaled(h);
    g_modifications++;
    CHECK(flags & SWP_NOACTIVATE);
    if (flags & SWP_NOMOVE) return TRUE;
    if (g_position_noop) return TRUE;
    RECT r = f->rect;
    OffsetRect(&r, x - r.left, y - r.top);
    if (on_private(&r) && !on_private(&f->rect) && (f->owner == OWNER || f->owner == NULL)) g_park_moves++;
    scale_for(f, &f->rect, &r);
    f->rect = r;
    if (f->show == SW_SHOWNORMAL) f->normal = r;
    return TRUE;
}
static UINT f_dpi(void *ctx, HWND h) { (void)ctx; return win(h) ? dpi_of(&win(h)->rect) : 96; }
static DWORD f_get_ex(void *ctx, HWND h) { (void)ctx; return win(h) ? win(h)->ex : 0; }
static BOOL f_set_ex(void *ctx, HWND h, DWORD ex)
{
    (void)ctx;
    if (!win(h) || fail("set_ex")) return FALSE;
    assert_journaled(h);
    g_modifications++;
    win(h)->ex = ex;
    return TRUE;
}
static HWND f_get_owner(void *ctx, HWND h) { (void)ctx; return win(h) ? win(h)->owner : NULL; }
static BOOL f_set_owner(void *ctx, HWND h, HWND owner)
{
    (void)ctx;
    if (!win(h) || fail("set_owner")) return FALSE;
    assert_journaled(h);
    g_modifications++;
    if (owner) g_owner_sets_nonnull++;
    if (!g_owner_noop) win(h)->owner = owner;
    return TRUE;
}
static BOOL f_mark(void *ctx, HWND h, BOOL set)
{
    (void)ctx;
    if (!win(h) || (set && fail("mark"))) return FALSE;
    assert_journaled(h);
    g_modifications++;
    if (set) g_mark_sets++;
    win(h)->marked = set;
    return TRUE;
}
static BOOL f_taskbar(void *ctx, HWND h, BOOL add)
{
    (void)ctx;
    if (!win(h) || fail(add ? "tab_add" : "tab_delete")) return FALSE;
    assert_journaled(h);
    g_modifications++;
    if (add) g_tab_adds++; else g_tab_deletes++;
    win(h)->tab = add;
    return TRUE;
}
static uint32_t f_owned(void *ctx, HWND root, HWND *out, uint32_t max)
{
    uint32_t n = 0;
    (void)ctx;
    for (int i = 0; i < MAXW && n < max; ++i) {
        FakeWin *f = &g_w[i];
        if (!f->exists || !f->visible || H(i) == root || (f->style & WS_CAPTION) != WS_CAPTION) continue;
        HWND o = f->owner;
        for (int d = 0; o && d < 8; ++d) {
            if (o == root) { out[n++] = H(i); break; }
            o = win(o) ? win(o)->owner : NULL;
        }
    }
    return n;
}
static DWORD f_activate(void *ctx, uint64_t lease, uint32_t w, uint32_t h, RECT *desktop, RECT *priv)
{
    (void)ctx; (void)w; (void)h;
    if (g_display_restoring) return ERROR_BUSY;
    if (g_display_fail) return ERROR_NOT_SUPPORTED;
    if (g_display_active && lease != g_display_lease) return ERROR_ACCESS_DENIED;
    if (!g_display_active) g_display_activations++;
    g_display_active = TRUE;
    g_display_lease = lease;
    g_display_drift = FALSE;
    *desktop = DESK; *priv = PRIV;
    return ERROR_SUCCESS;
}
static DWORD f_refresh(void *ctx, uint32_t w, uint32_t h, RECT *desktop, RECT *priv)
{
    (void)ctx; (void)w; (void)h;
    if (!g_display_active) return ERROR_NOT_READY;
    if (g_display_drift) return ERROR_INVALID_STATE;
    *desktop = DESK; *priv = PRIV;
    return ERROR_SUCCESS;
}
static DWORD f_release(void *ctx, uint64_t lease, DWORD (*restore)(void *), void *arg)
{
    (void)ctx;
    g_release_lease = lease;
    if (g_display_active && lease != g_display_lease) return ERROR_ACCESS_DENIED;
    g_display_restoring = g_display_active;
    DWORD error = restore(arg);
    if (error) return error;
    if (g_display_active) g_display_removals++;
    g_display_active = g_display_restoring = FALSE;
    return ERROR_SUCCESS;
}
static BOOL f_journal_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (g_journal_fail) return FALSE;
    g_journal_writes++;
    if (len) memcpy(g_journal, bytes, len);
    g_journal_len = len;
    if (g_capture_first && !g_first_journal_len && len) {
        AsbRwJournal j;
        if (!asb_rw_journal_decode(bytes, len, &j) && j.count) {
            memcpy(g_first_journal, bytes, len);
            g_first_journal_len = len;
        }
    }
    return TRUE;
}
static void f_log(void *ctx, const char *message)
{
    (void)ctx;
    if (GetEnvironmentVariableA("ASB_TEST_VERBOSE", NULL, 0)) printf("  log: %s\n", message);
}

static AsbRwShareOps g_ops = {
    NULL, f_probe, f_responsive, f_get_placement, f_set_placement, f_get_rect, f_show, f_position, f_dpi,
    f_get_ex, f_set_ex, f_get_owner, f_set_owner, f_mark, f_taskbar, f_owned, f_activate, f_refresh, f_release,
    f_journal_write, f_log
};

static AsbRwShare g_share;
static void share_new(HWND owner, uint64_t lease) { asb_rw_share_init(&g_share, &g_ops, owner, lease, 2560, 1440); }

static void expect_restored(HWND h, const FakeWin *orig)
{
    FakeWin *f = win(h);
    CHECK(f);
    CHECK(f->show == orig->show);
    CHECK(EqualRect(&f->normal, &orig->normal));
    CHECK(EqualRect(&f->rect, &orig->rect));
    CHECK((f->flags & WPF_RESTORETOMAXIMIZED) == (orig->flags & WPF_RESTORETOMAXIMIZED));
    CHECK(f->ex == orig->ex);
    CHECK(f->owner == orig->owner);
    CHECK(!f->marked);
    CHECK(f->tab);
}

static void expect_parked(HWND h)
{
    FakeWin *f = win(h);
    CHECK(f && f->show == SW_SHOWNORMAL && on_private(&f->rect));
    CHECK(f->marked && f->owner == g_share.owner && !(f->ex & WS_EX_APPWINDOW) && !f->tab);
    int i = asb_rw_share_find(&g_share, h);
    CHECK(i >= 0 && g_share.shared[i].j.state == ASB_RW_ENTRY_PARKED);
}

static uint32_t journal_count(void)
{
    AsbRwJournal j;
    if (!g_journal_len) return 0;
    CHECK(asb_rw_journal_decode(g_journal, g_journal_len, &j) == ASB_RW_OK);
    return j.count;
}

/* ---- tests -------------------------------------------------------------------------- */

static void test_journal_codec(void)
{
    static AsbRwJournal j, k;
    static uint8_t bytes[ASB_RW_JOURNAL_MAX_BYTES];
    memset(&j, 0, sizeof(j));
    j.lease = 0x0123456789ABCDEFull; j.display_active = 1; j.width = 2560; j.height = 1440;
    j.count = ASB_RW_SHARE_MAX;
    for (uint32_t i = 0; i < j.count; ++i) {
        AsbRwJournalEntry *e = &j.entries[i];
        e->identity.hwnd = 0x10000ull + i; e->identity.pid = 100 + i; e->identity.thread_id = 7; e->identity.process_start = ~0ull - i;
        e->state = 1 + i % 3; e->changes = ASB_RW_CHANGE_MASK & (i * 7u);
        e->show_cmd = SW_SHOWMINIMIZED; e->placement_flags = WPF_RESTORETOMAXIMIZED;
        e->min_position.x = -1; e->max_position.y = -32000;
        SetRect(&e->normal, -100, -200, 300, 400); SetRect(&e->frame, 1, 2, 3, 4); SetRect(&e->parked, 2000, 0, 2100, 50);
        e->ex_style = WS_EX_APPWINDOW | WS_EX_TOPMOST; e->dpi = 144; e->helper_owner = 0x9000;
    }
    size_t len = asb_rw_journal_encode(&j, bytes, sizeof(bytes));
    CHECK(len == ASB_RW_JOURNAL_MAX_BYTES);
    CHECK(asb_rw_journal_decode(bytes, len, &k) == ASB_RW_OK);
    CHECK(!memcmp(&j, &k, sizeof(j)));
    CHECK(asb_rw_journal_encode(&j, bytes, len - 1) == 0);
    for (size_t pos = 0; pos < len; pos += 37) {   /* any corruption is detected */
        bytes[pos] ^= 0x5A;
        CHECK(asb_rw_journal_decode(bytes, len, &k) != ASB_RW_OK);
        bytes[pos] ^= 0x5A;
    }
    for (size_t cut = 0; cut < len; cut += 101) CHECK(asb_rw_journal_decode(bytes, cut, &k) != ASB_RW_OK);
    bytes[4] = 2;
    CHECK(asb_rw_journal_decode(bytes, len, &k) == ASB_RW_ERR_WIRE_VERSION);
    bytes[4] = 1;
    j.count = 0; j.lease = 0;
    len = asb_rw_journal_encode(&j, bytes, sizeof(bytes));
    CHECK(asb_rw_journal_decode(bytes, len, &k) == ASB_RW_ERR_MALFORMED);   /* active display without lease */
    j.display_active = 0;
    len = asb_rw_journal_encode(&j, bytes, sizeof(bytes));
    CHECK(len == ASB_RW_JOURNAL_HEADER && asb_rw_journal_decode(bytes, len, &k) == ASB_RW_OK);
    puts("PASS journal codec: round trip at the size limit, corruption/truncation/version/lease rejected");
}

static void test_select_unselect_states(void)
{
    struct { UINT show, flags; DWORD ex; const char *name; } cases[] = {
        {SW_SHOWNORMAL, 0, WS_EX_APPWINDOW | WS_EX_WINDOWEDGE, "normal"},
        {SW_SHOWMINIMIZED, 0, WS_EX_WINDOWEDGE, "minimized"},
        {SW_SHOWMAXIMIZED, 0, WS_EX_APPWINDOW, "maximized"},
        {SW_SHOWMINIMIZED, WPF_RESTORETOMAXIMIZED, 0, "minimized-from-maximized"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        model_reset();
        share_new(OWNER, 0xAAAA);
        HWND a = add_window(0, 500, 200, 120, 780, 600, cases[c].show, cases[c].ex);
        g_w[0].flags = cases[c].flags;
        FakeWin orig = g_w[0];
        AsbRwIdentity none;
        memset(&none, 0, sizeof(none));
        CHECK(asb_rw_share_select(&g_share, &none, 1) == ASB_RW_ERR_NOT_ELIGIBLE);
        AsbRwIdentity id = ident(a);
        CHECK(asb_rw_share_select(&g_share, &id, 77) == ASB_RW_OK);
        expect_parked(a);
        CHECK(g_activations == 0 && g_display_activations == 1 && g_display_lease == 0xAAAA);
        /* Same offset from the desktop origin on the private display. */
        if (cases[c].show == SW_SHOWNORMAL) CHECK(g_w[0].rect.left == PRIV.left + 200 && g_w[0].rect.top == 120);
        int i = asb_rw_share_find(&g_share, a);
        uint32_t want = ASB_RW_CHANGE_PROPERTY | ASB_RW_CHANGE_MOVED | ASB_RW_CHANGE_OWNER | ASB_RW_CHANGE_TAB |
                        (cases[c].show != SW_SHOWNORMAL ? ASB_RW_CHANGE_SHOW : 0) |
                        (cases[c].ex & WS_EX_APPWINDOW ? ASB_RW_CHANGE_APPWINDOW : 0);
        CHECK(g_share.shared[i].j.changes == want && g_share.shared[i].wire_id == 77);
        CHECK(journal_count() == 1);
        CHECK(asb_rw_share_select(&g_share, &id, 78) == ASB_RW_OK);   /* idempotent */
        CHECK(g_share.count == 1 && g_share.shared[0].wire_id == 78);
        CHECK(asb_rw_share_unselect(&g_share, a) == ASB_RW_OK);
        expect_restored(a, &orig);
        CHECK((g_activations > 0) == (cases[c].show == SW_SHOWMAXIMIZED));   /* no no-activate maximize exists */
        CHECK(g_share.count == 0 && g_display_active && journal_count() == 0 && g_journal_len);  /* lease still journaled */
        CHECK(asb_rw_share_unselect(&g_share, a) == ASB_RW_ERR_NOT_ELIGIBLE);
        CHECK(asb_rw_share_release_display(&g_share) == ASB_RW_OK);
        CHECK(!g_display_active && g_display_removals == 1 && g_journal_len == 0);
        printf("PASS select/unselect %s original: parked renderable without activation, exact mixed-DPI restore\n", cases[c].name);
    }
}

static void test_rollback(void)
{
    struct { const char *op; int at; int noop; } steps[] = {
        {"mark", 1, 0}, {NULL, 0, 3}, {"position", 1, 0}, {NULL, 0, 1}, {"set_ex", 1, 0},
        {"set_owner", 1, 0}, {NULL, 0, 2},
    };
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
        model_reset();
        share_new(OWNER, 0xBBBB);
        HWND a = add_window(0, 600, 300, 200, 640, 480, SW_SHOWMINIMIZED, WS_EX_APPWINDOW);
        FakeWin orig = g_w[0];
        g_fail_op = steps[s].op; g_fail_at = steps[s].at;
        g_position_noop = steps[s].noop == 1;   /* moved but not on the private display */
        g_owner_noop = steps[s].noop == 2;      /* owner did not take */
        g_show_noop = steps[s].noop == 3;       /* window would not leave the minimized state */
        AsbRwIdentity id = ident(a);
        CHECK(asb_rw_share_select(&g_share, &id, 5) == ASB_RW_ERR_INTERNAL);
        g_fail_op = NULL; g_position_noop = g_owner_noop = g_show_noop = FALSE;
        expect_restored(a, &orig);
        CHECK(g_share.count == 0 && !g_display_active && g_display_removals == 1 && g_journal_len == 0);
    }
    /* Journal not durable: nothing is modified at all. */
    model_reset();
    share_new(OWNER, 0xBBBB);
    HWND a = add_window(0, 600, 300, 200, 640, 480, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    FakeWin orig = g_w[0];
    g_journal_fail = TRUE;
    AsbRwIdentity id = ident(a);
    CHECK(asb_rw_share_select(&g_share, &id, 5) == ASB_RW_ERR_INTERNAL);
    CHECK(g_modifications == 0 && g_display_activations == 0);
    expect_restored(a, &orig);
    /* Display activation failure: nothing modified, lease released. */
    g_journal_fail = FALSE;
    g_display_fail = TRUE;
    CHECK(asb_rw_share_select(&g_share, &id, 5) == ASB_RW_ERR_PRIVATE_DISPLAY);
    CHECK(g_modifications == 0 && !g_share.display_active && g_journal_len == 0);
    /* Rollback of a second window keeps the first shared and the display present. */
    g_display_fail = FALSE;
    HWND b = add_window(1, 601, 50, 60, 400, 300, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    FakeWin orig_b = g_w[1];
    CHECK(asb_rw_share_select(&g_share, &id, 5) == ASB_RW_OK);
    g_fail_op = "set_owner"; g_fail_at = 1; g_calls = 0;
    AsbRwIdentity idb = ident(b);
    CHECK(asb_rw_share_select(&g_share, &idb, 6) == ASB_RW_ERR_INTERNAL);
    g_fail_op = NULL;
    expect_restored(b, &orig_b);
    expect_parked(a);
    CHECK(g_share.count == 1 && g_display_active && journal_count() == 1);
    CHECK(asb_rw_share_restore_all(&g_share) == ASB_RW_OK);
    expect_restored(a, &orig);
    CHECK(!g_display_active && g_journal_len == 0);
    puts("PASS partial select failures roll back every step (mark/show/move/verify/style/owner), journal and display");
}

static void test_owned_and_independent(void)
{
    model_reset();
    share_new(OWNER, 0xCCCC);
    HWND root = add_window(0, 700, 100, 100, 800, 600, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    HWND dialog = add_window(1, 700, 760, 340, 400, 300, SW_SHOWNORMAL, WS_EX_DLGMODALFRAME);
    g_w[1].owner = root; g_w[1].tab = FALSE;
    HWND tooltip = add_window(2, 700, 150, 150, 80, 20, SW_SHOWNORMAL, WS_EX_TOOLWINDOW);
    g_w[2].owner = root; g_w[2].style = WS_POPUP | WS_VISIBLE;
    HWND independent = add_window(3, 700, 400, 300, 500, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    FakeWin ind = g_w[3], tip = g_w[2];
    AsbRwIdentity id = ident(root);
    CHECK(asb_rw_share_select(&g_share, &id, 1) == ASB_RW_OK);
    CHECK(on_private(&g_w[1].rect) && g_w[1].rect.left == PRIV.left + 760);   /* owned dialog follows */
    CHECK(!g_w[1].marked && g_w[1].owner == root);                             /* only moved */
    CHECK(EqualRect(&g_w[2].rect, &tip.rect));                                 /* popups are left to the app */
    CHECK(!memcmp(&g_w[3], &ind, sizeof(ind)));                                /* independent root untouched */
    /* An owned window as a root is refused. */
    AsbRwIdentity idd = ident(dialog);
    CHECK(asb_rw_share_select(&g_share, &idd, 2) == ASB_RW_ERR_NOT_ELIGIBLE);
    /* A dialog opened later on the desktop is parked by the next pass; the independent root is not. */
    HWND later = add_window(4, 700, 10, 10, 300, 200, SW_SHOWNORMAL, 0);
    g_w[4].owner = dialog;   /* owned through the chain */
    asb_rw_share_maintain(&g_share, NULL, 0, NULL, NULL, 0, NULL);
    CHECK(on_private(&g_w[4].rect));
    CHECK(!memcmp(&g_w[3], &ind, sizeof(ind)));
    CHECK(asb_rw_share_unselect(&g_share, root) == ASB_RW_OK);
    CHECK(!on_private(&g_w[1].rect) && g_w[1].rect.left == 760 && g_w[1].rect.top == 340);
    CHECK(!on_private(&g_w[4].rect) && g_w[4].rect.left == 10);
    CHECK(!memcmp(&g_w[3], &ind, sizeof(ind)));
    (void)tooltip; (void)later; (void)independent;
    puts("PASS owned dialogs (direct and chained) travel with the root; popups and independent same-process roots untouched");
}

static void test_concurrent_styles(void)
{
    model_reset();
    share_new(OWNER, 0xDDDD);
    HWND a = add_window(0, 800, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    HWND b = add_window(1, 801, 700, 100, 600, 400, SW_SHOWNORMAL, WS_EX_WINDOWEDGE);   /* never had APPWINDOW */
    AsbRwIdentity ia = ident(a), ib = ident(b);
    CHECK(asb_rw_share_select(&g_share, &ia, 1) == ASB_RW_OK);
    CHECK(asb_rw_share_select(&g_share, &ib, 2) == ASB_RW_OK);
    /* The applications change their own styles while shared. */
    g_w[0].ex |= WS_EX_TOPMOST | WS_EX_LAYERED;
    g_w[0].style &= ~(DWORD)WS_MAXIMIZEBOX;
    g_w[1].ex |= WS_EX_APPWINDOW;   /* tries to come back to the taskbar */
    g_w[1].tab = TRUE;
    asb_rw_share_maintain(&g_share, NULL, 0, NULL, NULL, 0, NULL);
    CHECK(!(g_w[1].ex & WS_EX_APPWINDOW) && !g_w[1].tab);   /* re-suppressed */
    CHECK(g_share.shared[1].j.changes & ASB_RW_CHANGE_APPWINDOW);
    CHECK(asb_rw_share_restore_all(&g_share) == ASB_RW_OK);
    CHECK((g_w[0].ex & (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_APPWINDOW)) == (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_APPWINDOW));
    CHECK(!(g_w[0].style & WS_MAXIMIZEBOX));
    CHECK(g_w[1].ex & WS_EX_APPWINDOW);                       /* the application's own wish */
    /* An application that re-owned its window keeps that owner. */
    model_reset();
    share_new(OWNER, 0xDDDD);
    a = add_window(0, 800, 100, 100, 600, 400, SW_SHOWNORMAL, 0);
    HWND other = add_window(1, 800, 0, 0, 10, 10, SW_SHOWNORMAL, 0);
    ia = ident(a);
    CHECK(asb_rw_share_select(&g_share, &ia, 1) == ASB_RW_OK);
    g_w[0].owner = other;
    g_check_journaled = FALSE;
    CHECK(asb_rw_share_unselect(&g_share, a) == ASB_RW_OK);
    CHECK(g_w[0].owner == other && !(g_w[0].ex & WS_EX_APPWINDOW));
    puts("PASS concurrent application style/owner changes survive; only integration-owned bits are restored");
}

static void test_destroyed_and_reused(void)
{
    uint64_t gone[4], restored[4];
    uint32_t n_gone = 0, n_restored = 0;
    model_reset();
    share_new(OWNER, 0xEEEE);
    HWND a = add_window(0, 900, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    HWND b = add_window(1, 901, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    AsbRwIdentity ia = ident(a), ib = ident(b);
    /* Stale identities are refused before anything is touched. */
    AsbRwIdentity stale = ia; stale.process_start++;
    CHECK(asb_rw_share_select(&g_share, &stale, 1) == ASB_RW_ERR_STALE_ID && g_modifications == 0);
    CHECK(asb_rw_share_select(&g_share, &ia, 11) == ASB_RW_OK);
    CHECK(asb_rw_share_select(&g_share, &ib, 12) == ASB_RW_OK);
    g_w[0].exists = FALSE;                                   /* destroyed */
    g_w[1].pid = 4242; g_w[1].start = 99;                    /* HWND value now belongs to another process */
    g_w[1].owner = NULL; g_w[1].marked = FALSE; g_w[1].ex = WS_EX_APPWINDOW; g_w[1].tab = TRUE;
    FakeWin reused = g_w[1];
    int before = g_modifications;
    asb_rw_share_maintain(&g_share, gone, 4, &n_gone, restored, 4, &n_restored);
    CHECK(n_gone == 2 && n_restored == 0 && ((gone[0] == 11 && gone[1] == 12) || (gone[0] == 12 && gone[1] == 11)));
    CHECK(g_modifications == before && !memcmp(&g_w[1], &reused, sizeof(reused)));
    CHECK(g_share.count == 0 && journal_count() == 0);
    /* Unselect of a reused handle drops it untouched too. */
    g_w[1] = g_w[2];
    b = add_window(1, 901, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    ib = ident(b);
    CHECK(asb_rw_share_select(&g_share, &ib, 13) == ASB_RW_OK);
    g_w[1].pid = 4343;
    before = g_modifications;
    CHECK(asb_rw_share_unselect(&g_share, b) == ASB_RW_ERR_STALE_ID && g_modifications == before);
    CHECK(asb_rw_share_restore_all(&g_share) == ASB_RW_OK && !g_display_active);
    puts("PASS destroyed and reused handles are dropped without touching the window now behind the HWND");
}

static void test_hung(void)
{
    uint64_t gone[4], restored[4];
    uint32_t n_gone = 0, n_restored = 0;
    model_reset();
    share_new(OWNER, 0xF0F0);
    HWND a = add_window(0, 1000, 100, 100, 600, 400, SW_SHOWMAXIMIZED, WS_EX_APPWINDOW);
    FakeWin orig = g_w[0];
    AsbRwIdentity ia = ident(a);
    g_w[0].responsive = FALSE;
    CHECK(asb_rw_share_select(&g_share, &ia, 1) == ASB_RW_ERR_BUSY && g_modifications == 0 && !g_display_active);
    g_w[0].responsive = TRUE;
    CHECK(asb_rw_share_select(&g_share, &ia, 21) == ASB_RW_OK);
    g_w[0].responsive = FALSE;
    int before = g_modifications;
    CHECK(asb_rw_share_unselect(&g_share, a) == ASB_RW_ERR_BUSY && g_modifications == before);
    CHECK(g_share.count == 1 && g_share.shared[0].j.state == ASB_RW_ENTRY_RESTORING);
    CHECK(asb_rw_share_select(&g_share, &ia, 21) == ASB_RW_ERR_BUSY);       /* transition pending */
    CHECK(asb_rw_share_restore_all(&g_share) == ASB_RW_ERR_BUSY);
    CHECK(asb_rw_share_release_display(&g_share) == ASB_RW_ERR_BUSY && g_display_active);  /* never under a source */
    asb_rw_share_maintain(&g_share, gone, 4, &n_gone, restored, 4, &n_restored);
    CHECK(n_restored == 0 && g_share.count == 1);
    g_w[0].responsive = TRUE;
    asb_rw_share_maintain(&g_share, gone, 4, &n_gone, restored, 4, &n_restored);
    CHECK(n_restored == 1 && restored[0] == 21 && g_share.count == 0);
    expect_restored(a, &orig);
    CHECK(asb_rw_share_release_display(&g_share) == ASB_RW_OK && !g_display_active);
    puts("PASS hung windows: select refused untouched, restoration pending and retried, display kept until restored");
}

static void test_recovery(void)
{
    model_reset();
    share_new(OWNER, 0x1111);
    HWND a = add_window(0, 1100, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    HWND b = add_window(1, 1101, 300, 200, 500, 300, SW_SHOWMINIMIZED, WS_EX_APPWINDOW);
    HWND c = add_window(2, 1102, 50, 50, 300, 300, SW_SHOWMAXIMIZED, WS_EX_APPWINDOW);
    HWND d = add_window(3, 1103, 50, 50, 300, 300, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    FakeWin oa = g_w[0], ob = g_w[1];
    AsbRwIdentity ia = ident(a), ib = ident(b), ic = ident(c), id = ident(d);
    CHECK(!asb_rw_share_select(&g_share, &ia, 1) && !asb_rw_share_select(&g_share, &ib, 2) &&
          !asb_rw_share_select(&g_share, &ic, 3) && !asb_rw_share_select(&g_share, &id, 4));
    /* Helper dies: Windows unlinks (does not destroy) windows it owned. */
    for (int i = 0; i < MAXW; ++i) if (g_w[i].owner == OWNER) g_w[i].owner = NULL;
    g_w[2].exists = FALSE;                          /* c closed meanwhile */
    g_w[3].pid = 7777;                              /* d's HWND reused */
    FakeWin reused = g_w[3];
    uint8_t saved[ASB_RW_JOURNAL_MAX_BYTES];
    size_t saved_len = g_journal_len;
    memcpy(saved, g_journal, saved_len);
    /* New helper, new owner window and a different service lease: recovery restores only. */
    share_new(OWNER2, 0x2222);
    g_mark_sets = g_owner_sets_nonnull = g_park_moves = 0;
    CHECK(asb_rw_share_recover(&g_share, saved, saved_len) == ASB_RW_OK);
    CHECK(g_mark_sets == 0 && g_owner_sets_nonnull == 0 && g_park_moves == 0);   /* never re-parked */
    expect_restored(a, &oa);
    expect_restored(b, &ob);
    CHECK(!memcmp(&g_w[3], &reused, sizeof(reused)));
    CHECK(g_release_lease == 0x1111 && !g_display_active && g_journal_len == 0 && g_share.count == 0);
    CHECK(asb_rw_share_recover(&g_share, NULL, 0) == ASB_RW_OK);

    /* Crash in the middle of parking: the PARKING record restores whatever was done. */
    model_reset();
    share_new(OWNER, 0x3333);
    a = add_window(0, 1200, 100, 100, 600, 400, SW_SHOWMINIMIZED, WS_EX_APPWINDOW);
    oa = g_w[0];
    g_capture_first = TRUE;
    ia = ident(a);
    CHECK(asb_rw_share_select(&g_share, &ia, 1) == ASB_RW_OK);
    AsbRwJournal first;
    CHECK(g_first_journal_len && !asb_rw_journal_decode(g_first_journal, g_first_journal_len, &first));
    CHECK(first.count == 1 && first.entries[0].state == ASB_RW_ENTRY_PARKING && first.display_active);
    g_w[0].owner = NULL;                            /* helper died */
    share_new(OWNER2, 0x4444);
    CHECK(asb_rw_share_recover(&g_share, g_first_journal, g_first_journal_len) == ASB_RW_OK);
    expect_restored(a, &oa);
    CHECK(!g_display_active);

    /* A hung survivor stays pending (display kept); a corrupt journal is discarded. */
    model_reset();
    share_new(OWNER, 0x5555);
    a = add_window(0, 1300, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    ia = ident(a);
    CHECK(asb_rw_share_select(&g_share, &ia, 1) == ASB_RW_OK);
    memcpy(saved, g_journal, g_journal_len);
    saved_len = g_journal_len;
    g_w[0].responsive = FALSE;
    share_new(OWNER2, 0x6666);
    CHECK(asb_rw_share_recover(&g_share, saved, saved_len) == ASB_RW_ERR_BUSY);
    CHECK(g_share.count == 1 && g_share.shared[0].wire_id == 0 && g_display_active && journal_count() == 1);
    g_w[0].responsive = TRUE;
    asb_rw_share_maintain(&g_share, NULL, 0, NULL, NULL, 0, NULL);
    CHECK(g_share.count == 0 && asb_rw_share_release_display(&g_share) == ASB_RW_OK && !g_display_active);
    saved[50] ^= 1;
    share_new(OWNER2, 0x6666);
    CHECK(asb_rw_share_recover(&g_share, saved, saved_len) == ASB_RW_ERR_MALFORMED && g_journal_len == 0);
    puts("PASS recovery after helper death: identities validated, never re-parked, PARKING records, journal lease, hung survivors");
}

static void test_display_drift_and_limits(void)
{
    model_reset();
    share_new(OWNER, 0x7777);
    HWND a = add_window(0, 1400, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    AsbRwIdentity ia = ident(a);
    CHECK(asb_rw_share_select(&g_share, &ia, 1) == ASB_RW_OK);
    g_display_drift = TRUE;                          /* watcher moved outputs around */
    OffsetRect(&g_w[0].rect, -PRIV.left, 0);         /* Windows migrated the window to the desktop */
    asb_rw_share_maintain(&g_share, NULL, 0, NULL, NULL, 0, NULL);
    CHECK(!g_display_drift && on_private(&g_w[0].rect));
    CHECK(asb_rw_share_restore_all(&g_share) == ASB_RW_OK);
    for (int i = 0; i < (int)ASB_RW_SHARE_MAX + 1; ++i) add_window(i, 2000 + (DWORD)i, 10 * i, 10, 100, 100, SW_SHOWNORMAL, 0);
    for (int i = 0; i < (int)ASB_RW_SHARE_MAX; ++i) {
        AsbRwIdentity x = ident(H(i));
        CHECK(asb_rw_share_select(&g_share, &x, (uint64_t)i + 1) == ASB_RW_OK);
    }
    AsbRwIdentity over = ident(H(ASB_RW_SHARE_MAX));
    CHECK(asb_rw_share_select(&g_share, &over, 99) == ASB_RW_ERR_LIMIT);
    CHECK(asb_rw_share_restore_all(&g_share) == ASB_RW_OK && !g_display_active && g_journal_len == 0);
    puts("PASS display drift re-placed, migrated windows re-parked, table limit, restore-all removes the display");
}

/* ---- helper session ------------------------------------------------------------------ */
#define MAXMSG 64
static uint8_t *g_sent[MAXMSG];
static size_t g_sent_len[MAXMSG];
static int g_sent_count;

static BOOL s_send(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    CHECK(g_sent_count < MAXMSG);
    g_sent[g_sent_count] = (uint8_t *)malloc(len);
    memcpy(g_sent[g_sent_count], bytes, len);
    g_sent_len[g_sent_count++] = len;
    return TRUE;
}
static void sent_clear(void)
{
    for (int i = 0; i < g_sent_count; ++i) free(g_sent[i]);
    g_sent_count = 0;
}
static void fill_facts(int i, AsbRwWindowFacts *f)
{
    FakeWin *w = &g_w[i];
    memset(f, 0, sizeof(*f));
    f->identity = ident(H(i));
    f->session_id = f->active_session_id = 1;
    f->style = w->style; f->ex_style = w->ex;
    f->visible = (uint8_t)w->visible; f->minimized = w->show == SW_SHOWMINIMIZED;
    f->elevation = w->elevated ? ASB_RW_ELEVATION_YES : ASB_RW_ELEVATION_NO;
    f->owner = (uint64_t)(ULONG_PTR)w->owner;
    f->rect.left = w->rect.left; f->rect.top = w->rect.top; f->rect.right = w->rect.right; f->rect.bottom = w->rect.bottom;
    f->dpi = 96;
    f->title_len = (uint16_t)swprintf((wchar_t *)f->title, ASB_RW_MAX_TITLE, L"Window %d", i);
    f->class_len = 4; memcpy(f->class_name, L"Fake", 8);
    f->app_len = 3; memcpy(f->app, L"App", 6);
}
static uint32_t s_enumerate(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx;
    *count = 0;
    for (int i = 0; i < MAXW && *count < max; ++i) if (g_w[i].exists) fill_facts(i, &out[(*count)++]);
    return ASB_RW_OK;
}
static BOOL s_facts(void *ctx, HWND h, AsbRwWindowFacts *f) { (void)ctx; int i = widx(h); if (i < 0 || !g_w[i].exists) return FALSE; fill_facts(i, f); return TRUE; }
static int s_probe(void *ctx, uint64_t h, AsbRwIdentity *live) { return f_probe(ctx, (HWND)(ULONG_PTR)h, live); }

static void host_send(AsbRwHelper *h, uint16_t type, uint32_t request_id, const AsbRwPayload *msg, uint32_t expect)
{
    static uint8_t buf[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD];
    size_t len = 0;
    CHECK(asb_rw_encode(type, request_id, msg, buf, sizeof(buf), &len) == ASB_RW_OK);
    CHECK(asb_rw_helper_receive(h, buf, len) == expect);
}
static AsbRwPayload g_rx;
static AsbRwHeader decode_sent(int i)
{
    AsbRwHeader hdr;
    const uint8_t *bulk; size_t bulk_len;
    CHECK(i < g_sent_count);
    CHECK(asb_rw_decode(ASB_CH_RW_CONTROL, g_sent[i], g_sent_len[i], &hdr, &g_rx, &bulk, &bulk_len) == ASB_RW_OK);
    return hdr;
}
static int find_result(uint64_t id, uint32_t state, uint32_t error)
{
    for (int i = 0; i < g_sent_count; ++i) {
        AsbRwHeader hdr = decode_sent(i);
        if (hdr.type == ASB_RW_SELECT_RESULT && g_rx.select_result.id == id && g_rx.select_result.state == state &&
            g_rx.select_result.error == error) return i;
    }
    return -1;
}
static uint64_t entry_id(const AsbRwHelper *h, HWND hwnd)
{
    for (uint32_t i = 0; i < h->inventory.count; ++i) {
        AsbRwIdentity live;
        if (asb_rw_ids_resolve((AsbRwIdTable *)&h->ids, h->inventory.entries[i].id, s_probe, NULL, &live) == ASB_RW_OK &&
            live.hwnd == (uint64_t)(ULONG_PTR)hwnd) return h->inventory.entries[i].id;
    }
    return 0;
}

static void test_helper_session(void)
{
    static AsbRwHelper h;
    static AsbRwPayload msg;
    AsbRwHelperOps ops = {NULL, s_send, s_enumerate, s_facts, s_probe, f_log};
    uint32_t caps = ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY;
    model_reset();
    share_new(OWNER, 0x8888);
    HWND a = add_window(0, 1500, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    HWND b = add_window(1, 1501, 200, 100, 600, 400, SW_SHOWMINIMIZED, WS_EX_APPWINDOW);
    HWND c = add_window(2, 1502, 300, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    FakeWin ob = g_w[1];

    CHECK(asb_rw_helper_open(&h, &ops, &g_share, caps, 1, 0) == ASB_RW_ERR_INTERNAL);   /* tag required */
    sent_clear();
    CHECK(asb_rw_helper_open(&h, &ops, &g_share, caps, 1, 0xC0FFEE) == ASB_RW_OK);
    CHECK(g_sent_count == 1 && decode_sent(0).type == ASB_RW_HELLO);
    CHECK(g_rx.hello.role == ASB_RW_ROLE_GUEST && g_rx.hello.session_tag == 0xC0FFEE && g_rx.hello.capabilities == caps &&
          g_rx.hello.helper_version == ASB_RW_HELPER_VERSION);
    sent_clear();
    /* Nothing before HELLO. */
    host_send(&h, ASB_RW_INVENTORY_REQUEST, 1, &msg, ASB_RW_ERR_MALFORMED);
    CHECK(decode_sent(0).type == ASB_RW_ERROR && g_rx.error.code == ASB_RW_ERR_MALFORMED);
    sent_clear();
    /* No common version: refused. */
    asb_rw_hello_init(&msg.hello, ASB_RW_ROLE_HOST, ASB_RW_CAP_KNOWN_MASK, 0x10000, 0, 0);
    msg.hello.min_version = msg.hello.max_version = 9;
    host_send(&h, ASB_RW_HELLO, 2, &msg, ASB_RW_ERR_UNSUPPORTED_VERSION);
    sent_clear();
    asb_rw_hello_init(&msg.hello, ASB_RW_ROLE_HOST, ASB_RW_CAP_KNOWN_MASK, 0x10000, 0, 0);
    host_send(&h, ASB_RW_HELLO, 3, &msg, ASB_RW_OK);
    CHECK(h.negotiated.error == ASB_RW_ERR_UPGRADE_REQUIRED && (h.negotiated.capabilities & caps) == caps);
    host_send(&h, ASB_RW_HELLO, 4, &msg, ASB_RW_ERR_MALFORMED);   /* duplicate */
    sent_clear();
    host_send(&h, ASB_RW_INVENTORY_REQUEST, 5, &msg, ASB_RW_OK);
    AsbRwHeader hdr = decode_sent(g_sent_count - 1);
    CHECK(hdr.type == ASB_RW_INVENTORY_SNAPSHOT && hdr.request_id == 5 && g_rx.snapshot.count == 3);
    uint64_t ida = entry_id(&h, a), idb = entry_id(&h, b), idc = entry_id(&h, c);
    CHECK(ida && idb && idc);
    sent_clear();

    msg.select.id = ida; msg.select.select = 1;
    host_send(&h, ASB_RW_SELECT_REQUEST, 6, &msg, ASB_RW_OK);
    int pending = find_result(ida, ASB_RW_STATE_PENDING, ASB_RW_OK), shared = find_result(ida, ASB_RW_STATE_SHARED, ASB_RW_OK);
    CHECK(pending >= 0 && shared > pending);
    expect_parked(a);
    BOOL changed = FALSE;   /* selected flag reaches the selector */
    for (int i = 0; i < g_sent_count; ++i) {
        hdr = decode_sent(i);
        if (hdr.type == ASB_RW_INVENTORY_EVENT && g_rx.event.entry.id == ida && g_rx.event.kind == ASB_RW_EVENT_CHANGED)
            changed |= (g_rx.event.entry.flags & ASB_RW_ENTRY_SELECTED) != 0;
    }
    CHECK(changed);
    sent_clear();
    msg.select.id = idb;
    host_send(&h, ASB_RW_SELECT_REQUEST, 7, &msg, ASB_RW_OK);
    CHECK(find_result(idb, ASB_RW_STATE_SHARED, ASB_RW_OK) >= 0);
    /* Shared roots stay listed although re-owned and suppressed. */
    asb_rw_helper_tick(&h, 1);
    CHECK(h.inventory.count == 3 && entry_id(&h, a) == ida && entry_id(&h, b) == idb);
    sent_clear();
    /* Forged / previous-session IDs. */
    msg.select.id = ida ^ 0x100000000ull;
    host_send(&h, ASB_RW_SELECT_REQUEST, 8, &msg, ASB_RW_OK);
    CHECK(find_result(msg.select.id, ASB_RW_STATE_FAILED, ASB_RW_ERR_STALE_ID) >= 0);
    sent_clear();
    /* Became elevated after being listed. */
    g_w[2].elevated = TRUE;
    msg.select.id = idc;
    host_send(&h, ASB_RW_SELECT_REQUEST, 9, &msg, ASB_RW_OK);
    CHECK(find_result(idc, ASB_RW_STATE_FAILED, ASB_RW_ERR_ELEVATED) >= 0 && !g_w[2].marked);
    g_w[2].elevated = FALSE;
    sent_clear();
    /* Unselect: RESTORING then NONE, original restored, entry kept. */
    msg.select.id = idb; msg.select.select = 0;
    host_send(&h, ASB_RW_SELECT_REQUEST, 10, &msg, ASB_RW_OK);
    int restoring = find_result(idb, ASB_RW_STATE_RESTORING, ASB_RW_OK), none = find_result(idb, ASB_RW_STATE_NONE, ASB_RW_OK);
    CHECK(restoring >= 0 && none > restoring);
    expect_restored(b, &ob);
    sent_clear();
    /* A shared root that closes is reported. */
    g_w[0].exists = FALSE;
    asb_rw_helper_tick(&h, 1);
    CHECK(find_result(ida, ASB_RW_STATE_NONE, ASB_RW_ERR_STALE_ID) >= 0);
    g_w[0].exists = TRUE;   /* (a new window takes slot 0 later) */
    g_w[0].pid = 1600; g_w[0].start = 5; g_w[0].owner = NULL; g_w[0].marked = FALSE;
    sent_clear();
    /* Unimplemented requests answer UPGRADE_REQUIRED; guest->host types are refused. */
    memset(&msg, 0, sizeof(msg));
    msg.geometry.id = idc;
    host_send(&h, ASB_RW_GEOMETRY_COMMAND, 11, &msg, ASB_RW_OK);
    hdr = decode_sent(0);
    CHECK(hdr.type == ASB_RW_ERROR && hdr.request_id == 11 && g_rx.error.code == ASB_RW_ERR_UPGRADE_REQUIRED);
    sent_clear();
    msg.select_result.id = idc;
    host_send(&h, ASB_RW_SELECT_RESULT, 12, &msg, ASB_RW_OK);
    CHECK(decode_sent(0).type == ASB_RW_ERROR && g_rx.error.code == ASB_RW_ERR_UNKNOWN_TYPE);
    sent_clear();
    /* RESTORE_ALL acknowledges with the same request id after restoring. */
    msg.select.id = idc; msg.select.select = 1;
    host_send(&h, ASB_RW_SELECT_REQUEST, 13, &msg, ASB_RW_OK);
    expect_parked(c);
    sent_clear();
    host_send(&h, ASB_RW_RESTORE_ALL, 14, &msg, ASB_RW_OK);
    CHECK(find_result(idc, ASB_RW_STATE_NONE, ASB_RW_OK) >= 0);
    hdr = decode_sent(g_sent_count - 1);
    CHECK(hdr.type == ASB_RW_RESTORE_ALL && hdr.request_id == 14);
    CHECK(!g_w[2].marked && g_share.count == 0 && !g_display_active);
    sent_clear();
    /* Malformed bytes drop the connection; closing restores every window. */
    msg.select.id = idc; msg.select.select = 1;
    host_send(&h, ASB_RW_SELECT_REQUEST, 15, &msg, ASB_RW_OK);
    expect_parked(c);
    uint8_t junk[20] = {0x41, 0x53, 0x52, 0x57, 1, 0, 6, 0, 0, 0, 0, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    CHECK(asb_rw_helper_receive(&h, junk, sizeof(junk)) != ASB_RW_OK);
    asb_rw_helper_close(&h);
    CHECK(!g_w[2].marked && g_w[2].owner == NULL && g_share.count == 0 && !g_display_active);
    sent_clear();
    /* A host without selection support cannot select. */
    CHECK(asb_rw_helper_open(&h, &ops, &g_share, caps, 1, 0xBEEF) == ASB_RW_OK);
    asb_rw_hello_init(&msg.hello, ASB_RW_ROLE_HOST, ASB_RW_CAP_INVENTORY, 0x10000, 0, 0);
    host_send(&h, ASB_RW_HELLO, 1, &msg, ASB_RW_OK);
    host_send(&h, ASB_RW_INVENTORY_REQUEST, 2, &msg, ASB_RW_OK);
    idc = entry_id(&h, c);
    sent_clear();
    msg.select.id = idc; msg.select.select = 1;
    host_send(&h, ASB_RW_SELECT_REQUEST, 3, &msg, ASB_RW_OK);
    CHECK(find_result(idc, ASB_RW_STATE_FAILED, ASB_RW_ERR_UPGRADE_REQUIRED) >= 0 && !g_w[2].marked);
    asb_rw_helper_close(&h);
    sent_clear();
    puts("PASS helper session: HELLO/version gate, snapshot/events, select/unselect acks, stale/elevated, close/restore-all/upgrade");
}

/* ---- surfaces (eh6.5) ---------------------------------------------------------------- */
static const char *g_class[MAXW];
static uint64_t g_cap_root;
static uint32_t g_cap_n, g_cap_calls;
static AsbRwCaptureTarget g_cap[ASB_RW_MAX_SURFACES];

/* Visible windows, top first (higher model index = higher in z order). */
static uint32_t s_scan(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx;
    *count = 0;
    for (int i = MAXW - 1; i >= 0 && *count < max; --i) {
        if (!g_w[i].exists || !g_w[i].visible) continue;
        AsbRwWindowFacts *f = &out[(*count)++];
        fill_facts(i, f);
        if (g_class[i]) {
            f->class_len = 0;
            for (const char *c = g_class[i]; *c; ++c) f->class_name[f->class_len++] = (uint16_t)*c;
        }
        HWND o = g_w[i].owner;
        while (o && f->owner_depth < ASB_RW_MAX_OWNER_DEPTH) {
            f->owner_chain[f->owner_depth++] = (uint64_t)(ULONG_PTR)o;
            FakeWin *ow = win(o);
            o = ow ? ow->owner : NULL;
        }
    }
    return ASB_RW_OK;
}
static void s_capture(void *ctx, uint64_t root_id, const AsbRwCaptureTarget *targets, uint32_t n)
{
    (void)ctx;
    g_cap_root = root_id;
    g_cap_n = n;
    g_cap_calls++;
    if (n) memcpy(g_cap, targets, n * sizeof(*targets));
}
static int count_sent(uint16_t type)
{
    int n = 0;
    for (int i = 0; i < g_sent_count; ++i) n += decode_sent(i).type == type;
    return n;
}
static int last_surfaces(void)
{
    for (int i = g_sent_count - 1; i >= 0; --i) if (decode_sent(i).type == ASB_RW_SURFACES) return i;
    return -1;
}
static HWND add_popup(int i, DWORD pid, HWND owner, LONG x, LONG y, LONG w, LONG h, const char *cls)
{
    HWND hw = add_window(i, pid, x, y, w, h, SW_SHOWNORMAL, WS_EX_TOOLWINDOW);
    g_w[i].style = WS_POPUP | WS_VISIBLE;
    g_w[i].owner = owner;
    g_w[i].start = 0x1D0000000000000ull + pid;
    g_class[i] = cls;
    return hw;
}

static void test_helper_surfaces(void)
{
    static AsbRwHelper h;
    static AsbRwPayload msg;
    AsbRwHelperOps ops = {NULL, s_send, s_enumerate, s_facts, s_probe, f_log, s_scan, s_capture};
    uint32_t caps = ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | ASB_RW_CAP_FRAMES_CPU |
                    ASB_RW_CAP_POPUPS;
    model_reset();
    memset((void *)g_class, 0, sizeof(g_class));
    g_cap_calls = 0;
    share_new(OWNER, 0x7777);
    HWND a = add_window(0, 1500, 100, 100, 600, 400, SW_SHOWNORMAL, WS_EX_APPWINDOW);
    sent_clear();
    CHECK(asb_rw_helper_open(&h, &ops, &g_share, caps, 1, 0xFACE) == ASB_RW_OK);
    asb_rw_hello_init(&msg.hello, ASB_RW_ROLE_HOST, ASB_RW_CAP_KNOWN_MASK, 0x10000, 0, 0);
    host_send(&h, ASB_RW_HELLO, 1, &msg, ASB_RW_OK);
    host_send(&h, ASB_RW_INVENTORY_REQUEST, 2, &msg, ASB_RW_OK);
    uint64_t ida = entry_id(&h, a);
    CHECK(ida);
    sent_clear();
    /* Nothing shared: no surfaces, no capture. */
    asb_rw_helper_tick(&h, 0);
    CHECK(count_sent(ASB_RW_SURFACES) == 0 && g_cap_calls == 0);
    msg.select.id = ida; msg.select.select = 1;
    host_send(&h, ASB_RW_SELECT_REQUEST, 3, &msg, ASB_RW_OK);
    expect_parked(a);
    /* Select acknowledges, then SURFACES lists the parked root alone. */
    int s = last_surfaces();
    CHECK(s > find_result(ida, ASB_RW_STATE_SHARED, ASB_RW_OK));
    decode_sent(s);
    CHECK(g_rx.surfaces.root_id == ida && g_rx.surfaces.count == 1 && g_rx.surfaces.surfaces[0].surface_id == ida &&
          g_rx.surfaces.surfaces[0].kind == ASB_RW_SURFACE_ROOT && g_rx.surfaces.surfaces[0].z == 0);
    CHECK(g_cap_root == ida && g_cap_n == 1 && g_cap[0].hwnd == (uint64_t)(ULONG_PTR)a && g_cap[0].root_id == ida);
    RECT parked = g_w[0].rect;
    sent_clear();

    /* Popups appear: an owned tooltip overflowing the root, an unowned same-process popup on the
       private display (XAML bridge), an owned menu above it; NOT an independent same-process
       captioned window, another process's popup, or a shadow. */
    add_popup(1, 1500, a, parked.right - 50, parked.top + 40, 300, 60, "tooltips_class32");
    add_popup(2, 1500, NULL, parked.left + 100, parked.bottom - 20, 260, 180, "Microsoft.UI.Content.PopupWindowSiteBridge");
    add_popup(3, 1500, a, parked.right - 80, parked.top + 120, 280, 400, "#32768");
    add_window(4, 1500, PRIV.left + 50, 50, 400, 300, SW_SHOWNORMAL, WS_EX_APPWINDOW);   /* independent root */
    add_popup(5, 2600, NULL, parked.left + 10, parked.top + 10, 100, 100, "Popup");      /* other process */
    add_popup(6, 1500, a, parked.left - 8, parked.top - 8, 700, 500, "SysShadow");
    asb_rw_helper_tick(&h, 0);
    s = last_surfaces();
    CHECK(s >= 0);
    decode_sent(s);
    AsbRwSurfaces got = g_rx.surfaces;
    CHECK(got.root_id == ida && got.count == 4);
    CHECK(got.surfaces[0].surface_id == ida && got.surfaces[0].kind == ASB_RW_SURFACE_ROOT);
    /* bottom-to-top: tooltip (index 1), bridge (2), menu (3) */
    CHECK(got.surfaces[1].kind == ASB_RW_SURFACE_TOOLTIP && got.surfaces[2].kind == ASB_RW_SURFACE_POPUP &&
          got.surfaces[3].kind == ASB_RW_SURFACE_MENU);
    for (uint32_t i = 0; i < got.count; ++i) CHECK(got.surfaces[i].z == i);
    CHECK(got.surfaces[1].rect.right > got.root_rect.right);              /* outside the root */
    uint64_t tip = got.surfaces[1].surface_id, bridge = got.surfaces[2].surface_id, menu = got.surfaces[3].surface_id;
    CHECK(tip != ida && bridge != ida && menu != ida && tip != bridge && bridge != menu);
    CHECK(asb_rw_id_session_tag(tip) == 0xFACE && asb_rw_id_session_tag(menu) == 0xFACE);
    AsbRwIdentity live;
    CHECK(asb_rw_ids_resolve(&h.ids, tip, s_probe, NULL, &live) == ASB_RW_OK && live.hwnd == (uint64_t)(ULONG_PTR)H(1));
    CHECK(g_cap_n == 4 && g_cap[3].hwnd == (uint64_t)(ULONG_PTR)H(3) && g_cap[3].kind == ASB_RW_SURFACE_MENU &&
          g_cap[3].z == 3 && g_cap[1].surface_id == tip);
    sent_clear();
    /* Unchanged: no message, no capture request. */
    uint32_t calls = g_cap_calls;
    asb_rw_helper_tick(&h, 0);
    CHECK(count_sent(ASB_RW_SURFACES) == 0 && g_cap_calls == calls);
    /* A popup moves: resent with the same IDs. */
    OffsetRect(&g_w[3].rect, 0, 30);
    asb_rw_helper_tick(&h, 0);
    s = last_surfaces();
    CHECK(s >= 0);
    decode_sent(s);
    CHECK(g_rx.surfaces.count == 4 && g_rx.surfaces.surfaces[3].surface_id == menu &&
          g_rx.surfaces.surfaces[3].rect.top == got.surfaces[3].rect.top + 30);
    sent_clear();
    /* The menu closes: dropped from the set and its ID released (stale from now on). */
    g_w[3].exists = FALSE;
    asb_rw_helper_tick(&h, 0);
    decode_sent(last_surfaces());
    CHECK(g_rx.surfaces.count == 3 && g_cap_n == 3);
    CHECK(asb_rw_ids_resolve(&h.ids, menu, s_probe, NULL, &live) == ASB_RW_ERR_STALE_ID);
    sent_clear();
    /* Unselect: SURFACES with count 0 and an empty capture set; popup IDs released. */
    msg.select.id = ida; msg.select.select = 0;
    host_send(&h, ASB_RW_SELECT_REQUEST, 4, &msg, ASB_RW_OK);
    s = last_surfaces();
    CHECK(s >= 0);
    decode_sent(s);
    CHECK(g_rx.surfaces.root_id == ida && g_rx.surfaces.count == 0);
    CHECK(g_cap_root == ida && g_cap_n == 0);
    CHECK(asb_rw_ids_resolve(&h.ids, tip, s_probe, NULL, &live) == ASB_RW_ERR_STALE_ID);
    sent_clear();
    /* Closing the session ends capture for every root still shared. */
    msg.select.id = ida; msg.select.select = 1;
    host_send(&h, ASB_RW_SELECT_REQUEST, 5, &msg, ASB_RW_OK);
    CHECK(g_cap_root == ida && g_cap_n >= 1);
    asb_rw_helper_close(&h);
    CHECK(g_cap_root == ida && g_cap_n == 0 && g_share.count == 0);
    sent_clear();
    memset((void *)g_class, 0, sizeof(g_class));
    puts("PASS helper surfaces: parked root + owned/unowned popups bottom-to-top, outside-root rects, stable IDs, "
         "change-only updates, popup close releases IDs, unselect/close end capture");
}

int main(void)
{
    test_journal_codec();
    test_select_unselect_states();
    test_rollback();
    test_owned_and_independent();
    test_concurrent_styles();
    test_destroyed_and_reused();
    test_hung();
    test_recovery();
    test_display_drift_and_limits();
    test_helper_session();
    test_helper_surfaces();
    puts("PASS remote windows parking, restoration journal and helper session");
    return 0;
}
