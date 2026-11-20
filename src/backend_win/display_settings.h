#pragma once
#include <windows.h>

/* Dimensions are persisted together. BOOL inputs must be TRUE or FALSE. */
typedef struct AsbDisplayResolution {
    int windowed_width, windowed_height;
    BOOL fullscreen_override;
    int fullscreen_width, fullscreen_height;
} AsbDisplayResolution;

typedef struct AsbDisplaySettings {
    AsbDisplayResolution resolution;
    BOOL transmit_keyboard_hotkeys, share_clipboard;
    BOOL fullscreen_pinned, suppress_fullscreen_reminder;
} AsbDisplaySettings;

enum {
    ASB_DISPLAY_RESOLUTION = 1, ASB_DISPLAY_HOTKEYS = 2,
    ASB_DISPLAY_CLIPBOARD = 4, ASB_DISPLAY_PIN = 8,
    ASB_DISPLAY_REMINDER = 16
};

/* Internal path-based storage. Reads missing files as defaults without creating
 * them. Updates reload under a cross-process mutex and replace atomically.
 * Invalid JSON/I/O failures are reported and never replaced with defaults. */
HRESULT asb_display_settings_read(const wchar_t *disk, AsbDisplaySettings *out);
HRESULT asb_display_settings_update(const wchar_t *disk, unsigned fields,
                                    const AsbDisplaySettings *patch);
BOOL asb_display_resolution_valid(const AsbDisplayResolution *value);

/* Shared editor guard and compare/update under the same storage mutex.
 * expected==NULL performs an unconditional validated update. */
HRESULT asb_display_resolution_edit(const wchar_t *disk, BOOL running, BOOL starting,
    BOOL building, const AsbDisplayResolution *value, const AsbDisplayResolution *expected);
