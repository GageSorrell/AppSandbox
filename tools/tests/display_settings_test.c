#include "../../src/backend_win/display_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (win32 %lu)\n", __LINE__, #x, GetLastError()); exit(1); } } while (0)
#define OK(x) CHECK(SUCCEEDED(x))
static wchar_t disk[MAX_PATH], path[MAX_PATH];
static void fixture(const char *json)
{
    FILE *f = NULL;
    CHECK(!_wfopen_s(&f, path, L"wb"));
    CHECK(fwrite(json, 1, strlen(json), f) == strlen(json));
    CHECK(!fclose(f));
}
static char *contents(void)
{
    FILE *f = NULL;
    CHECK(!_wfopen_s(&f, path, L"rb"));
    CHECK(!fseek(f, 0, SEEK_END));
    long size = ftell(f);
    CHECK(size >= 0);
    rewind(f);
    char *bytes = calloc((size_t)size + 1, 1);
    CHECK(bytes && fread(bytes, 1, size, f) == (size_t)size);
    CHECK(!fclose(f));
    return bytes;
}
static AsbDisplaySettings read_settings(void)
{
    AsbDisplaySettings out;
    OK(asb_display_settings_read(disk, &out));
    return out;
}
static int worker(const wchar_t *worker_disk, unsigned field)
{
    AsbDisplaySettings patch = {{2560,1440,TRUE,3840,2160},TRUE,FALSE,TRUE,TRUE};
    for (int i = 0; i < 100; ++i)
        if (FAILED(asb_display_settings_update(worker_disk, field, &patch))) return 2;
    return 0;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && !wcscmp(argv[1], L"readreminder")) {
        AsbDisplaySettings saved;
        return FAILED(asb_display_settings_read(argv[2], &saved)) || !saved.suppress_fullscreen_reminder;
    }
    if (argc == 3 && !wcscmp(argv[1], L"readpin")) {
        AsbDisplaySettings saved;
        return FAILED(asb_display_settings_read(argv[2], &saved)) || !saved.fullscreen_pinned;
    }
    if (argc == 4 && !wcscmp(argv[1], L"worker")) return worker(argv[2], (unsigned)_wtoi(argv[3]));
    wchar_t temp[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"ast", 0, directory));
    CHECK(DeleteFileW(directory));
    CHECK(CreateDirectoryW(directory, NULL));
    swprintf_s(disk, MAX_PATH, L"%s\\disk.vhdx", directory);
    swprintf_s(path, MAX_PATH, L"%s\\display_settings.json", directory);
    AsbDisplaySettings s = read_settings();
    CHECK(s.resolution.windowed_width == 1920 && s.resolution.windowed_height == 1080);
    CHECK(s.resolution.fullscreen_width == 1920 && s.resolution.fullscreen_height == 1080);
    CHECK(!s.resolution.fullscreen_override && !s.fullscreen_pinned && !s.suppress_fullscreen_reminder);
    CHECK(!s.transmit_keyboard_hotkeys && s.share_clipboard);
    CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES); /* pure reads */
    fixture("{\"transmitKeyboardHotkeys\":1,\"shareClipboard\":false,\"future\":{\"text\":\"caf\xc3\xa9\",\"array\":[1,true,null]}}");
    s = read_settings();
    CHECK(s.transmit_keyboard_hotkeys && !s.share_clipboard && s.resolution.windowed_width == 1920);
    s.fullscreen_pinned = TRUE;
    OK(asb_display_settings_update(disk, ASB_DISPLAY_PIN, &s));
    char *bytes = contents();
    CHECK(strstr(bytes, "\"future\":{\"text\":\"caf\xc3\xa9\",\"array\":[1,true,null]}"));
    free(bytes);
    const char *bad[] = {"\"\"", "-1", "null", "\"1920\"", "1920.5", "1e3", "639", "7681", "2147483648", "true"};
    for (int i = 0; i < (int)ARRAYSIZE(bad); ++i) {
        char json[512];
        sprintf_s(json, sizeof(json), "{\"windowedWidth\":%s,\"windowedHeight\":720,\"fullscreenWidth\":%s,\"fullscreenHeight\":1440}", bad[i], bad[i]);
        fixture(json); s = read_settings();
        CHECK(s.resolution.windowed_width == 1920 && s.resolution.windowed_height == 1080);
        CHECK(s.resolution.fullscreen_width == 1920 && s.resolution.fullscreen_height == 1080);
    }
    fixture("{\"windowedWidth\":1920,\"windowedHeight\":null,\"fullscreenWidth\":2560,\"fullscreenHeight\":1440}");
    s = read_settings();
    CHECK(s.resolution.windowed_width == 1920 && s.resolution.windowed_height == 1080);
    CHECK(s.resolution.fullscreen_width == 2560 && s.resolution.fullscreen_height == 1440);
    fixture("{\"windowedWidth\":1280,\"windowedHeight\":720,\"fullscreenWidth\":null,\"fullscreenHeight\":1440}");
    s = read_settings();
    CHECK(s.resolution.windowed_width == 1280 && s.resolution.windowed_height == 720);
    CHECK(s.resolution.fullscreen_width == 1920 && s.resolution.fullscreen_height == 1080);
    fixture("{\"windowedWidth\":1920,\"windowedHeight\":479,\"fullscreenWidth\":1920,\"fullscreenHeight\":4321}");
    s = read_settings();
    CHECK(s.resolution.windowed_height == 1080 && s.resolution.fullscreen_height == 1080);
    fixture("{\"windowedWidth\":2560,\"fullscreenHeight\":1440}");
    s = read_settings();
    CHECK(s.resolution.windowed_width == 1920 && s.resolution.fullscreen_height == 1080);
    fixture("{}");
    s.resolution = (AsbDisplayResolution){640,480,TRUE,7680,4320};
    OK(asb_display_settings_update(disk, ASB_DISPLAY_RESOLUTION, &s));
    AsbDisplaySettings roundtrip = read_settings();
    CHECK(roundtrip.resolution.windowed_width == 640 && roundtrip.resolution.windowed_height == 480);
    CHECK(roundtrip.resolution.fullscreen_width == 7680 && roundtrip.resolution.fullscreen_height == 4320 && roundtrip.resolution.fullscreen_override);
    /* Disabled override retains its custom dimensions. */
    s.resolution.fullscreen_override = FALSE;
    OK(asb_display_settings_update(disk, ASB_DISPLAY_RESOLUTION, &s));
    CHECK(read_settings().resolution.fullscreen_width == 7680);
    bytes = contents();
    s.resolution.windowed_width = 639;
    CHECK(asb_display_settings_update(disk, 1, &s) == E_INVALIDARG);
    s.resolution = (AsbDisplayResolution){640,479,FALSE,1920,1080};
    CHECK(asb_display_settings_update(disk, 1, &s) == E_INVALIDARG);
    s.resolution = (AsbDisplayResolution){7681,1080,FALSE,1920,1080};
    CHECK(asb_display_settings_update(disk, 1, &s) == E_INVALIDARG);
    s.resolution = (AsbDisplayResolution){1920,4321,FALSE,1920,1080};
    CHECK(asb_display_settings_update(disk, 1, &s) == E_INVALIDARG);
    s.resolution = (AsbDisplayResolution){1920,1080,FALSE,639,1080};
    CHECK(asb_display_settings_update(disk, 1, &s) == E_INVALIDARG);
    s.resolution.fullscreen_width = 1920; s.resolution.fullscreen_override = 2;
    CHECK(asb_display_settings_update(disk, 1, &s) == E_INVALIDARG);
    s.fullscreen_pinned = 2;
    CHECK(asb_display_settings_update(disk, 8, &s) == E_INVALIDARG);
    s.fullscreen_pinned = TRUE;
    CHECK(asb_display_settings_update(disk, 32, &s) == E_INVALIDARG);
    CHECK(asb_display_settings_read(NULL, &s) == E_INVALIDARG);
    CHECK(asb_display_settings_read(disk, NULL) == E_INVALIDARG);
    CHECK(asb_display_settings_update(disk, 8, NULL) == E_INVALIDARG);
    /* Permit reads, deny replacement: force a failure after the temp is written. */
    HANDLE held = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(FAILED(asb_display_settings_update(disk, ASB_DISPLAY_PIN, &s)));
    CHECK(CloseHandle(held));
    char *after = contents(); CHECK(!strcmp(bytes, after)); free(bytes); free(after);
    fixture("{\"broken\":");
    CHECK(FAILED(asb_display_settings_update(disk, 8, &s)));
    bytes = contents(); CHECK(!strcmp(bytes, "{\"broken\":")); free(bytes);
    /* Deliberately stale snapshots must only update their selected field. */
    fixture("{\"future\":[1,2,3]}");
    AsbDisplaySettings stale = read_settings();
    s = stale; s.transmit_keyboard_hotkeys = TRUE;
    OK(asb_display_settings_update(disk, 2, &s));
    stale.share_clipboard = FALSE;
    OK(asb_display_settings_update(disk, 4, &stale));
    stale.resolution = (AsbDisplayResolution){2560,1440,TRUE,3840,2160};
    OK(asb_display_settings_update(disk, 1, &stale));
    stale.fullscreen_pinned = TRUE;
    OK(asb_display_settings_update(disk, 8, &stale));
    stale.suppress_fullscreen_reminder = TRUE;
    OK(asb_display_settings_update(disk, 16, &stale));
    s = read_settings();
    CHECK(s.transmit_keyboard_hotkeys && !s.share_clipboard && s.fullscreen_pinned && s.suppress_fullscreen_reminder);
    CHECK(s.resolution.windowed_width == 2560 && s.resolution.windowed_height == 1440);
    CHECK(s.resolution.fullscreen_override && s.resolution.fullscreen_width == 3840 && s.resolution.fullscreen_height == 2160);
    fixture("{\"future\":[1,2,3]}");
    /* Competing processes exercise the same mutex used by app/core consumers. */
    CHECK(GetModuleFileNameW(NULL, exe, MAX_PATH));
    HANDLE children[5];
    for (int i = 0; i < 5; ++i) {
        wchar_t command[MAX_PATH * 3];
        swprintf_s(command, ARRAYSIZE(command), L"\"%s\" worker \"%s\" %u", exe, disk, 1u << i);
        STARTUPINFOW startup = {sizeof(startup)};
        PROCESS_INFORMATION process;
        CHECK(CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
        CloseHandle(process.hThread); children[i] = process.hProcess;
    }
    CHECK(WaitForMultipleObjects(5, children, TRUE, 30000) == WAIT_OBJECT_0);
    for (int i = 0; i < 5; ++i) {
        DWORD code; CHECK(GetExitCodeProcess(children[i], &code) && !code); CloseHandle(children[i]);
    }
    s = read_settings();
    CHECK(s.transmit_keyboard_hotkeys && !s.share_clipboard && s.fullscreen_pinned && s.suppress_fullscreen_reminder);
    CHECK(s.resolution.windowed_width == 2560 && s.resolution.windowed_height == 1440);
    CHECK(s.resolution.fullscreen_override && s.resolution.fullscreen_width == 3840 && s.resolution.fullscreen_height == 2160);
    bytes = contents(); CHECK(strstr(bytes, "\"future\":[1,2,3]")); free(bytes);
    CHECK(DeleteFileW(path));
    AsbDisplaySettings pin = {0}; pin.fullscreen_pinned = TRUE;
    OK(asb_display_settings_update(disk, ASB_DISPLAY_PIN, &pin));
    s = read_settings();
    CHECK(s.fullscreen_pinned && !s.transmit_keyboard_hotkeys && s.share_clipboard);
    CHECK(s.resolution.windowed_width == 1920 && s.resolution.fullscreen_height == 1080);
    /* A fresh process reloads the saved pin. Another VM defaults independently. */
    wchar_t command[MAX_PATH * 3], other_directory[MAX_PATH], other_disk[MAX_PATH], other_path[MAX_PATH];
    swprintf_s(command, ARRAYSIZE(command), L"\"%s\" readpin \"%s\"", exe, disk);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    CHECK(CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
    CloseHandle(process.hThread);
    CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code; CHECK(GetExitCodeProcess(process.hProcess, &code) && !code); CloseHandle(process.hProcess);
    swprintf_s(other_directory, ARRAYSIZE(other_directory), L"%s\\other-vm", directory);
    CHECK(CreateDirectoryW(other_directory, NULL));
    swprintf_s(other_disk, ARRAYSIZE(other_disk), L"%s\\disk.vhdx", other_directory);
    swprintf_s(other_path, ARRAYSIZE(other_path), L"%s\\display_settings.json", other_directory);
    OK(asb_display_settings_read(other_disk, &s)); CHECK(!s.fullscreen_pinned);
    OK(asb_display_settings_update(other_disk, ASB_DISPLAY_PIN, &s));
    CHECK(read_settings().fullscreen_pinned);
    /* Reminder suppression shares the same per-VM file and survives restart. */
    pin.suppress_fullscreen_reminder = TRUE;
    OK(asb_display_settings_update(disk, ASB_DISPLAY_REMINDER, &pin));
    swprintf_s(command, ARRAYSIZE(command), L"\"%s\" readreminder \"%s\"", exe, disk);
    CHECK(CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
    CloseHandle(process.hThread);
    CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &code) && !code); CloseHandle(process.hProcess);
    OK(asb_display_settings_read(other_disk, &s)); CHECK(!s.suppress_fullscreen_reminder);
    CHECK(DeleteFileW(other_path)); CHECK(RemoveDirectoryW(other_directory));
    pin.fullscreen_pinned = FALSE;
    OK(asb_display_settings_update(disk, ASB_DISPLAY_PIN, &pin));
    CHECK(!read_settings().fullscreen_pinned);
    CHECK(DeleteFileW(path));
    /* Failed replacements must not leave temporary files behind. */
    CHECK(RemoveDirectoryW(directory));
    puts("PASS display settings: defaults, legacy, malformed pairs, bounds, atomic failure, stale updates, 500 cross-process updates");
    return 0;
}
