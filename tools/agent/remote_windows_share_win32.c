/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_share_win32
 *
 * @file      remote_windows_share_win32.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Win32 side of guest window parking (AppSandbox-eh6.4). */
#define COBJMACROS
#include "remote_windows_share_win32.h"
#include "remote_windows_inventory.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")

#define W(ctx) ((AsbRwWin32 *)(ctx))

static int op_probe(void *ctx, HWND hwnd, AsbRwIdentity *live)
{
    (void)ctx;
    return asb_rw_probe_identity(NULL, (uint64_t)(ULONG_PTR)hwnd, live);
}

/* Never wait on a hung application: restoration of such a window is retried instead. */
static BOOL op_responsive(void *ctx, HWND hwnd)
{
    DWORD_PTR result = 0;
    (void)ctx;
    if (IsHungAppWindow(hwnd)) return FALSE;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) != 0;
}

static BOOL op_get_placement(void *ctx, HWND hwnd, WINDOWPLACEMENT *wp)
{
    (void)ctx;
    wp->length = sizeof(*wp);
    return GetWindowPlacement(hwnd, wp);
}

static BOOL op_set_placement(void *ctx, HWND hwnd, const WINDOWPLACEMENT *wp)
{
    (void)ctx;
    return SetWindowPlacement(hwnd, wp);
}

static BOOL op_get_rect(void *ctx, HWND hwnd, RECT *rect)
{
    (void)ctx;
    return GetWindowRect(hwnd, rect);
}

static BOOL op_show(void *ctx, HWND hwnd, int cmd)
{
    (void)ctx;
    ShowWindow(hwnd, cmd);   /* returns the previous visibility, not success */
    return IsWindow(hwnd);
}

static BOOL op_position(void *ctx, HWND hwnd, int x, int y, UINT flags)
{
    (void)ctx;
    return SetWindowPos(hwnd, NULL, x, y, 0, 0, flags | SWP_NOSIZE | SWP_NOZORDER);
}

static UINT op_dpi(void *ctx, HWND hwnd)
{
    (void)ctx;
    return GetDpiForWindow(hwnd);
}

static DWORD op_get_ex_style(void *ctx, HWND hwnd)
{
    (void)ctx;
    return (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
}

static BOOL op_set_ex_style(void *ctx, HWND hwnd, DWORD ex_style)
{
    (void)ctx;
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)ex_style);
    return previous != 0 || GetLastError() == 0;
}

static HWND op_get_owner(void *ctx, HWND hwnd)
{
    (void)ctx;
    return GetWindow(hwnd, GW_OWNER);
}

static BOOL op_set_owner(void *ctx, HWND hwnd, HWND owner)
{
    (void)ctx;
    SetLastError(0);
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner);
    return GetLastError() == 0;
}

static BOOL op_mark(void *ctx, HWND hwnd, BOOL set)
{
    (void)ctx;
    if (set) return SetPropW(hwnd, ASB_PRIVATE_PARKED_PROPERTY, (HANDLE)(ULONG_PTR)1);
    RemovePropW(hwnd, ASB_PRIVATE_PARKED_PROPERTY);
    return TRUE;
}

static BOOL op_taskbar(void *ctx, HWND hwnd, BOOL add)
{
    AsbRwWin32 *w = W(ctx);
    if (!w->taskbar) {
        ITaskbarList *list = NULL;
        if (FAILED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList, (void **)&list)))
            return FALSE;
        if (FAILED(ITaskbarList_HrInit(list))) {
            ITaskbarList_Release(list);
            return FALSE;
        }
        w->taskbar = list;
    }
    ITaskbarList *list = (ITaskbarList *)w->taskbar;
    return SUCCEEDED(add ? ITaskbarList_AddTab(list, hwnd) : ITaskbarList_DeleteTab(list, hwnd));
}

typedef struct OwnedScan { HWND root; HWND *out; uint32_t max, count; } OwnedScan;

