#pragma once

/* Shared guest-driver presets; first entry remains the default. */
#define ASB_COMMON_DISPLAY_RESOLUTIONS(X) \
    X(1920, 1080) \
    X(1024, 768) \
    X(1280, 720) \
    X(1280, 800) \
    X(1280, 1024) \
    X(1366, 768) \
    X(1440, 900) \
    X(1600, 900) \
    X(1680, 1050) \
    X(1920, 1200) \
    X(2560, 1440) \
    X(2560, 1600) \
    X(3440, 1440) \
    X(3840, 2160) \
    X(5120, 1440) \
    X(5120, 2880) \
    X(7680, 4320)
