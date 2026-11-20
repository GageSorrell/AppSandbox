#pragma once
#include "../../tools/transport/asb_display_resize.h"

/* Desired session policy is independent of received frames and protocol mode.
 * DISPLAY_SIZE_GUEST means fixed saved dimensions, not follow-the-guest. */
static inline DisplayGpuPacket asb_windowed_target(uint32_t saved_width,
    uint32_t saved_height, uint32_t mode, uint32_t client_width, uint32_t client_height)
{
    DisplayGpuPacket target = {0};
    target.magic = DISPLAY_GPU_MAGIC;
    target.version = DISPLAY_GPU_VERSION;
    target.type = DISPLAY_GPU_RESIZE;
    target.slot = mode;
    target.width = saved_width;
    target.height = saved_height;
    if (mode == DISPLAY_SIZE_WINDOW) {
        target.width = client_width < ASB_WINDOWS_MIN_WIDTH ? ASB_WINDOWS_MIN_WIDTH :
            client_width > DISPLAY_MAX_WIDTH ? DISPLAY_MAX_WIDTH : client_width;
        target.height = client_height < ASB_WINDOWS_MIN_HEIGHT ? ASB_WINDOWS_MIN_HEIGHT :
            client_height > DISPLAY_MAX_HEIGHT ? DISPLAY_MAX_HEIGHT : client_height;
    }
    return target;
}

static inline int asb_windowed_needs_request(const DisplayGpuPacket *target,
    uint32_t actual_width, uint32_t actual_height, uint32_t guest_mode,
    const DisplayGpuPacket *queued, const AsbDisplayResizeState *state)
{
    if (queued->type || state->pending.type) return 0;
    if (state->failed.type && asb_resize_same(target, &state->failed)) return 0;
    return target->width != actual_width || target->height != actual_height ||
        target->slot != guest_mode;
}
