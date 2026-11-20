#include "display_settings.h"
#include "../core/api_json.h"
#include <stdlib.h>
#include <stdio.h>
#include <wctype.h>
#include <string.h>

/* Limit resource use, but never silently truncate or rewrite an oversized file. */
#define MAX_SETTINGS_BYTES (1024 * 1024)
static const wchar_t *keys[] = {
    L"windowedWidth", L"windowedHeight", L"fullscreenOverride",
    L"fullscreenWidth", L"fullscreenHeight", L"transmitKeyboardHotkeys",
    L"shareClipboard", L"fullscreenPinned", L"suppressFullscreenReminder"
};
static const unsigned groups[] = {1,1,1,1,1,2,4,8,16};

BOOL asb_display_resolution_valid(const AsbDisplayResolution *v)
{
    return v && v->windowed_width >= 640 && v->windowed_width <= 7680 &&
        v->windowed_height >= 480 && v->windowed_height <= 4320 &&
        v->fullscreen_width >= 640 && v->fullscreen_width <= 7680 &&
        v->fullscreen_height >= 480 && v->fullscreen_height <= 4320 &&
        (v->fullscreen_override == FALSE || v->fullscreen_override == TRUE);
}
static HRESULT settings_path(const wchar_t *disk, wchar_t *path)
{
    wchar_t full[MAX_PATH];
    if (!disk || !*disk) return E_INVALIDARG;
    DWORD n = GetFullPathNameW(disk, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) return E_INVALIDARG;
    wchar_t *slash = wcsrchr(full, L'\\');
    if (!slash) return E_INVALIDARG;
    slash[1] = 0;
    return swprintf_s(path, MAX_PATH, L"%sdisplay_settings.json", full) < 0 ? E_INVALIDARG : S_OK;
}
static HRESULT lock_settings(const wchar_t *path, HANDLE *lock)
{
    unsigned long long hash = 14695981039346656037ULL;
    for (const wchar_t *p = path; *p; ++p) {
        hash ^= (unsigned)towlower(*p);
        hash *= 1099511628211ULL;
    }
    wchar_t name[80];
    swprintf_s(name, 80, L"Global\\AppSandbox.DisplaySettings.%016llx", hash);
    *lock = CreateMutexW(NULL, FALSE, name);
    if (!*lock) return HRESULT_FROM_WIN32(GetLastError());
    DWORD result = WaitForSingleObject(*lock, INFINITE);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return S_OK;
    HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(*lock);
    *lock = NULL;
    return hr;
}
static HRESULT load(const wchar_t *path, wchar_t **text, ApiJson *json, ApiJsonToken **tokens)
{
    HRESULT hr = S_OK;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, 0, NULL);
    char *bytes = NULL;
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(error);
        *text = _wcsdup(L"{}");
        if (!*text) return E_OUTOFMEMORY;
    } else {
        LARGE_INTEGER size;
        DWORD count;
        if (!GetFileSizeEx(file, &size)) { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }
        if (size.QuadPart > MAX_SETTINGS_BYTES) { hr = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); goto done; }
        bytes = malloc((size_t)size.QuadPart + 1);
        if (!bytes) { hr = E_OUTOFMEMORY; goto done; }
        if (!ReadFile(file, bytes, (DWORD)size.QuadPart, &count, NULL)) {
            hr = HRESULT_FROM_WIN32(GetLastError()); goto done;
        }
        if (count != size.QuadPart || memchr(bytes, 0, count)) { hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); goto done; }
        int offset = count >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3) ? 3 : 0;
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset, (int)count - offset, NULL, 0);
        if (!length) { hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); goto done; }
        *text = calloc((size_t)length + 1, sizeof(wchar_t));
        if (!*text) { hr = E_OUTOFMEMORY; goto done; }
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset, (int)count - offset, *text, length);
    }
    size_t capacity = wcslen(*text) + 1;
    *tokens = calloc(capacity, sizeof(ApiJsonToken));
    if (!*tokens) { hr = E_OUTOFMEMORY; goto done; }
    if (!api_json_parse(json, *text, *tokens, (int)capacity) || (*tokens)[0].type != API_JSON_OBJECT)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
