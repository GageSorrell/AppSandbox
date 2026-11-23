/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_frames_vm_check
 *
 * @file      remote_windows_frames_vm_check.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * eh6.5 native check against a running Windows 11 guest (host side, unelevated).
 *
 * Drives the real guest helper over AF_HYPERV: channel 9 HELLO, inventory, SELECT of the window
 * titled --title (remote-windows-capture-test-app.exe running in the guest), then receives
 * channel 10 frames with the real host receiver (src/backend_win/remote_windows_frames_host.c):
 *
 *   1. shared GPU pools (keyed-mutex textures duplicated into the helper with
 *      D3DKMTDuplicateHandle), including a host device loss + reconnect half way;
 *   2. forced BGRA CPU frames on a new frame channel of the same control session;
 *   3. unselect: SURFACES count 0, every surface ended, nothing left allocated.
 *
 * Checks live root content (changing frames + magenta marker), the owned alpha popup (partial,
 * premultiplied alpha; rect beyond the root's right edge), the overflowing menu (own surface,
 * rect beyond the root, SURFACE_END when it closes), guest device-loss recovery (generation jump
 * without a size change, frames continue), bounds and teardown. Evidence: BMPs + check.log.
 *
 *   remote-windows-frames-vm-check.exe [--vm <runtime-guid>] [--title <text>] [--seconds <per phase>]
 *                                      [--evidence <dir>] [--probe]
 */
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma warning(pop)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../src/backend_win/hcs_services.h"
#include "../../src/backend_win/remote_windows_frames_host.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
typedef struct HvAddr { ADDRESS_FAMILY family; USHORT reserved; GUID vm; GUID service; } HvAddr;

static int g_failures, g_checks;
static FILE *g_log;
static const char *g_evidence;

