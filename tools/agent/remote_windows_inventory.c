/* Guest-side Win32 collector for the Remote Windows inventory (AppSandbox-eh6.2). */
#include "remote_windows_inventory.h"

#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

static uint64_t process_start(HANDLE process)
{
    FILETIME created, exited, kernel, user;
    if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
}

/* Elevated, or unreadable (treated as ineligible): a standard-user helper never drives these. */
static uint8_t process_elevation(HANDLE process)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation = { 0 };
    DWORD size = 0;
    uint8_t result = ASB_RW_ELEVATION_UNKNOWN;
    if (process && OpenProcessToken(process, TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
            result = elevation.TokenIsElevated ? ASB_RW_ELEVATION_YES : ASB_RW_ELEVATION_NO;
        CloseHandle(token);
    }
    return result;
}

static void copy_text(uint16_t *dst, uint16_t *dst_len, uint16_t cap, const wchar_t *src)
{
    size_t n = wcslen(src);
    if (n > cap) n = cap;
    memcpy(dst, src, n * sizeof(uint16_t));
    *dst_len = (uint16_t)n;
}

/* Application name: the executable's FileDescription ("Notepad", "Windows Explorer"), else its
   file name without extension. Used to tell duplicate titles apart. */
static void app_name(HANDLE process, AsbRwWindowFacts *f)
{
    wchar_t path[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(path);
    f->app_len = 0;
    if (!process || !QueryFullProcessImageNameW(process, 0, path, &n)) return;
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path, &ignored);
    if (size && size < 1024 * 1024) {
        void *info = malloc(size);
        if (info && GetFileVersionInfoW(path, 0, size, info)) {
            struct { WORD language, codepage; } *tr = NULL;
            UINT tr_len = 0;
            if (VerQueryValueW(info, L"\\VarFileInfo\\Translation", (void **)&tr, &tr_len) && tr_len >= 4) {
                wchar_t key[64];
                wchar_t *desc = NULL;
                UINT desc_len = 0;
                swprintf_s(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->language, tr->codepage);
                if (VerQueryValueW(info, key, (void **)&desc, &desc_len) && desc && desc_len > 1 && desc[0])
                    copy_text(f->app, &f->app_len, ASB_RW_MAX_APP_NAME, desc);
            }
        }
        free(info);
    }
    if (!f->app_len) {
        wchar_t *base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        wchar_t *dot = wcsrchr(base, L'.');
        if (dot) *dot = 0;
        copy_text(f->app, &f->app_len, ASB_RW_MAX_APP_NAME, base);
    }
}

DWORD asb_rw_active_session(void)
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

static void owner_chain(HWND start, AsbRwWindowFacts *f)
{
    HWND h = start;
    f->owner_depth = 0;
    while (h && f->owner_depth < ASB_RW_MAX_OWNER_DEPTH) {
        f->owner_chain[f->owner_depth++] = (uint64_t)(ULONG_PTR)h;
        h = GetWindow(h, GW_OWNER);
    }
}

BOOL asb_rw_collect_facts(HWND hwnd, DWORD helper_pid, DWORD active_session, AsbRwWindowFacts *f)
{
    DWORD pid = 0, tid;
    wchar_t text[ASB_RW_MAX_TITLE + 1];
    memset(f, 0, sizeof(*f));
    tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || !pid) return FALSE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    f->identity.hwnd = (uint64_t)(ULONG_PTR)hwnd;
    f->identity.pid = pid;
    f->identity.thread_id = tid;
    f->identity.process_start = process_start(process);
    f->elevation = process_elevation(process);
    app_name(process, f);
    if (process) CloseHandle(process);

    if (!ProcessIdToSessionId(pid, (DWORD *)&f->session_id)) f->session_id = (uint32_t)-1;
    f->active_session_id = active_session;
    f->is_helper = helper_pid && pid == helper_pid;
    f->style = (uint32_t)GetWindowLongPtrW(hwnd, GWL_STYLE);
    f->ex_style = (uint32_t)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    f->visible = IsWindowVisible(hwnd) ? 1 : 0;
    f->minimized = IsIconic(hwnd) ? 1 : 0;
    f->foreground = GetForegroundWindow() == hwnd;
    DWORD cloaked = 0;
    f->cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;

    int n = GetClassNameW(hwnd, text, ASB_RW_MAX_CLASS_NAME + 1);
    text[n > 0 ? n : 0] = 0;
    copy_text(f->class_name, &f->class_len, ASB_RW_MAX_CLASS_NAME, text);
    n = GetWindowTextW(hwnd, text, ASB_RW_MAX_TITLE + 1);
    text[n > 0 ? n : 0] = 0;
    copy_text(f->title, &f->title_len, ASB_RW_MAX_TITLE, text);

    HWND owner = GetWindow(hwnd, GW_OWNER);
    f->owner = (uint64_t)(ULONG_PTR)owner;
    if (owner) {
        owner_chain(owner, f);
    } else if (f->class_len == 6 && !memcmp(f->class_name, L"#32768", 6 * sizeof(uint16_t))) {
        /* Unowned "#32768" menus are associated through their thread's menu owner. */
        GUITHREADINFO gui = { sizeof(gui) };
        if (GetGUIThreadInfo(tid, &gui) && gui.hwndMenuOwner) {
            f->menu_owner = (uint64_t)(ULONG_PTR)gui.hwndMenuOwner;
            owner_chain(gui.hwndMenuOwner, f);
        }
    }

    RECT r = { 0 };
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || IsRectEmpty(&r))
        GetWindowRect(hwnd, &r);
    f->rect.left = r.left; f->rect.top = r.top; f->rect.right = r.right; f->rect.bottom = r.bottom;
    f->dpi = GetDpiForWindow(hwnd);
    return IsWindow(hwnd);
}

