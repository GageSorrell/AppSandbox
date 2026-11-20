#pragma once
#include "windowed_resolution.h"

/* Current desktop pixels are a fallback only. Never synthesize a supported
 * maximum from independently ranked width and height values. */
static inline DisplayGpuPacket asb_fullscreen_fallback(int64_t width, int64_t height)
{
    if (width <= 0 || height <= 0 || width > UINT32_MAX || height > UINT32_MAX)
        return asb_windowed_target(1920, 1080, DISPLAY_SIZE_GUEST, 0, 0);
    DisplayGpuPacket target = asb_windowed_target(0, 0, DISPLAY_SIZE_WINDOW,
        (uint32_t)width, (uint32_t)height);
    target.slot = DISPLAY_SIZE_GUEST;
    return target;
}
