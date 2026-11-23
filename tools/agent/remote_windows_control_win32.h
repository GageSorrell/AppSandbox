/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_control_win32
 *
 * @file      remote_windows_control_win32.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Win32 window control for host proxies (AppSandbox-eh6.6), used by the guest helper's
 * AsbRwHelperOps: visible-frame geometry, in-place resize/move without activation, SC_CLOSE,
 * the application's own WM_NCHITTEST, window state/tracking limits and icons; eh6.7 foreground
 * queries and activation for host input.
 *
 * Every call that sends a message to another process uses SendMessageTimeout with
 * SMTO_ABORTIFHUNG, so a hung application never blocks the helper's control thread for long.
 * WM_GETMINMAXINFO and WM_GETICON are marshalled across processes by the system.
 */
#ifndef REMOTE_WINDOWS_CONTROL_WIN32_H
#define REMOTE_WINDOWS_CONTROL_WIN32_H

#include <windows.h>
#include "remote_windows_helper.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Window rectangle and visible frame (DWMWA_EXTENDED_FRAME_BOUNDS, else the window rectangle). */
BOOL asb_rw_ctl_frames(HWND hwnd, RECT *window, RECT *visible);
/* SetWindowPos to `window` (size only when `size`), never activating or changing z order. */
BOOL asb_rw_ctl_set_rect(HWND hwnd, const RECT *window, BOOL size);
/* Answers WM_NULL within 500 ms and is not flagged hung. */
BOOL asb_rw_ctl_responsive(HWND hwnd);
/* Posts WM_SYSCOMMAND SC_CLOSE (the application's normal close path, prompts included). */
BOOL asb_rw_ctl_post_close(HWND hwnd);
/* WM_NCHITTEST at a screen point: deepest visible child first, HTTRANSPARENT falls through to
   the parent. Returns the HT* code, HTNOWHERE (0) on timeout or for negative codes. */
uint32_t asb_rw_ctl_hit_test(HWND root, int x, int y);
/* Style flags, DPI and placement show command; with `limits`, also WM_GETMINMAXINFO tracking
   limits converted to visible-frame sizes (max 0 when not below the system default). */
BOOL asb_rw_ctl_window_state(HWND hwnd, BOOL limits, AsbRwWindowState *state, UINT *show_cmd);
/* SW_SHOWNOACTIVATE: undo an application minimize/maximize without activating. */
BOOL asb_rw_ctl_normalize(HWND hwnd);
/* Icon key (WM_GETICON / class icon handle, 1 for the executable's icon) and, with `icon`,
   the icon rendered as premultiplied BGRA at its own size (16..ASB_RW_MAX_ICON). */
BOOL asb_rw_ctl_icon(HWND hwnd, uint64_t *key, AsbRwWindowIcon *icon);

/* eh6.7: the foreground window followed by its owner chain (at most max); 0 when none. */
uint32_t asb_rw_ctl_foreground(uint64_t *chain, uint32_t max);
/* Makes hwnd the foreground window (foreground-lock fallback: brief AttachThreadInput to the
   current foreground thread, then SwitchToThisWindow). TRUE when it is the foreground. */
BOOL asb_rw_ctl_activate(HWND hwnd);
/* The shell's desktop window (activated when shared windows must give up the foreground). */
uint64_t asb_rw_ctl_desktop(void);

/* Fills the eh6.6/eh6.7 members of `ops` with the functions above (ctx unused). */
void asb_rw_ctl_ops(AsbRwHelperOps *ops);

#ifdef __cplusplus
}
#endif
#endif