static void tlog(const char *fmt, ...)
{
    va_list a;
    char text[1200];
    SYSTEMTIME t;
    GetLocalTime(&t);
    va_start(a, fmt);
    vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    printf("%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text); fflush(g_log); }
}
#define CHECK(cond, what) do { ++g_checks; if (!(cond)) { ++g_failures; tlog("FAIL %s (%s)", what, #cond); } else tlog("ok   %s", what); } while (0)

static void sink_log(void *ctx, const char *m) { tlog("  [%s] %s", (const char *)ctx, m); }

/* ============================================================ channel 9 */

static SOCKET g_ctl = INVALID_SOCKET;
static CRITICAL_SECTION g_ctl_lock;
static uint32_t g_tag;
static int g_guest_gpu;   /* guest helper offered FRAMES_GPU (hardware capture adapter) */
static uint8_t g_rxbuf[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD];
static AsbRwPayload g_msg;
static volatile LONG g_ctl_stop;
/* reader-thread observations */
static volatile LONG g_shared_state = -1, g_surfaces_msgs, g_last_surface_count = -1;
static AsbRwSurfaces g_last_surfaces;

static SOCKET hv_connect(const GUID *vm, unsigned port)
{
    HvAddr a;
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, 1);
    if (s == INVALID_SOCKET) return s;
    memset(&a, 0, sizeof(a));
    a.family = AF_HYPERV;
    a.vm = *vm;
    hcs_service_guid(L"windows", port, &a.service);
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static int recv_exact(SOCKET s, uint8_t *p, size_t n, DWORD timeout_ms)
{
    ULONGLONG end = GetTickCount64() + timeout_ms;
    while (n) {
        fd_set set;
        struct timeval tv = { 0, 100000 };
        FD_ZERO(&set);
        FD_SET(s, &set);
        int r = select(0, &set, NULL, NULL, &tv);
        if (r < 0 || g_ctl_stop) return 0;
        if (!r) { if (GetTickCount64() > end) return 0; continue; }
        int got = recv(s, (char *)p, (int)n, 0);
        if (got <= 0) return 0;
        p += got;
        n -= (size_t)got;
    }
    return 1;
}

static int ctl_send(uint16_t type, uint32_t request_id, const AsbRwPayload *m)
{
    static uint8_t out[ASB_RW_HEADER_BYTES + 512];
    size_t n = 0;
    if (asb_rw_encode(type, request_id, m, out, sizeof(out), &n)) return 0;
    EnterCriticalSection(&g_ctl_lock);
    int ok = send(g_ctl, (const char *)out, (int)n, 0) == (int)n;
    LeaveCriticalSection(&g_ctl_lock);
    return ok;
}

/* One complete control message into g_msg; returns its type or 0. */
static uint16_t ctl_recv(DWORD timeout_ms)
{
    AsbRwHeader h;
    size_t total = 0;
    if (!recv_exact(g_ctl, g_rxbuf, ASB_RW_HEADER_BYTES, timeout_ms)) return 0;
    if (asb_rw_frame_check(ASB_CH_RW_CONTROL, g_rxbuf, ASB_RW_HEADER_BYTES, &total)) return 0;
    if (!recv_exact(g_ctl, g_rxbuf + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES, 10000)) return 0;
    if (asb_rw_decode(ASB_CH_RW_CONTROL, g_rxbuf, total, &h, &g_msg, NULL, NULL)) return 0;
    return h.type;
}

static void log_surfaces(const AsbRwSurfaces *s)
{
    tlog("  SURFACES root %016llx rect (%d,%d,%d,%d) count %u", (unsigned long long)s->root_id, s->root_rect.left,
         s->root_rect.top, s->root_rect.right, s->root_rect.bottom, s->count);
    for (uint32_t i = 0; i < s->count; ++i)
        tlog("    z%u kind %u id %016llx rect (%d,%d,%d,%d)", s->surfaces[i].z, s->surfaces[i].kind,
             (unsigned long long)s->surfaces[i].surface_id, s->surfaces[i].rect.left, s->surfaces[i].rect.top,
             s->surfaces[i].rect.right, s->surfaces[i].rect.bottom);
}

static DWORD WINAPI ctl_reader(LPVOID unused)
{
    (void)unused;
    while (!g_ctl_stop) {
        uint16_t type = ctl_recv(1000);
        if (!type) {
            if (g_ctl_stop) break;
            fd_set set; struct timeval tv = { 0, 0 };
            FD_ZERO(&set); FD_SET(g_ctl, &set);
            if (select(0, &set, NULL, NULL, &tv) != 0) { tlog("  control channel closed"); break; }
            continue;
        }
        if (type == ASB_RW_SURFACES) {
            g_last_surfaces = g_msg.surfaces;
            InterlockedExchange(&g_last_surface_count, (LONG)g_msg.surfaces.count);
            InterlockedIncrement(&g_surfaces_msgs);
            log_surfaces(&g_msg.surfaces);
        } else if (type == ASB_RW_SELECT_RESULT) {
            tlog("  SELECT_RESULT %016llx state %u error %s", (unsigned long long)g_msg.select_result.id,
                 g_msg.select_result.state, asb_rw_error_name(g_msg.select_result.error));
            InterlockedExchange(&g_shared_state, (LONG)g_msg.select_result.state);
        } else if (type == ASB_RW_ERROR) {
            tlog("  ERROR %s detail %u", asb_rw_error_name(g_msg.error.code), g_msg.error.detail);
        }
    }
    return 0;
}

static int text_contains(const uint16_t *s, uint16_t n, const wchar_t *needle)
{
    size_t k = wcslen(needle);
    for (size_t i = 0; i + k <= n; ++i)
        if (!_wcsnicmp((const wchar_t *)s + i, needle, k)) return 1;
    return 0;
}

/* ============================================================ frames */

typedef struct Seen {
    uint64_t id;
    uint32_t kind, frames, distinct, magenta, partial_alpha, premul_ok, transport, width, height;
    uint32_t generation, gen_jumps_same_size, frames_after_jump, regressed, recovered;
    uint32_t hash;
    AsbRwRect rect;
    int saved;
} Seen;
static Seen g_seen[128];
static uint32_t g_seen_n;
static ID3D11Texture2D *g_staging;
static UINT g_sw, g_sh;

static Seen *seen(uint64_t id)
{
    for (uint32_t i = 0; i < g_seen_n; ++i) if (g_seen[i].id == id) return &g_seen[i];
    if (g_seen_n == 128) return NULL;
    memset(&g_seen[g_seen_n], 0, sizeof(g_seen[0]));
    g_seen[g_seen_n].id = id;
    return &g_seen[g_seen_n++];
}

static void write_bmp(const char *name, const uint32_t *px, uint32_t stride_px, uint32_t w, uint32_t h)
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
    for (uint32_t y = 0; y < h; ++y) fwrite(px + (size_t)y * stride_px, 4, w, f);
    fclose(f);
}

