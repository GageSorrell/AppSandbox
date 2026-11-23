/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_frames_native_test
 *
 * @file      remote_windows_frames_native_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows capture + frame delivery, native (AppSandbox-eh6.5).
 *
 * Real Windows Graphics Capture through the real guest capture engine and the real host frame
 * receiver, connected over a loopback TCP socket instead of AF_HYPERV (GPU pools are shared with
 * DuplicateHandle instead of D3DKMTDuplicateHandle; everything else is the product path).
 * Surfaces come from the real scan + asb_rw_collect_surfaces. Test windows: an animated root
 * with a magenta marker, an owned per-pixel-alpha popup overflowing the root's right edge, and
 * a popup menu that overflows it too.
 *
 * Per transport (shared GPU pools, then forced BGRA CPU): live content, alpha, popup metadata
 * (kind, z, rect outside the root), menu surface + SURFACE_END on close, resize generations,
 * engine device loss and recovery, host device loss + reconnect, a stale-session frame channel
 * refused, queue/memory bounds with a slow consumer, teardown without leaked textures.
 *
 *   remote-windows-frames-native-test.exe [--evidence <dir>]
 */
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma warning(pop)
#include <dwmapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../agent/remote_windows_capture.h"
#include "../agent/remote_windows_inventory.h"
#include "../../src/backend_win/remote_windows_frames_host.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fflush(stdout); } } while (0)

static const char *g_evidence;
static FILE *g_log;

static void tlog(const char *fmt, ...)
{
    va_list a;
    char text[1024];
    va_start(a, fmt);
    vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    printf("%s\n", text);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", text); fflush(g_log); }
}

static void sink_log(void *ctx, const char *message) { tlog("  [%s] %s", (const char *)ctx, message); }

/* ============================================================ test windows */

#define WM_APP_MENU  (WM_APP + 1)
#define WM_APP_SIZE  (WM_APP + 2)
#define WM_APP_QUIT  (WM_APP + 3)

static HWND g_root, g_alpha;
static volatile LONG g_menu_open, g_menu_close, g_ticks;
static HANDLE g_ui_ready;

static void paint_alpha(void)
{
    RECT r;
    GetWindowRect(g_alpha, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            BYTE a = (BYTE)(x * 255 / (w - 1));           /* horizontal alpha ramp */
            BYTE g = (BYTE)(200 * a / 255);               /* premultiplied green */
            ((DWORD *)bits)[y * w + x] = ((DWORD)a << 24) | ((DWORD)g << 8);
        }
    POINT src = { 0, 0 }, dst = { r.left, r.top };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_alpha, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK root_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        InterlockedIncrement(&g_ticks);
        if (g_menu_close && g_menu_open) EndMenu();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        LONG t = g_ticks;
        HBRUSH bg = CreateSolidBrush(RGB((t * 7) & 255, 90, (t * 3) & 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT marker = { 16, 16, 64, 64 };
        HBRUSH mg = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &marker, mg);
        DeleteObject(mg);
        char text[64];
        int n = sprintf_s(text, sizeof(text), "eh6.5 frame %ld", t);
        TextOutA(dc, 80, 30, text, n);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_APP_MENU: {
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < 12; ++i) {
            char item[64];
            sprintf_s(item, sizeof(item), "Overflowing menu item %d with a long label", i + 1);
            AppendMenuA(menu, MF_STRING, 100 + i, item);
        }
        RECT r;
        GetWindowRect(hwnd, &r);
        InterlockedExchange(&g_menu_open, 1);
        TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, r.right - 60, r.top + 90, hwnd, NULL);
        InterlockedExchange(&g_menu_open, 0);
        InterlockedExchange(&g_menu_close, 0);
        DestroyMenu(menu);
        return 0;
    }
    case WM_APP_SIZE:
        SetWindowPos(hwnd, NULL, 0, 0, (int)wp, (int)lp, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_APP_QUIT:
        DestroyWindow(g_alpha);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID unused)
{
    (void)unused;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = root_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"AsbEh65Root";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AsbEh65Alpha";
    RegisterClassW(&wc);
    g_root = CreateWindowExW(0, L"AsbEh65Root", L"eh6.5 capture test", WS_OVERLAPPEDWINDOW, 200, 160, 640, 420, NULL, NULL,
                             wc.hInstance, NULL);
    RECT r;
    GetWindowRect(g_root, &r);
    g_alpha = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"AsbEh65Alpha", L"", WS_POPUP,
                              r.right - 80, r.top + 200, 220, 120, g_root, NULL, wc.hInstance, NULL);
    ShowWindow(g_root, SW_SHOWNOACTIVATE);
    paint_alpha();
    ShowWindow(g_alpha, SW_SHOWNOACTIVATE);
    SetTimer(g_root, 1, 15, NULL);
    SetEvent(g_ui_ready);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ============================================================ loopback link */

