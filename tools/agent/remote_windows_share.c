/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_share
 *
 * @file      remote_windows_share.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Guest window parking, restoration and the recovery journal (AppSandbox-eh6.4).
   See remote_windows_share.h for the transaction and restoration rules. */
#include "remote_windows_share.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ================================ journal codec =========================== */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }
static void put_rect(uint8_t *p, const RECT *r)
{
    put32(p, (uint32_t)r->left); put32(p + 4, (uint32_t)r->top);
    put32(p + 8, (uint32_t)r->right); put32(p + 12, (uint32_t)r->bottom);
}
static void get_rect(const uint8_t *p, RECT *r)
{
    r->left = (LONG)get32(p); r->top = (LONG)get32(p + 4);
    r->right = (LONG)get32(p + 8); r->bottom = (LONG)get32(p + 12);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* CRC over the whole journal with its CRC field (offset 36) taken as zero. */
static uint32_t journal_crc(const uint8_t *bytes, size_t len)
{
    static const uint8_t zero[4] = {0};
    uint32_t crc = crc32_update(0, bytes, 36);
    crc = crc32_update(crc, zero, 4);
    return crc32_update(crc, bytes + ASB_RW_JOURNAL_HEADER, len - ASB_RW_JOURNAL_HEADER);
}

size_t asb_rw_journal_encode(const AsbRwJournal *journal, uint8_t *out, size_t cap)
{
    if (!journal || journal->count > ASB_RW_SHARE_MAX) return 0;
    size_t len = ASB_RW_JOURNAL_HEADER + (size_t)journal->count * ASB_RW_JOURNAL_ENTRY;
    if (!out || cap < len) return 0;
    memset(out, 0, len);
    put32(out + 0, ASB_RW_JOURNAL_MAGIC);
    put32(out + 4, ASB_RW_JOURNAL_VERSION);
    put32(out + 8, (uint32_t)len);
    put32(out + 12, journal->count);
    put64(out + 16, journal->lease);
    put32(out + 24, journal->display_active ? 1u : 0u);
    put32(out + 28, journal->width);
    put32(out + 32, journal->height);
    for (uint32_t i = 0; i < journal->count; ++i) {
        const AsbRwJournalEntry *e = &journal->entries[i];
        uint8_t *p = out + ASB_RW_JOURNAL_HEADER + (size_t)i * ASB_RW_JOURNAL_ENTRY;
        put64(p + 0, e->identity.hwnd);
        put32(p + 8, e->identity.pid);
        put32(p + 12, e->identity.thread_id);
        put64(p + 16, e->identity.process_start);
        put32(p + 24, e->state);
        put32(p + 28, e->changes);
        put32(p + 32, e->show_cmd);
        put32(p + 36, e->placement_flags);
        put32(p + 40, (uint32_t)e->min_position.x);
        put32(p + 44, (uint32_t)e->min_position.y);
        put32(p + 48, (uint32_t)e->max_position.x);
        put32(p + 52, (uint32_t)e->max_position.y);
        put_rect(p + 56, &e->normal);
        put_rect(p + 72, &e->frame);
        put_rect(p + 88, &e->parked);
        put32(p + 104, e->ex_style);
        put32(p + 108, e->dpi);
        put64(p + 112, e->original_owner);
        put64(p + 120, e->helper_owner);
    }
    put32(out + 36, journal_crc(out, len));
    return len;
}

uint32_t asb_rw_journal_decode(const uint8_t *bytes, size_t len, AsbRwJournal *journal)
{
    if (!bytes || !journal || len < ASB_RW_JOURNAL_HEADER || len > ASB_RW_JOURNAL_MAX_BYTES)
        return ASB_RW_ERR_MALFORMED;
    if (get32(bytes) != ASB_RW_JOURNAL_MAGIC) return ASB_RW_ERR_MALFORMED;
    if (get32(bytes + 4) != ASB_RW_JOURNAL_VERSION) return ASB_RW_ERR_WIRE_VERSION;
    uint32_t count = get32(bytes + 12);
    if (get32(bytes + 8) != len || count > ASB_RW_SHARE_MAX ||
        len != ASB_RW_JOURNAL_HEADER + (size_t)count * ASB_RW_JOURNAL_ENTRY ||
        get32(bytes + 36) != journal_crc(bytes, len))
        return ASB_RW_ERR_MALFORMED;
    memset(journal, 0, sizeof(*journal));
    journal->lease = get64(bytes + 16);
    journal->display_active = get32(bytes + 24);
    journal->width = get32(bytes + 28);
    journal->height = get32(bytes + 32);
    journal->count = count;
    if (journal->display_active > 1u || (journal->display_active && !journal->lease)) return ASB_RW_ERR_MALFORMED;
    for (uint32_t i = 0; i < count; ++i) {
        AsbRwJournalEntry *e = &journal->entries[i];
        const uint8_t *p = bytes + ASB_RW_JOURNAL_HEADER + (size_t)i * ASB_RW_JOURNAL_ENTRY;
        e->identity.hwnd = get64(p + 0);
        e->identity.pid = get32(p + 8);
        e->identity.thread_id = get32(p + 12);
        e->identity.process_start = get64(p + 16);
        e->state = get32(p + 24);
        e->changes = get32(p + 28);
        e->show_cmd = get32(p + 32);
        e->placement_flags = get32(p + 36);
        e->min_position.x = (LONG)get32(p + 40);
        e->min_position.y = (LONG)get32(p + 44);
        e->max_position.x = (LONG)get32(p + 48);
        e->max_position.y = (LONG)get32(p + 52);
        get_rect(p + 56, &e->normal);
        get_rect(p + 72, &e->frame);
        get_rect(p + 88, &e->parked);
        e->ex_style = get32(p + 104);
        e->dpi = get32(p + 108);
        e->original_owner = get64(p + 112);
        e->helper_owner = get64(p + 120);
        if (!e->identity.hwnd || !e->identity.pid || e->state < ASB_RW_ENTRY_PARKING ||
            e->state > ASB_RW_ENTRY_RESTORING || (e->changes & ~ASB_RW_CHANGE_MASK))
            return ASB_RW_ERR_MALFORMED;
    }
    return ASB_RW_OK;
}

/* ================================ helpers ================================= */

#define WINDOW(e) ((HWND)(ULONG_PTR)(e)->identity.hwnd)

static void share_log(AsbRwShare *s, const char *fmt, ...)
{
    char text[512];
    va_list args;
    if (!s->ops.log) return;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    s->ops.log(s->ops.ctx, text);
}

static BOOL persist(AsbRwShare *s)
{
    static AsbRwJournal journal;   /* single control thread */
    static uint8_t bytes[ASB_RW_JOURNAL_MAX_BYTES];
    if (!s->display_active && !s->count) return s->ops.journal_write(s->ops.ctx, NULL, 0);
    memset(&journal, 0, sizeof(journal));
    journal.lease = s->lease;
    journal.display_active = s->display_active ? 1u : 0u;
    journal.width = s->width;
    journal.height = s->height;
    journal.count = s->count;
    for (uint32_t i = 0; i < s->count; ++i) journal.entries[i] = s->shared[i].j;
    size_t len = asb_rw_journal_encode(&journal, bytes, sizeof(bytes));
    return len && s->ops.journal_write(s->ops.ctx, bytes, len);
}

static int live_matches(AsbRwShare *s, const AsbRwIdentity *id)
{
    AsbRwIdentity live;
    memset(&live, 0, sizeof(live));
    if (!id->hwnd || !s->ops.probe(s->ops.ctx, (HWND)(ULONG_PTR)id->hwnd, &live)) return 0;
    return live.hwnd == id->hwnd && live.pid == id->pid && live.thread_id == id->thread_id &&
           live.process_start == id->process_start;
}

static BOOL center_in(const RECT *r, const RECT *area)
{
    LONG x = r->left + (r->right - r->left) / 2, y = r->top + (r->bottom - r->top) / 2;
    return !IsRectEmpty(area) && x >= area->left && x < area->right && y >= area->top && y < area->bottom;
}

static void clamp_into(const RECT *area, LONG w, LONG h, LONG *x, LONG *y)
{
    if (*x + w > area->right) *x = area->right - w;
    if (*y + h > area->bottom) *y = area->bottom - h;
    if (*x < area->left) *x = area->left;
    if (*y < area->top) *y = area->top;
}

/* Same offset from the desktop origin, kept inside `to`. */
static void translate(const RECT *r, const RECT *from, const RECT *to, LONG *x, LONG *y)
{
    *x = to->left + (r->left - from->left);
    *y = to->top + (r->top - from->top);
    clamp_into(to, r->right - r->left, r->bottom - r->top, x, y);
}

static void remove_at(AsbRwShare *s, uint32_t i)
{
    memmove(&s->shared[i], &s->shared[i + 1], (s->count - i - 1) * sizeof(s->shared[0]));
    s->count--;
    memset(&s->shared[s->count], 0, sizeof(s->shared[0]));
}

int asb_rw_share_find(const AsbRwShare *s, HWND hwnd)
{
    for (uint32_t i = 0; i < s->count; ++i)
        if (WINDOW(&s->shared[i].j) == hwnd) return (int)i;
    return -1;
}

void asb_rw_share_init(AsbRwShare *s, const AsbRwShareOps *ops, HWND owner, uint64_t lease,
                       uint32_t width, uint32_t height)
{
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    s->owner = owner;
    s->lease = lease;
    s->width = width;
    s->height = height;
}

/* ================================ display ================================= */

static DWORD sources_restored(void *arg)
{
    AsbRwShare *s = (AsbRwShare *)arg;
    return s->count ? ERROR_BUSY : ERROR_SUCCESS;
}

uint32_t asb_rw_share_release_display(AsbRwShare *s)
{
    if (!s->display_active) return ASB_RW_OK;
    if (s->count) return ASB_RW_ERR_BUSY;   /* never remove the display under unrestored sources */
    DWORD error = s->ops.display_release(s->ops.ctx, s->lease, sources_restored, s);
    if (error == ERROR_ACCESS_DENIED || error == ERROR_NOT_FOUND || error == ERROR_FILE_NOT_FOUND) {
        /* Another lease owns the output (or the driver/device is gone): nothing of ours left. */
        share_log(s, "private display not ours to remove (%lu); forgetting lease", error);
        error = ERROR_SUCCESS;
    }
    if (error) {
        share_log(s, "private display removal failed (%lu); retrying later", error);
        return ASB_RW_ERR_PRIVATE_DISPLAY;
    }
    s->display_active = FALSE;
    SetRectEmpty(&s->private_rect);
    persist(s);
    share_log(s, "private display removed");
    return ASB_RW_OK;
}

static uint32_t ensure_display(AsbRwShare *s, BOOL *activated_here)
{
    DWORD error;
    *activated_here = FALSE;
    if (s->display_active) {
        if (!s->ops.display_refresh(s->ops.ctx, s->width, s->height, &s->desktop, &s->private_rect))
            return ASB_RW_OK;
        error = s->ops.display_activate(s->ops.ctx, s->lease, s->width, s->height, &s->desktop, &s->private_rect);
        if (error) share_log(s, "private display re-placement failed (%lu)", error);
        return error ? ASB_RW_ERR_PRIVATE_DISPLAY : ASB_RW_OK;
    }
    /* Journal the lease before the output exists so recovery can always remove it. */
    s->display_active = TRUE;
    if (!persist(s)) {
        s->display_active = FALSE;
        return ASB_RW_ERR_INTERNAL;
    }
    error = s->ops.display_activate(s->ops.ctx, s->lease, s->width, s->height, &s->desktop, &s->private_rect);
    if (error) {
        share_log(s, "private display activation failed (%lu)", error);
        asb_rw_share_release_display(s);
        return ASB_RW_ERR_PRIVATE_DISPLAY;
    }
    *activated_here = TRUE;
    share_log(s, "private display active at (%ld,%ld)-(%ld,%ld)", s->private_rect.left, s->private_rect.top,
              s->private_rect.right, s->private_rect.bottom);
    return ASB_RW_OK;
}

/* ================================ restoration ============================= */

/* Owned windows left on the private display follow their root back by the same delta. */
static void return_owned(AsbRwShare *s, HWND root, const RECT *before)
{
    HWND owned[ASB_RW_OWNED_MAX];
    RECT after;
    if (IsRectEmpty(&s->private_rect) || IsRectEmpty(&s->desktop) || !s->ops.get_rect(s->ops.ctx, root, &after))
        return;
    uint32_t n = s->ops.owned(s->ops.ctx, root, owned, ASB_RW_OWNED_MAX);
    for (uint32_t i = 0; i < n; ++i) {
        RECT r;
        if (!s->ops.get_rect(s->ops.ctx, owned[i], &r) || !center_in(&r, &s->private_rect)) continue;
        LONG x = r.left + (after.left - before->left), y = r.top + (after.top - before->top);
        clamp_into(&s->desktop, r.right - r.left, r.bottom - r.top, &x, &y);
        s->ops.position(s->ops.ctx, owned[i], x, y, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* Undo the integration-owned changes recorded in e->changes, and nothing else. */
static uint32_t restore_entry(AsbRwShare *s, AsbRwJournalEntry *e)
{
    HWND h = WINDOW(e);
    RECT before;
    if (!live_matches(s, &e->identity)) return ASB_RW_ERR_STALE_ID;
    if (!s->ops.responsive(s->ops.ctx, h)) return ASB_RW_ERR_BUSY;
    SetRectEmpty(&before);
    s->ops.get_rect(s->ops.ctx, h, &before);
    if (e->changes & ASB_RW_CHANGE_OWNER) {
        HWND current = s->ops.get_owner(s->ops.ctx, h);
        /* Reset only our own owner. NULL means the helper died and the system already unlinked it;
           any other owner was set by the application and is left alone. */
        if (current && (current == (HWND)(ULONG_PTR)e->helper_owner || current == s->owner))
            s->ops.set_owner(s->ops.ctx, h, (HWND)(ULONG_PTR)e->original_owner);
    }
    if (e->changes & ASB_RW_CHANGE_APPWINDOW) {
        DWORD ex = s->ops.get_ex_style(s->ops.ctx, h);
        if (!(ex & WS_EX_APPWINDOW)) s->ops.set_ex_style(s->ops.ctx, h, ex | WS_EX_APPWINDOW);
    }
    if (e->changes & (ASB_RW_CHANGE_OWNER | ASB_RW_CHANGE_APPWINDOW))
        s->ops.position(s->ops.ctx, h, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (e->changes & (ASB_RW_CHANGE_SHOW | ASB_RW_CHANGE_MOVED)) {
        WINDOWPLACEMENT wp;
        memset(&wp, 0, sizeof(wp));
        wp.length = sizeof(wp);
        wp.flags = e->placement_flags & (WPF_SETMINPOSITION | WPF_RESTORETOMAXIMIZED);
        wp.showCmd = e->show_cmd == SW_SHOWMINIMIZED ? SW_SHOWMINNOACTIVE :
                     e->show_cmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE;
        wp.ptMinPosition = e->min_position;
        wp.ptMaxPosition = e->max_position;
        wp.rcNormalPosition = e->normal;
        /* Mixed DPI: the first placement crosses back to the original monitor, where the window
           rescales (WM_DPICHANGED); the second re-applies the journaled rectangle at that DPI. */
        s->ops.set_placement(s->ops.ctx, h, &wp);
        s->ops.set_placement(s->ops.ctx, h, &wp);
    }
    if (e->changes & ASB_RW_CHANGE_MOVED) return_owned(s, h, &before);
    if (e->changes & ASB_RW_CHANGE_TAB) s->ops.taskbar(s->ops.ctx, h, TRUE);
    if (e->changes & ASB_RW_CHANGE_PROPERTY) s->ops.mark(s->ops.ctx, h, FALSE);
    return ASB_RW_OK;
}

/* Restores entry i; removes it unless restoration is pending. Returns restore_entry's result. */
static uint32_t restore_at(AsbRwShare *s, uint32_t i)
{
    AsbRwJournalEntry *e = &s->shared[i].j;
    uint32_t result = restore_entry(s, e);
    if (result == ASB_RW_ERR_BUSY) {
        e->state = ASB_RW_ENTRY_RESTORING;
        share_log(s, "hwnd=%p pid=%u not responding; restoration pending", (void *)WINDOW(e), e->identity.pid);
    } else {
        share_log(s, result == ASB_RW_OK ? "hwnd=%p pid=%u restored" : "hwnd=%p pid=%u gone or reused; dropped",
                  (void *)WINDOW(e), e->identity.pid);
        remove_at(s, i);
    }
    return result;
}

/* ================================ parking ================================= */

static void park_owned(AsbRwShare *s, HWND root)
{
    HWND owned[ASB_RW_OWNED_MAX];
    uint32_t n = s->ops.owned(s->ops.ctx, root, owned, ASB_RW_OWNED_MAX);
    for (uint32_t i = 0; i < n; ++i) {
        RECT r;
        LONG x, y;
        if (!s->ops.get_rect(s->ops.ctx, owned[i], &r) || center_in(&r, &s->private_rect)) continue;
        translate(&r, &s->desktop, &s->private_rect, &x, &y);
        s->ops.position(s->ops.ctx, owned[i], x, y, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* Each change is recorded in *applied before it is attempted; restore_entry's guards make
   undoing an unapplied change a no-op. */
static uint32_t park(AsbRwShare *s, AsbRwJournalEntry *e, uint32_t *applied)
{
    HWND h = WINDOW(e);
    WINDOWPLACEMENT wp;
    RECT r;
    LONG x, y;
    *applied |= ASB_RW_CHANGE_PROPERTY;
    if (!s->ops.mark(s->ops.ctx, h, TRUE)) return ASB_RW_ERR_INTERNAL;
    if (e->changes & ASB_RW_CHANGE_SHOW) {
        /* Minimized windows do not render; restore without activation (never SW_RESTORE). */
        *applied |= ASB_RW_CHANGE_SHOW;
        for (int attempt = 0; attempt < 2; ++attempt) {
            s->ops.show(s->ops.ctx, h, SW_SHOWNOACTIVATE);
            memset(&wp, 0, sizeof(wp));
            wp.length = sizeof(wp);
            if (!s->ops.get_placement(s->ops.ctx, h, &wp)) return ASB_RW_ERR_INTERNAL;
            if (wp.showCmd == SW_SHOWNORMAL) break;
        }
        if (wp.showCmd != SW_SHOWNORMAL) return ASB_RW_ERR_INTERNAL;
    }
    if (!s->ops.get_rect(s->ops.ctx, h, &r)) return ASB_RW_ERR_STALE_ID;
    translate(&r, &s->desktop, &s->private_rect, &x, &y);
    *applied |= ASB_RW_CHANGE_MOVED;
    if (!s->ops.position(s->ops.ctx, h, x, y, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) return ASB_RW_ERR_INTERNAL;
    /* A per-monitor-aware window may rescale on the new DPI; only its center must be parked. */
    if (!s->ops.get_rect(s->ops.ctx, h, &r) || !center_in(&r, &s->private_rect)) return ASB_RW_ERR_INTERNAL;
    if (e->changes & ASB_RW_CHANGE_APPWINDOW) {
        *applied |= ASB_RW_CHANGE_APPWINDOW;
        DWORD ex = s->ops.get_ex_style(s->ops.ctx, h);
        if (!s->ops.set_ex_style(s->ops.ctx, h, ex & ~(DWORD)WS_EX_APPWINDOW)) return ASB_RW_ERR_INTERNAL;
    }
    *applied |= ASB_RW_CHANGE_OWNER;
    if (!s->ops.set_owner(s->ops.ctx, h, s->owner) || s->ops.get_owner(s->ops.ctx, h) != s->owner)
        return ASB_RW_ERR_INTERNAL;
    s->ops.position(s->ops.ctx, h, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    /* Best effort: an owned window without WS_EX_APPWINDOW has no taskbar button anyway. */
    if (s->ops.taskbar(s->ops.ctx, h, FALSE)) *applied |= ASB_RW_CHANGE_TAB;
    return ASB_RW_OK;
}

uint32_t asb_rw_share_select(AsbRwShare *s, const AsbRwIdentity *identity, uint64_t wire_id)
{
    HWND h = identity ? (HWND)(ULONG_PTR)identity->hwnd : NULL;
    WINDOWPLACEMENT wp;
    BOOL activated_here = FALSE;
    if (!h || !s->owner) return ASB_RW_ERR_NOT_ELIGIBLE;
    int existing = asb_rw_share_find(s, h);
    if (existing >= 0) {
        AsbRwShared *x = &s->shared[existing];
        if (x->j.state != ASB_RW_ENTRY_PARKED) return ASB_RW_ERR_BUSY;
        if (!live_matches(s, &x->j.identity)) return ASB_RW_ERR_STALE_ID;
        x->wire_id = wire_id;
        return ASB_RW_OK;   /* already shared */
    }
    if (s->count >= ASB_RW_SHARE_MAX) return ASB_RW_ERR_LIMIT;
    if (!live_matches(s, identity)) return ASB_RW_ERR_STALE_ID;
    if (s->ops.get_owner(s->ops.ctx, h)) return ASB_RW_ERR_NOT_ELIGIBLE;   /* owned windows are surfaces */
    if (!s->ops.responsive(s->ops.ctx, h)) return ASB_RW_ERR_BUSY;
    uint32_t result = ensure_display(s, &activated_here);
    if (result) return result;

    AsbRwShared *x = &s->shared[s->count];
    AsbRwJournalEntry *e = &x->j;
    memset(x, 0, sizeof(*x));
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    x->wire_id = wire_id;
    e->identity = *identity;
    if (!s->ops.get_placement(s->ops.ctx, h, &wp) || !s->ops.get_rect(s->ops.ctx, h, &e->frame)) {
        if (activated_here) asb_rw_share_release_display(s);
        return ASB_RW_ERR_STALE_ID;
    }
    e->show_cmd = wp.showCmd;
    e->placement_flags = wp.flags;
    e->min_position = wp.ptMinPosition;
    e->max_position = wp.ptMaxPosition;
    e->normal = wp.rcNormalPosition;
    e->ex_style = s->ops.get_ex_style(s->ops.ctx, h);
    e->dpi = s->ops.dpi(s->ops.ctx, h);
    e->original_owner = 0;
    e->helper_owner = (uint64_t)(ULONG_PTR)s->owner;
    e->state = ASB_RW_ENTRY_PARKING;
    e->changes = ASB_RW_CHANGE_PROPERTY | ASB_RW_CHANGE_MOVED | ASB_RW_CHANGE_OWNER | ASB_RW_CHANGE_TAB |
                 (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_SHOWMAXIMIZED ? ASB_RW_CHANGE_SHOW : 0) |
                 (e->ex_style & WS_EX_APPWINDOW ? ASB_RW_CHANGE_APPWINDOW : 0);
    s->count++;
    /* Originals are durable before anything is modified. */
    if (!persist(s)) {
        s->count--;
        share_log(s, "journal write failed; hwnd=%p not shared", (void *)h);
        if (activated_here) asb_rw_share_release_display(s);
        return ASB_RW_ERR_INTERNAL;
    }
    uint32_t applied = 0;
    result = park(s, e, &applied);
    if (result) {
        share_log(s, "parking hwnd=%p failed (%s) after changes 0x%x; rolling back", (void *)h,
                  asb_rw_error_name(result), applied);
        e->changes = applied;
        restore_at(s, s->count - 1u);
        persist(s);
        if (activated_here && !s->count) asb_rw_share_release_display(s);
        return result == ASB_RW_ERR_STALE_ID ? result : ASB_RW_ERR_INTERNAL;
    }
    e->changes = applied;
    e->state = ASB_RW_ENTRY_PARKED;
    s->ops.get_rect(s->ops.ctx, h, &e->parked);
    if (!persist(s)) share_log(s, "journal update failed; the PARKING record still allows recovery");
    park_owned(s, h);
    share_log(s, "hwnd=%p pid=%u parked at (%ld,%ld) show=%u ex=0x%x changes=0x%x", (void *)h, e->identity.pid,
              e->parked.left, e->parked.top, e->show_cmd, e->ex_style, e->changes);
    return ASB_RW_OK;
}

uint32_t asb_rw_share_unselect(AsbRwShare *s, HWND hwnd)
{
    int i = asb_rw_share_find(s, hwnd);
    if (i < 0) return ASB_RW_ERR_NOT_ELIGIBLE;
    uint32_t result = restore_at(s, (uint32_t)i);
    persist(s);
    return result;
}

uint32_t asb_rw_share_restore_all(AsbRwShare *s)
{
    for (uint32_t i = s->count; i-- > 0;) restore_at(s, i);
    persist(s);
    if (s->count) return ASB_RW_ERR_BUSY;
    return asb_rw_share_release_display(s);
}

void asb_rw_share_maintain(AsbRwShare *s, uint64_t *gone, uint32_t max_gone, uint32_t *gone_count,
                           uint64_t *restored, uint32_t max_restored, uint32_t *restored_count)
{
    BOOL dirty = FALSE;
    if (gone_count) *gone_count = 0;
    if (restored_count) *restored_count = 0;
    if (s->display_active && s->count &&
        s->ops.display_refresh(s->ops.ctx, s->width, s->height, &s->desktop, &s->private_rect)) {
        /* The layout drifted (e.g. the display watcher closed a gap): re-place and re-verify. */
        DWORD error = s->ops.display_activate(s->ops.ctx, s->lease, s->width, s->height, &s->desktop, &s->private_rect);
        if (error) share_log(s, "private display re-placement failed (%lu)", error);
    }
    for (uint32_t i = s->count; i-- > 0;) {
        AsbRwShared *x = &s->shared[i];
        AsbRwJournalEntry *e = &x->j;
        HWND h = WINDOW(e);
        if (!live_matches(s, &e->identity)) {
            share_log(s, "hwnd=%p pid=%u destroyed or reused; dropped", (void *)h, e->identity.pid);
            if (gone && gone_count && *gone_count < max_gone) gone[(*gone_count)++] = x->wire_id;
            remove_at(s, i);
            dirty = TRUE;
            continue;
        }
        if (e->state == ASB_RW_ENTRY_RESTORING) {
            uint64_t id = x->wire_id;
            if (restore_at(s, i) != ASB_RW_ERR_BUSY) {
                if (restored && restored_count && *restored_count < max_restored) restored[(*restored_count)++] = id;
                dirty = TRUE;
            }
            continue;
        }
        if (e->state != ASB_RW_ENTRY_PARKED || !s->ops.responsive(s->ops.ctx, h)) continue;
        WINDOWPLACEMENT wp;
        RECT r;
        memset(&wp, 0, sizeof(wp));
        wp.length = sizeof(wp);
        if (s->ops.get_placement(s->ops.ctx, h, &wp) && wp.showCmd != SW_SHOWMINIMIZED &&
            s->ops.get_rect(s->ops.ctx, h, &r) && !center_in(&r, &s->private_rect) && !IsRectEmpty(&s->private_rect)) {
            LONG px, py;
            translate(&r, &s->desktop, &s->private_rect, &px, &py);
            s->ops.position(s->ops.ctx, h, px, py, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            share_log(s, "hwnd=%p left the private display; re-parked", (void *)h);
        }
        /* Applications may restore their own shell presence; re-suppress and remember to undo it. */
        DWORD ex = s->ops.get_ex_style(s->ops.ctx, h);
        if (ex & WS_EX_APPWINDOW) {
            s->ops.set_ex_style(s->ops.ctx, h, ex & ~(DWORD)WS_EX_APPWINDOW);
            if (s->ops.taskbar(s->ops.ctx, h, FALSE)) e->changes |= ASB_RW_CHANGE_TAB;
            e->changes |= ASB_RW_CHANGE_APPWINDOW;
            dirty = TRUE;
        }
        if (!s->ops.get_owner(s->ops.ctx, h) && s->ops.set_owner(s->ops.ctx, h, s->owner)) {
            e->changes |= ASB_RW_CHANGE_OWNER;
            e->helper_owner = (uint64_t)(ULONG_PTR)s->owner;
            dirty = TRUE;
        }
        park_owned(s, h);
    }
    if (dirty) persist(s);
}

uint32_t asb_rw_share_recover(AsbRwShare *s, const uint8_t *bytes, size_t len)
{
    static AsbRwJournal journal;
    if (!bytes || !len) return ASB_RW_OK;
    uint32_t result = asb_rw_journal_decode(bytes, len, &journal);
    if (result) {
        share_log(s, "journal unreadable (%s); discarded", asb_rw_error_name(result));
        s->ops.journal_write(s->ops.ctx, NULL, 0);
        return result;
    }
    if (journal.display_active) {
        s->lease = journal.lease;
        s->width = journal.width;
        s->height = journal.height;
        s->display_active = TRUE;
        s->ops.display_refresh(s->ops.ctx, s->width, s->height, &s->desktop, &s->private_rect);
    }
    for (uint32_t i = 0; i < journal.count && s->count < ASB_RW_SHARE_MAX; ++i) {
        AsbRwShared *x = &s->shared[s->count++];
        memset(x, 0, sizeof(*x));
        x->j = journal.entries[i];
        x->j.state = ASB_RW_ENTRY_RESTORING;   /* never re-parked */
    }
    share_log(s, "recovering %u journaled window(s), display %s", journal.count,
              journal.display_active ? "active" : "absent");
    for (uint32_t i = s->count; i-- > 0;) restore_at(s, i);
    persist(s);
    if (!s->count) asb_rw_share_release_display(s);
    return s->count || s->display_active ? ASB_RW_ERR_BUSY : ASB_RW_OK;
}