static void inspect(ID3D11Device *dev, ID3D11DeviceContext *ctx, const AsbRwHostFrame *f, const char *phase)
{
    const AsbRwFrameHeader *h = &f->header;
    Seen *s = seen(h->surface_id);
    if (!s) return;
    const uint32_t *px;
    uint32_t stride;
    D3D11_MAPPED_SUBRESOURCE m;
    int mapped = 0;
    if (h->transport == ASB_RW_TRANSPORT_GPU) {
        if (!g_staging || g_sw < h->width || g_sh < h->height) {
            D3D11_TEXTURE2D_DESC d;
            if (g_staging) ID3D11Texture2D_Release(g_staging);
            g_staging = NULL;
            memset(&d, 0, sizeof(d));
            d.Width = h->width > g_sw ? h->width : g_sw; d.Height = h->height > g_sh ? h->height : g_sh;
            d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_staging))) return;
            g_sw = d.Width; g_sh = d.Height;
        }
        D3D11_BOX box = { 0, 0, 0, h->width, h->height, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)g_staging, 0, 0, 0, 0, (ID3D11Resource *)f->texture, 0, &box);
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
        mapped = 1;
        px = (const uint32_t *)m.pData;
        stride = m.RowPitch / 4;
    } else {
        px = (const uint32_t *)f->pixels;
        stride = h->stride / 4;
    }
    uint32_t hash = 2166136261u, magenta = 0, partial = 0, bad = 0;
    for (uint32_t y = 0; y < h->height; y += 2)
        for (uint32_t x = 0; x < h->width; x += 2) {
            uint32_t p = px[y * stride + x], a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            hash = (hash ^ p) * 16777619u;
            if (r > 240 && g < 16 && b > 240 && a == 255) magenta++;
            if (a > 8 && a < 247) partial++;
            if (r > a + 1 || g > a + 1 || b > a + 1) bad++;
        }
    if (s->frames && s->generation && h->generation > s->generation && h->width == s->width && h->height == s->height) {
        s->gen_jumps_same_size++;
        s->frames_after_jump = 0;
        tlog("  surface %016llx kind %u: generation %u -> %u at the same size (device/pool recovery)",
             (unsigned long long)s->id, h->kind, s->generation, h->generation);
    }
    if (h->generation < s->generation) s->regressed = 1;
    s->frames++;
    s->frames_after_jump++;
    if (s->gen_jumps_same_size && s->frames_after_jump >= 10) s->recovered = 1;
    if (hash != s->hash) s->distinct++;
    s->hash = hash;
    if (magenta > 16) s->magenta = 1;
    if (partial > 16) s->partial_alpha = 1;
    s->premul_ok = !bad;
    s->kind = h->kind; s->transport = h->transport; s->width = h->width; s->height = h->height; s->rect = h->rect;
    s->generation = h->generation;
    if (g_evidence && phase && s->frames == 6) {
        char name[96];
        sprintf_s(name, sizeof(name), "%s-%016llx-kind%u", phase, (unsigned long long)s->id, h->kind);
        write_bmp(name, px, stride, h->width, h->height);
    }
    if (mapped) ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_staging, 0);
}

