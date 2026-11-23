/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_proxy_vm_check.c
 *
 * @file      remote_windows_proxy_vm_check.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows proxies against a real Windows guest (AppSandbox-eh6.6, host side).
 *
 * Runs the real per-VM manager (remote_windows_proxy.cpp) over AF_HYPERV channels 9/10 of a
 * running VM whose guest has the eh6.6 helper, selects guest windows by title and keeps their
 * proxies on this desktop for interactive qualification (move, resize, Snap, minimize/maximize,
 * menus, dialogs, Close with unsaved changes). Automated checks per proxy: shell entry
 * (unowned, APPWINDOW, title, icon), frames presented, no host caption (client top == visible
 * top) and popups owned by the root proxy. Stopping (timeout or Ctrl+C) ends sharing; the guest
 * restores its windows.
 *
 *   remote-windows-proxy-vm-check.exe --vm <runtime-id> [--select <title part>]... [--seconds N] [--cpu]
 *                                     [--log <file>] [--evidence <dir>]
 * Exit: 0 all selected windows shared with frames, 1 a check failed, 2 guest too old (upgrade
 * required) or not reachable, 3 usage.
 */
#include <winsock2.h>
#include <windows.h>
#include <dwmapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/backend_win/remote_windows_proxy.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")

static FILE *g_log;
static volatile LONG g_stop;
static int g_failures;

static void tlog(const char *fmt, ...)
{
    char text[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    printf("%s\n", text);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", text); fflush(g_log); }
}

static const wchar_t *g_evidence;

/* Screenshot of a proxy (raised topmost for a moment, never activated) as a BMP. */
static void snap_proxy(HWND h, uint64_t id)
{
    RECT r;
    if (!g_evidence || !GetWindowRect(h, &r)) return;
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Sleep(600);
    InflateRect(&r, 24, 24);
    int w = r.right - r.left, hh = r.bottom - r.top;
    BITMAPINFO bi;
    void *bits = NULL;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, hh, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\proxy-%016llx.bmp", g_evidence, (unsigned long long)id);
    FILE *f = NULL;
    if (!_wfopen_s(&f, path, L"wb") && f) {
        BITMAPFILEHEADER fh;
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader); fh.bfSize = fh.bfOffBits + (DWORD)(w * hh * 4);
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        fwrite(bits, 4, (size_t)w * hh, f);
        fclose(f);
        tlog("  evidence: %ls", path);
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static void mgr_log(void *ctx, const char *message) { (void)ctx; tlog("  [manager] %s", message); }

static BOOL WINAPI on_ctrl(DWORD type) { (void)type; InterlockedExchange(&g_stop, 1); return TRUE; }

static void narrow(const uint16_t *s, uint32_t n, char *out, size_t cap)
{
    WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)s, (int)n, out, (int)cap - 1, NULL, NULL);
    out[n < cap - 1 ? n : cap - 1] = 0;
    out[cap - 1] = 0;
}

static int contains(const uint16_t *title, uint32_t len, const wchar_t *part)
{
    wchar_t buf[ASB_RW_MAX_TITLE + 1];
    uint32_t n = len < ASB_RW_MAX_TITLE ? len : ASB_RW_MAX_TITLE;
    for (uint32_t i = 0; i < n; ++i) buf[i] = (wchar_t)towlower(title[i]);
    buf[n] = 0;
    wchar_t want[256];
    size_t k = 0;
    for (; part[k] && k < 255; ++k) want[k] = (wchar_t)towlower(part[k]);
    want[k] = 0;
    return wcsstr(buf, want) != NULL;
}

#define CHECK(cond, what) do { if (!(cond)) { ++g_failures; tlog("FAIL %s", what); } else tlog("ok   %s", what); } while (0)

