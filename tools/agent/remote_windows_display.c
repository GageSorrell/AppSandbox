/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_display
 *
 * @file      remote_windows_display.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Guest side of the private sharing display: driver lease commands (via the VDD's
   private-display device interface) and the extended desktop+private topology. */
#include "remote_windows_display.h"
#include <setupapi.h>
#include <stdlib.h>
#include <stdint.h>
#pragma comment(lib, "setupapi.lib")

static DWORD display_command_reply(AsbRwDisplay *display, UINT32 op, UINT32 width, UINT32 height,
                                   AsbPrivateDisplayReply *out)
{
    AsbPrivateDisplayRequest request = {sizeof(request), ASB_PRIVATE_DISPLAY_VERSION, op, width, height, 0, 0};
    AsbPrivateDisplayReply reply = {0};
    DWORD bytes = 0;
    request.token = display->token;
    if (!DeviceIoControl(display->driver, ASB_PRIVATE_IOCTL, &request, sizeof(request),
                         &reply, sizeof(reply), &bytes, NULL)) return GetLastError();
    if (bytes != sizeof(reply) || reply.size != sizeof(reply) ||
        reply.version != ASB_PRIVATE_DISPLAY_VERSION || reply.desktop_id != ASB_DISPLAY_DESKTOP ||
        reply.private_id != ASB_DISPLAY_PRIVATE || reply.state > ASB_PRIVATE_RESTORING)
        return ERROR_REVISION_MISMATCH;
    if (out) *out = reply;
    return ERROR_SUCCESS;
}

static DWORD display_command(AsbRwDisplay *display, UINT32 op, UINT32 width, UINT32 height)
{
    return display_command_reply(display, op, width, height, NULL);
}

DWORD asb_rw_display_open(AsbRwDisplay *display, UINT64 token)
{
    if (!display || !token) return ERROR_INVALID_PARAMETER;
    ZeroMemory(display, sizeof(*display));
    display->driver = INVALID_HANDLE_VALUE;
    display->token = token;
    HDEVINFO devices = SetupDiGetClassDevsW(&ASB_PRIVATE_DISPLAY_INTERFACE, NULL, NULL,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ERROR_NOT_FOUND;
    SP_DEVICE_INTERFACE_DATA item = {sizeof(item)};
    if (SetupDiEnumDeviceInterfaces(devices, NULL, &ASB_PRIVATE_DISPLAY_INTERFACE, 0, &item)) {
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &item, NULL, 0, &bytes, NULL);
        if (bytes > sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && bytes <= 65536) {
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = calloc(1, bytes);
            if (!detail) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(devices, &item, detail, bytes, NULL, NULL)) {
                display->driver = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                error = display->driver == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            } else error = GetLastError();
            free(detail);
        } else error = ERROR_INVALID_DATA;
    }
done:
    SetupDiDestroyDeviceInfoList(devices);
    if (!error) {
        error = display_command(display, ASB_PRIVATE_QUERY, 0, 0);
        if (error) asb_rw_display_close(display);
    }
    return error;
}

static BOOL same_adapter(LUID a, LUID b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

static UINT32 path_role(const DISPLAYCONFIG_PATH_INFO *path)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME target = {0};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path->targetInfo.adapterId;
    target.header.id = path->targetInfo.id;
    return DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS ?
        asb_display_role(target.monitorDevicePath) : 0;
}

/* Find the desktop (ASB0001) and private (ASB0002) outputs by PnP identity. The
   desktop must be the primary at (0,0); every other active output keeps its own
   source/target modes and the private output goes immediately beyond the rightmost of
   them (outside the viewer/screenshot/absolute-input region), top-aligned. With apply,
   set that topology without saving it to the display database; without apply, verify
   it and publish both rectangles. Foreign outputs (e.g. the Hyper-V synthetic display
   before the display watcher disables it) are kept; a private output cloned onto
   another output's source is refused. */