static SOCKET g_listen = INVALID_SOCKET;
static int g_port;

static SOCKET engine_accept(void *ctx, DWORD timeout_ms)
{
    (void)ctx;
    fd_set set;
    struct timeval tv = { 0, (long)timeout_ms * 1000 };
    FD_ZERO(&set);
    FD_SET(g_listen, &set);
    if (select(0, &set, NULL, NULL, &tv) <= 0) return INVALID_SOCKET;
    return accept(g_listen, NULL, NULL);
}

static void engine_close(void *ctx, SOCKET s) { (void)ctx; closesocket(s); }

static SOCKET connect_loopback(void)
{
    struct sockaddr_in a;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

/* ============================================================ host device */

static ID3D11Device *create_host_device(ID3D11DeviceContext **ctx)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL, *chosen = NULL;
    ID3D11Device *device = NULL;
    *ctx = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return NULL;
    for (UINT i = 0; !chosen && IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(adapter, &d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !(d.VendorId == 0x1414 && d.DeviceId == 0x008C)) chosen = adapter;
        else IDXGIAdapter1_Release(adapter);
    }
    if (chosen) {
        D3D11CreateDevice((IDXGIAdapter *)chosen, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                          D3D11_SDK_VERSION, &device, NULL, ctx);
        IDXGIAdapter1_Release(chosen);
    }
    IDXGIFactory1_Release(factory);
    if (device) {
        ID3D10Multithread *mt = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_ID3D10Multithread, (void **)&mt))) {
            ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
            ID3D10Multithread_Release(mt);
        }
    }
    return device;
}

/* ============================================================ surfaces (helper rules) */

typedef struct Tracked { uint64_t hwnd, id; } Tracked;
static Tracked g_tracked[64];
static uint32_t g_tracked_n;
static uint64_t g_next_id = 0x1000;
static const uint64_t ROOT_ID = 0x500;
static AsbRwWindowFacts g_scan[ASB_RW_MAX_WINDOWS];
static uint8_t g_claimed[ASB_RW_MAX_WINDOWS];

static uint64_t id_for(uint64_t hwnd)
{
    if (hwnd == (uint64_t)(ULONG_PTR)g_root) return ROOT_ID;
    for (uint32_t i = 0; i < g_tracked_n; ++i) if (g_tracked[i].hwnd == hwnd) return g_tracked[i].id;
    if (g_tracked_n < 64) { g_tracked[g_tracked_n].hwnd = hwnd; g_tracked[g_tracked_n].id = g_next_id++; return g_tracked[g_tracked_n++].id; }
    return 0;
}

static uint32_t update_surfaces(AsbRwCapture *engine, AsbRwCaptureTarget *targets)
{
    uint32_t count = 0, idx[ASB_RW_MAX_SURFACES], kinds[ASB_RW_MAX_SURFACES];
    asb_rw_scan_visible_windows(g_scan, ASB_RW_MAX_WINDOWS, &count);
    memset(g_claimed, 0, sizeof(g_claimed));
    AsbRwRect virt = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                       GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                       GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    uint32_t n = asb_rw_collect_surfaces(g_scan, count, (uint64_t)(ULONG_PTR)g_root, &virt, g_claimed, idx, kinds, ASB_RW_MAX_SURFACES);
    for (uint32_t i = 0; i < n; ++i) {
        AsbRwCaptureTarget *t = &targets[i];
        memset(t, 0, sizeof(*t));
        t->hwnd = g_scan[idx[i]].identity.hwnd;
        t->surface_id = id_for(t->hwnd);
        t->root_id = ROOT_ID;
        t->kind = kinds[i];
        t->z = i;
        t->rect = g_scan[idx[i]].rect;
    }
    asb_rw_capture_set(engine, ROOT_ID, targets, n);
    return n;
}

