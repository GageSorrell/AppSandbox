/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_share
 *
 * @file      remote_windows_share.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows guest window parking and restoration journal (AppSandbox-eh6.4).
 *
 * Runs in the standard-user helper (appsandbox-remote-windows.exe) on one control thread.
 * Every Win32 effect goes through AsbRwShareOps so the transaction, rollback and recovery
 * rules are tested against a window model (tools/tests/remote_windows_share_test.c).
 *
 * Select is transactional:
 *   1. validate the identity (HWND + PID + thread + process start) and responsiveness;
 *   2. lease and verify the private display (lease token journaled BEFORE activation);
 *   3. journal the original placement, show state, extended style and owner, with the set
 *      of changes the helper is about to make, durably and BEFORE modifying anything;
 *   4. mark the root (ASB_PRIVATE_PARKED_PROPERTY), restore a minimized/maximized root to
 *      its normal state without activating it (so it renders for capture), move it onto
 *      the private display, and remove its shell presence: clear WS_EX_APPWINDOW, own it
 *      by the helper's hidden window and delete its taskbar tab (owner-based suppression
 *      keeps the application's frame unchanged, unlike WS_EX_TOOLWINDOW);
 *   5. journal PARKED and acknowledge. Any failing step restores what was done.
 *
 * Restoration (unselect, rollback, disconnect, exit, recovery) touches only the changes the
 * journal records as integration-owned: the owner is reset only while it is still the
 * helper's (or already gone), WS_EX_APPWINDOW is re-added only if the helper cleared it,
 * other style bits keep whatever the application set meanwhile. Owned windows on the
 * private display travel back with their root. Nothing is ever closed. A destroyed or
 * reused handle (identity mismatch) is dropped without touching it. A hung window is not
 * waited on: its restoration stays pending and is retried, and the private display is not
 * removed while any source remains unrestored.
 *
 * The journal is recovery data only. It records originals and the display lease, never a
 * selection to re-establish: recovery restores surviving sources and removes the display.
 */
#ifndef REMOTE_WINDOWS_SHARE_H
#define REMOTE_WINDOWS_SHARE_H

#include <windows.h>
#include "../transport/asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_SHARE_MAX   32u          /* concurrently shared roots */
#define ASB_RW_OWNED_MAX   64u          /* owned windows examined per root and pass */

/* Integration-owned changes recorded per root. */
#define ASB_RW_CHANGE_PROPERTY  0x0001u /* ASB_PRIVATE_PARKED_PROPERTY set on the root */
#define ASB_RW_CHANGE_SHOW      0x0002u /* un-minimized / un-maximized (no activation) for capture */
#define ASB_RW_CHANGE_MOVED     0x0004u /* moved onto the private display */
#define ASB_RW_CHANGE_APPWINDOW 0x0008u /* WS_EX_APPWINDOW cleared */
#define ASB_RW_CHANGE_OWNER     0x0010u /* owned by the helper's hidden window */
#define ASB_RW_CHANGE_TAB       0x0020u /* taskbar tab deleted */
#define ASB_RW_CHANGE_MASK      0x003Fu

/* Entry states. */
#define ASB_RW_ENTRY_PARKING    1u      /* journaled; modifications may be partial */
#define ASB_RW_ENTRY_PARKED     2u
#define ASB_RW_ENTRY_RESTORING  3u      /* restoration pending (hung window or recovery) */

typedef struct AsbRwJournalEntry {
    AsbRwIdentity identity;
    uint32_t state, changes;
    uint32_t show_cmd, placement_flags;  /* original WINDOWPLACEMENT */
    POINT min_position, max_position;
    RECT normal;                         /* rcNormalPosition (workspace coordinates) */
    RECT frame;                          /* original window rectangle (screen) */
    RECT parked;                         /* window rectangle after parking */
    uint32_t ex_style;                   /* original extended style */
    uint32_t dpi;
    uint64_t original_owner;             /* always 0 for eligible roots; kept for validation */
    uint64_t helper_owner;               /* hidden owner the helper installed */
} AsbRwJournalEntry;

typedef struct AsbRwJournal {
    uint64_t lease;                      /* private-display lease token */
    uint32_t display_active;             /* activation requested and not yet removed */
    uint32_t width, height;              /* private mode */
    uint32_t count;
    AsbRwJournalEntry entries[ASB_RW_SHARE_MAX];
} AsbRwJournal;

#define ASB_RW_JOURNAL_MAGIC   0x4A525341u /* "ASRJ" */
#define ASB_RW_JOURNAL_VERSION 1u
#define ASB_RW_JOURNAL_HEADER  40u
#define ASB_RW_JOURNAL_ENTRY   128u
#define ASB_RW_JOURNAL_MAX_BYTES (ASB_RW_JOURNAL_HEADER + ASB_RW_SHARE_MAX * ASB_RW_JOURNAL_ENTRY)

/* Explicit little-endian codec with a CRC-32; truncated, oversized, corrupted or
   wrong-version journals are rejected (ASB_RW_ERR_MALFORMED / ASB_RW_ERR_WIRE_VERSION). */
size_t asb_rw_journal_encode(const AsbRwJournal *journal, uint8_t *out, size_t cap);
uint32_t asb_rw_journal_decode(const uint8_t *bytes, size_t len, AsbRwJournal *journal);

