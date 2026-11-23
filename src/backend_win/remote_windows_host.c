/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_host
 *
 * @file      remote_windows_host.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows per-VM host glue (AppSandbox-eh6.7). See remote_windows_host.h. */
#include <winsock2.h>
#include "headless_vm.h"
#include "remote_windows_host.h"
#include "remote_windows_proxy.h"
#include "vm_integration.h"
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AsbRwHost {
    UINT64 vm_id;
    GUID runtime;
    wchar_t name[128];
    /* eh6.9 drag broker (medium integrity; see remote_windows_drag_host.h) */
    CRITICAL_SECTION drag_lock;
    HANDLE drag_process, drag_watch, drag_stop_event;
    uint64_t drag_cookie;
    unsigned drag_restarts;
    AsbInputSession *input;
    VmIntegration *integration;
    uint32_t clip_client;
    AsbRwManager *manager;
    volatile LONG stopping;
    void (*log)(void *ctx, const char *message);
    void (*changed)(void *ctx);
    void *log_ctx;
};

static void host_changed(void *ctx)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    if (h->changed && !h->stopping) h->changed(h->log_ctx);
}

static void host_log(void *ctx, const char *message)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    if (h->log) h->log(h->log_ctx, message);
}

/* ---- input port: the VM's input session as the Remote Windows user source ---- */

static uint32_t port_claim(void *ctx, int renew)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    AsbInputUserState st;
    asb_input_user_state(h->input, &st);
    if (!st.connected || !st.user_owns) return 0;   /* automation owns input, or no channel */
    return asb_input_focus(h->input, ASB_INPUT_SOURCE_WINDOWS, renew);
}

static uint32_t port_send(void *ctx, uint32_t epoch, const InputPacket *packet)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    return asb_input_send_user(h->input, ASB_INPUT_SOURCE_WINDOWS, epoch, packet);
}

static void port_guard(void *ctx, int on)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    asb_input_guard(h->input, ASB_INPUT_SOURCE_WINDOWS, on);
}

static uint32_t port_hold(void *ctx)
{
    return asb_input_hold(((AsbRwHost *)ctx)->input);
}

static void port_unhold(void *ctx, uint32_t hold, int ok)
{
    asb_input_unhold(((AsbRwHost *)ctx)->input, hold, ok);
}

static void port_state(void *ctx, AsbRwInputPortState *out)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    AsbInputUserState st;
    asb_input_user_state(h->input, &st);
    out->epoch = st.epoch;
    out->windows_current = st.source == ASB_INPUT_SOURCE_WINDOWS;
    out->user_owns = st.user_owns && st.connected;
    out->hold = st.hold;
}

static void input_watch(void *ctx)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    if (!h->stopping && h->manager) asb_rw_manager_input_changed(h->manager);
}

static void proxies_focus(void *ctx, int focused)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    vm_integration_client_focus(h->integration, h->clip_client, focused != 0);
}

/* ---- drag broker (eh6.9) ---- */

static BOOL process_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION e = {0};
    DWORD n = 0;
    BOOL elevated = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n)) elevated = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

/* Starts AppSandboxDrag.exe beside this executable. When elevated, the broker is created as a
   child of the shell (explorer.exe) so it inherits the user's medium-integrity token: UIPI
   blocks OLE drag and drop between Explorer and elevated windows. NULL on failure. */
