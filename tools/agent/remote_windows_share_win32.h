/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_share_win32
 *
 * @file      remote_windows_share_win32.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Real Win32 implementation of AsbRwShareOps for the standard-user helper (AppSandbox-eh6.4):
   cross-process window state, ITaskbarList, the VDD private-display lease and the durable
   journal file. Call from one STA (COM) thread. */
#ifndef REMOTE_WINDOWS_SHARE_WIN32_H
#define REMOTE_WINDOWS_SHARE_WIN32_H

#include "remote_windows_share.h"
#include "remote_windows_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwWin32 {
    AsbRwDisplay display;
    BOOL display_open;
    void *taskbar;                      /* ITaskbarList* */
    WCHAR journal_path[MAX_PATH];
    void (*log)(const char *message);
    /* Native tests on a machine without the VDD: fixed desktop/private rectangles. */
    BOOL fake_display;
    RECT fake_desktop, fake_private;
} AsbRwWin32;

/* %LOCALAPPDATA%\AppSandbox\RemoteWindows (created). */
BOOL asb_rw_state_dir(WCHAR *path, DWORD capacity);
void asb_rw_win32_init(AsbRwWin32 *w, const WCHAR *journal_path, void (*log)(const char *message));
void asb_rw_win32_ops(AsbRwWin32 *w, AsbRwShareOps *ops);
void asb_rw_win32_close(AsbRwWin32 *w);
/* Whole journal file (NULL/0 when absent). Caller frees *bytes. */
BOOL asb_rw_journal_read(const WCHAR *path, uint8_t **bytes, size_t *len);
/* Hidden, never-shown owner window class used for owner-based shell suppression. */
#define ASB_RW_OWNER_CLASS L"AppSandbox.RemoteWindows.Owner"

#ifdef __cplusplus
}
#endif
#endif
