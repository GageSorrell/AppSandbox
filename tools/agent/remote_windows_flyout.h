/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_flyout
 *
 * @file      remote_windows_flyout.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows window-selection flyout (AppSandbox-eh6.8): ONE native Win32 implementation
 * shared by the host app (tray left-click) and the guest helper (its own tray icon).
 *
 * The flyout shows the guest's live inventory as a list of application windows, each with an
 * icon, a title (duplicates distinguished by application name, then by number), a toggle and a
 * status. Toggles only REQUEST a change: the row shows "Requesting..." until the owner's model
 * (acknowledged selection state) changes; rejected requests show the error. Rows of selected
 * windows stay listed as long as the window lives. Status states cover connecting, not
 * connected, upgrade required, no / several Windows guests, empty inventory and errors.
 *
 * Presentation: a borderless popup anchored to the tray icon (PowerToys-style: centred on the
 * icon, on the taskbar's side, clamped to the work area, a gap from the taskbar), DWM rounded
 * corners, the system light/dark theme (live), per-monitor DPI (fonts, rows, toggles), a
 * scrolling list with keyboard navigation (arrows/Home/End/PgUp/PgDn, Space/Enter toggles, Esc
 * closes) and standard accessibility: the list is a Win32 ListView with checkbox state images,
 * so MSAA/UI Automation expose each window as a checkable list item named by its title.
 * The flyout closes when it loses activation.
 *
 * Threading: every call happens on the thread that created the flyout (it owns the windows).
 */
#ifndef REMOTE_WINDOWS_FLYOUT_H
#define REMOTE_WINDOWS_FLYOUT_H

#include <windows.h>
#include <stdint.h>
#include "../transport/asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_FLYOUT_MAX_ITEMS ASB_RW_MAX_INVENTORY
#define ASB_RW_FLYOUT_LABEL     (ASB_RW_MAX_TITLE + ASB_RW_MAX_APP_NAME + 16)

/* Overall state shown by the flyout. */
#define ASB_RW_FLYOUT_READY        0u   /* list (empty -> "no windows" text) */
#define ASB_RW_FLYOUT_CONNECTING   1u
#define ASB_RW_FLYOUT_DISCONNECTED 2u
#define ASB_RW_FLYOUT_UPGRADE      3u   /* guest components too old; list shown disabled */
#define ASB_RW_FLYOUT_NO_GUEST     4u   /* host: no Windows guest running */
#define ASB_RW_FLYOUT_MULTIPLE     5u   /* host: more than one Windows guest running */
#define ASB_RW_FLYOUT_ERROR        6u

typedef struct AsbRwFlyoutItem {
    uint64_t id;
    uint32_t state;                     /* acknowledged ASB_RW_STATE_* */
    uint32_t error;                     /* AsbRwError of the last transition (0 = none) */
    uint32_t pending;                   /* the owner has a request in flight for this row */
    uint32_t minimized;
    wchar_t label[ASB_RW_FLYOUT_LABEL];
    HICON icon;                         /* borrowed (the flyout copies it); NULL = generic */
} AsbRwFlyoutItem;

typedef struct AsbRwFlyoutModel {
    wchar_t heading[160];               /* e.g. "Windows on Main" */
    uint32_t status;                    /* ASB_RW_FLYOUT_* */
    wchar_t message[320];               /* detail for non-READY states; READY: the hint line */
    uint32_t count;
    AsbRwFlyoutItem items[ASB_RW_FLYOUT_MAX_ITEMS];
} AsbRwFlyoutModel;

/* ---- pure helpers (tested without windows) -------------------------------------------------- */

/* Rows from inventory entries plus acknowledged state/error per entry (NULL = none): labels
   use asb_rw_entry_label, so duplicate titles stay distinguishable. Icons are left NULL. */
void asb_rw_flyout_fill(AsbRwFlyoutModel *m, const AsbRwInventoryEntry *entries, const uint32_t *states,
                        const uint32_t *errors, uint32_t count);
/* Status column text for a row: "", "Requesting...", "Sharing...", "Shared", "Restoring...",
   or "Failed: <reason>" (requested = toggled in this flyout, not yet acknowledged). */
void asb_rw_flyout_row_status(uint32_t state, uint32_t error, int requested, wchar_t *out, size_t cap);
/* Toggle position of a row: 1 on, 0 off. */
int asb_rw_flyout_row_on(uint32_t state);
/* The text shown in place of / under the list. */
void asb_rw_flyout_status_text(const AsbRwFlyoutModel *m, wchar_t *out, size_t cap);
/* Whether rows can be toggled at all in this state. */
int asb_rw_flyout_interactive(uint32_t status);

/* Placement: `anchor` is the tray icon rectangle (screen), `work`/`monitor` the monitor's work
   area and bounds. The flyout (w x h) goes on the taskbar's side of the work area, centred on
   the icon and clamped inside the work area, `gap` pixels from the edge. An anchor inside the
   work area (overflow area, no taskbar edge) opens above it, or below when there is no room. */
void asb_rw_flyout_place(const RECT *anchor, const RECT *work, const RECT *monitor, int w, int h, int gap, RECT *out);

/* Tray click sequences. Single left clicks are deferred by the double-click time so a double
   click only runs its own action; the second button-up of a double click and the click that
   dismissed an open flyout do nothing. */
#define ASB_RW_TRAY_NONE   0u
#define ASB_RW_TRAY_ARM    1u   /* start a one-shot timer of dbl_ms; then call asb_rw_tray_timer */
#define ASB_RW_TRAY_OPEN   2u   /* single left click: toggle the flyout */
#define ASB_RW_TRAY_DOUBLE 3u   /* double click */
#define ASB_RW_TRAY_MENU   4u   /* right click: context menu */
typedef struct AsbRwTrayClicks {
    int armed;
    uint64_t armed_at, double_at, closed_at;
} AsbRwTrayClicks;
uint32_t asb_rw_tray_event(AsbRwTrayClicks *c, UINT mouse_msg, uint64_t now_ms, uint32_t dbl_ms);
uint32_t asb_rw_tray_timer(AsbRwTrayClicks *c, uint64_t now_ms);
void asb_rw_tray_closed(AsbRwTrayClicks *c, uint64_t now_ms);   /* flyout closed by deactivation */

/* Premultiplied BGRA icon (protocol) -> HICON (NULL when empty). */
HICON asb_rw_icon_to_hicon(const AsbRwWindowIcon *icon);

/* ---- tray icon (Explorer restarts recreate it) ---------------------------------------------- */

typedef struct AsbRwTrayIcon {
    HWND hwnd;
    UINT id, callback;
    HICON icon;
    wchar_t tip[128];
    UINT taskbar_created;               /* RegisterWindowMessage(L"TaskbarCreated") */
    int added;
    uint32_t adds;                      /* successful NIM_ADDs (diagnostics/tests) */
} AsbRwTrayIcon;
BOOL asb_rw_tray_add(AsbRwTrayIcon *t);
void asb_rw_tray_remove(AsbRwTrayIcon *t);
/* Call for every message of t->hwnd: re-adds the icon on TaskbarCreated (returns TRUE then). */
BOOL asb_rw_tray_message(AsbRwTrayIcon *t, UINT msg);
BOOL asb_rw_tray_rect(const AsbRwTrayIcon *t, RECT *out);

/* ---- the flyout window ------------------------------------------------------------------------ */

typedef struct AsbRwFlyout AsbRwFlyout;

#define ASB_RW_THEME_SYSTEM 0
#define ASB_RW_THEME_LIGHT  1
#define ASB_RW_THEME_DARK   2

typedef struct AsbRwFlyoutConfig {
    void *ctx;
    void (*toggle)(void *ctx, uint64_t id, int select);   /* request only; the model answers */
    void (*closed)(void *ctx, int deactivated);
    int theme;                          /* ASB_RW_THEME_* (tests force one) */
    uint32_t dpi;                       /* tests: force a DPI; 0 = the monitor's */
} AsbRwFlyoutConfig;

AsbRwFlyout *asb_rw_flyout_create(const AsbRwFlyoutConfig *config);
void asb_rw_flyout_destroy(AsbRwFlyout *f);
/* Replaces the content, keeping focus/scroll on the same rows (by id). */
void asb_rw_flyout_update(AsbRwFlyout *f, const AsbRwFlyoutModel *m);
/* Shows anchored to `anchor` (tray icon rect; NULL = the cursor) and takes the focus. */
void asb_rw_flyout_show(AsbRwFlyout *f, const RECT *anchor);
void asb_rw_flyout_hide(AsbRwFlyout *f);
int asb_rw_flyout_visible(const AsbRwFlyout *f);

typedef struct AsbRwFlyoutInfo {
    HWND hwnd, list, heading, status;
    uint32_t dpi, dark, rows, row_height, visible_rows;
    uint32_t requested;                 /* rows waiting for acknowledgement */
} AsbRwFlyoutInfo;
void asb_rw_flyout_info(const AsbRwFlyout *f, AsbRwFlyoutInfo *out);

#ifdef __cplusplus
}
#endif
#endif