/* ============================================================ frame checks */

typedef struct Seen {
    uint64_t surface_id;
    uint32_t kind, z, frames, distinct, magenta, partial_alpha, premultiplied_ok, max_generation, generation_regressed;
    uint32_t last_hash, width, height, transport;
    AsbRwRect rect;
} Seen;

static Seen g_seen[64];
static uint32_t g_seen_n;
static ID3D11Texture2D *g_staging;
static UINT g_sw, g_sh;
static uint32_t *g_pixels;

static Seen *seen_for(uint64_t id)
{
    for (uint32_t i = 0; i < g_seen_n; ++i) if (g_seen[i].surface_id == id) return &g_seen[i];
    if (g_seen_n >= 64) return NULL;
    memset(&g_seen[g_seen_n], 0, sizeof(g_seen[0]));
    g_seen[g_seen_n].surface_id = id;
    return &g_seen[g_seen_n++];
}

static void write_bmp(const char *name, const uint32_t *px, uint32_t w, uint32_t h)
{
    char path[MAX_PATH];
    FILE *f = NULL;
    if (!g_evidence) return;
    sprintf_s(path, sizeof(path), "%s\\%s.bmp", g_evidence, name);
    if (fopen_s(&f, path, "wb") || !f) return;
    BITMAPFILEHEADER fh;
    BITMAPV5HEADER ih;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    ih.bV5Size = sizeof(ih); ih.bV5Width = (LONG)w; ih.bV5Height = -(LONG)h; ih.bV5Planes = 1; ih.bV5BitCount = 32;
    ih.bV5Compression = BI_BITFIELDS; ih.bV5RedMask = 0x00FF0000; ih.bV5GreenMask = 0x0000FF00; ih.bV5BlueMask = 0x000000FF;
    ih.bV5AlphaMask = 0xFF000000; ih.bV5CSType = LCS_sRGB;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* Reads the frame's pixels (GPU: copy to staging on the host device) and records facts. */
static void inspect(ID3D11Device *dev, ID3D11DeviceContext *ctx, const AsbRwHostFrame *f, const char *tag)
{
    const AsbRwFrameHeader *h = &f->header;
    Seen *s = seen_for(h->surface_id);
    if (!s) return;
    uint32_t w = h->width, ht = h->height;
    const uint32_t *px = NULL;
    uint32_t stride_px = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    int mapped = 0;
    if (h->transport == ASB_RW_TRANSPORT_GPU) {
        if (!g_staging || g_sw < w || g_sh < ht) {
            if (g_staging) ID3D11Texture2D_Release(g_staging);
            D3D11_TEXTURE2D_DESC d;
            memset(&d, 0, sizeof(d));
            d.Width = w > g_sw ? w : g_sw; d.Height = ht > g_sh ? ht : g_sh; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_staging = NULL;
            if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_staging))) return;
            g_sw = d.Width; g_sh = d.Height;
        }
        D3D11_BOX box = { 0, 0, 0, w, ht, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)g_staging, 0, 0, 0, 0, (ID3D11Resource *)f->texture, 0, &box);
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
        mapped = 1;
        px = (const uint32_t *)m.pData;
        stride_px = m.RowPitch / 4;
    } else {
        px = (const uint32_t *)f->pixels;
        stride_px = h->stride / 4;
    }
    uint32_t hash = 2166136261u, magenta = 0, partial = 0, premul_bad = 0;
    for (uint32_t y = 0; y < ht; y += 2)
        for (uint32_t x = 0; x < w; x += 2) {
            uint32_t p = px[y * stride_px + x];
            hash = (hash ^ p) * 16777619u;
            uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (r > 240 && g < 16 && b > 240 && a == 255) magenta++;
            if (a > 8 && a < 247) partial++;
            if (r > a + 1 || g > a + 1 || b > a + 1) premul_bad++;
        }
    s->frames++;
    if (hash != s->last_hash) s->distinct++;
    s->last_hash = hash;
    if (magenta > 16) s->magenta = 1;
    if (partial > 16) s->partial_alpha = 1;
    s->premultiplied_ok = premul_bad == 0;
    if (h->generation < s->max_generation) s->generation_regressed = 1;
    if (h->generation > s->max_generation) s->max_generation = h->generation;
    s->kind = h->kind; s->z = h->z; s->rect = h->rect; s->width = w; s->height = ht; s->transport = h->transport;
    if (tag && g_evidence) {
        if (!g_pixels || 1) {
            free(g_pixels);
            g_pixels = (uint32_t *)malloc((size_t)w * ht * 4);
        }
        if (g_pixels) {
            for (uint32_t y = 0; y < ht; ++y) memcpy(g_pixels + (size_t)y * w, px + (size_t)y * stride_px, (size_t)w * 4);
            write_bmp(tag, g_pixels, w, ht);
        }
    }
    if (mapped) ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_staging, 0);
}

