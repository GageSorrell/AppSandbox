/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_inventory
 *
 * @file      remote_windows_inventory.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

#ifndef REMOTE_WINDOWS_INVENTORY_H
#define REMOTE_WINDOWS_INVENTORY_H

/* Guest-side Win32 collector for the Remote Windows inventory (AppSandbox-eh6.2).
   Fills platform-neutral AsbRwWindowFacts from live top-level windows in the caller's session;
   eligibility, ownership association and diffing are in tools/transport/asb_remote_windows.c. */

#include <windows.h>
#include "../transport/asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Facts for one top-level window. helper_pid marks the integration helper's own windows.
   Returns FALSE if the window vanished while being inspected. */
BOOL asb_rw_collect_facts(HWND hwnd, DWORD helper_pid, DWORD active_session, AsbRwWindowFacts *facts);

/* All top-level windows of the current desktop, top of z order first. Returns ASB_RW_OK, or
   ASB_RW_ERR_LIMIT when more than max windows exist (the first max are returned). */
uint32_t asb_rw_enumerate_windows(AsbRwWindowFacts *out, uint32_t max, DWORD helper_pid, uint32_t *count);

/* Lightweight per-tick scan for popup tracking (eh6.5): visible top-level windows only, top
   first, with identity, class, styles, owner/menu-owner chains, frame bounds and cloaking -
   no application names or elevation. Returns ASB_RW_OK or ASB_RW_ERR_LIMIT. */
uint32_t asb_rw_scan_visible_windows(AsbRwWindowFacts *out, uint32_t max, uint32_t *count);

/* AsbRwIdentityProbe for asb_rw_ids_resolve: re-reads pid/thread/process start of a live HWND. */
int asb_rw_probe_identity(void *ctx, uint64_t hwnd, AsbRwIdentity *live);

/* Session the inventory is restricted to (the active console session). */
DWORD asb_rw_active_session(void);

#ifdef __cplusplus
}
#endif
#endif
