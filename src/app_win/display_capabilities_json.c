#include "display_capabilities_json.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct Writer { wchar_t *text; size_t capacity, used; HRESULT status; } Writer;
static void write_json(Writer *w, const wchar_t *format, ...)
{
    if (FAILED(w->status)) return;
    va_list args;
    va_start(args, format);
    int count = _vsnwprintf_s(w->text + w->used, w->capacity - w->used, _TRUNCATE, format, args);
    va_end(args);
    if (count < 0) w->status = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    else w->used += (size_t)count;
}
static void write_mode(Writer *w, AsbDisplayMode mode)
{
    write_json(w, L"{\"width\":%d,\"height\":%d}", mode.width, mode.height);
}
HRESULT display_capabilities_json(const AsbDisplayCapabilities *caps, wchar_t **out)
{
    if (!caps || !out) return E_INVALIDARG;
    *out = NULL;
    /* Each pair uses under 64 characters, each monitor's metadata under 512.
     * Check arithmetic and fail instead of delivering a truncated message. */
    size_t capacity = 1024, limit = SIZE_MAX / sizeof(wchar_t);
    if (caps->monitor_count > (limit - capacity) / 512) return E_OUTOFMEMORY;
    capacity += caps->monitor_count * 512;
    size_t pairs = caps->preset_count;
    for (size_t i = 0; i < caps->monitor_count; ++i) {
        if (caps->monitors[i].mode_count > SIZE_MAX - pairs) return E_OUTOFMEMORY;
        pairs += caps->monitors[i].mode_count;
    }
    if (pairs > (limit - capacity) / 64) return E_OUTOFMEMORY;
    capacity += pairs * 64;
    Writer w = {calloc(capacity, sizeof(wchar_t)), capacity, 0, S_OK};
    if (!w.text) return E_OUTOFMEMORY;
    write_json(&w, L"{\"type\":\"displayCapabilities\",\"ok\":%s,\"errorCode\":%ld,\"monitors\":[",
               SUCCEEDED(caps->status) ? L"true" : L"false", caps->status);
    for (size_t i = 0; SUCCEEDED(caps->status) && i < caps->monitor_count; ++i) {
        const AsbMonitorCapability *monitor = &caps->monitors[i];
        write_json(&w, L"%s{\"device\":\"", i ? L"," : L"");
        for (size_t c = 0; c < CCHDEVICENAME && monitor->device[c]; ++c)
            write_json(&w, L"\\u%04x", (unsigned)monitor->device[c]);
        write_json(&w, L"\",\"maximum\":");
        if (monitor->has_maximum) write_mode(&w, monitor->maximum);
        else write_json(&w, L"null");
        write_json(&w, L",\"modes\":[");
        for (size_t m = 0; m < monitor->mode_count; ++m) {
            if (m) write_json(&w, L",");
            write_mode(&w, monitor->modes[m]);
        }
        write_json(&w, L"]}");
    }
    write_json(&w, L"],\"presets\":[");
    for (size_t i = 0; SUCCEEDED(caps->status) && i < caps->preset_count; ++i) {
        if (i) write_json(&w, L",");
        write_mode(&w, caps->presets[i]);
    }
    write_json(&w, L"]}");
    if (FAILED(w.status)) { free(w.text); return w.status; }
    *out = w.text;
    return S_OK;
}