/* Pulls the newest frame of every surface for `ms`, slowly (consumer is slower than capture). */
static void consume(AsbRwFramesHost *host, ID3D11Device *dev, ID3D11DeviceContext *ctx, DWORD ms, DWORD period,
                    const char *evidence_prefix)
{
    ULONGLONG end = GetTickCount64() + ms;
    AsbRwHostSurfaceInfo info[64];
    int saved[64] = { 0 };
    while (GetTickCount64() < end) {
        uint32_t n = asb_rw_frames_host_surfaces(host, info, 64);
        for (uint32_t i = 0; i < n; ++i) {
            AsbRwHostFrame f;
            if (asb_rw_frames_host_acquire(host, info[i].surface_id, &f) != 1) continue;
            char tag[96];
            int save = evidence_prefix && i < 64 && !saved[i] && f.header.frame_seq > 3;
            if (save) {
                sprintf_s(tag, sizeof(tag), "%s-surface%u-kind%u", evidence_prefix, i, f.header.kind);
                saved[i] = 1;
            }
            inspect(dev, ctx, &f, save ? tag : NULL);
            asb_rw_frames_host_release(host, &f);
        }
        Sleep(period);
    }
}

static Seen *seen_kind(uint32_t kind)
{
    for (uint32_t i = 0; i < g_seen_n; ++i) if (g_seen[i].kind == kind && g_seen[i].frames) return &g_seen[i];
    return NULL;
}

static int wait_for(int (*cond)(void *), void *arg, DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (cond(arg)) return 1;
        Sleep(20);
    }
    return cond(arg);
}

typedef struct HostWait { AsbRwFramesHost *host; uint32_t value; } HostWait;
static int host_running(void *a) { AsbRwFramesHostStats s; asb_rw_frames_host_stats(((HostWait *)a)->host, &s); return s.state == ASB_RW_HOST_RUNNING; }
static int host_done(void *a) { AsbRwFramesHostStats s; asb_rw_frames_host_stats(((HostWait *)a)->host, &s); return s.state >= ASB_RW_HOST_CLOSED; }
static int host_surfaces_at_most(void *a) { AsbRwFramesHostStats s; asb_rw_frames_host_stats(((HostWait *)a)->host, &s); return s.surfaces <= ((HostWait *)a)->value; }
static int engine_disconnected(void *a) { AsbRwCaptureStats s; asb_rw_capture_stats((AsbRwCapture *)a, &s); return !s.connected; }
static int engine_recovered(void *a) { AsbRwCaptureStats s; asb_rw_capture_stats((AsbRwCapture *)a, &s); return s.device_recoveries >= 1; }
static int menu_open(void *a) { (void)a; return g_menu_open != 0; }