static HANDLE launch_broker(AsbRwHost *h)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 256], guid[64];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
    if (!slash) return NULL;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - exe), L"AppSandboxDrag.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return NULL;
    wchar_t name[128];
    wcscpy_s(name, ARRAYSIZE(name), h->name[0] ? h->name : L"the guest");
    for (wchar_t *q = name; *q; ++q) if (*q == L'"' || *q == L'\\') *q = L'\'';   /* stays one argument */
    StringFromGUID2(&h->runtime, guid, ARRAYSIZE(guid));
    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\" --vm %s --cookie %016llx --parent %lu --name \"%s\"", exe, guid,
               (unsigned long long)h->drag_cookie, GetCurrentProcessId(), name);
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    HANDLE shell = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T size = 0;
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.StartupInfo.cb = sizeof(si.StartupInfo);
    if (process_elevated()) {
        DWORD shell_pid = 0;
        HWND shell_window = GetShellWindow();
        if (shell_window) GetWindowThreadProcessId(shell_window, &shell_pid);
        shell = shell_pid ? OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shell_pid) : NULL;
        if (!shell) {
            host_log(h, "drag: no shell process to start the drag broker from; drag and drop is off");
            return NULL;
        }
        InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(size);
        if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &size) ||
            !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &shell, sizeof(shell), NULL, NULL)) {
            if (attrs) free(attrs);
            CloseHandle(shell);
            return NULL;
        }
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = attrs;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    /* The process object would otherwise carry the elevated creator's default DACL and label,
       so the user (medium integrity) could not even end the broker. */
    SECURITY_ATTRIBUTES psa = { sizeof(psa), NULL, FALSE };
    PSECURITY_DESCRIPTOR psd = NULL;
    {
        HANDLE token = NULL;
        BYTE buf[256];
        DWORD len = 0;
        wchar_t *sid = NULL, sddl[256];
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
            GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len) &&
            ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid) &&
            swprintf_s(sddl, ARRAYSIZE(sddl), L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%s)S:(ML;;NW;;;ME)", sid) > 0 &&
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &psd, NULL))
            psa.lpSecurityDescriptor = psd;
        if (sid) LocalFree(sid);
        if (token) CloseHandle(token);
    }
    BOOL ok = CreateProcessW(exe, cmd, psd ? &psa : NULL, NULL, FALSE, flags, NULL, NULL, &si.StartupInfo, &pi);
    if (psd) LocalFree(psd);
    if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
    if (shell) CloseHandle(shell);
    if (!ok) {
        char text[96];
        snprintf(text, sizeof(text), "drag: broker start failed (%lu)", GetLastError());
        host_log(h, text);
        return NULL;
    }
    CloseHandle(pi.hThread);
    {
        char text[96];
        snprintf(text, sizeof(text), "drag: broker started (pid %lu)", pi.dwProcessId);
        host_log(h, text);
    }
    return pi.hProcess;
}

/* Restarts a broker that exits unexpectedly (bounded). */
static DWORD WINAPI drag_watch(LPVOID arg)
{
    AsbRwHost *h = (AsbRwHost *)arg;
    for (;;) {
        EnterCriticalSection(&h->drag_lock);
        HANDLE process = h->drag_process;
        LeaveCriticalSection(&h->drag_lock);
        HANDLE waits[2] = { h->drag_stop_event, process };
        DWORD w = WaitForMultipleObjects(process ? 2 : 1, waits, FALSE, process ? INFINITE : 5000);
        if (w == WAIT_OBJECT_0) break;
        if (h->drag_restarts >= 5) {
            if (WaitForSingleObject(h->drag_stop_event, INFINITE) == WAIT_OBJECT_0) break;
        }
        if (WaitForSingleObject(h->drag_stop_event, 2000 << (h->drag_restarts < 3 ? h->drag_restarts : 3)) == WAIT_OBJECT_0) break;
        h->drag_restarts++;
        host_log(h, "drag: broker exited; restarting");
        HANDLE next = launch_broker(h);
        EnterCriticalSection(&h->drag_lock);
        if (h->drag_process) CloseHandle(h->drag_process);
        h->drag_process = next;
        LeaveCriticalSection(&h->drag_lock);
    }
    return 0;
}

static int drag_start(void *ctx, uint64_t cookie)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    h->drag_cookie = cookie;
    h->drag_process = launch_broker(h);
    if (!h->drag_process) return 0;
    h->drag_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    h->drag_watch = h->drag_stop_event ? CreateThread(NULL, 0, drag_watch, h, 0, NULL) : NULL;
    return 1;
}