static void consume(AsbRwFramesHost *host, ID3D11Device *dev, ID3D11DeviceContext *ctx, DWORD ms, const char *phase)
{
    ULONGLONG end = GetTickCount64() + ms;
    AsbRwHostSurfaceInfo info[128];
    while (GetTickCount64() < end) {
        AsbRwFramesHostStats st;
        asb_rw_frames_host_stats(host, &st);
        if (st.state >= ASB_RW_HOST_CLOSED) break;
        uint32_t n = asb_rw_frames_host_surfaces(host, info, 128);
        for (uint32_t i = 0; i < n; ++i) {
            AsbRwHostFrame f;
            if (asb_rw_frames_host_acquire(host, info[i].surface_id, &f) != 1) continue;
            inspect(dev, ctx, &f, phase);
            asb_rw_frames_host_release(host, &f);
        }
        Sleep(33);
    }
}

static ID3D11Device *host_device(ID3D11DeviceContext **ctx)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL, *chosen = NULL;
    ID3D11Device *dev = NULL;
    *ctx = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return NULL;
    for (UINT i = 0; !chosen && IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(adapter, &d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) chosen = adapter; else IDXGIAdapter1_Release(adapter);
    }
    if (chosen) {
        D3D11CreateDevice((IDXGIAdapter *)chosen, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                          D3D11_SDK_VERSION, &dev, NULL, ctx);
        IDXGIAdapter1_Release(chosen);
    }
    IDXGIFactory1_Release(factory);
    if (dev) {
        ID3D10Multithread *mt = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mt))) {
            ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
            ID3D10Multithread_Release(mt);
        }
    }
    return dev;
}

static AsbRwFramesHost *frames_start(const GUID *vm, ID3D11Device *dev, int gpu, const char *name)
{
    AsbRwFramesHostConfig hc;
    memset(&hc, 0, sizeof(hc));
    hc.vm_id = *vm;
    hc.device = dev;
    hc.allow_gpu = (uint32_t)gpu;
    hc.slots = 3;
    hc.max_cpu_bytes = 128u << 20;
    hc.ctx = (void *)name;
    hc.log = sink_log;
    SOCKET s = asb_rw_frames_connect_vm(vm, 5000);
    if (s == INVALID_SOCKET) { tlog("  frame channel connect failed (%d)", WSAGetLastError()); return NULL; }
    AsbRwFramesHost *h = asb_rw_frames_host_start(s, g_tag, &hc);
    for (int i = 0; i < 100 && h; ++i) {
        AsbRwFramesHostStats st;
        asb_rw_frames_host_stats(h, &st);
        if (st.state != ASB_RW_HOST_CONNECTING) break;
        Sleep(50);
    }
    return h;
}

static Seen *seen_kind(uint32_t kind)
{
    Seen *best = NULL;
    for (uint32_t i = 0; i < g_seen_n; ++i)
        if (g_seen[i].kind == kind && (!best || g_seen[i].frames > best->frames)) best = &g_seen[i];
    return best;
}

static void stats_line(AsbRwFramesHost *h, const char *what)
{
    AsbRwFramesHostStats st;
    asb_rw_frames_host_stats(h, &st);
    tlog("  %s: state %u gpu_agreed %u surfaces %u frames gpu %llu cpu %llu superseded %llu stale %llu pools %llu/%llu failed "
         "retired %llu ended %llu cpu_bytes %llu textures %llu",
         what, st.state, st.agreement.gpu, st.surfaces, st.frames_gpu, st.frames_cpu, st.superseded, st.stale, st.pools_created,
         st.pools_failed, st.pools_retired, st.surfaces_ended, st.cpu_bytes, st.textures_live);
}