int wmain(int argc, wchar_t **argv)
{
    GUID vm = { 0, 0, 0, { 0 } };
    int have_vm = 0, seconds = 120, cpu = 0, nselect = 0;
    const wchar_t *select[16];
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--vm") && i + 1 < argc) {
            wchar_t braced[64];
            const wchar_t *id = argv[++i];
            swprintf_s(braced, 64, id[0] == L'{' ? L"%s" : L"{%s}", id);
            have_vm = CLSIDFromString(braced, &vm) == S_OK;
        }
        else if (!_wcsicmp(argv[i], L"--select") && i + 1 < argc && nselect < 16) select[nselect++] = argv[++i];
        else if (!_wcsicmp(argv[i], L"--seconds") && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (!_wcsicmp(argv[i], L"--cpu")) cpu = 1;
        else if (!_wcsicmp(argv[i], L"--log") && i + 1 < argc) _wfopen_s(&g_log, argv[++i], L"w");
        else if (!_wcsicmp(argv[i], L"--evidence") && i + 1 < argc) g_evidence = argv[++i];
    }
    if (!have_vm) {
        printf("usage: remote-windows-proxy-vm-check --vm <runtime-id> [--select <title part>]... [--seconds N] [--cpu]\n");
        return 3;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    AsbRwManagerConfig mc;
    memset(&mc, 0, sizeof(mc));
    mc.vm_id = vm;
    mc.log = mgr_log;
    mc.allow_gpu = cpu ? 0u : 1u;
    mc.group = L"VmCheck";
    AsbRwManager *m = asb_rw_manager_start(&mc);
    if (!m) { tlog("manager could not start"); return 2; }

    AsbRwManagerStatus st;
    ULONGLONG end = GetTickCount64() + 20000;
    do { Sleep(100); asb_rw_manager_status(m, &st); } while (st.state < ASB_RW_MGR_READY && GetTickCount64() < end && !g_stop);
    tlog("manager state %u, guest helper %08x caps %08x missing %08x (%s)", st.state, st.guest_helper_version,
         st.guest_capabilities, st.missing, asb_rw_error_name(st.negotiation_error));
    if (st.state != ASB_RW_MGR_READY) {
        tlog(st.state == ASB_RW_MGR_LIMITED ? "guest components too old for proxies: %s" : "guest not reachable on channel 9%s",
             st.state == ASB_RW_MGR_LIMITED ? asb_rw_error_text(ASB_RW_ERR_UPGRADE_REQUIRED) : "");
        asb_rw_manager_stop(m);
        return 2;
    }
    Sleep(1200);   /* first snapshot */
    static AsbRwManagerEntry entries[ASB_RW_MAX_INVENTORY];
    uint32_t n = asb_rw_manager_inventory(m, entries, ASB_RW_MAX_INVENTORY);
    tlog("guest windows (%u):", n);
    for (uint32_t i = 0; i < n; ++i) {
        char title[512], app[256];
        narrow(entries[i].entry.title, entries[i].entry.title_len, title, sizeof(title));
        narrow(entries[i].entry.app, entries[i].entry.app_len, app, sizeof(app));
        tlog("  %016llx  %-40s  %s", (unsigned long long)entries[i].entry.id, title, app);
    }
    uint64_t chosen[16];
    int nchosen = 0;
    for (int k = 0; k < nselect; ++k) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!contains(entries[i].entry.title, entries[i].entry.title_len, select[k])) continue;
            uint32_t r = asb_rw_manager_select(m, entries[i].entry.id, 1);
            tlog("select \"%ls\" -> %016llx: %s", select[k], (unsigned long long)entries[i].entry.id, asb_rw_error_name(r));
            if (!r) chosen[nchosen++] = entries[i].entry.id;
            break;
        }
    }
    /* Wait for proxies with frames. */
    static AsbRwProxyInfo infos[32];
    end = GetTickCount64() + 15000;
    uint32_t np = 0;
    for (;;) {
        np = asb_rw_manager_proxies(m, infos, 32);
        int drawn = 0;
        for (uint32_t i = 0; i < np; ++i) drawn += infos[i].content_w > 0;   /* a real frame, not just a clear */
        if (drawn >= nchosen || GetTickCount64() > end || g_stop) break;
        Sleep(100);
    }
    for (int k = 0; k < nchosen; ++k) {
        AsbRwProxyInfo *p = NULL;
        for (uint32_t i = 0; i < np; ++i) if (infos[i].id == chosen[k]) p = &infos[i];
        char what[160];
        sprintf_s(what, sizeof(what), "proxy for %016llx exists with frames", (unsigned long long)chosen[k]);
        CHECK(p && p->content_w > 0, what);
        if (!p) continue;
        RECT c, vis;
        POINT o = { 0, 0 };
        WCHAR title[256];
        GetClientRect(p->hwnd, &c);
        ClientToScreen(p->hwnd, &o);
        DwmGetWindowAttribute(p->hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis));
        GetWindowTextW(p->hwnd, title, 256);
        LONG ex = GetWindowLongW(p->hwnd, GWL_EXSTYLE);
        tlog("  proxy hwnd %p \"%ls\" client %ldx%ld content %ux%u frames %llu popups %u", (void *)p->hwnd, title, c.right,
             c.bottom, p->content_w, p->content_h, (unsigned long long)p->presented, p->count);
        CHECK(GetWindow(p->hwnd, GW_OWNER) == NULL && (ex & WS_EX_APPWINDOW) && !(ex & WS_EX_TOOLWINDOW),
              "  shell entry (unowned, APPWINDOW)");
        CHECK(title[0] != 0, "  guest title");
        CHECK(o.y == vis.top, "  no host caption (client top == visible top)");
        /* The client is the guest's visible frame; the capture may lack a self-drawn frame's 2 px
           side/bottom border strip (WinUI), which the proxy keeps transparent. */
        CHECK(p->content_w && (uint32_t)c.right >= p->content_w && (uint32_t)c.right - p->content_w <= 8 &&
              (uint32_t)c.bottom >= p->content_h && (uint32_t)c.bottom - p->content_h <= 8,
              "  client == guest visible frame (capture within its border strip)");
        snap_proxy(p->hwnd, p->id);
    }
    tlog("interactive for %d s (Ctrl+C ends; sharing ends and the guest restores its windows)", seconds);
    end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    ULONGLONG next = 0;
    while (!g_stop && GetTickCount64() < end) {
        if (GetTickCount64() >= next) {
            next = GetTickCount64() + 5000;
            asb_rw_manager_status(m, &st);
            np = asb_rw_manager_proxies(m, infos, 32);
            tlog("status: state %u frames %u gpu %u proxies %u presented %llu", st.state, st.frames_state, st.frames_gpu, np,
                 (unsigned long long)st.frames_presented);
            for (uint32_t i = 0; i < np; ++i) {
                tlog("  %016llx frames %llu content %ux%u flags %02x popups %u", (unsigned long long)infos[i].id,
                     (unsigned long long)infos[i].presented, infos[i].content_w, infos[i].content_h, infos[i].window_flags,
                     infos[i].count);
                for (uint32_t s = 0; s < infos[i].count; ++s)
                    tlog("    surface %016llx kind %u owner-ok %d frames %llu", (unsigned long long)infos[i].surfaces[s].surface_id,
                         infos[i].surfaces[s].kind, GetWindow(infos[i].surfaces[s].hwnd, GW_OWNER) == infos[i].hwnd,
                         (unsigned long long)infos[i].surfaces[s].presented);
            }
        }
        Sleep(100);
    }
    asb_rw_manager_stop(m);
    tlog("%s (%d failure(s))", g_failures ? "RESULT FAIL" : "RESULT PASS", g_failures);
    if (g_log) fclose(g_log);
    return g_failures ? 1 : 0;
}