static AsbRwFramesHost *start_host(ID3D11Device *dev, uint32_t tag, int gpu, const char *name)
{
    AsbRwFramesHostConfig hc;
    memset(&hc, 0, sizeof(hc));
    hc.device = dev;
    hc.allow_gpu = (uint32_t)gpu;
    hc.slots = 3;
    hc.max_cpu_bytes = 96u << 20;
    hc.ctx = (void *)name;
    hc.log = sink_log;
    SOCKET s = connect_loopback();
    if (s == INVALID_SOCKET) return NULL;
    return asb_rw_frames_host_start(s, tag, &hc);
}

/* ============================================================ one transport */

static void run_mode(int gpu)
{
    const char *mode = gpu ? "gpu" : "cpu";
    const uint32_t tag = gpu ? 0x6510u : 0x6511u;
    AsbRwCaptureTarget targets[ASB_RW_MAX_SURFACES];
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device *dev = create_host_device(&ctx);
    CHECK(dev != NULL);
    if (!dev) return;
    tlog("== %s transport ==", mode);
    g_seen_n = 0;

    AsbRwCaptureConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.ctx = "engine";
    cc.accept = engine_accept;
    cc.close = engine_close;
    cc.log = sink_log;
    cc.allow_gpu = (uint32_t)gpu;
    cc.slots = 3;
    cc.max_cpu_bytes = 96u << 20;
    AsbRwCapture *engine = asb_rw_capture_start(&cc);
    CHECK(engine != NULL);
    if (!engine) return;
    asb_rw_capture_session(engine, tag);
    uint32_t n = update_surfaces(engine, targets);
    CHECK(n >= 2);   /* root + owned alpha popup */

    AsbRwFramesHost *host = start_host(dev, tag, gpu, "host");
    HostWait hw = { host, 0 };
    CHECK(host && wait_for(host_running, &hw, 5000));
    AsbRwFramesHostStats hs;
    asb_rw_frames_host_stats(host, &hs);
    CHECK(hs.agreement.gpu == (uint32_t)gpu);

    /* ---- live content, alpha, metadata; slow consumer for bounds ---- */
    consume(host, dev, ctx, 2500, 120, g_evidence ? mode : NULL);
    Seen *root = seen_kind(ASB_RW_SURFACE_ROOT), *popup = seen_kind(ASB_RW_SURFACE_POPUP);
    CHECK(root && root->distinct >= 5 && root->magenta);
    CHECK(root && root->transport == (gpu ? ASB_RW_TRANSPORT_GPU : ASB_RW_TRANSPORT_CPU));
    CHECK(popup && popup->partial_alpha && popup->premultiplied_ok);
    CHECK(root && popup && popup->z > root->z && popup->rect.right > root->rect.right);
    asb_rw_frames_host_stats(host, &hs);
    tlog("  live: root %u frames (%u distinct) %ux%u, popup %u frames alpha=%u; host gpu=%llu cpu=%llu superseded=%llu "
         "cpu_bytes=%llu textures=%llu pools=%llu",
         root ? root->frames : 0, root ? root->distinct : 0, root ? root->width : 0, root ? root->height : 0,
         popup ? popup->frames : 0, popup ? popup->partial_alpha : 0, hs.frames_gpu, hs.frames_cpu, hs.superseded,
         hs.cpu_bytes, hs.textures_live, hs.pools_created);
    CHECK(hs.superseded > 0);                                    /* slow consumer: newest frame wins */
    CHECK(hs.cpu_bytes <= hs.agreement.max_cpu_bytes);
    CHECK(gpu ? (hs.frames_gpu > 0 && hs.frames_cpu == 0 && hs.pools_created >= 2)
              : (hs.frames_cpu > 0 && hs.frames_gpu == 0 && hs.pools_created == 0));
    CHECK(hs.textures_live <= (uint64_t)hs.surfaces * hs.agreement.slots + 2u);
    CHECK(gpu || hs.cpu_bytes <= (uint64_t)hs.surfaces * 2u * 1024u * 1024u * 4u);

    /* ---- overflowing menu: separate surface outside the root, SURFACE_END on close ---- */
    PostMessageW(g_root, WM_APP_MENU, 0, 0);
    CHECK(wait_for(menu_open, NULL, 3000));
    ULONGLONG until = GetTickCount64() + 3000;
    Seen *menu = NULL;
    while (GetTickCount64() < until && !(menu && menu->frames >= 2)) {
        update_surfaces(engine, targets);
        consume(host, dev, ctx, 150, 30, NULL);
        menu = seen_kind(ASB_RW_SURFACE_MENU);
    }
    CHECK(menu && menu->frames >= 1);
    root = seen_kind(ASB_RW_SURFACE_ROOT);
    CHECK(menu && root && menu->rect.right > root->rect.right && menu->z > root->z);
    if (menu) tlog("  menu: %u frames, rect (%d,%d,%d,%d) root right %d", menu->frames, menu->rect.left, menu->rect.top,
                   menu->rect.right, menu->rect.bottom, root ? root->rect.right : 0);
    if (g_evidence && menu) consume(host, dev, ctx, 200, 50, gpu ? "gpu-menu" : "cpu-menu");
    asb_rw_frames_host_stats(host, &hs);
    uint64_t ended_before = hs.surfaces_ended;
    uint32_t surfaces_with_menu = hs.surfaces;
    InterlockedExchange(&g_menu_close, 1);
    until = GetTickCount64() + 3000;
    while (GetTickCount64() < until) {
        update_surfaces(engine, targets);
        consume(host, dev, ctx, 100, 30, NULL);
        asb_rw_frames_host_stats(host, &hs);
        if (!g_menu_open && hs.surfaces_ended > ended_before) break;
    }
    CHECK(!g_menu_open);
    CHECK(hs.surfaces_ended > ended_before && hs.surfaces < surfaces_with_menu);
    tlog("  menu closed: surfaces %u -> %u, ended %llu", surfaces_with_menu, hs.surfaces, hs.surfaces_ended);

    /* ---- resize: new generation, never an older one after it ---- */
    root = seen_kind(ASB_RW_SURFACE_ROOT);
    uint32_t gen_before = root ? root->max_generation : 0;
    PostMessageW(g_root, WM_APP_SIZE, 980, 640);
    until = GetTickCount64() + 3000;
    while (GetTickCount64() < until) {
        update_surfaces(engine, targets);
        consume(host, dev, ctx, 100, 30, NULL);
        root = seen_kind(ASB_RW_SURFACE_ROOT);
        if (root && root->max_generation > gen_before && root->width >= 900) break;
    }
    CHECK(root && root->max_generation > gen_before && root->width >= 900 && !root->generation_regressed);
    tlog("  resize: root generation %u -> %u, %ux%u", gen_before, root ? root->max_generation : 0, root ? root->width : 0,
         root ? root->height : 0);
    PostMessageW(g_root, WM_APP_SIZE, 640, 420);

    /* ---- engine (guest) device loss: sessions restart, new generations, frames continue ---- */
    gen_before = root ? root->max_generation : 0;
    uint32_t frames_before = root ? root->frames : 0;
    asb_rw_capture_simulate_device_loss(engine);
    CHECK(wait_for(engine_recovered, engine, 5000));
    consume(host, dev, ctx, 1200, 40, NULL);
    root = seen_kind(ASB_RW_SURFACE_ROOT);
    CHECK(root && root->frames > frames_before + 3 && root->max_generation > gen_before && !root->generation_regressed);
    AsbRwCaptureStats es;
    asb_rw_capture_stats(engine, &es);
    tlog("  engine device loss: losses %llu recoveries %llu, root generation %u -> %u", es.device_losses,
         es.device_recoveries, gen_before, root ? root->max_generation : 0);
    CHECK(es.device_losses == 1 && es.device_recoveries == 1);

    /* ---- host device loss: connection closes and everything is freed; reconnect resumes ---- */
    asb_rw_frames_host_simulate_device_loss(host);
    CHECK(wait_for(host_done, &hw, 3000));
    asb_rw_frames_host_stats(host, &hs);
    CHECK(hs.state == ASB_RW_HOST_DEVICE_LOST && hs.surfaces == 0 && hs.cpu_bytes == 0);
    asb_rw_frames_host_stop(host);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; g_sw = g_sh = 0; }
    CHECK(wait_for(engine_disconnected, engine, 3000));
    /* A frame channel for another control session is refused. */
    dev = create_host_device(&ctx);
    host = start_host(dev, tag + 1, gpu, "stale-host");
    hw.host = host;
    CHECK(host && wait_for(host_done, &hw, 5000));
    asb_rw_frames_host_stats(host, &hs);
    CHECK(hs.state == ASB_RW_HOST_CLOSED && hs.error == ASB_RW_ERR_STALE_ID);
    asb_rw_frames_host_stop(host);
    /* The right session reconnects with a new device; frames resume. */
    host = start_host(dev, tag, gpu, "host2");
    hw.host = host;
    CHECK(host && wait_for(host_running, &hw, 5000));
    root = seen_kind(ASB_RW_SURFACE_ROOT);
    frames_before = root ? root->frames : 0;
    consume(host, dev, ctx, 1200, 40, NULL);
    root = seen_kind(ASB_RW_SURFACE_ROOT);
    CHECK(root && root->frames > frames_before + 3);
    asb_rw_frames_host_stats(host, &hs);
    CHECK(gpu ? hs.frames_gpu > 0 : hs.frames_cpu > 0);
    tlog("  host device loss: reconnected (%s), root frames %u -> %u", hs.agreement.gpu ? "GPU" : "CPU", frames_before,
         root ? root->frames : 0);

    /* ---- teardown: every surface ends, nothing stays allocated ---- */
    asb_rw_capture_set(engine, ROOT_ID, NULL, 0);
    hw.value = 0;
    CHECK(wait_for(host_surfaces_at_most, &hw, 3000));
    asb_rw_frames_host_stats(host, &hs);
    CHECK(hs.surfaces == 0 && hs.cpu_bytes == 0 && hs.textures_live == 0);
    asb_rw_capture_stats(engine, &es);
    tlog("  teardown: host surfaces %u, cpu_bytes %llu, textures %llu; engine sent gpu=%llu cpu=%llu superseded=%llu "
         "pools=%llu failures=%llu ended=%llu",
         hs.surfaces, hs.cpu_bytes, hs.textures_live, es.sent_gpu, es.sent_cpu, es.superseded, es.pools_opened,
         es.pool_failures, es.surfaces_ended);
    CHECK(gpu ? es.sent_gpu > 0 && es.pools_opened > 0 : es.sent_cpu > 0 && es.sent_gpu == 0);
    asb_rw_capture_session(engine, 0);
    asb_rw_frames_host_stop(host);
    asb_rw_capture_stop(engine);
    if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; g_sw = g_sh = 0; }
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--evidence") && i + 1 < argc) g_evidence = argv[++i];
    if (g_evidence) {
        char path[MAX_PATH];
        CreateDirectoryA(g_evidence, NULL);
        sprintf_s(path, sizeof(path), "%s\\native-test.log", g_evidence);
        fopen_s(&g_log, path, "w");
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int alen = sizeof(a);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) || listen(g_listen, 4) ||
        getsockname(g_listen, (struct sockaddr *)&a, &alen)) {
        printf("FAIL loopback listener\n");
        return 2;
    }
    g_port = ntohs(a.sin_port);
    g_ui_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE ui = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    WaitForSingleObject(g_ui_ready, 5000);
    Sleep(300);
    run_mode(1);
    run_mode(0);
    PostMessageW(g_root, WM_APP_QUIT, 0, 0);
    WaitForSingleObject(ui, 3000);
    closesocket(g_listen);
    WSACleanup();
    if (g_failures) {
        tlog("FAILED remote windows frames native: %d of %d checks", g_failures, g_checks);
        return 1;
    }
    tlog("PASS remote windows frames native: WGC root + alpha popup + overflowing menu, shared GPU pools and forced CPU, "
         "resize generations, engine and host device loss, stale session, bounds, teardown (%d checks)", g_checks);
    return 0;
}