done:
    free(bytes);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return hr;
}
static void decode(const ApiJson *j, AsbDisplaySettings *out)
{
    int v[9] = {1920,1080,0,1920,1080,0,1,0,0};
    BOOL valid[9];
    for (int i = 0; i < 9; ++i) {
        int token = api_json_field(j, 0, keys[i]);
        valid[i] = api_json_int(j, token, &v[i]);
        if (i == 2 || i >= 5) {
            if (!valid[i]) valid[i] = api_json_bool(j, token, &v[i]);
        }
    }
    if (!valid[0] || !valid[1] || v[0] < 640 || v[0] > 7680 || v[1] < 480 || v[1] > 4320) { v[0] = 1920; v[1] = 1080; }
    if (!valid[3] || !valid[4] || v[3] < 640 || v[3] > 7680 || v[4] < 480 || v[4] > 4320) { v[3] = 1920; v[4] = 1080; }
    *out = (AsbDisplaySettings){{v[0],v[1],v[2] != 0,v[3],v[4]},v[5] != 0,v[6] != 0,v[7] != 0,v[8] != 0};
}
static HRESULT store(const wchar_t *path, const ApiJson *j, unsigned fields, const AsbDisplaySettings *patch)
{
    int values[] = {patch->resolution.windowed_width, patch->resolution.windowed_height,
        patch->resolution.fullscreen_override, patch->resolution.fullscreen_width,
        patch->resolution.fullscreen_height, patch->transmit_keyboard_hotkeys,
        patch->share_clipboard, patch->fullscreen_pinned, patch->suppress_fullscreen_reminder};
    size_t capacity = wcslen(j->text) + 1024;
    wchar_t *output = calloc(capacity, sizeof(wchar_t));
    if (!output) return E_OUTOFMEMORY;
    size_t used = 1;
    output[0] = L'{';
    for (int i = 1; i < j->tokens[0].next;) {
        int key = i, value = j->tokens[i].next;
        i = j->tokens[value].next;
        BOOL replaced = FALSE;
        wchar_t name[128];
        if (api_json_string(j, key, name, ARRAYSIZE(name)))
            for (int k = 0; k < 9; ++k)
                if ((fields & groups[k]) && !wcscmp(name, keys[k])) replaced = TRUE;
        if (replaced) continue;
        if (used > 1) output[used++] = L',';
        size_t n = (size_t)(j->tokens[value].end - j->tokens[key].start);
        memcpy(output + used, j->text + j->tokens[key].start, n * sizeof(wchar_t));
        used += n;
    }
    for (int k = 0; k < 9; ++k) if (fields & groups[k]) {
        if (used > 1) output[used++] = L',';
        used += swprintf_s(output + used, capacity - used, L"\"%s\":%d", keys[k], values[k]);
    }
    output[used++] = L'}';
    output[used++] = L'\n';
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, output, (int)used, NULL, 0, NULL, NULL);
    if (count > MAX_SETTINGS_BYTES) { free(output); return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); }
    char *bytes = count ? malloc(count) : NULL;
    if (!bytes) { free(output); return count ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, output, (int)used, bytes, count, NULL, NULL);
    free(output);
    wchar_t directory[MAX_PATH], temporary[MAX_PATH];
    wcscpy_s(directory, MAX_PATH, path);
    *wcsrchr(directory, L'\\') = 0;
    HRESULT hr = S_OK;
    if (!GetTempFileNameW(directory, L"asd", 0, temporary)) {
        free(bytes); return HRESULT_FROM_WIN32(GetLastError());
    }
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) hr = HRESULT_FROM_WIN32(GetLastError());
    else {
        DWORD written;
        if (!WriteFile(file, bytes, count, &written, NULL)) hr = HRESULT_FROM_WIN32(GetLastError());
        else if (written != (DWORD)count) hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        if (SUCCEEDED(hr) && !FlushFileBuffers(file)) hr = HRESULT_FROM_WIN32(GetLastError());
        if (!CloseHandle(file) && SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    free(bytes);
    if (SUCCEEDED(hr) && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) DeleteFileW(temporary);
    return hr;
}
HRESULT asb_display_settings_read(const wchar_t *disk, AsbDisplaySettings *out)
{
    if (!out) return E_INVALIDARG;
    *out = (AsbDisplaySettings){{1920,1080,FALSE,1920,1080},FALSE,TRUE,FALSE,FALSE};
    wchar_t path[MAX_PATH], *text = NULL;
    ApiJsonToken *tokens = NULL;
    ApiJson json;
    HANDLE lock = NULL;
    HRESULT hr = settings_path(disk, path);
    if (SUCCEEDED(hr)) hr = lock_settings(path, &lock);
    if (SUCCEEDED(hr)) hr = load(path, &text, &json, &tokens);
    if (SUCCEEDED(hr)) decode(&json, out);
    free(tokens); free(text);
    if (lock) { ReleaseMutex(lock); CloseHandle(lock); }
    return hr;
}
static HRESULT settings_update(const wchar_t *disk, unsigned fields, const AsbDisplaySettings *patch,
                               const AsbDisplayResolution *expected)
{
    if (!patch || !fields || (fields & ~31u)) return E_INVALIDARG;
    if ((fields & ASB_DISPLAY_RESOLUTION) && !asb_display_resolution_valid(&patch->resolution)) return E_INVALIDARG;
    if (((fields & 2) && patch->transmit_keyboard_hotkeys != 0 && patch->transmit_keyboard_hotkeys != 1) ||
        ((fields & 4) && patch->share_clipboard != 0 && patch->share_clipboard != 1) ||
        ((fields & 8) && patch->fullscreen_pinned != 0 && patch->fullscreen_pinned != 1) ||
        ((fields & 16) && patch->suppress_fullscreen_reminder != 0 && patch->suppress_fullscreen_reminder != 1)) return E_INVALIDARG;
    wchar_t path[MAX_PATH], *text = NULL;
    ApiJsonToken *tokens = NULL;
    ApiJson json;
    HANDLE lock = NULL;
    HRESULT hr = settings_path(disk, path);
    if (SUCCEEDED(hr)) hr = lock_settings(path, &lock);
    if (SUCCEEDED(hr)) hr = load(path, &text, &json, &tokens);
    if (SUCCEEDED(hr) && expected) {
        AsbDisplaySettings current;
        decode(&json, &current);
        const AsbDisplayResolution *r = &current.resolution;
        if (r->windowed_width != expected->windowed_width || r->windowed_height != expected->windowed_height ||
            r->fullscreen_override != expected->fullscreen_override || r->fullscreen_width != expected->fullscreen_width ||
            r->fullscreen_height != expected->fullscreen_height) hr = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    if (SUCCEEDED(hr)) hr = store(path, &json, fields, patch);
    free(tokens); free(text);
    if (lock) { ReleaseMutex(lock); CloseHandle(lock); }
    return hr;
}

HRESULT asb_display_settings_update(const wchar_t *disk, unsigned fields, const AsbDisplaySettings *patch)
{
    return settings_update(disk, fields, patch, NULL);
}
HRESULT asb_display_resolution_edit(const wchar_t *disk, BOOL running, BOOL starting,
    BOOL building, const AsbDisplayResolution *value, const AsbDisplayResolution *expected)
{
    if (!value) return E_INVALIDARG;
    if (running || starting || building) return E_ACCESSDENIED;
    if (!asb_display_resolution_valid(value) || (expected && !asb_display_resolution_valid(expected))) return E_INVALIDARG;
    AsbDisplaySettings patch = {0};
    patch.resolution = *value;
    return settings_update(disk, ASB_DISPLAY_RESOLUTION, &patch, expected);
}
