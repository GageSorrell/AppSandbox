/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_display_toggle
 *
 * @file      remote_windows_display_toggle.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Guest-only manual-testing helper for the private sharing display (eh6.3). Run in the
   signed-in guest session as a standard user:

       asb-private-display.exe start [width height]   (default 2560x1440)
       asb-private-display.exe stop
       asb-private-display.exe status

   start journals a random lease token to %LOCALAPPDATA%\AppSandbox\private-display.lease
   BEFORE activating, then leaves the private output in place after exiting (the driver
   never removes it on handle close). stop restores with that token and removes the
   output. Nothing is parked by this tool; the display watcher returns any window that
   lands on the private output to the desktop. Not part of the product. */
#include "../agent/remote_windows_display.c"
#include <bcrypt.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "bcrypt.lib")

static BOOL lease_path(WCHAR *path, DWORD size)
{
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, size);
    if (!length || length >= size - 64) return FALSE;
    wcscat_s(path, size, L"\\AppSandbox");
    CreateDirectoryW(path, NULL);
    wcscat_s(path, size, L"\\private-display.lease");
    return TRUE;
}

static UINT64 read_lease(const WCHAR *path)
{
    FILE *file = NULL;
    unsigned long long token = 0;
    if (_wfopen_s(&file, path, L"r") || !file) return 0;
    if (fscanf_s(file, "%llx", &token) != 1) token = 0;
    fclose(file);
    return token;
}

static BOOL write_lease(const WCHAR *path, UINT64 token)
{
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"w") || !file) return FALSE;
    BOOL ok = fprintf(file, "%016llx\n", (unsigned long long)token) > 0;
    return fclose(file) == 0 && ok;
}

static DWORD restore_nothing(void *data) { (void)data; return ERROR_SUCCESS; }

static const char *state_name(UINT32 state)
{
    return state == ASB_PRIVATE_ABSENT ? "off" : state == ASB_PRIVATE_READY ? "on" :
           state == ASB_PRIVATE_RESTORING ? "restoring" : "unknown";
}

static int status(void)
{
    AsbRwDisplay display;
    UINT32 state = 0, width = 0, height = 0;
    DWORD error = asb_rw_display_open(&display, 1); /* QUERY ignores the token */
    if (!error) error = asb_rw_display_query(&display, &state, &width, &height);
    asb_rw_display_close(&display);
    if (error) {
        printf("Could not query the private display (error %lu). Is AppSandboxVDD installed?\n", error);
        return 1;
    }
    if (state == ASB_PRIVATE_ABSENT) printf("Private display: off\n");
    else printf("Private display: %s, %ux%u\n", state_name(state), width, height);
    return 0;
}

static UINT64 new_token(void)
{
    UINT64 token = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&token, sizeof(token), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !token)
        token = GetTickCount64() | 1;
    return token;
}

static int start(const WCHAR *path, UINT32 width, UINT32 height)
{
    UINT64 saved = read_lease(path);
    UINT64 token = saved ? saved : new_token(); /* a new lease every time it was off */
    AsbRwDisplay display;
    UINT32 state = 0;
    DWORD error = asb_rw_display_open(&display, token);
    if (!error) error = asb_rw_display_query(&display, &state, NULL, NULL);
    if (error) {
        printf("Could not open the private display (error %lu). Is AppSandboxVDD installed?\n", error);
        asb_rw_display_close(&display);
        return 1;
    }
    if (state != ASB_PRIVATE_ABSENT && !saved) {
        printf("The private display is %s under a lease this tool does not hold; not touching it.\n",
               state_name(state));
        asb_rw_display_close(&display);
        return 1;
    }
    if (state == ASB_PRIVATE_ABSENT) {
        if (saved) display.token = token = new_token(); /* stale lease from an earlier run */
        /* Journal the lease before the output can exist. */
        if (!write_lease(path, token)) {
            printf("Could not save the lease to %ls; not starting.\n", path);
            asb_rw_display_close(&display);
            return 1;
        }
    }
    error = asb_rw_display_activate(&display, width, height);
    if (error) {
        printf("Starting the private display at %ux%u failed (error %lu); removing it again.\n",
               width, height, error);
        if (!asb_rw_display_restore(&display, restore_nothing, NULL)) DeleteFileW(path);
        asb_rw_display_close(&display);
        return 1;
    }
    printf("Private display on: %ux%u at (%ld,%ld), right of the desktop (%ldx%ld, still primary).\n",
           width, height, display.private_rect.left, display.private_rect.top,
           display.desktop.right - display.desktop.left, display.desktop.bottom - display.desktop.top);
    asb_rw_display_close(&display);
    return 0;
}

static int stop(const WCHAR *path)
{
    UINT64 token = read_lease(path);
    AsbRwDisplay display;
    UINT32 state = 0;
    DWORD error = asb_rw_display_open(&display, token ? token : 1); /* QUERY ignores the token */
    if (!error) error = asb_rw_display_query(&display, &state, NULL, NULL);
    if (error) {
        printf("Could not open the private display (error %lu).\n", error);
        asb_rw_display_close(&display);
        return 1;
    }
    if (state == ASB_PRIVATE_ABSENT) {
        printf("Private display is already off.\n");
        DeleteFileW(path);
        asb_rw_display_close(&display);
        return 0;
    }
    if (!token) {
        printf("The private display is %s but no lease was saved at %ls.\n"
               "If the eh6.3 native check started it, it prints a --recover token.\n", state_name(state), path);
        asb_rw_display_close(&display);
        return 1;
    }
    error = asb_rw_display_restore(&display, restore_nothing, NULL);
    asb_rw_display_close(&display);
    if (error) {
        printf("Stopping the private display failed (error %lu); the lease is kept for a retry.\n", error);
        return 1;
    }
    DeleteFileW(path);
    printf("Private display off.\n");
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    WCHAR path[MAX_PATH];
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!lease_path(path, MAX_PATH)) {
        printf("LOCALAPPDATA is not usable.\n");
        return 1;
    }
    if (argc >= 2 && !_wcsicmp(argv[1], L"start")) {
        UINT32 width = argc >= 4 ? (UINT32)_wtoi(argv[2]) : 2560;
        UINT32 height = argc >= 4 ? (UINT32)_wtoi(argv[3]) : 1440;
        return start(path, width, height);
    }
    if (argc >= 2 && !_wcsicmp(argv[1], L"stop")) return stop(path);
    if (argc >= 2 && !_wcsicmp(argv[1], L"status")) return status();
    printf("usage: asb-private-display.exe start [width height] | stop | status\n");
    return 2;
}
