/**
 *
 *
 * @module AppSandbox/tools/tests/vm_integration_test
 *
 * @file      vm_integration_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Per-VM clipboard coordinator and audio lifetime (AppSandbox-eh6.7), against a fake backend:
 * one clipboard/audio per VM whatever holds it, focus-gated sync across the viewer and the
 * Remote Windows proxies, sharing setting, survival of viewer closure, shared mute, and no
 * deadlock when the clipboard logs synchronously from calls made under the coordinator's lock
 * (the hang seen on the first eh6.7 install). A watchdog turns any deadlock into a failure.
 */
#include "../../src/backend_win/vm_integration.h"
#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile LONG g_creates, g_destroys, g_sync, g_sync_calls, g_sharing, g_audio_starts, g_audio_stops, g_logs, g_updates;
static VmIntegration *g_last_vi;
static int g_fake_clipboard;

static void *fake_create(const GUID *runtime, const wchar_t *os, HWND hwnd, VmIntegration *vi)
{
    (void)runtime; (void)os;
    if (!hwnd) return NULL;
    g_last_vi = vi;
    InterlockedIncrement(&g_creates);
    vm_integration_log(vi, L"CLIP: created");   /* vm_clipboard logs from its own calls too */
    return &g_fake_clipboard;
}
static void fake_destroy(void *c) { (void)c; InterlockedIncrement(&g_destroys); }
static void fake_sync(void *c, BOOL on)
{
    (void)c;
    InterlockedExchange(&g_sync, on ? 1 : 0);
    InterlockedIncrement(&g_sync_calls);
    /* vm_clipboard_set_sync_enabled logs "Window focused - clipboard sync ON" synchronously. */
    vm_integration_log(g_last_vi, on ? L"CLIP: sync ON" : L"CLIP: sync OFF");
}
static void fake_sharing(void *c, BOOL on) { (void)c; InterlockedExchange(&g_sharing, on ? 1 : 0); vm_integration_log(g_last_vi, L"CLIP: sharing"); }
static void fake_update(void *c) { (void)c; InterlockedIncrement(&g_updates); }
static void fake_apply(void *c) { (void)c; }
static BOOL fake_audio_start(VmIntegration *vi) { (void)vi; InterlockedIncrement(&g_audio_starts); return TRUE; }
static void fake_audio_stop(VmIntegration *vi) { (void)vi; InterlockedIncrement(&g_audio_stops); }
static BOOL fake_load_sharing(UINT64 vm_id) { (void)vm_id; return TRUE; }
static void fake_log(const wchar_t *m) { (void)m; InterlockedIncrement(&g_logs); }

static volatile LONG g_sink;
static void sink(void *ctx, const wchar_t *m) { (void)m; InterlockedIncrement((volatile LONG *)ctx); }

static DWORD WINAPI watchdog(LPVOID arg)
{
    if (WaitForSingleObject((HANDLE)arg, 30000) == WAIT_TIMEOUT) {
        printf("FAIL: deadlock (no progress for 30 s)\nRESULT FAIL\n");
        fflush(stdout);
        ExitProcess(3);
    }
    return 0;
}

static void wait_created(VmIntegration *vi)
{
    VmIntegrationInfo info;
    for (int i = 0; i < 200; ++i) {
        vm_integration_info(vi, &info);
        if (info.clipboard) return;
        Sleep(10);
    }
}

