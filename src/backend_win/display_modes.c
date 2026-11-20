#include "display_modes.h"
#include "../../tools/transport/asb_display_resolutions.h"
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <wchar.h>

#define COMMON_MODE(w,h) {w,h},
static const AsbDisplayMode common_modes[] = {
    ASB_COMMON_DISPLAY_RESOLUTIONS(COMMON_MODE)
    {640,480}, {800,600}, {2560,1080}, {3840,1600}, {4096,2160}
};
#undef COMMON_MODE

static int compare_mode(const void *left, const void *right)
{
    const AsbDisplayMode *a = left, *b = right;
    LONGLONG aa = (LONGLONG)a->width * a->height, ba = (LONGLONG)b->width * b->height;
    if (aa != ba) return aa > ba ? 1 : -1;
    if (a->width != b->width) return a->width > b->width ? 1 : -1;
    return (a->height > b->height) - (a->height < b->height);
}
static BOOL within_bounds(AsbDisplayMode mode)
{
    return mode.width >= 640 && mode.width <= 7680 && mode.height >= 480 && mode.height <= 4320;
}
static HRESULT append(void **items, size_t *count, size_t size, const void *item)
{
    if (*count >= SIZE_MAX / size - 1) return E_OUTOFMEMORY;
    void *next = realloc(*items, (*count + 1) * size);
    if (!next) return E_OUTOFMEMORY;
    memcpy((char *)next + *count * size, item, size);
    *items = next;
    ++*count;
    return S_OK;
}
static void unique_modes(AsbDisplayMode *modes, size_t *count)
{
    if (!*count) return;
    qsort(modes, *count, sizeof(*modes), compare_mode);
    size_t n = 1;
    for (size_t i = 1; i < *count; ++i)
        if (compare_mode(&modes[i], &modes[n-1])) modes[n++] = modes[i];
    *count = n;
}
void asb_display_capabilities_free(AsbDisplayCapabilities *caps)
{
    if (!caps) return;
    for (size_t i = 0; i < caps->monitor_count; ++i) free(caps->monitors[i].modes);
    free(caps->monitors);
    free(caps->presets);
    *caps = (AsbDisplayCapabilities){0};
}
BOOL asb_display_capabilities_accommodates(const AsbDisplayCapabilities *caps, AsbDisplayMode mode)
{
    if (!caps || FAILED(caps->status) || !within_bounds(mode)) return FALSE;
    for (size_t i = 0; i < caps->monitor_count; ++i)
        for (size_t m = 0; m < caps->monitors[i].mode_count; ++m) {
            AsbDisplayMode supported = caps->monitors[i].modes[m];
            if (supported.width >= mode.width && supported.height >= mode.height) return TRUE;
        }
    return FALSE;
}
HRESULT asb_display_capabilities_maximum(const AsbDisplayCapabilities *caps,
                                        const wchar_t *device, AsbDisplayMode *out)
{
    if (!caps || !device || !out) return E_INVALIDARG;
    *out = (AsbDisplayMode){0};
    if (FAILED(caps->status)) return caps->status;
    for (size_t i = 0; i < caps->monitor_count; ++i)
        if (!wcscmp(caps->monitors[i].device, device) && caps->monitors[i].has_maximum) {
            *out = caps->monitors[i].maximum;
            return S_OK;
        }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
HRESULT asb_display_capabilities_build(const AsbDisplayModeSource *source, AsbDisplayCapabilities *out)
{
    if (!out) return E_INVALIDARG;
    *out = (AsbDisplayCapabilities){0};
    HRESULT hr = E_INVALIDARG;
    if (!source || !source->device || !source->mode) goto failed;
    for (size_t i = 0;; ++i) {
        AsbMonitorCapability monitor = {0};
        hr = source->device(source->context, i, monitor.device);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) goto failed;
        if (!monitor.device[0] || !wmemchr(monitor.device, 0, CCHDEVICENAME)) { hr = E_INVALIDARG; goto failed; }
        hr = append((void **)&out->monitors, &out->monitor_count, sizeof(monitor), &monitor);
        if (FAILED(hr)) goto failed;
        AsbMonitorCapability *current = &out->monitors[out->monitor_count-1];
        for (size_t m = 0;; ++m) {
            AsbDisplayMode mode = {0};
            hr = source->mode(source->context, monitor.device, m, &mode);
            if (hr == S_FALSE) break;
            if (FAILED(hr)) goto failed;
            if (mode.width <= 0 || mode.height <= 0) { hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); goto failed; }
            hr = append((void **)&current->modes, &current->mode_count, sizeof(mode), &mode);
            if (FAILED(hr)) goto failed;
        }
        if (!current->mode_count) { hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto failed; }
        unique_modes(current->modes, &current->mode_count);
        for (size_t m = 0; m < current->mode_count; ++m)
            if (within_bounds(current->modes[m])) {
                current->maximum = current->modes[m];
                current->has_maximum = TRUE;
            }
    }
    if (!out->monitor_count) { hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto failed; }
    for (size_t i = 0; i < ARRAYSIZE(common_modes) + out->monitor_count; ++i) {
        AsbDisplayMode mode = i < ARRAYSIZE(common_modes) ? common_modes[i] :
            out->monitors[i-ARRAYSIZE(common_modes)].maximum;
        if (asb_display_capabilities_accommodates(out, mode)) {
            hr = append((void **)&out->presets, &out->preset_count, sizeof(mode), &mode);
            if (FAILED(hr)) goto failed;
        }
    }
    unique_modes(out->presets, &out->preset_count);
    out->status = S_OK;
    return S_OK;