static void drag_stop(void *ctx)
{
    AsbRwHost *h = (AsbRwHost *)ctx;
    if (h->drag_stop_event) SetEvent(h->drag_stop_event);
    if (h->drag_watch) { WaitForSingleObject(h->drag_watch, INFINITE); CloseHandle(h->drag_watch); h->drag_watch = NULL; }
    EnterCriticalSection(&h->drag_lock);
    if (h->drag_process) {
        TerminateProcess(h->drag_process, 0);   /* its staging is collected on the next start */
        WaitForSingleObject(h->drag_process, 2000);
        CloseHandle(h->drag_process);
        h->drag_process = NULL;
    }
    LeaveCriticalSection(&h->drag_lock);
    if (h->drag_stop_event) { CloseHandle(h->drag_stop_event); h->drag_stop_event = NULL; }
}

/* ---- lifetime ---- */

AsbRwHost *asb_rw_host_start(const AsbRwHostConfig *config)
{
    AsbHeadlessTarget target;
    AsbRwManagerConfig mc;
    AsbRwHost *h = (AsbRwHost *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->vm_id = config->vm_id;
    h->runtime = config->runtime;
    if (config->name) wcsncpy_s(h->name, ARRAYSIZE(h->name), config->name, _TRUNCATE);
    InitializeCriticalSection(&h->drag_lock);
    h->log = config->log;
    h->changed = config->changed;
    h->log_ctx = config->ctx;
    memset(&target, 0, sizeof(target));
    target.id = config->vm_id;
    target.runtime = config->runtime;
    if (config->name) wcsncpy_s(target.name, ARRAYSIZE(target.name), config->name, _TRUNCATE);
    if (config->os_type) wcsncpy_s(target.os_type, ARRAYSIZE(target.os_type), config->os_type, _TRUNCATE);
    h->input = asb_input_session(&target);
    if (h->input) asb_input_viewer(h->input, TRUE);   /* keeps the guest input channel connected */
    h->integration = vm_integration_acquire(config->vm_id, &config->runtime, config->os_type, config->name);
    h->clip_client = vm_integration_client_add(h->integration);

    memset(&mc, 0, sizeof(mc));
    mc.vm_id = config->runtime;
    mc.log = host_log;
    mc.changed = host_changed;
    mc.ctx = h;   /* log and focus */
    mc.allow_gpu = 1;
    mc.group = config->name;
    if (h->input) {
        mc.input.ctx = h;
        mc.input.claim = port_claim;
        mc.input.send = port_send;
        mc.input.guard = port_guard;
        mc.input.hold = port_hold;
        mc.input.unhold = port_unhold;
        mc.input.state = port_state;
    }
    mc.focus = proxies_focus;
    mc.drag_start = drag_start;   /* eh6.9 */
    mc.drag_stop = drag_stop;
    h->manager = asb_rw_manager_start(&mc);
    if (!h->manager) {
        asb_rw_host_stop(h);
        return NULL;
    }
    if (h->input) {
        asb_input_watch(h->input, input_watch, h);
        asb_rw_manager_input_changed(h->manager);
    }
    return h;
}

void asb_rw_host_stop(AsbRwHost *h)
{
    if (!h) return;
    InterlockedExchange(&h->stopping, 1);
    if (h->input) asb_input_watch(h->input, NULL, NULL);
    if (h->manager) asb_rw_manager_stop(h->manager);   /* releases held input, drops the guard */
    h->manager = NULL;
    if (h->input) asb_input_guard(h->input, ASB_INPUT_SOURCE_WINDOWS, FALSE);
    vm_integration_client_remove(h->integration, h->clip_client);
    vm_integration_release(h->integration);
    if (h->input) {
        asb_input_viewer(h->input, FALSE);
        asb_input_release(h->input);
    }
    DeleteCriticalSection(&h->drag_lock);
    free(h);
}

struct AsbRwManager *asb_rw_host_manager(const AsbRwHost *h)
{
    return h ? h->manager : NULL;
}

UINT64 asb_rw_host_vm_id(const AsbRwHost *h)
{
    return h ? h->vm_id : 0;
}