typedef struct { HWND *list; uint32_t max, count, overflow; } Collect;

static BOOL CALLBACK collect_hwnd(HWND hwnd, LPARAM lp)
{
    Collect *c = (Collect *)lp;
    if (c->count < c->max) c->list[c->count++] = hwnd;
    else c->overflow = 1;
    return TRUE;
}

uint32_t asb_rw_enumerate_windows(AsbRwWindowFacts *out, uint32_t max, DWORD helper_pid, uint32_t *count)
{
    Collect c = { NULL, max, 0, 0 };
    *count = 0;
    if (!max) return ASB_RW_ERR_LIMIT;
    c.list = (HWND *)calloc(max, sizeof(HWND));
    if (!c.list) return ASB_RW_ERR_INTERNAL;
    EnumWindows(collect_hwnd, (LPARAM)&c);
    DWORD session = asb_rw_active_session();
    for (uint32_t i = 0; i < c.count; ++i)
        if (asb_rw_collect_facts(c.list[i], helper_pid, session, &out[*count])) (*count)++;
    free(c.list);
    return c.overflow ? ASB_RW_ERR_LIMIT : ASB_RW_OK;
}

typedef struct { DWORD pid; uint64_t start; } StartCache;

uint32_t asb_rw_scan_visible_windows(AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    Collect c = { NULL, max * 4u, 0, 0 };
    StartCache cache[64];
    uint32_t cached = 0, result = ASB_RW_OK;
    *count = 0;
    if (!max) return ASB_RW_ERR_LIMIT;
    c.list = (HWND *)calloc(c.max, sizeof(HWND));
    if (!c.list) return ASB_RW_ERR_INTERNAL;
    EnumWindows(collect_hwnd, (LPARAM)&c);
    for (uint32_t i = 0; i < c.count; ++i) {
        HWND hwnd = c.list[i];
        if (!IsWindowVisible(hwnd)) continue;
        if (*count >= max) { result = ASB_RW_ERR_LIMIT; break; }
        AsbRwWindowFacts *f = &out[*count];
        DWORD pid = 0, tid = GetWindowThreadProcessId(hwnd, &pid);
        wchar_t cls[ASB_RW_MAX_CLASS_NAME + 1];
        if (!tid) continue;
        memset(f, 0, sizeof(*f));
        f->identity.hwnd = (uint64_t)(ULONG_PTR)hwnd;
        f->identity.pid = pid;
        f->identity.thread_id = tid;
        uint32_t k = 0;
        while (k < cached && cache[k].pid != pid) ++k;
        if (k == cached) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            uint64_t start = process_start(process);
            if (process) CloseHandle(process);
            if (cached < 64) { cache[cached].pid = pid; cache[cached].start = start; ++cached; }
            f->identity.process_start = start;
        } else {
            f->identity.process_start = cache[k].start;
        }
        f->visible = 1;
        f->style = (uint32_t)GetWindowLongPtrW(hwnd, GWL_STYLE);
        f->ex_style = (uint32_t)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        f->minimized = IsIconic(hwnd) ? 1 : 0;
        DWORD cloaked = 0;
        f->cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
        int n = GetClassNameW(hwnd, cls, ASB_RW_MAX_CLASS_NAME + 1);
        cls[n > 0 ? n : 0] = 0;
        copy_text(f->class_name, &f->class_len, ASB_RW_MAX_CLASS_NAME, cls);
        HWND owner = GetWindow(hwnd, GW_OWNER);
        f->owner = (uint64_t)(ULONG_PTR)owner;
        if (owner) {
            owner_chain(owner, f);
        } else if (f->class_len == 6 && !memcmp(f->class_name, L"#32768", 6 * sizeof(uint16_t))) {
            GUITHREADINFO gui = { sizeof(gui) };
            if (GetGUIThreadInfo(tid, &gui) && gui.hwndMenuOwner) {
                f->menu_owner = (uint64_t)(ULONG_PTR)gui.hwndMenuOwner;
                owner_chain(gui.hwndMenuOwner, f);
            }
        }
        RECT r = { 0 };
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || IsRectEmpty(&r))
            GetWindowRect(hwnd, &r);
        f->rect.left = r.left; f->rect.top = r.top; f->rect.right = r.right; f->rect.bottom = r.bottom;
        f->dpi = GetDpiForWindow(hwnd);
        (*count)++;
    }
    if (c.overflow) result = ASB_RW_ERR_LIMIT;
    free(c.list);
    return result;
}

int asb_rw_probe_identity(void *ctx, uint64_t hwnd, AsbRwIdentity *live)
{
    HWND h = (HWND)(ULONG_PTR)hwnd;
    DWORD pid = 0;
    (void)ctx;
    if (!IsWindow(h)) return 0;
    DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (!tid) return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    live->hwnd = hwnd;
    live->pid = pid;
    live->thread_id = tid;
    live->process_start = process_start(process);
    if (process) CloseHandle(process);
    return 1;
}
