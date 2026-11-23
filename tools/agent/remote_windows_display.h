/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_display
 *
 * @file      remote_windows_display.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

#pragma once
#include "../transport/asb_private_display.h"

/* Helper/supervisor owns this lease. Journal token before parking any sources.
   Calls must be serialized by the helper. Never remove on handle close. */
typedef struct AsbRwDisplay {
    HANDLE driver;
    UINT64 token;
    RECT desktop, private_rect;
} AsbRwDisplay;
typedef DWORD (*AsbRwRestoreSources)(void *context);

DWORD asb_rw_display_open(AsbRwDisplay *display, UINT64 token);
/* Leases (or re-confirms) the private output, then applies and verifies the extended
   topology: desktop primary at (0,0), other outputs untouched, private immediately
   beyond the rightmost of them. Unsupported modes, cloned outputs and topologies
   Windows will not accept return ERROR_NOT_SUPPORTED; ERROR_ACCESS_DENIED means another
   lease owns the output. On failure the lease stays held: call asb_rw_display_restore. */
DWORD asb_rw_display_activate(AsbRwDisplay *display, UINT32 width, UINT32 height);
/* Re-verifies the topology and republishes desktop/private rectangles without changing
   anything. Call after WM_DISPLAYCHANGE (the display watcher may close a gap left by a
   foreign output, moving the private output and the windows on it) before using
   private_rect. ERROR_INVALID_STATE: the layout drifted; activate again to re-place. */
DWORD asb_rw_display_refresh(AsbRwDisplay *display, UINT32 width, UINT32 height);
/* ASB_PRIVATE_ABSENT/READY/RESTORING plus the leased mode. Needs no matching token. */
DWORD asb_rw_display_query(AsbRwDisplay *display, UINT32 *state, UINT32 *width, UINT32 *height);
/* Callback must restore/validate ALL surviving sources and shell entries;
   failure keeps the display present in RESTORING for supervised retry. */
DWORD asb_rw_display_restore(AsbRwDisplay *display, AsbRwRestoreSources restore, void *context);
void asb_rw_display_close(AsbRwDisplay *display);