#define ASB_MAX_TOPOLOGY_PATHS 16u
#define ASB_TOPOLOGY_REJECT_RETRIES 30 /* x 100 ms */
static DWORD display_topology(AsbRwDisplay *display, UINT32 width, UINT32 height, BOOL apply)
{
    UINT32 paths_count = 0, modes_count = 0;
    UINT32 flags = apply ? QDC_ALL_PATHS : QDC_ONLY_ACTIVE_PATHS;
    LONG error = GetDisplayConfigBufferSizes(flags, &paths_count, &modes_count);
    if (error) return (DWORD)error;
    if (paths_count > 256 || modes_count > 1024) return ERROR_NOT_SUPPORTED;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(paths_count ? paths_count : 1, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(modes_count ? modes_count : 1, sizeof(*modes));
    if (!paths || !modes) { free(paths); free(modes); return ERROR_NOT_ENOUGH_MEMORY; }
    error = QueryDisplayConfig(flags, &paths_count, paths, &modes_count, modes, NULL);
    if (error) goto done;
    UINT32 desktop = UINT32_MAX, private_path = UINT32_MAX, others = 0;
    LONG right = 0;
    for (UINT32 i = 0; i < paths_count; ++i) {
        if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
        UINT32 role = path_role(&paths[i]);
        if (role == ASB_DISPLAY_PRIVATE) continue;
        UINT32 index = paths[i].sourceInfo.modeInfoIdx;
        if (index >= modes_count || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            error = ERROR_NOT_READY; goto done; /* mid-reconfiguration */
        }
        const DISPLAYCONFIG_SOURCE_MODE *mode = &modes[index].sourceMode;
        if (mode->position.x + (LONG)mode->width > right) right = mode->position.x + (LONG)mode->width;
        if (role == ASB_DISPLAY_DESKTOP && desktop == UINT32_MAX) desktop = i;
        if (++others > ASB_MAX_TOPOLOGY_PATHS - 1) { error = ERROR_NOT_SUPPORTED; goto done; }
    }
    if (desktop == UINT32_MAX) { error = ERROR_NOT_READY; goto done; }
    const DISPLAYCONFIG_SOURCE_MODE primary = modes[paths[desktop].sourceInfo.modeInfoIdx].sourceMode;
    if (primary.position.x || primary.position.y) {
        /* The display watcher restores the desktop as primary; retry after it does. */
        error = ERROR_NOT_READY; goto done;
    }
    for (UINT32 i = 0; i < paths_count; ++i) {
        if (!paths[i].targetInfo.targetAvailable || path_role(&paths[i]) != ASB_DISPLAY_PRIVATE) continue;
        if (!same_adapter(paths[i].sourceInfo.adapterId, paths[desktop].sourceInfo.adapterId)) continue;
        BOOL shared = FALSE; /* source already driving another active output: a clone */
        for (UINT32 k = 0; k < paths_count; ++k) {
            if (k == i || !(paths[k].flags & DISPLAYCONFIG_PATH_ACTIVE) ||
                path_role(&paths[k]) == ASB_DISPLAY_PRIVATE) continue;
            shared |= paths[k].sourceInfo.id == paths[i].sourceInfo.id &&
                same_adapter(paths[k].sourceInfo.adapterId, paths[i].sourceInfo.adapterId);
        }
        if (shared) {
            if (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) { error = ERROR_NOT_SUPPORTED; goto done; }
            continue;
        }
        if (private_path == UINT32_MAX || (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) private_path = i;
    }
    if (private_path == UINT32_MAX) { error = ERROR_NOT_READY; goto done; }
    if (!apply) {
        UINT32 index = paths[private_path].sourceInfo.modeInfoIdx;
        if (!(paths[private_path].flags & DISPLAYCONFIG_PATH_ACTIVE)) { error = ERROR_NOT_READY; goto done; }
        if (index >= modes_count || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            error = ERROR_NOT_SUPPORTED; goto done;
        }
        const DISPLAYCONFIG_SOURCE_MODE *private_mode = &modes[index].sourceMode;
        if (private_mode->width != width || private_mode->height != height ||
            private_mode->position.x != right || private_mode->position.y != 0) {
            error = ERROR_INVALID_STATE; goto done;
        }
        SetRect(&display->desktop, 0, 0, (int)primary.width, (int)primary.height);
        SetRect(&display->private_rect, (int)right, 0, (int)(right + (LONG)width), (int)height);
    } else {
        /* Compact mode table: only the modes the supplied paths reference. */
        DISPLAYCONFIG_PATH_INFO chosen[ASB_MAX_TOPOLOGY_PATHS];
        DISPLAYCONFIG_MODE_INFO supplied[2 * ASB_MAX_TOPOLOGY_PATHS];
        UINT32 count = 0, used = 0;
        ZeroMemory(chosen, sizeof(chosen));
        ZeroMemory(supplied, sizeof(supplied));
        for (UINT32 i = 0; i < paths_count; ++i) {
            if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) || path_role(&paths[i]) == ASB_DISPLAY_PRIVATE) continue;
            DISPLAYCONFIG_PATH_INFO *path = &chosen[used++];
            *path = paths[i];
            supplied[count] = modes[paths[i].sourceInfo.modeInfoIdx];
            path->sourceInfo.modeInfoIdx = count++;
            UINT32 target = paths[i].targetInfo.modeInfoIdx;
            if (target < modes_count && modes[target].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                supplied[count] = modes[target];
                path->targetInfo.modeInfoIdx = count++;
            } else {
                path->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            }
        }
        DISPLAYCONFIG_PATH_INFO *path = &chosen[used++];
        *path = paths[private_path];
        DISPLAYCONFIG_MODE_INFO *mode = &supplied[count];
        mode->infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        mode->adapterId = path->sourceInfo.adapterId;
        mode->id = path->sourceInfo.id;
        mode->sourceMode.width = width;
        mode->sourceMode.height = height;
        mode->sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
        mode->sourceMode.position.x = right;
        mode->sourceMode.position.y = 0;
        path->sourceInfo.modeInfoIdx = count++;
        path->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path->flags = DISPLAYCONFIG_PATH_ACTIVE;
        UINT32 options = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
        error = SetDisplayConfig(used, chosen, count, supplied, options | SDC_VALIDATE);
        if (!error) error = SetDisplayConfig(used, chosen, count, supplied, options | SDC_APPLY);
    }
done:
    free(modes);
    free(paths);
    return (DWORD)error;
}