int main(void)
{
    static const GUID rt1 = { 0x11111111, 0x1111, 0x1111, { 1, 1, 1, 1, 1, 1, 1, 1 } };
    static const GUID rt2 = { 0x22222222, 0x2222, 0x2222, { 2, 2, 2, 2, 2, 2, 2, 2 } };
    VmIntegrationBackend b;
    VmIntegrationInfo info;
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, watchdog, done, 0, NULL));
    memset(&b, 0, sizeof(b));
    b.clipboard_create = fake_create;
    b.clipboard_destroy = fake_destroy;
    b.clipboard_sync = fake_sync;
    b.clipboard_sharing = fake_sharing;
    b.clipboard_update = fake_update;
    b.clipboard_apply = fake_apply;
    b.audio_start = fake_audio_start;
    b.audio_stop = fake_audio_stop;
    b.load_sharing = fake_load_sharing;
    b.log = fake_log;
    vm_integration_set_backend(&b);

    /* One instance per VM; the Remote Windows host and the viewer share it. */
    VmIntegration *host = vm_integration_acquire(1, &rt1, L"Windows", L"Main");
    CHECK(host != NULL, "acquire");
    wait_created(host);
    VmIntegration *viewer = vm_integration_acquire(1, &rt1, L"Windows", L"Main");
    CHECK(viewer == host, "same VM -> same instance");
    vm_integration_info(host, &info);
    CHECK(info.refs == 2 && info.clipboard_creates == 1 && info.audio_starts == 1, "one clipboard/audio (refs %u creates %u audio %u)",
          info.refs, info.clipboard_creates, info.audio_starts);
    CHECK(g_creates == 1 && g_audio_starts == 1, "backend created once");
    CHECK(vm_integration_count() == 1, "one live instance");

    /* Focus clients: sync follows focus across viewer and proxies, under the sharing setting. */
    uint32_t proxies = vm_integration_client_add(host), view = vm_integration_client_add(viewer);
    CHECK(proxies && view && proxies != view, "client ids");
    vm_integration_set_log(viewer, sink, (void *)&g_sink);
    CHECK(!g_sync, "no focus -> no sync");
    vm_integration_client_focus(viewer, view, TRUE);   /* logs under the lock: must not deadlock */
    CHECK(g_sync == 1, "viewer focus -> sync on");
    CHECK(g_sink > 0, "clipboard log reached the viewer sink (%ld)", (long)g_sink);
    vm_integration_client_focus(viewer, view, FALSE);
    vm_integration_client_focus(host, proxies, TRUE);
    CHECK(g_sync == 1, "focus moved to a proxy -> sync stays on");
    vm_integration_info(host, &info);
    CHECK(info.focused == 1 && info.clients == 2, "one focused of two clients");
    vm_integration_set_clipboard_sharing(viewer, FALSE);
    CHECK(!g_sync && !g_sharing && !vm_integration_clipboard_sharing(host), "sharing off -> sync off");
    vm_integration_set_clipboard_sharing(viewer, TRUE);
    CHECK(g_sync == 1 && g_sharing == 1, "sharing on again -> sync on (proxy focused)");
    vm_integration_client_focus(host, proxies, FALSE);
    CHECK(!g_sync, "nothing focused -> sync off");

    /* Mute is per VM. */
    vm_integration_set_audio_muted(viewer, TRUE);
    CHECK(vm_integration_audio_muted(host), "mute shared");

    /* The viewer closes: clipboard and audio stay for the shared windows. */
    vm_integration_set_log(viewer, NULL, NULL);
    vm_integration_client_remove(viewer, view);
    vm_integration_release(viewer);
    vm_integration_info(host, &info);
    CHECK(info.refs == 1 && info.clipboard == &g_fake_clipboard && info.audio_running, "viewer closed: services kept");
    CHECK(g_destroys == 0 && g_audio_stops == 0, "nothing torn down");
    vm_integration_client_focus(host, proxies, TRUE);
    CHECK(g_sync == 1, "proxies still drive sync after viewer closure");

    /* Reopening the viewer reuses the same connections. */
    viewer = vm_integration_acquire(1, &rt1, L"Windows", L"Main");
    CHECK(viewer == host && g_creates == 1 && g_audio_starts == 1, "reopen: no second clipboard/audio");
    CHECK(vm_integration_audio_muted(viewer), "reopened viewer sees the VM mute");

    /* A clipboard change on the host reaches the coordinator's window. */
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, 8);
        if (mem) { memcpy(GlobalLock(mem), "eh6.7\0\0", 8); GlobalUnlock(mem); SetClipboardData(CF_TEXT, mem); }
        CloseClipboard();
        for (int i = 0; i < 100 && !g_updates; ++i) Sleep(10);
        CHECK(g_updates > 0, "WM_CLIPBOARDUPDATE delivered to the coordinator window");
    }

    /* Another VM gets its own services. */
    VmIntegration *other = vm_integration_acquire(2, &rt2, L"Windows", L"Other");
    wait_created(other);
    CHECK(other && other != host && vm_integration_count() == 2 && g_creates == 2, "second VM separate");
    vm_integration_release(other);

    /* Last holder gone: everything closes. */
    vm_integration_client_remove(host, proxies);
    vm_integration_release(viewer);
    vm_integration_release(host);
    CHECK(vm_integration_count() == 0, "no live instances");
    CHECK(g_destroys == 2 && g_audio_stops == 2, "clipboards destroyed %ld, audio stopped %ld", (long)g_destroys,
          (long)g_audio_stops);

    SetEvent(done);
    printf("RESULT %s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