/* Runs one transport phase and checks it. */
static void phase(const GUID *vm, int gpu, DWORD seconds, ID3D11Device **dev, ID3D11DeviceContext **ctx)
{
    const char *name = gpu ? "gpu" : "cpu";
    AsbRwFramesHostStats st;
    tlog("== phase %s (%lu s) ==", name, seconds);
    memset(g_seen, 0, sizeof(g_seen));
    g_seen_n = 0;
    AsbRwFramesHost *h = frames_start(vm, *dev, gpu, name);
    CHECK(h != NULL, "frame channel connected");
    if (!h) return;
    asb_rw_frames_host_stats(h, &st);
    CHECK(st.state == ASB_RW_HOST_RUNNING, "frame channel negotiated for the control session");
    if (gpu && g_guest_gpu) CHECK(st.agreement.gpu == 1, "shared GPU pools agreed (guest helper adapter matches the host GPU)");
    else if (gpu) CHECK(st.agreement.gpu == 0, "guest without a GPU adapter: GPU allowed by the host, CPU frames negotiated");
    else CHECK(st.agreement.gpu == 0, "forced CPU frames");
    int host_loss = gpu;
    gpu = (int)st.agreement.gpu;   /* expected transport from here on */
    DWORD first = host_loss ? seconds * 1000 / 2 : seconds * 1000;
    consume(h, *dev, *ctx, first, name);
    stats_line(h, name);
    if (host_loss) {
        /* Host device loss half way: everything freed, reconnect with a new device. */
        asb_rw_frames_host_simulate_device_loss(h);
        for (int i = 0; i < 60; ++i) { asb_rw_frames_host_stats(h, &st); if (st.state >= ASB_RW_HOST_CLOSED) break; Sleep(50); }
        CHECK(st.state == ASB_RW_HOST_DEVICE_LOST && st.surfaces == 0 && st.cpu_bytes == 0, "host device loss closes and frees");
        asb_rw_frames_host_stop(h);
        if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; g_sw = g_sh = 0; }
        ID3D11DeviceContext_Release(*ctx);
        ID3D11Device_Release(*dev);
        *dev = host_device(ctx);
        Seen *r = seen_kind(ASB_RW_SURFACE_ROOT);
        uint32_t before = r ? r->frames : 0;
        Sleep(500);
        /* A new frame channel is a new generation space: only order within one connection matters. */
        for (uint32_t i = 0; i < g_seen_n; ++i) { g_seen[i].generation = 0; g_seen[i].frames_after_jump = 0; }
        h = frames_start(vm, *dev, 1, "reconnect");
        CHECK(h != NULL, "frame channel reconnected after host device loss");
        if (!h) return;
        consume(h, *dev, *ctx, seconds * 1000 - first, name);
        r = seen_kind(ASB_RW_SURFACE_ROOT);
        CHECK(r && r->frames > before + 10, "frames resumed after host device loss");
        stats_line(h, "gpu after reconnect");
    }
    asb_rw_frames_host_stats(h, &st);
    Seen *root = seen_kind(ASB_RW_SURFACE_ROOT), *popup = seen_kind(ASB_RW_SURFACE_POPUP), *menu = seen_kind(ASB_RW_SURFACE_MENU);
    tlog("  root: %u frames (%u distinct) %ux%u transport %u gen %u magenta %u", root ? root->frames : 0,
         root ? root->distinct : 0, root ? root->width : 0, root ? root->height : 0, root ? root->transport : 0,
         root ? root->generation : 0, root ? root->magenta : 0);
    CHECK(root && root->frames >= 20 && root->distinct >= 10 && root->magenta, "live root content");
    CHECK(root && root->transport == (gpu ? ASB_RW_TRANSPORT_GPU : ASB_RW_TRANSPORT_CPU), "root frames on the expected transport");
    CHECK(gpu ? st.frames_gpu > 0 : (st.frames_cpu > 0 && st.frames_gpu == 0), "frames counted on the expected transport");
    CHECK(popup && popup->partial_alpha && popup->premul_ok, "owned popup keeps premultiplied per-pixel alpha");
    CHECK(popup && root && popup->rect.right > root->rect.right, "owned popup extends beyond the root's right edge");
    if (menu) tlog("  menu: %u frames rect (%d,%d,%d,%d), root right %d", menu->frames, menu->rect.left, menu->rect.top,
                   menu->rect.right, menu->rect.bottom, root ? root->rect.right : 0);
    CHECK(menu && menu->frames >= 1 && root && menu->rect.right > root->rect.right,
          "overflowing menu captured as its own surface beyond the root");
    CHECK(st.surfaces_ended >= 1, "closed menu ended its surface (SURFACE_END)");
    CHECK(root && root->recovered && !root->regressed,
          "guest device loss recovered: new generation at the same size, frames continued");
    CHECK(st.cpu_bytes <= st.agreement.max_cpu_bytes && st.textures_live <= (uint64_t)st.surfaces * st.agreement.slots + 2u,
          "queues and memory stay bounded");
    asb_rw_frames_host_stop(h);
    if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; g_sw = g_sh = 0; }
}