failed:
    asb_display_capabilities_free(out);
    out->status = hr;
    return hr;
}

/* The GDI names from GetMonitorInfo also identify the viewer's target monitor.
 * Each refresh is a fresh snapshot; no host display settings are changed. */
typedef struct NativeDevices {
    wchar_t (*names)[CCHDEVICENAME];
    size_t count;
    HRESULT status;
} NativeDevices;
static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data)
{
    (void)dc; (void)rect;
    NativeDevices *devices = (NativeDevices *)data;
    MONITORINFOEXW info = {0};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info)) {
        DWORD error = GetLastError();
        devices->status = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        return FALSE;
    }
    /* Exclude mirroring/pseudo devices even if present in monitor enumeration. */
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW adapter = {0};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, i, &adapter, 0)) { devices->status = E_FAIL; return FALSE; }
        if (wcscmp(adapter.DeviceName, info.szDevice)) continue;
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) return TRUE;
        break;
    }
    for (size_t i = 0; i < devices->count; ++i)
        if (!wcscmp(devices->names[i], info.szDevice)) return TRUE;
    devices->status = append((void **)&devices->names, &devices->count, sizeof(info.szDevice), info.szDevice);
    return SUCCEEDED(devices->status);
}
static HRESULT native_device(void *context, size_t index, wchar_t out[CCHDEVICENAME])
{
    NativeDevices *devices = context;
    if (index >= devices->count) return S_FALSE;
    wcscpy_s(out, CCHDEVICENAME, devices->names[index]);
    return S_OK;
}
static HRESULT native_mode(void *context, const wchar_t *device, size_t index, AsbDisplayMode *out)
{
    (void)context;
    if (index >= ENUM_CURRENT_SETTINGS) return E_FAIL;
    DEVMODEW mode = {0};
    mode.dmSize = sizeof(mode);
    /* Zero flags: respect monitor capabilities and current orientation.
     * Starting at index zero refreshes Windows' enumeration cache. */
    if (!EnumDisplaySettingsExW(device, (DWORD)index, &mode, 0))
        return index ? S_FALSE : E_FAIL;
    if ((mode.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) != (DM_PELSWIDTH | DM_PELSHEIGHT) ||
        mode.dmPelsWidth > INT_MAX || mode.dmPelsHeight > INT_MAX) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    *out = (AsbDisplayMode){(int)mode.dmPelsWidth, (int)mode.dmPelsHeight};
    return S_OK;
}
HRESULT asb_display_capabilities_enumerate(AsbDisplayCapabilities *out)
{
    if (!out) return E_INVALIDARG;
    *out = (AsbDisplayCapabilities){0};
    NativeDevices devices = {0};
    BOOL enumerated = EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&devices);
    HRESULT hr = devices.status;
    if (!enumerated && SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        AsbDisplayModeSource source = {&devices, native_device, native_mode};
        hr = asb_display_capabilities_build(&source, out);
    }
    free(devices.names);
    out->status = hr;
    return hr;
}