DWORD asb_rw_display_activate(AsbRwDisplay *display, UINT32 width, UINT32 height)
{
    if (!display || !asb_private_mode_supported(width, height)) return ERROR_NOT_SUPPORTED;
    DWORD error = display_command(display, ASB_PRIVATE_ACTIVATE, width, height);
    if (error) return error;
    /* Arrival/enumeration is asynchronous. Do not park until verified. On failure
       retain the lease for explicit restoration/removal by the supervisor. */
    /* Windows rejects the topology for about a second after the output arrives, while it
       still enumerates the new target's modes (natively SetDisplayConfig returned
       ERROR_NOT_SUPPORTED, then succeeded ~1 s later), so rejections are retried for up
       to ~3 s before being reported as unsupported. */
    int applies = 0, rejected = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        error = display_topology(display, width, height, FALSE);
        if (!error) break;
        if ((error == ERROR_INVALID_STATE || error == ERROR_NOT_READY) && applies < 3) {
            DWORD applied = display_topology(display, width, height, TRUE);
            if (!applied) { ++applies; continue; }
            if (applied == ERROR_NOT_SUPPORTED || applied == ERROR_INVALID_PARAMETER ||
                applied == ERROR_GEN_FAILURE) {
                error = ++rejected < ASB_TOPOLOGY_REJECT_RETRIES ? ERROR_NOT_READY : ERROR_NOT_SUPPORTED;
            } else {
                error = applied;
            }
        }
        if (error != ERROR_NOT_READY && error != ERROR_INSUFFICIENT_BUFFER) break;
        Sleep(100);
    }
    if (error == ERROR_INVALID_STATE) error = ERROR_NOT_SUPPORTED;
    return error;
}

DWORD asb_rw_display_refresh(AsbRwDisplay *display, UINT32 width, UINT32 height)
{
    if (!display) return ERROR_INVALID_PARAMETER;
    DWORD error = display_topology(display, width, height, FALSE);
    if (error) SetRectEmpty(&display->private_rect);
    return error;
}

DWORD asb_rw_display_query(AsbRwDisplay *display, UINT32 *state, UINT32 *width, UINT32 *height)
{
    AsbPrivateDisplayReply reply = {0};
    if (!display || !state) return ERROR_INVALID_PARAMETER;
    DWORD error = display_command_reply(display, ASB_PRIVATE_QUERY, 0, 0, &reply);
    if (error) return error;
    *state = reply.state;
    if (width) *width = reply.width;
    if (height) *height = reply.height;
    return ERROR_SUCCESS;
}

DWORD asb_rw_display_restore(AsbRwDisplay *display, AsbRwRestoreSources restore, void *context)
{
    if (!display || !restore) return ERROR_INVALID_PARAMETER;
    DWORD error = display_command(display, ASB_PRIVATE_RESTORE, 0, 0);
    if (!error) error = restore(context);
    if (!error) error = display_command(display, ASB_PRIVATE_REMOVE, 0, 0);
    if (!error) SetRectEmpty(&display->private_rect);
    return error;
}

void asb_rw_display_close(AsbRwDisplay *display)
{
    if (display && display->driver && display->driver != INVALID_HANDLE_VALUE) {
        CloseHandle(display->driver);
        display->driver = INVALID_HANDLE_VALUE;
    }
}