int wmain(int argc, wchar_t **argv)
{
    GUID vm;
    const wchar_t *title = L"eh6.5 capture test";
    DWORD seconds = 60;
    int probe = 0;
    char evidence[MAX_PATH] = { 0 };
    CLSIDFromString(L"{52fb82b7-1d7e-54a4-80f4-3578cf3b4507}", &vm);
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--vm") && i + 1 < argc) {
            wchar_t b[64];
            swprintf_s(b, 64, L"{%s}", argv[++i]);
            CLSIDFromString(b, &vm);
        } else if (!wcscmp(argv[i], L"--title") && i + 1 < argc) title = argv[++i];
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--evidence") && i + 1 < argc) WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, evidence, MAX_PATH, NULL, NULL);
        else if (!wcscmp(argv[i], L"--probe")) probe = 1;
    }
    if (evidence[0]) {
        char path[MAX_PATH];
        g_evidence = evidence;
        CreateDirectoryA(evidence, NULL);
        sprintf_s(path, sizeof(path), "%s\\check.log", evidence);
        fopen_s(&g_log, path, "w");
    }
    InitializeCriticalSection(&g_ctl_lock);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_ctl = hv_connect(&vm, ASB_CH_RW_CONTROL);
    if (g_ctl == INVALID_SOCKET) { tlog("control channel 9 connect failed (%d): helper not running, or VM started without services 9-11", WSAGetLastError()); return 2; }
    if (ctl_recv(5000) != ASB_RW_HELLO) { tlog("no guest HELLO"); return 2; }
    AsbRwHello guest = g_msg.hello, local;
    asb_rw_hello_init(&local, ASB_RW_ROLE_HOST, ASB_RW_CAP_KNOWN_MASK, 0x00010000u, 0, 0);
    AsbRwNegotiated n = asb_rw_negotiate(&local, &guest);
    g_tag = n.session_tag;
    tlog("guest helper %08x caps %08x driver %u tag %08x -> %s (missing %08x)", guest.helper_version, guest.capabilities,
         guest.driver_version, guest.session_tag, asb_rw_error_name(n.error), n.missing_required);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hello = local;
    ctl_send(ASB_RW_HELLO, 1, &g_msg);
    memset(&g_msg, 0, sizeof(g_msg));
    ctl_send(ASB_RW_INVENTORY_REQUEST, 2, &g_msg);
    uint64_t target = 0;
    for (int i = 0; i < 50; ++i) {
        uint16_t type = ctl_recv(3000);
        if (!type) break;
        if (type != ASB_RW_INVENTORY_SNAPSHOT) continue;
        for (uint32_t k = 0; k < g_msg.snapshot.count; ++k) {
            const AsbRwInventoryEntry *e = &g_msg.snapshot.entries[k];
            char t8[512];
            WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)e->title, e->title_len, t8, sizeof(t8) - 1, NULL, NULL);
            t8[e->title_len < 511 ? WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)e->title, e->title_len, NULL, 0, NULL, NULL) : 0] = 0;
            tlog("  inventory %016llx pid %u flags %02x \"%s\"", (unsigned long long)e->id, e->pid, e->flags, t8);
            if (!target && text_contains(e->title, e->title_len, title)) target = e->id;
        }
        break;
    }
    CHECK(guest.capabilities & ASB_RW_CAP_FRAMES_CPU, "guest helper offers CPU frames (eh6.5 helper)");
    CHECK(guest.capabilities & ASB_RW_CAP_POPUPS, "guest helper offers popup surfaces");
    g_guest_gpu = (guest.capabilities & ASB_RW_CAP_FRAMES_GPU) != 0;
    tlog("%s guest helper offers GPU frames: %s", g_guest_gpu ? "ok  " : "note",
         g_guest_gpu ? "yes" : "no (no hardware capture adapter in the guest; CPU fallback)");
    if (probe) { closesocket(g_ctl); return g_failures ? 1 : 0; }
    CHECK(target != 0, "test window listed in the guest inventory");
    if (!target) return 1;

    HANDLE reader = CreateThread(NULL, 0, ctl_reader, NULL, 0, NULL);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = target;
    g_msg.select.select = 1;
    ctl_send(ASB_RW_SELECT_REQUEST, 3, &g_msg);
    for (int i = 0; i < 100 && g_shared_state != ASB_RW_STATE_SHARED && g_shared_state != ASB_RW_STATE_FAILED; ++i) Sleep(100);
    CHECK(g_shared_state == ASB_RW_STATE_SHARED, "test window shared (parked on the private display)");
    for (int i = 0; i < 50 && g_last_surface_count < 2; ++i) Sleep(100);
    CHECK(g_last_surface_count >= 2, "SURFACES lists the root and its owned popup");

    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device *dev = host_device(&ctx);
    CHECK(dev != NULL, "host render device");
    if (dev) {
        phase(&vm, 1, seconds, &dev, &ctx);
        phase(&vm, 0, seconds, &dev, &ctx);
        /* Teardown: unselect while a frame channel is open. */
        tlog("== teardown ==");
        AsbRwFramesHost *h = frames_start(&vm, dev, 1, "teardown");
        if (h) consume(h, dev, ctx, 1500, NULL);
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.select.id = target;
        g_msg.select.select = 0;
        ctl_send(ASB_RW_SELECT_REQUEST, 4, &g_msg);
        for (int i = 0; i < 50 && g_shared_state != ASB_RW_STATE_NONE; ++i) Sleep(100);
        CHECK(g_shared_state == ASB_RW_STATE_NONE, "unselect restored the window");
        for (int i = 0; i < 30 && g_last_surface_count != 0; ++i) Sleep(100);
        CHECK(g_last_surface_count == 0, "SURFACES count 0 after unselect");
        if (h) {
            AsbRwFramesHostStats st;
            for (int i = 0; i < 30; ++i) { asb_rw_frames_host_stats(h, &st); if (!st.surfaces) break; Sleep(100); }
            stats_line(h, "after unselect");
            CHECK(st.surfaces == 0 && st.cpu_bytes == 0 && st.textures_live == 0, "every surface ended; nothing left allocated");
            asb_rw_frames_host_stop(h);
        }
        if (g_staging) ID3D11Texture2D_Release(g_staging);
        ID3D11DeviceContext_Release(ctx);
        ID3D11Device_Release(dev);
    }
    InterlockedExchange(&g_ctl_stop, 1);
    shutdown(g_ctl, SD_BOTH);
    WaitForSingleObject(reader, 3000);
    closesocket(g_ctl);
    tlog("%s eh6.5 VM frame check: %d of %d checks failed", g_failures ? "FAILED" : "PASS", g_failures, g_checks);
    if (g_log) fclose(g_log);
    return g_failures ? 1 : 0;
}