/* Win32 boundary. Window functions take the root or owned HWND. */
typedef struct AsbRwShareOps {
    void *ctx;
    int  (*probe)(void *ctx, HWND hwnd, AsbRwIdentity *live);      /* 0: window gone */
    BOOL (*responsive)(void *ctx, HWND hwnd);                      /* answers within a short timeout */
    BOOL (*get_placement)(void *ctx, HWND hwnd, WINDOWPLACEMENT *wp);
    BOOL (*set_placement)(void *ctx, HWND hwnd, const WINDOWPLACEMENT *wp);
    BOOL (*get_rect)(void *ctx, HWND hwnd, RECT *rect);
    BOOL (*show)(void *ctx, HWND hwnd, int cmd);                   /* ShowWindow */
    BOOL (*position)(void *ctx, HWND hwnd, int x, int y, UINT flags); /* SetWindowPos, size kept */
    UINT (*dpi)(void *ctx, HWND hwnd);
    DWORD (*get_ex_style)(void *ctx, HWND hwnd);
    BOOL (*set_ex_style)(void *ctx, HWND hwnd, DWORD ex_style);
    HWND (*get_owner)(void *ctx, HWND hwnd);
    BOOL (*set_owner)(void *ctx, HWND hwnd, HWND owner);
    BOOL (*mark)(void *ctx, HWND hwnd, BOOL set);                  /* ASB_PRIVATE_PARKED_PROPERTY */
    BOOL (*taskbar)(void *ctx, HWND hwnd, BOOL add);               /* ITaskbarList AddTab/DeleteTab */
    /* Visible captioned windows whose owner chain contains root (owned dialogs). */
    uint32_t (*owned)(void *ctx, HWND root, HWND *out, uint32_t max);
    /* Private display: activate+verify, re-verify, and RESTORE -> callback -> REMOVE. */
    DWORD (*display_activate)(void *ctx, uint64_t lease, uint32_t width, uint32_t height,
                              RECT *desktop, RECT *private_rect);
    DWORD (*display_refresh)(void *ctx, uint32_t width, uint32_t height, RECT *desktop, RECT *private_rect);
    DWORD (*display_release)(void *ctx, uint64_t lease, DWORD (*restore)(void *arg), void *arg);
    /* Durable, atomic replacement of the journal (len 0: nothing to recover). */
    BOOL (*journal_write)(void *ctx, const uint8_t *bytes, size_t len);
    void (*log)(void *ctx, const char *message);
} AsbRwShareOps;

typedef struct AsbRwShared {
    AsbRwJournalEntry j;
    uint64_t wire_id;                    /* current-session ID; 0 for recovered entries */
} AsbRwShared;

typedef struct AsbRwShare {
    AsbRwShareOps ops;
    HWND owner;                          /* helper's hidden owner window */
    uint64_t lease;
    uint32_t width, height;
    BOOL display_active;
    RECT desktop, private_rect;
    uint32_t count;
    AsbRwShared shared[ASB_RW_SHARE_MAX];
} AsbRwShare;

void asb_rw_share_init(AsbRwShare *s, const AsbRwShareOps *ops, HWND owner, uint64_t lease,
                       uint32_t width, uint32_t height);

/* Transactional select (see above). ASB_RW_OK when parked; otherwise nothing remains changed:
   STALE_ID (identity mismatch), NOT_ELIGIBLE, BUSY (not responding), LIMIT, PRIVATE_DISPLAY,
   INTERNAL (journal not durable or a modification failed and was rolled back). */
uint32_t asb_rw_share_select(AsbRwShare *s, const AsbRwIdentity *identity, uint64_t wire_id);

/* Restores one root. ASB_RW_OK: restored; STALE_ID: window gone or reused, dropped untouched;
   BUSY: not responding, restoration pending (retried by asb_rw_share_maintain);
   NOT_ELIGIBLE: not shared. */
uint32_t asb_rw_share_unselect(AsbRwShare *s, HWND hwnd);

/* Restores every root and, once none remains, removes the private display.
   ASB_RW_OK, or BUSY while hung roots keep the display present. */
uint32_t asb_rw_share_restore_all(AsbRwShare *s);

/* Removes the private display if nothing is shared (helper calls this after a short idle). */
uint32_t asb_rw_share_release_display(AsbRwShare *s);

/* Periodic pass: drops destroyed/reused roots (reported in gone[]), retries pending
   restorations (reported in restored[]), re-verifies the private display, keeps parked
   roots and their owned dialogs on it and re-applies shell suppression an application undid. */
void asb_rw_share_maintain(AsbRwShare *s, uint64_t *gone, uint32_t max_gone, uint32_t *gone_count,
                           uint64_t *restored, uint32_t max_restored, uint32_t *restored_count);

/* Index of a shared root, or -1. */
int asb_rw_share_find(const AsbRwShare *s, HWND hwnd);

/* Loads a journal left by a previous helper and restores surviving sources (identities
   validated, never re-parked); entries that cannot be restored yet stay as RESTORING.
   Removes the private display with the journaled lease once nothing remains.
   ASB_RW_OK when nothing is left to recover, BUSY when entries remain, MALFORMED/WIRE_VERSION
   for an unreadable journal (discarded). */
uint32_t asb_rw_share_recover(AsbRwShare *s, const uint8_t *journal, size_t len);

#ifdef __cplusplus
}
#endif
#endif
