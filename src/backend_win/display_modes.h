#pragma once
#include <windows.h>
#include <stddef.h>

typedef struct AsbDisplayMode { int width, height; } AsbDisplayMode;
typedef struct AsbMonitorCapability {
    wchar_t device[CCHDEVICENAME]; /* MONITORINFOEXW.szDevice / GDI display name */
    AsbDisplayMode *modes;         /* unique pairs, sorted ascending */
    size_t mode_count;
    BOOL has_maximum;             /* at least one mode within application bounds */
    AsbDisplayMode maximum;
} AsbMonitorCapability;
typedef struct AsbDisplayCapabilities {
    HRESULT status;               /* failed snapshots contain no partial results */
    AsbMonitorCapability *monitors;
    size_t monitor_count;
    AsbDisplayMode *presets;       /* filtered union, sorted ascending */
    size_t preset_count;
} AsbDisplayCapabilities;

/* Internal source seam: S_OK produces one item, S_FALSE ends enumeration,
 * failures invalidate the entire snapshot. Indices restart at zero on refresh. */
typedef struct AsbDisplayModeSource {
    void *context;
    HRESULT (*device)(void *, size_t, wchar_t out[CCHDEVICENAME]);
    HRESULT (*mode)(void *, const wchar_t *, size_t, AsbDisplayMode *);
} AsbDisplayModeSource;
HRESULT asb_display_capabilities_build(const AsbDisplayModeSource *source, AsbDisplayCapabilities *out);
HRESULT asb_display_capabilities_enumerate(AsbDisplayCapabilities *out);
void asb_display_capabilities_free(AsbDisplayCapabilities *caps);
BOOL asb_display_capabilities_accommodates(const AsbDisplayCapabilities *caps, AsbDisplayMode mode);
HRESULT asb_display_capabilities_maximum(const AsbDisplayCapabilities *caps,
                                        const wchar_t *device, AsbDisplayMode *out);
