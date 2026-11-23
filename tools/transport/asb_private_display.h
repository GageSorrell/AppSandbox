/**
 *
 *
 * @module AppSandbox/tools/transport/asb_private_display
 *
 * @file      asb_private_display.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Internal guest helper <-> VDD contract. No host/public API. */
#pragma once
#include <windows.h>
#include <winioctl.h>
#include "asb_display_resize.h"

static const GUID ASB_PRIVATE_DISPLAY_INTERFACE =
    {0x66d5c6a1,0x78f7,0x44af,{0x91,0xb3,0x33,0x04,0xf1,0x65,0xc5,0x28}};
#define ASB_PRIVATE_DISPLAY_VERSION 1u
#define ASB_DISPLAY_DESKTOP 1u /* EDID ASB0001, connector 0 */
#define ASB_DISPLAY_PRIVATE 2u /* EDID ASB0002, connector 1 */
#define ASB_PRIVATE_QUERY 0u
#define ASB_PRIVATE_ACTIVATE 1u
#define ASB_PRIVATE_RESTORE 2u /* freezes activation until restoration finishes */
#define ASB_PRIVATE_REMOVE 3u  /* caller has restored every surviving source */
#define ASB_PRIVATE_ABSENT 0u
#define ASB_PRIVATE_READY 1u
#define ASB_PRIVATE_RESTORING 2u
#define ASB_PRIVATE_IOCTL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

/* Fixed-width, pointer-free, identical on x64/ARM64. Token is a random per-session
   lease, retained by the recovery supervisor; QUERY never discloses it.
   Close/disconnect never removes a display underneath unrestored windows. */
typedef struct AsbPrivateDisplayRequest {
    UINT32 size, version, operation, width, height, reserved;
    UINT64 token;
} AsbPrivateDisplayRequest;
typedef struct AsbPrivateDisplayReply {
    UINT32 size, version, state, width, height;
    UINT32 desktop_id, private_id, reserved;
} AsbPrivateDisplayReply;

/* Same Windows guest mode limits as the desktop monitor (640x480 .. 7680x4320).
   Out-of-range requests are refused (STATUS_NOT_SUPPORTED / ERROR_NOT_SUPPORTED). */
static inline BOOL asb_private_mode_supported(UINT32 width, UINT32 height)
{
    return asb_windows_display_dimensions(width, height) != 0;
}

/* Window property (any non-NULL value) that the sharing helper sets on a root window
   BEFORE parking it on the private output and removes after restoring it. The guest
   display watcher moves every other window it finds on the private output back to the
   desktop output (windows land there when Windows migrates them while the desktop
   output is re-created, e.g. during a viewer resize). Windows owned by a marked root,
   and caption-less popups (menus, tooltips) of a process with a marked root, stay, as
   do Explorer's per-output surfaces (taskbars, desktop), tool windows and cloaked windows. */
#define ASB_PRIVATE_PARKED_PROPERTY L"AppSandbox.PrivateDisplay.Parked"

/* Compare PnP identities, never translated/friendly names or enumeration order. */
static inline UINT32 asb_display_role(const WCHAR *device_path)
{
    if (!device_path) return 0;
    for (const WCHAR *p = device_path; *p; ++p) {
        if ((p == device_path || p[-1] == L'\\' || p[-1] == L'#') &&
            !_wcsnicmp(p, L"ASB000", 6) && (p[6] == L'1' || p[6] == L'2') &&
            (p[7] == L'\\' || p[7] == L'#' || p[7] == 0))
            return p[6] == L'1' ? ASB_DISPLAY_DESKTOP : ASB_DISPLAY_PRIVATE;
    }
    return 0;
}