static BOOL CALLBACK owned_window(HWND hwnd, LPARAM lp)
{
    OwnedScan *scan = (OwnedScan *)lp;
    if (scan->count >= scan->max || hwnd == scan->root || !IsWindowVisible(hwnd)) return TRUE;
    if ((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return TRUE;   /* dialogs only */
    HWND owner = GetWindow(hwnd, GW_OWNER);
    for (int depth = 0; owner && depth < (int)ASB_RW_MAX_OWNER_DEPTH; ++depth) {
        if (owner == scan->root) {
            scan->out[scan->count++] = hwnd;
            break;
        }
        owner = GetWindow(owner, GW_OWNER);
    }
    return TRUE;
}

static uint32_t op_owned(void *ctx, HWND root, HWND *out, uint32_t max)
{
    OwnedScan scan = {root, out, max, 0};
    (void)ctx;
    EnumWindows(owned_window, (LPARAM)&scan);
    return scan.count;
}

static DWORD open_display(AsbRwWin32 *w, uint64_t lease)
{
    if (w->display_open && w->display.token == lease) return ERROR_SUCCESS;
    if (w->display_open) asb_rw_display_close(&w->display);
    w->display_open = FALSE;
    DWORD error = asb_rw_display_open(&w->display, lease);
    if (!error) w->display_open = TRUE;
    return error;
}

static DWORD op_display_activate(void *ctx, uint64_t lease, uint32_t width, uint32_t height,
                                 RECT *desktop, RECT *private_rect)
{
    AsbRwWin32 *w = W(ctx);
    if (w->fake_display) { *desktop = w->fake_desktop; *private_rect = w->fake_private; return ERROR_SUCCESS; }
    DWORD error = open_display(w, lease);
    if (!error) error = asb_rw_display_activate(&w->display, width, height);
    if (!error) { *desktop = w->display.desktop; *private_rect = w->display.private_rect; }
    return error;
}

static DWORD op_display_refresh(void *ctx, uint32_t width, uint32_t height, RECT *desktop, RECT *private_rect)
{
    AsbRwWin32 *w = W(ctx);
    if (w->fake_display) { *desktop = w->fake_desktop; *private_rect = w->fake_private; return ERROR_SUCCESS; }
    if (!w->display_open) return ERROR_NOT_READY;
    DWORD error = asb_rw_display_refresh(&w->display, width, height);
    if (!error) { *desktop = w->display.desktop; *private_rect = w->display.private_rect; }
    return error;
}

static DWORD op_display_release(void *ctx, uint64_t lease, DWORD (*restore)(void *arg), void *arg)
{
    AsbRwWin32 *w = W(ctx);
    if (w->fake_display) return restore(arg);
    DWORD error = open_display(w, lease);
    if (!error) error = asb_rw_display_restore(&w->display, restore, arg);
    return error;
}

/* Atomic, durable replacement: write a sibling file, flush it, then rename over the journal. */
static BOOL op_journal_write(void *ctx, const uint8_t *bytes, size_t len)
{
    AsbRwWin32 *w = W(ctx);
    WCHAR tmp[MAX_PATH + 8];
    if (!w->journal_path[0]) return FALSE;
    if (!len) return DeleteFileW(w->journal_path) || GetLastError() == ERROR_FILE_NOT_FOUND;
    swprintf_s(tmp, MAX_PATH + 8, L"%s.tmp", w->journal_path);
    HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL ok = len <= MAXDWORD && WriteFile(file, bytes, (DWORD)len, &written, NULL) && written == len &&
              FlushFileBuffers(file);
    CloseHandle(file);
    ok = ok && MoveFileExW(tmp, w->journal_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static void op_log(void *ctx, const char *message)
{
    if (W(ctx)->log) W(ctx)->log(message);
}

BOOL asb_rw_journal_read(const WCHAR *path, uint8_t **bytes, size_t *len)
{
    LARGE_INTEGER size;
    *bytes = NULL;
    *len = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    BOOL ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
    if (ok && size.QuadPart) {
        DWORD read = 0;
        *bytes = (uint8_t *)malloc((size_t)size.QuadPart);
        ok = *bytes && ReadFile(file, *bytes, (DWORD)size.QuadPart, &read, NULL) && read == (DWORD)size.QuadPart;
        if (ok) *len = read;
        else { free(*bytes); *bytes = NULL; }
    }
    CloseHandle(file);
    return ok;
}

BOOL asb_rw_state_dir(WCHAR *path, DWORD capacity)
{
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base))) return FALSE;
    int n = swprintf_s(path, capacity, L"%s\\AppSandbox", base);
    CoTaskMemFree(base);
    if (n <= 0) return FALSE;
    CreateDirectoryW(path, NULL);
    if (wcscat_s(path, capacity, L"\\RemoteWindows")) return FALSE;
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void asb_rw_win32_init(AsbRwWin32 *w, const WCHAR *journal_path, void (*log)(const char *message))
{
    ZeroMemory(w, sizeof(*w));
    w->display.driver = INVALID_HANDLE_VALUE;
    if (journal_path) wcscpy_s(w->journal_path, MAX_PATH, journal_path);
    w->log = log;
}

void asb_rw_win32_ops(AsbRwWin32 *w, AsbRwShareOps *ops)
{
    ZeroMemory(ops, sizeof(*ops));
    ops->ctx = w;
    ops->probe = op_probe;
    ops->responsive = op_responsive;
    ops->get_placement = op_get_placement;
    ops->set_placement = op_set_placement;
    ops->get_rect = op_get_rect;
    ops->show = op_show;
    ops->position = op_position;
    ops->dpi = op_dpi;
    ops->get_ex_style = op_get_ex_style;
    ops->set_ex_style = op_set_ex_style;
    ops->get_owner = op_get_owner;
    ops->set_owner = op_set_owner;
    ops->mark = op_mark;
    ops->taskbar = op_taskbar;
    ops->owned = op_owned;
    ops->display_activate = op_display_activate;
    ops->display_refresh = op_display_refresh;
    ops->display_release = op_display_release;
    ops->journal_write = op_journal_write;
    ops->log = op_log;
}

void asb_rw_win32_close(AsbRwWin32 *w)
{
    if (w->taskbar) ITaskbarList_Release((ITaskbarList *)w->taskbar);
    w->taskbar = NULL;
    if (w->display_open) asb_rw_display_close(&w->display);
    w->display_open = FALSE;
}
