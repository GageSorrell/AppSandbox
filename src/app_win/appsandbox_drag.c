/**
 *
 *
 * @module AppSandbox/src/app_win/appsandbox_drag
 *
 * @file      appsandbox_drag.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * AppSandboxDrag.exe: the medium-integrity Remote Windows drag broker (AppSandbox-eh6.9).
 *
 *   AppSandboxDrag.exe --vm <runtime GUID> --cookie <hex> --parent <pid> [--name <VM name>]
 *
 * Started by AppSandbox.exe (one per Windows guest manager) with the shell's token, so host OLE
 * drag and drop with Explorer and other medium-integrity applications works while AppSandbox.exe
 * stays elevated. Exits when the parent process ends (or is terminated by it). No UI besides
 * the drop overlay and the copy progress window; logs to
 * %LOCALAPPDATA%\AppSandbox\RemoteWindows\drag-broker.log. See remote_windows_drag_host.h.
 */
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../backend_win/remote_windows_drag_host.h"
#include "../../tools/agent/remote_windows_files_win32.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static CRITICAL_SECTION g_log_lock;
static WCHAR g_log_path[MAX_PATH];

static void log_line(void *ctx, const char *message)
{
    (void)ctx;
    FILE *f = NULL;
    SYSTEMTIME t;
    if (!g_log_path[0]) return;
    EnterCriticalSection(&g_log_lock);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(g_log_path, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > 1024 * 1024)) {
        WCHAR old[MAX_PATH];
        swprintf_s(old, MAX_PATH, L"%s.old", g_log_path);
        MoveFileExW(g_log_path, old, MOVEFILE_REPLACE_EXISTING);
    }
    if (!_wfopen_s(&f, g_log_path, L"a") && f) {
        GetLocalTime(&t);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, GetCurrentProcessId(), message);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    AsbRwDragHostConfig cfg;
    WCHAR stage[MAX_PATH] = L"", dir[MAX_PATH] = L"", name[128] = L"the guest";
    DWORD parent = 0;
    BOOL have_vm = FALSE;
    (void)inst; (void)prev; (void)cmd; (void)show;
    InitializeCriticalSection(&g_log_lock);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--vm")) {
            WCHAR braced[64];
            swprintf_s(braced, ARRAYSIZE(braced), argv[i + 1][0] == L'{' ? L"%s" : L"{%s}", argv[i + 1]);
            have_vm = SUCCEEDED(CLSIDFromString(braced, &cfg.vm_id));
            ++i;
        } else if (!_wcsicmp(argv[i], L"--cookie")) {
            cfg.cookie = _wcstoui64(argv[++i], NULL, 16);
        } else if (!_wcsicmp(argv[i], L"--parent")) {
            parent = (DWORD)wcstoul(argv[++i], NULL, 10);
        } else if (!_wcsicmp(argv[i], L"--name")) {
            wcsncpy_s(name, ARRAYSIZE(name), argv[++i], _TRUNCATE);
        }
    }
    if (argv) LocalFree(argv);
    if (asb_rw_files_user_dir(L"", dir, ARRAYSIZE(dir))) swprintf_s(g_log_path, MAX_PATH, L"%sdrag-broker.log", dir);
    if (!have_vm || !cfg.cookie) {
        log_line(NULL, "start: missing --vm or --cookie");
        return 2;
    }
    /* One broker per manager cookie. */
    WCHAR mutex_name[96];
    swprintf_s(mutex_name, ARRAYSIZE(mutex_name), L"Local\\AppSandbox.RemoteWindows.DragBroker.%016llx", (unsigned long long)cfg.cookie);
    HANDLE mutex = CreateMutexW(NULL, TRUE, mutex_name);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        log_line(NULL, "start: another broker serves this manager");
        return 3;
    }
    HANDLE parent_process = parent ? OpenProcess(SYNCHRONIZE, FALSE, parent) : NULL;
    if (parent && !parent_process) {
        log_line(NULL, "start: parent process is gone");
        return 4;
    }
    if (!asb_rw_files_user_dir(L"DragStage", stage, ARRAYSIZE(stage))) stage[0] = 0;
    cfg.log = log_line;
    cfg.stage_parent = stage;
    cfg.guest_name = name;
    {
        char text[256];
        snprintf(text, sizeof(text), "start: broker for cookie %016llx, parent %lu, guest \"%ls\"", (unsigned long long)cfg.cookie,
                 parent, name);
        log_line(NULL, text);
    }
    AsbRwDragHost *host = asb_rw_drag_host_start(&cfg);
    if (!host) {
        log_line(NULL, "start: broker failed to start");
        return 1;
    }
    if (parent_process) WaitForSingleObject(parent_process, INFINITE);
    else Sleep(INFINITE);
    log_line(NULL, "exit: parent ended");
    asb_rw_drag_host_stop(host);
    if (parent_process) CloseHandle(parent_process);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
