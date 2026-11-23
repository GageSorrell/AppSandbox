/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_tray
 *
 * @file      remote_windows_tray.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Guest tray icon of the Remote Windows helper (AppSandbox-eh6.8).
 *
 * Runs on its own UI thread (the control thread must never wait on UI). Left click (or double
 * click) opens the shared selection flyout (remote_windows_flyout.c) with the model the control
 * thread publishes; right click offers "Restore all windows" and "Exit". Every user action is
 * handed to the control thread through `post`, which performs it (the toggle becomes a guest
 * selector request to the host; Exit restores every window before the helper exits). The icon
 * is re-added when Explorer restarts and retried until the taskbar exists.
 */
#ifndef REMOTE_WINDOWS_TRAY_H
#define REMOTE_WINDOWS_TRAY_H

#include <windows.h>
#include <stdint.h>
#include "remote_windows_flyout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_ACTION_SELECT      1u
#define ASB_RW_ACTION_RESTORE_ALL 2u
#define ASB_RW_ACTION_EXIT        3u

typedef struct AsbRwGuestTray AsbRwGuestTray;

typedef struct AsbRwGuestTrayConfig {
    void *ctx;
    void (*post)(void *ctx, uint32_t action, uint64_t id, int select);   /* tray thread */
    HICON icon;                     /* NULL: the executable's first icon */
    int theme;                      /* ASB_RW_THEME_* (tests) */
} AsbRwGuestTrayConfig;

AsbRwGuestTray *asb_rw_guest_tray_start(const AsbRwGuestTrayConfig *config);
/* Any thread: copies the model (hwnds[i] = the row's window, for its icon; may be NULL). */
void asb_rw_guest_tray_publish(AsbRwGuestTray *t, const AsbRwFlyoutModel *m, const uint64_t *hwnds);
void asb_rw_guest_tray_stop(AsbRwGuestTray *t);

/* Tests: the tray window and flyout (tray thread objects; read-only use). */
HWND asb_rw_guest_tray_window(AsbRwGuestTray *t);
AsbRwFlyout *asb_rw_guest_tray_flyout(AsbRwGuestTray *t);
uint32_t asb_rw_guest_tray_adds(AsbRwGuestTray *t);

#ifdef __cplusplus
}
#endif
#endif
