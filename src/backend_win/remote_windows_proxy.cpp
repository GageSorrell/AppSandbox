/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_proxy
 *
 * @file      remote_windows_proxy.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows per-VM host manager and native proxy windows (AppSandbox-eh6.6).
   See remote_windows_proxy.h for the design; the protocol rules live in remote_windows_session.c. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "remote_windows_proxy.h"
#include <windowsx.h>
#include <wctype.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>

#include "remote_windows_frames_host.h"
#include "remote_windows_session.h"
extern "C" {
#include "hcs_services.h"   /* C header without its own extern "C" */
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#define HV_PROTOCOL_RAW 1
#define HVSOCKET_CONNECT_TIMEOUT 1

namespace {

struct HvSockAddr { ADDRESS_FAMILY family; USHORT reserved; GUID vm_id; GUID service_id; };

const wchar_t kMessageClass[] = L"AppSandbox.RemoteWindows.Manager";
const wchar_t kProxyClass[] = L"AppSandbox.RemoteWindows.Proxy";
const wchar_t kPopupClass[] = L"AppSandbox.RemoteWindows.Popup";
const wchar_t kDragCookieProp[] = ASB_RW_DRAG_COOKIE_PROP;
const wchar_t kDragSurfaceProp[] = ASB_RW_DRAG_SURFACE_PROP;

enum : UINT {
    WM_RW_CONNECTED = WM_APP + 0x100,   /* wParam generation */
    WM_RW_MESSAGE,                      /* wParam generation, lParam Blob* */
    WM_RW_DISCONNECTED,                 /* wParam generation */
    WM_RW_FRAMES_SOCKET,                /* wParam generation, lParam SOCKET */
    WM_RW_FRAMES_DIRTY,
    WM_RW_SELECT,                       /* wParam id ptr, lParam select */
    WM_RW_RESTORE_ALL,
    WM_RW_STOP,
    WM_RW_INPUT_CHANGED,                /* eh6.7: the VM's input arbitration changed */
};

const UINT_PTR kTickTimer = 1;
const UINT kTickMs = 50;
const DWORD kConnectTimeoutMs = 3000;

struct Blob { size_t len; uint8_t data[1]; };

struct Render {
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11Texture2D> content;
    UINT swap_w = 0, swap_h = 0;
    UINT tex_w = 0, tex_h = 0;          /* content texture size */
    UINT frame_w = 0, frame_h = 0;      /* content size of the newest frame */
    bool has_content = false;
    uint64_t presented = 0;
};

struct Proxy;

struct SurfaceWin {
    uint64_t surface_id = 0;
    uint32_t kind = 0;
    HWND hwnd = nullptr;
    Proxy *proxy = nullptr;
    Render render;
    AsbRwRect guest = {};               /* guest rectangle (SURFACES) */
    AsbRwWindowState state = {};
    bool have_state = false;
    bool programmatic = false;          /* SetWindowPos from the manager in progress */
    bool in_sizemove = false;
    int pressed = 0;                    /* caption button pressed (HT*) */
};

struct Proxy {
    uint64_t id = 0;
    SurfaceWin root;
    std::vector<SurfaceWin *> popups;   /* bottom-to-top */
    AsbRwRect guest_root = {};          /* SURFACES.root_rect */
    HICON icon = nullptr;
    bool aumid = false;
};

struct Insets { int left, right, bottom, top_max; };

} // namespace

struct AsbRwManager {
    AsbRwManagerConfig config = {};
    wchar_t group[64] = {};             /* copy of config.group */
    HANDLE ui_thread = nullptr, conn_thread = nullptr;
    HANDLE ready = nullptr, stop_event = nullptr;
    HWND hwnd = nullptr;                /* message window (UI thread) */
    volatile LONG stopping = 0;
    volatile LONG frames_dirty = 0;
    bool started_ok = false;

    /* connection (connection thread; the socket is closed under send_lock) */
    CRITICAL_SECTION send_lock;
    SOCKET control = INVALID_SOCKET;
    volatile LONG generation = 0;

    /* UI thread */
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> dcomp;
    AsbRwSession *session = nullptr;
    LONG session_generation = 0;
    AsbRwFramesHost *frames = nullptr;
    bool frames_connecting = false;
    ULONGLONG frames_retry_at = 0;
    std::map<uint64_t, Proxy *> proxies;
    std::map<uint64_t, SurfaceWin *> by_surface;   /* every surface window, root included */
    uint64_t connections = 0, presented_total = 0;
    ULONGLONG last_publish = 0;
    bool host_active = false;           /* a proxy window has host activation (eh6.7) */
    volatile LONG input_dirty = 0;

    /* snapshots for other threads */
    CRITICAL_SECTION lock;
    AsbRwManagerStatus status = {};
    std::vector<AsbRwManagerEntry> entries;
    std::vector<AsbRwProxyInfo> infos;
    std::map<uint64_t, HICON> icons;    /* eh6.8 selector icons by inventory id (under lock) */

    /* eh6.9 drag and drop: the medium-integrity broker finds our windows by this cookie */
    uint64_t drag_cookie = 0;
    bool drag_ok = false;               /* config.drag_start succeeded: file capabilities offered */
};

namespace {

/* ================================ logging ================================= */

void mlog(AsbRwManager *m, const char *fmt, ...)
{
    char text[640];
    va_list args;
    if (!m->config.log) return;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    m->config.log(m->config.ctx, text);
}

void notify_changed(AsbRwManager *m)
{
    if (m->config.changed) m->config.changed(m->config.ctx);
}

/* ================================ sockets ================================= */

SOCKET connect_hv(const GUID *vm_id, int channel, DWORD timeout_ms)
{
    HvSockAddr addr = {};
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return s;
    DWORD t = timeout_ms;
    setsockopt(s, HV_PROTOCOL_RAW, HVSOCKET_CONNECT_TIMEOUT, (const char *)&t, sizeof(t));
    addr.family = AF_HYPERV;
    addr.vm_id = *vm_id;
    hcs_service_guid(L"windows", (unsigned)channel, &addr.service_id);
    if (connect(s, (const sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET connect_channel(AsbRwManager *m, int channel, DWORD timeout_ms)
{
    if (m->config.connect) return (SOCKET)m->config.connect(m->config.ctx, channel, timeout_ms);
    return connect_hv(&m->config.vm_id, channel, timeout_ms);
}

bool recv_exact(SOCKET s, uint8_t *p, size_t len)
{
    while (len) {
        int n = recv(s, (char *)p, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

DWORD WINAPI connection_thread(LPVOID arg)
{
    AsbRwManager *m = (AsbRwManager *)arg;
    DWORD backoff = 1000;
    std::vector<uint8_t> header(ASB_RW_HEADER_BYTES);
    while (!m->stopping) {
        SOCKET s = connect_channel(m, ASB_CH_RW_CONTROL, kConnectTimeoutMs);
        if (s == INVALID_SOCKET) {
            if (WaitForSingleObject(m->stop_event, backoff) == WAIT_OBJECT_0) break;
            backoff = backoff * 2 > 15000 ? 15000 : backoff * 2;
            continue;
        }
        backoff = 1000;
        DWORD send_timeout = 10000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));
        EnterCriticalSection(&m->send_lock);
        m->control = s;
        LONG gen = InterlockedIncrement(&m->generation);
        LeaveCriticalSection(&m->send_lock);
        PostMessageW(m->hwnd, WM_RW_CONNECTED, (WPARAM)gen, 0);
        for (;;) {
            size_t total = 0;
            if (!recv_exact(s, header.data(), ASB_RW_HEADER_BYTES)) break;
            uint32_t r = asb_rw_frame_check(ASB_CH_RW_CONTROL, header.data(), ASB_RW_HEADER_BYTES, &total);
            if (r || total < ASB_RW_HEADER_BYTES) { mlog(m, "manager: bad control frame (%s)", asb_rw_error_name(r)); break; }
            Blob *b = (Blob *)malloc(sizeof(Blob) + total);
            if (!b) break;
            b->len = total;
            memcpy(b->data, header.data(), ASB_RW_HEADER_BYTES);
            if (!recv_exact(s, b->data + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES) ||
                !PostMessageW(m->hwnd, WM_RW_MESSAGE, (WPARAM)gen, (LPARAM)b)) {
                free(b);
                break;
            }
        }
        EnterCriticalSection(&m->send_lock);
        closesocket(s);
        m->control = INVALID_SOCKET;
        LeaveCriticalSection(&m->send_lock);
        PostMessageW(m->hwnd, WM_RW_DISCONNECTED, (WPARAM)gen, 0);
        if (!m->stopping) WaitForSingleObject(m->stop_event, 1000);
    }
    return 0;
}

struct FramesConnect { AsbRwManager *m; LONG gen; };

DWORD WINAPI frames_connect_thread(LPVOID arg)
{
    FramesConnect *fc = (FramesConnect *)arg;
    SOCKET s = connect_channel(fc->m, ASB_CH_RW_FRAMES, kConnectTimeoutMs);
    if (!PostMessageW(fc->m->hwnd, WM_RW_FRAMES_SOCKET, (WPARAM)fc->gen, (LPARAM)s) && s != INVALID_SOCKET) closesocket(s);
    free(fc);
    return 0;
}

/* ================================ device ================================== */

bool create_device(AsbRwManager *m)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, m->device.ReleaseAndGetAddressOf(), nullptr,
                                   m->context.ReleaseAndGetAddressOf());
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               m->device.ReleaseAndGetAddressOf(), nullptr, m->context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { mlog(m, "manager: D3D11 device failed (0x%08lX)", (unsigned long)hr); return false; }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m->device.As(&mt))) mt->SetMultithreadProtected(TRUE);   /* the frame receiver creates pools */
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(m->device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(m->factory.ReleaseAndGetAddressOf()))))
        return false;
    hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(m->dcomp.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) { mlog(m, "manager: DirectComposition failed (0x%08lX)", (unsigned long)hr); return false; }
    return true;
}

bool device_lost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

/* ================================ rendering =============================== */

void release_render(Render &r)
{
    r.visual.Reset();
    r.target.Reset();
    r.swap.Reset();
    r.content.Reset();
    r.swap_w = r.swap_h = r.tex_w = r.tex_h = 0;
    r.has_content = false;
}

HRESULT ensure_target(AsbRwManager *m, SurfaceWin *w)
{
    Render &r = w->render;
    if (r.target) return S_OK;
    HRESULT hr = m->dcomp->CreateTargetForHwnd(w->hwnd, TRUE, &r.target);
    if (SUCCEEDED(hr)) hr = m->dcomp->CreateVisual(&r.visual);
    if (SUCCEEDED(hr)) hr = r.target->SetRoot(r.visual.Get());
    return hr;
}

HRESULT ensure_swap(AsbRwManager *m, SurfaceWin *w, UINT width, UINT height)
{
    Render &r = w->render;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    HRESULT hr = ensure_target(m, w);
    if (FAILED(hr)) return hr;
    if (r.swap && r.swap_w == width && r.swap_h == height) return S_OK;
    if (r.swap) {
        hr = r.swap->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    } else {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = width;
        d.Height = height;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = m->factory->CreateSwapChainForComposition(m->device.Get(), &d, nullptr, &r.swap);
        if (SUCCEEDED(hr)) hr = r.visual->SetContent(r.swap.Get());
        if (SUCCEEDED(hr)) hr = m->dcomp->Commit();
    }
    if (SUCCEEDED(hr)) { r.swap_w = width; r.swap_h = height; }
    return hr;
}

void handle_device_loss(AsbRwManager *m, HRESULT hr);

/* Draws the newest content into the window at its current client size: content at (0,0), the
   remainder filled (opaque for framed windows during live resize, transparent for popups). */
void present(AsbRwManager *m, SurfaceWin *w)
{
    RECT rc;
    if (!w->hwnd || !GetClientRect(w->hwnd, &rc) || IsIconic(w->hwnd)) return;
    UINT cw = (UINT)(rc.right - rc.left), ch = (UINT)(rc.bottom - rc.top);
    if (!cw || !ch) return;
    HRESULT hr = ensure_swap(m, w, cw, ch);
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(hr)) hr = w->render.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (SUCCEEDED(hr)) hr = m->device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    if (FAILED(hr)) { if (device_lost(hr)) handle_device_loss(m, hr); return; }
    bool framed = w->kind == ASB_RW_SURFACE_ROOT || w->kind == ASB_RW_SURFACE_OWNED;
    static const float fill_framed[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
    static const float fill_clear[4] = { 0, 0, 0, 0 };
    /* Windows that draw their own frame (WinUI, e.g. Notepad) can have a capture a few pixels
       smaller than their DWM visible frame: the 2 px side/bottom border strip is not captured.
       Such a small gap is that strip - keep it transparent and centre the content horizontally
       (no top strip) so it lines up with the visible frame used for hit testing. Larger gaps
       (a live resize ahead of the guest) are filled opaquely. */
    UINT gap_w = cw > w->render.frame_w ? cw - w->render.frame_w : 0, gap_h = ch > w->render.frame_h ? ch - w->render.frame_h : 0;
    bool border_gap = w->render.has_content && gap_w <= 8 && gap_h <= 8;
    m->context->ClearRenderTargetView(rtv.Get(), framed && w->render.has_content && !border_gap ? fill_framed : fill_clear);
    if (w->render.has_content) {
        UINT dx = border_gap ? gap_w / 2 : 0;
        D3D11_BOX box = { 0, 0, 0, w->render.frame_w < cw ? w->render.frame_w : cw,
                          w->render.frame_h < ch ? w->render.frame_h : ch, 1 };
        if (box.right && box.bottom) m->context->CopySubresourceRegion(back.Get(), 0, dx, 0, 0, w->render.content.Get(), 0, &box);
    }
    hr = w->render.swap->Present(0, 0);
    if (FAILED(hr)) { if (device_lost(hr)) handle_device_loss(m, hr); return; }
    w->render.presented++;
    m->presented_total++;
}

/* Copies one received frame into the window's content texture, then releases the frame. */
void upload(AsbRwManager *m, SurfaceWin *w, AsbRwHostFrame *f)
{
    Render &r = w->render;
    UINT fw = f->header.width, fh = f->header.height;
    if (!r.content || r.tex_w < fw || r.tex_h < fh || r.tex_w > fw * 2 || r.tex_h > fh * 2) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = fw;
        d.Height = fh;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        r.content.Reset();
        HRESULT hr = m->device->CreateTexture2D(&d, nullptr, &r.content);
        if (FAILED(hr)) {
            asb_rw_frames_host_release(m->frames, f);
            if (device_lost(hr)) handle_device_loss(m, hr);
            return;
        }
        r.tex_w = fw;
        r.tex_h = fh;
    }
    D3D11_BOX box = { 0, 0, 0, fw, fh, 1 };
    if (f->texture) m->context->CopySubresourceRegion(r.content.Get(), 0, 0, 0, 0, f->texture, 0, &box);
    else if (f->pixels) m->context->UpdateSubresource(r.content.Get(), 0, &box, f->pixels, f->header.stride, 0);
    r.frame_w = fw;
    r.frame_h = fh;
    r.has_content = true;
    asb_rw_frames_host_release(m->frames, f);
}

void drain_frames(AsbRwManager *m)
{
    if (!m->frames || !m->device) return;
    for (auto &it : m->by_surface) {
        SurfaceWin *w = it.second;
        AsbRwHostFrame frame;
        memset(&frame, 0, sizeof(frame));
        if (asb_rw_frames_host_acquire(m->frames, w->surface_id, &frame) != 1) continue;
        upload(m, w, &frame);
        if (!m->frames) return;   /* device loss stopped the receiver */
        present(m, w);
    }
}

/* ================================ geometry helpers ======================== */


UINT window_dpi(HWND hwnd)
{
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return dpi ? dpi : 96;
}

Insets frame_insets(HWND hwnd, DWORD style, DWORD ex)
{
    RECT a = { 0, 0, 100, 100 };
    AdjustWindowRectExForDpi(&a, style, FALSE, ex, window_dpi(hwnd));
    Insets i = { -a.left, a.right - 100, a.bottom - 100, a.bottom - 100 };
    return i;
}

/* Client insets of a framed proxy: the system frame for its style and DPI on the left, right
   and bottom (native resize borders and shadow), none at the top (no host caption). DWM keeps a
   2 px border inside that margin; proxies set DWMWA_BORDER_COLOR to none, so the strip is
   transparent and the guest's own frame is the only border drawn. top_max: maximized windows
   overhang their monitor by the frame on every side. */
Insets insets_of(HWND hwnd)
{
    return frame_insets(hwnd, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE));
}

void client_origin(HWND hwnd, POINT *pt)
{
    pt->x = pt->y = 0;
    ClientToScreen(hwnd, pt);
}

/* Window rectangle for a framed proxy whose client area is `client` (screen coordinates). */
RECT framed_rect(HWND hwnd, const RECT &client)
{
    Insets in = insets_of(hwnd);
    RECT r = { client.left - in.left, client.top, client.right + in.right, client.bottom + in.bottom };
    return r;
}

void set_client_size(SurfaceWin *w, int cw, int ch)
{
    Insets in = insets_of(w->hwnd);
    w->programmatic = true;
    SetWindowPos(w->hwnd, nullptr, 0, 0, cw + in.left + in.right, ch + in.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    w->programmatic = false;
}

/* Places popups/owned windows relative to the root proxy's client origin; restacks on demand. */
void layout_popups(AsbRwManager *m, Proxy *p, bool restack)
{
    if (!p->root.hwnd || IsIconic(p->root.hwnd)) {
        for (SurfaceWin *w : p->popups) if (IsWindowVisible(w->hwnd)) ShowWindow(w->hwnd, SW_HIDE);
        return;
    }
    POINT origin;
    client_origin(p->root.hwnd, &origin);
    for (SurfaceWin *w : p->popups) {
        if (w->in_sizemove) continue;
        int x = origin.x + (w->guest.left - p->guest_root.left), y = origin.y + (w->guest.top - p->guest_root.top);
        int cw = w->guest.right - w->guest.left, ch = w->guest.bottom - w->guest.top;
        RECT target = { x, y, x + cw, y + ch };
        if (w->kind == ASB_RW_SURFACE_OWNED) target = framed_rect(w->hwnd, target);
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | (restack ? 0u : SWP_NOZORDER);
        if (!IsWindowVisible(w->hwnd)) flags |= SWP_SHOWWINDOW;
        w->programmatic = true;
        /* bottom-to-top: each window goes directly above the previous one */
        SetWindowPos(w->hwnd, restack ? HWND_TOP : nullptr, target.left, target.top, target.right - target.left,
                     target.bottom - target.top, flags);
        w->programmatic = false;
        present(m, w);
    }
}

/* ================================ window icon / identity ================== */

HICON icon_from(const AsbRwWindowIcon *icon)
{
    if (!icon->width || !icon->height) return nullptr;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = (LONG)icon->width;
    bi.bV5Height = -(LONG)icon->height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    const uint8_t *src = icon->pixels;
    uint8_t *dst = (uint8_t *)bits;
    for (uint32_t i = 0; i < icon->width * icon->height; ++i) {
        uint8_t a = src[i * 4 + 3];
        for (int c = 0; c < 3; ++c) {   /* icons take straight alpha */
            unsigned v = a ? (unsigned)src[i * 4 + c] * 255u / a : 0u;
            dst[i * 4 + c] = (uint8_t)(v > 255 ? 255 : v);
        }
        dst[i * 4 + 3] = a;
    }
    HBITMAP mask = CreateBitmap((int)icon->width, (int)icon->height, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return h;
}

void set_aumid(AsbRwManager *m, Proxy *p, const uint16_t *app, uint32_t app_len, bool clear)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(p->root.hwnd, IID_PPV_ARGS(&store)))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (!clear) {
        /* Group proxies per guest application, apart from App Sandbox itself; never pinnable. */
        wchar_t id[128];
        int n = swprintf_s(id, ARRAYSIZE(id), L"AppSandbox.RemoteWindows.%s.", m->config.group ? m->config.group : L"Guest");
        for (uint32_t i = 0; i < app_len && n > 0 && n < (int)ARRAYSIZE(id) - 1; ++i) {
            wchar_t c = (wchar_t)app[i];
            id[n++] = (iswalnum(c) || c == L'.') ? c : L'_';
        }
        if (n > 0) id[n] = 0;
        InitPropVariantFromString(id, &v);
        store->SetValue(PKEY_AppUserModel_ID, v);
        PropVariantClear(&v);
        InitPropVariantFromBoolean(TRUE, &v);
        store->SetValue(PKEY_AppUserModel_PreventPinning, v);
        PropVariantClear(&v);
        p->aumid = true;
    } else if (p->aumid) {
        store->SetValue(PKEY_AppUserModel_ID, v);   /* VT_EMPTY removes the property */
        store->SetValue(PKEY_AppUserModel_PreventPinning, v);
        p->aumid = false;
    }
    store->Commit();
}

/* ================================ input (eh6.7) =========================== */

AsbRwSession *input_session(AsbRwManager *m)
{
    if (m->session) m->session->now_ms = GetTickCount64();
    return m->session;
}

/* Clipboard focus client: one for all proxies of this VM. */
void set_host_active(AsbRwManager *m, bool active)
{
    if (m->host_active == active) return;
    m->host_active = active;
    if (m->config.focus) m->config.focus(m->config.ctx, active ? 1 : 0);
}

bool ours(AsbRwManager *m, HWND hwnd)
{
    return hwnd && GetPropW(hwnd, L"AsbRwManager") == (HANDLE)m;
}

void on_activate(AsbRwManager *m, SurfaceWin *w, bool active, HWND other)
{
    AsbRwSession *s = input_session(m);
    if (active) {
        if (s && w->proxy) asb_rw_session_input_focus(s, w->proxy->id, w->surface_id);
        set_host_active(m, true);
        return;
    }
    if (ours(m, other)) return;   /* activation moves to another proxy of this VM */
    if (s) asb_rw_session_input_focus(s, 0, 0);
    set_host_active(m, false);
}

/* Client-area mouse input of a root, owned window or popup proxy (1 px = 1 guest px). */
bool forward_mouse(AsbRwManager *m, SurfaceWin *w, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    uint32_t kind = 0, button = 0;
    int32_t wheel = 0;
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg) {
    case WM_MOUSEMOVE: kind = ASB_RW_MOUSE_MOVE; break;
    case WM_LBUTTONDOWN: kind = ASB_RW_MOUSE_DOWN; button = INPUT_BTN_LEFT; break;
    case WM_LBUTTONUP: kind = ASB_RW_MOUSE_UP; button = INPUT_BTN_LEFT; break;
    case WM_RBUTTONDOWN: kind = ASB_RW_MOUSE_DOWN; button = INPUT_BTN_RIGHT; break;
    case WM_RBUTTONUP: kind = ASB_RW_MOUSE_UP; button = INPUT_BTN_RIGHT; break;
    case WM_MBUTTONDOWN: kind = ASB_RW_MOUSE_DOWN; button = INPUT_BTN_MIDDLE; break;
    case WM_MBUTTONUP: kind = ASB_RW_MOUSE_UP; button = INPUT_BTN_MIDDLE; break;
    case WM_MOUSEWHEEL:
        kind = ASB_RW_MOUSE_WHEEL;
        wheel = GET_WHEEL_DELTA_WPARAM(wp);
        ScreenToClient(hwnd, &pt);
        break;
    default: return false;
    }
    AsbRwSession *s = input_session(m);
    if (!s || !w->proxy) return false;
    if (kind == ASB_RW_MOUSE_MOVE && m->drag_ok && GetCapture() == hwnd && (wp & (MK_LBUTTON | MK_RBUTTON)) &&
        !asb_rw_session_drag_busy(s) && asb_rw_session_files_enabled(s, ASB_RW_CAP_FILES_FROM_GUEST)) {
        /* eh6.9: the pointer left every proxy with a button held: maybe a guest OLE drag. */
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (!PtInRect(&rc, pt) && !ours(m, WindowFromPoint(screen))) {
            uint32_t held = (wp & MK_LBUTTON) ? INPUT_BTN_LEFT : INPUT_BTN_RIGHT;
            if (asb_rw_session_drag_probe(s, w->proxy->id, w->surface_id, held) == ASB_RW_OK) {
                AllowSetForegroundWindow(ASFW_ANY);   /* the broker's host drag needs the foreground */
                return true;
            }
        }
    }
    if (kind == ASB_RW_MOUSE_DOWN) SetCapture(hwnd);   /* drags keep reporting outside the proxy */
    asb_rw_session_input_mouse(s, w->proxy->id, w->surface_id, kind, button, pt.x, pt.y, wheel);
    if (kind == ASB_RW_MOUSE_UP && !(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) && GetCapture() == hwnd) ReleaseCapture();
    return true;
}

/* Keys of the active proxy. FALSE: the key stays on the host (shell shortcuts, no input). */
bool forward_key(AsbRwManager *m, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwSession *s = input_session(m);
    if (!s) return false;
    uint32_t vk = (uint32_t)wp, scan = (uint32_t)(lp >> 16) & 0xFFu;
    int ext = (int)((lp >> 24) & 1), up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    bool alt = GetKeyState(VK_MENU) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    if (vk == VK_LWIN || vk == VK_RWIN) return false;
    if (!up && asb_rw_input_key_is_local(vk, alt, ctrl, win)) return false;
    if (!scan) {   /* synthesized keys may carry no scan code */
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        scan = sc & 0xFFu;
        if ((sc & 0xFF00u) == 0xE000u) ext = 1;
    }
    return asb_rw_session_input_key(s, vk, scan, ext, up) != 0;
}

void input_sync(AsbRwManager *m)
{
    AsbRwSession *s = input_session(m);
    if (!s || !m->config.input.state) return;
    AsbRwInputPortState ps = {};
    m->config.input.state(m->config.input.ctx, &ps);
    AsbRwInputRouterState st = { ps.epoch, ps.windows_current, ps.user_owns, ps.hold };
    asb_rw_session_input_changed(s, &st);
}

/* A destroyed active proxy gets no WM_ACTIVATE; follow the real foreground. */
void check_host_active(AsbRwManager *m)
{
    if (!m->host_active || ours(m, GetForegroundWindow())) return;
    AsbRwSession *s = input_session(m);
    if (s) asb_rw_session_input_focus(s, 0, 0);
    set_host_active(m, false);
}

/* ================================ window procedures ======================= */

/* Window classes are process-wide; procedures find their state through GWLP_USERDATA (the
   surface) and the "AsbRwManager" window property (set right after creation). */
SurfaceWin *surface_of(HWND hwnd) { return (SurfaceWin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA); }

bool resizable(const SurfaceWin *w) { return !w->have_state || (w->state.flags & ASB_RW_WS_RESIZABLE); }

LRESULT map_hit(const SurfaceWin *w, uint32_t code, bool zoomed)
{
    switch (code) {
    case HTCAPTION: case HTSYSMENU: return HTCAPTION;
    case HTMINBUTTON: return w->kind == ASB_RW_SURFACE_ROOT ? HTMINBUTTON : HTCAPTION;
    case HTMAXBUTTON: return w->kind == ASB_RW_SURFACE_ROOT && resizable(w) ? HTMAXBUTTON : HTCAPTION;
    case HTCLOSE: return HTCLOSE;
    case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM: case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT:
    case HTBOTTOMRIGHT:
        if (zoomed || !resizable(w)) return code == HTTOP || code == HTTOPLEFT || code == HTTOPRIGHT ? HTCAPTION : HTCLIENT;
        return (LRESULT)code;
    default: return HTCLIENT;
    }
}

void report_size(AsbRwManager *m, SurfaceWin *w)
{
    if (!m->session || w->programmatic) return;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    if (w->kind == ASB_RW_SURFACE_ROOT) {
        uint32_t show = IsIconic(w->hwnd) ? ASB_RW_SHOW_MINIMIZED : IsZoomed(w->hwnd) ? ASB_RW_SHOW_MAXIMIZED : ASB_RW_SHOW_NORMAL;
        asb_rw_session_resize(m->session, w->proxy->id, show, rc.right - rc.left, rc.bottom - rc.top);
    } else if (w->kind == ASB_RW_SURFACE_OWNED) {
        POINT root_origin, own;
        client_origin(w->proxy->root.hwnd, &root_origin);
        client_origin(w->hwnd, &own);
        asb_rw_session_move_owned(m->session, w->proxy->id, w->surface_id, own.x - root_origin.x, own.y - root_origin.y,
                                  rc.right - rc.left, rc.bottom - rc.top);
    }
}

LRESULT framed_proc(AsbRwManager *m, SurfaceWin *w, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATE:
        on_activate(m, w, LOWORD(wp) != WA_INACTIVE, (HWND)lp);
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        if (forward_mouse(m, w, hwnd, msg, wp, lp)) return 0;
        break;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (forward_key(m, msg, wp, lp)) return 0;
        break;
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        if (m->session && m->session->input.host_root) return 0;   /* the guest translates its own keys */
        break;
    case WM_NCCALCSIZE:
        if (wp) {
            /* No host caption: the client area is the guest's visible frame. The invisible
               side/bottom borders stay (native resize, shadow); maximized windows overhang the
               monitor by the frame on every side. */
            NCCALCSIZE_PARAMS *params = (NCCALCSIZE_PARAMS *)lp;
            RECT *r = &params->rgrc[0];
            Insets in = insets_of(hwnd);
            r->left += in.left;
            r->right -= in.right;
            r->bottom -= in.bottom;
            if (IsZoomed(hwnd)) r->top += in.top_max;
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcW(hwnd, msg, wp, lp);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        ScreenToClient(hwnd, &pt);
        if (!PtInRect(&rc, pt)) {
            if (def >= HTLEFT && def <= HTBOTTOMRIGHT && !resizable(w)) return HTBORDER;
            return def;
        }
        if (!m->session) return HTCLIENT;
        uint32_t code = asb_rw_session_hit_test(m->session, w->proxy->id, w->surface_id, pt.x, pt.y);
        return map_hit(w, code, IsZoomed(hwnd) != 0);
    }
    case WM_NCLBUTTONDOWN:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) { w->pressed = (int)wp; return 0; }
        w->pressed = 0;
        break;
    case WM_NCLBUTTONUP:
        if (w->pressed && (int)wp == w->pressed) {
            WPARAM cmd = wp == HTMINBUTTON ? SC_MINIMIZE : wp == HTCLOSE ? SC_CLOSE : IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE;
            w->pressed = 0;
            PostMessageW(hwnd, WM_SYSCOMMAND, cmd, lp);
            return 0;
        }
        w->pressed = 0;
        break;
    case WM_NCMOUSELEAVE:
        w->pressed = 0;
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        Insets in = insets_of(hwnd);
        if (w->have_state && !resizable(w) && w->render.has_content) {
            LONG fw = (LONG)(w->guest.right - w->guest.left), fh = (LONG)(w->guest.bottom - w->guest.top);
            mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = fw + in.left + in.right;
            mmi->ptMinTrackSize.y = mmi->ptMaxTrackSize.y = fh + in.bottom;
        } else if (w->have_state) {
            if (w->state.min_width) mmi->ptMinTrackSize.x = w->state.min_width + in.left + in.right;
            if (w->state.min_height) mmi->ptMinTrackSize.y = w->state.min_height + in.bottom;
            if (w->state.max_width) mmi->ptMaxTrackSize.x = w->state.max_width + in.left + in.right;
            if (w->state.max_height) mmi->ptMaxTrackSize.y = w->state.max_height + in.bottom;
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        w->in_sizemove = true;
        if (m->session && w->kind == ASB_RW_SURFACE_ROOT) asb_rw_session_sizing(m->session, w->proxy->id, 1);
        return 0;
    case WM_EXITSIZEMOVE:
        w->in_sizemove = false;
        if (m->session && w->kind == ASB_RW_SURFACE_ROOT) asb_rw_session_sizing(m->session, w->proxy->id, 0);
        report_size(m, w);
        return 0;
    case WM_SIZE:
        if (w->kind == ASB_RW_SURFACE_ROOT) {
            report_size(m, w);
            present(m, w);
            layout_popups(m, w->proxy, false);
        } else if (!w->programmatic && w->in_sizemove) {
            present(m, w);
        }
        return 0;
    case WM_MOVE:
        if (w->kind == ASB_RW_SURFACE_ROOT) layout_popups(m, w->proxy, false);
        else if (!w->programmatic && !w->in_sizemove) report_size(m, w);
        return 0;
    case WM_DPICHANGED: {
        /* Keep 1:1 guest pixels: same client size, new frame insets at the suggested place. */
        RECT *suggested = (RECT *)lp, rc;
        GetClientRect(hwnd, &rc);
        Insets in = insets_of(hwnd);
        w->programmatic = true;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, rc.right + in.left + in.right, rc.bottom + in.bottom,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        w->programmatic = false;
            return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            if (m->session) asb_rw_session_close(m->session, w->proxy->id, w->surface_id);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (m->session) asb_rw_session_close(m->session, w->proxy->id, w->surface_id);
        return 0;   /* the guest application decides; the proxy goes when its window goes */
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        present(m, w);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (w->kind == ASB_RW_SURFACE_ROOT) set_aumid(m, w->proxy, nullptr, 0, true);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK proxy_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        ((SurfaceWin *)cs->lpCreateParams)->hwnd = hwnd;
    }
    SurfaceWin *w = surface_of(hwnd);
    AsbRwManager *m = (AsbRwManager *)GetPropW(hwnd, L"AsbRwManager");
    if (!w || !m) return DefWindowProcW(hwnd, msg, wp, lp);
    return framed_proc(m, w, hwnd, msg, wp, lp);
}

LRESULT CALLBACK popup_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        ((SurfaceWin *)cs->lpCreateParams)->hwnd = hwnd;
    }
    SurfaceWin *w = surface_of(hwnd);
    AsbRwManager *m = (AsbRwManager *)GetPropW(hwnd, L"AsbRwManager");
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   /* menus/tooltips never take activation from their root */
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        if (w && m && forward_mouse(m, w, hwnd, msg, wp, lp)) return 0;
        break;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        if (w && m) present(m, w);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ================================ proxies ================================= */

void register_classes(HINSTANCE inst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = proxy_wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kProxyClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = popup_wndproc;
    wc.lpszClassName = kPopupClass;
    wc.hIcon = nullptr;
    RegisterClassExW(&wc);
}

/* eh6.9: the drag broker (another process) recognizes proxy surfaces by these properties and
   names the surface in FILE_POINTER (window properties are readable across integrity levels). */
void tag_drag(AsbRwManager *m, SurfaceWin *w, HWND hwnd)
{
    if (!m->drag_ok) return;
    SetPropW(hwnd, kDragCookieProp, (HANDLE)(UINT_PTR)m->drag_cookie);
    SetPropW(hwnd, kDragSurfaceProp, (HANDLE)(UINT_PTR)w->surface_id);
}

void destroy_surface_window(AsbRwManager *m, SurfaceWin *w)
{
    m->by_surface.erase(w->surface_id);
    release_render(w->render);
    if (w->hwnd) {
        RemovePropW(w->hwnd, L"AsbRwManager");
        RemovePropW(w->hwnd, kDragCookieProp);
        RemovePropW(w->hwnd, kDragSurfaceProp);
        HWND h = w->hwnd;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (w->kind == ASB_RW_SURFACE_ROOT) set_aumid(m, w->proxy, nullptr, 0, true);
        DestroyWindow(h);
        w->hwnd = nullptr;
    }
}

HWND create_framed(AsbRwManager *m, SurfaceWin *w, HWND owner, const RECT &client, const wchar_t *title)
{
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | (owner ? 0u : WS_EX_APPWINDOW);
    if (owner) style &= ~(DWORD)(WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    /* Create, then size from the real insets at the window's own DPI. */
    HWND hwnd = CreateWindowExW(ex, kProxyClass, title, style, client.left, client.top,
                                client.right - client.left, client.bottom - client.top, owner, nullptr,
                                GetModuleHandleW(nullptr), w);
    if (!hwnd) return nullptr;
    SetPropW(hwnd, L"AsbRwManager", m);
    tag_drag(m, w, hwnd);
    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF no_border = DWMWA_COLOR_NONE;   /* the guest frame brings its own border */
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &no_border, sizeof(no_border));
    MARGINS margins = { 0, 0, 1, 0 };   /* keeps the top shadow/border with no host caption */
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    RECT wr = framed_rect(hwnd, client);
    w->programmatic = true;
    SetWindowPos(hwnd, nullptr, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    w->programmatic = false;
    return hwnd;
}

/* Initial host placement: the guest offset inside the work area under the cursor, fully visible. */
RECT initial_client(const AsbRwProxyInit *init)
{
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT work = mi.rcWork;
    int w = init->guest_rect.right - init->guest_rect.left, h = init->guest_rect.bottom - init->guest_rect.top;
    if (w < 64) w = 640;
    if (h < 48) h = 480;
    int x = work.left + 40 + (init->guest_rect.left > 0 ? init->guest_rect.left % 200 : 0);
    int y = work.top + 40 + (init->guest_rect.top > 0 ? init->guest_rect.top % 120 : 0);
    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    RECT r = { x, y, x + w, y + h };
    return r;
}

void copy_title(const uint16_t *src, uint32_t len, wchar_t *dst, size_t cap)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = (wchar_t)src[i];
    dst[n] = 0;
}

void apply_window_state(AsbRwManager *m, SurfaceWin *w, const AsbRwWindowState *st)
{
    bool first = !w->have_state;
    w->state = *st;
    w->have_state = true;
    LONG style = GetWindowLongW(w->hwnd, GWL_STYLE), want = style;
    if (w->kind == ASB_RW_SURFACE_ROOT) {
        want = (st->flags & ASB_RW_WS_MINIMIZABLE) ? (want | WS_MINIMIZEBOX) : (want & ~WS_MINIMIZEBOX);
        want = (st->flags & ASB_RW_WS_MAXIMIZABLE) && (st->flags & ASB_RW_WS_RESIZABLE) ? (want | WS_MAXIMIZEBOX)
                                                                                         : (want & ~WS_MAXIMIZEBOX);
    }
    if (want != style) {
        SetWindowLongW(w->hwnd, GWL_STYLE, want);
        w->programmatic = true;
        SetWindowPos(w->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        w->programmatic = false;
        }
    BOOL enabled = !(st->flags & ASB_RW_WS_DISABLED);
    if (!IsWindowEnabled(w->hwnd) != !enabled) EnableWindow(w->hwnd, enabled);
    (void)first;
    (void)m;
}

void frames_notify(void *ctx, uint64_t surface_id);

/* ================================ session ops (UI thread) ================= */

int op_send(void *ctx, const uint8_t *bytes, size_t len)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    int ok = 1;
    EnterCriticalSection(&m->send_lock);
    if (m->control == INVALID_SOCKET) ok = 0;
    while (ok && len) {
        int n = send(m->control, (const char *)bytes, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) { ok = 0; shutdown(m->control, SD_BOTH); break; }
        bytes += n;
        len -= (size_t)n;
    }
    LeaveCriticalSection(&m->send_lock);
    return ok;
}

void op_log(void *ctx, const char *message)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    if (m->config.log) m->config.log(m->config.ctx, message);
}

int op_proxy_create(void *ctx, uint64_t id, const AsbRwProxyInit *init)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    if (m->proxies.count(id)) return 1;
    Proxy *p = new Proxy();
    p->id = id;
    p->root.surface_id = id;
    p->root.kind = ASB_RW_SURFACE_ROOT;
    p->root.proxy = p;
    p->guest_root = init->guest_rect;
    p->root.guest = init->guest_rect;
    wchar_t title[ASB_RW_MAX_TITLE + 1];
    copy_title(init->title, init->title_len, title, ARRAYSIZE(title));
    RECT client = initial_client(init);
    if (!create_framed(m, &p->root, nullptr, client, title)) {
        mlog(m, "manager: proxy window creation failed (%lu)", GetLastError());
        delete p;
        return 0;
    }
    m->proxies[id] = p;
    m->by_surface[id] = &p->root;
    set_aumid(m, p, init->app, init->app_len, false);
    ShowWindow(p->root.hwnd, m->config.no_activate ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    present(m, &p->root);
    frames_notify(m, id);   /* a frame may already be waiting */
    return 1;
}

void op_proxy_destroy(void *ctx, uint64_t id)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    auto it = m->proxies.find(id);
    if (it == m->proxies.end()) return;
    Proxy *p = it->second;
    for (SurfaceWin *w : p->popups) { destroy_surface_window(m, w); delete w; }
    p->popups.clear();
    destroy_surface_window(m, &p->root);
    if (p->icon) DestroyIcon(p->icon);
    m->proxies.erase(it);
    delete p;
}

void op_proxy_title(void *ctx, uint64_t id, const uint16_t *title, uint32_t len)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    auto it = m->proxies.find(id);
    if (it == m->proxies.end()) return;
    wchar_t text[ASB_RW_MAX_TITLE + 1];
    copy_title(title, len, text, ARRAYSIZE(text));
    SetWindowTextW(it->second->root.hwnd, text);
}

void store_icon(AsbRwManager *m, const AsbRwWindowIcon *icon)
{
    HICON h = icon_from(icon);
    EnterCriticalSection(&m->lock);
    auto it = m->icons.find(icon->id);
    if (it != m->icons.end()) { if (it->second) DestroyIcon(it->second); m->icons.erase(it); }
    if (h) m->icons[icon->id] = h;
    LeaveCriticalSection(&m->lock);
}

void op_inventory_icon(void *ctx, const AsbRwWindowIcon *icon)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    store_icon(m, icon);
    notify_changed(m);
}

void op_proxy_icon(void *ctx, uint64_t id, const AsbRwWindowIcon *icon)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    store_icon(m, icon);
    auto it = m->proxies.find(id);
    if (it == m->proxies.end()) return;
    Proxy *p = it->second;
    HICON h = icon_from(icon);
    SendMessageW(p->root.hwnd, WM_SETICON, ICON_BIG, (LPARAM)h);
    SendMessageW(p->root.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)h);
    if (p->icon) DestroyIcon(p->icon);
    p->icon = h;
}

SurfaceWin *find_surface(AsbRwManager *m, uint64_t id, uint64_t surface_id)
{
    auto it = m->by_surface.find(surface_id);
    if (it == m->by_surface.end() || !it->second->proxy || it->second->proxy->id != id) return nullptr;
    return it->second;
}

void op_proxy_state(void *ctx, uint64_t id, uint64_t surface_id, const AsbRwWindowState *state)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    SurfaceWin *w = find_surface(m, id, surface_id);
    if (w && w->hwnd && (w->kind == ASB_RW_SURFACE_ROOT || w->kind == ASB_RW_SURFACE_OWNED)) apply_window_state(m, w, state);
}

void op_proxy_size(void *ctx, uint64_t id, int32_t width, int32_t height)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    auto it = m->proxies.find(id);
    if (it == m->proxies.end() || IsIconic(it->second->root.hwnd) || IsZoomed(it->second->root.hwnd)) return;
    set_client_size(&it->second->root, width, height);
    present(m, &it->second->root);
    layout_popups(m, it->second, false);
}

void op_proxy_show(void *ctx, uint64_t id, uint32_t show)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    auto it = m->proxies.find(id);
    if (it == m->proxies.end()) return;
    ShowWindow(it->second->root.hwnd, show == ASB_RW_SHOW_MINIMIZED ? SW_MINIMIZE :
                                      show == ASB_RW_SHOW_MAXIMIZED ? SW_MAXIMIZE : SW_RESTORE);
}

void op_proxy_surfaces(void *ctx, uint64_t id, const AsbRwSurfaces *surfaces)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    auto it = m->proxies.find(id);
    if (it == m->proxies.end()) return;
    Proxy *p = it->second;
    p->guest_root = surfaces->root_rect;
    p->root.guest = surfaces->root_rect;
    std::vector<SurfaceWin *> next;
    bool restack = false;
    for (uint32_t i = 0; i < surfaces->count; ++i) {
        const AsbRwSurface *s = &surfaces->surfaces[i];
        if (s->kind == ASB_RW_SURFACE_ROOT) continue;
        SurfaceWin *w = nullptr;
        for (SurfaceWin *old : p->popups) if (old->surface_id == s->surface_id) w = old;
        if (!w) {
            w = new SurfaceWin();
            w->surface_id = s->surface_id;
            w->kind = s->kind;
            w->proxy = p;
            w->guest = s->rect;
            RECT r = { 0, 0, s->rect.right - s->rect.left, s->rect.bottom - s->rect.top };
            if (s->kind == ASB_RW_SURFACE_OWNED) {
                if (!create_framed(m, w, p->root.hwnd, r, L"")) { delete w; continue; }
            } else {
                HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, kPopupClass, L"",
                                         WS_POPUP, r.left, r.top, r.right, r.bottom, p->root.hwnd, nullptr,
                                         GetModuleHandleW(nullptr), w);
                if (!h) { delete w; continue; }
                SetPropW(h, L"AsbRwManager", m);
                tag_drag(m, w, h);
            }
            m->by_surface[w->surface_id] = w;
            restack = true;
        }
        w->guest = s->rect;
        next.push_back(w);
    }
    for (SurfaceWin *old : p->popups) {
        bool keep = false;
        for (SurfaceWin *w : next) keep |= w == old;
        if (!keep) { destroy_surface_window(m, old); delete old; restack = true; }
    }
    if (!restack && next.size() == p->popups.size())
        for (size_t i = 0; i < next.size(); ++i) restack |= next[i] != p->popups[i];
    HWND foreground = GetForegroundWindow();
    bool ours = foreground == p->root.hwnd;
    for (SurfaceWin *w : p->popups) ours |= foreground == w->hwnd;
    SurfaceWin *new_owned = nullptr;
    for (SurfaceWin *w : next) {
        bool existed = false;
        for (SurfaceWin *old : p->popups) existed |= old == w;
        if (!existed && w->kind == ASB_RW_SURFACE_OWNED) new_owned = w;
    }
    p->popups = next;
    layout_popups(m, p, restack);
    /* A dialog the application opens while its root proxy is active takes the focus, as on the guest. */
    if (new_owned && ours && !m->config.no_activate) SetForegroundWindow(new_owned->hwnd);
    if (restack) frames_notify(m, 0);   /* frames for new windows may already be waiting */
}

void op_proxy_activate_modal(void *ctx, uint64_t id)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    auto it = m->proxies.find(id);
    if (it == m->proxies.end()) return;
    Proxy *p = it->second;
    for (auto w = p->popups.rbegin(); w != p->popups.rend(); ++w) {
        if ((*w)->kind != ASB_RW_SURFACE_OWNED || !IsWindowEnabled((*w)->hwnd)) continue;
        FLASHWINFO fi = { sizeof(fi), (*w)->hwnd, FLASHW_CAPTION, 3, 0 };
        if (!m->config.no_activate) SetForegroundWindow((*w)->hwnd);
        FlashWindowEx(&fi);
        return;
    }
    FLASHWINFO fi = { sizeof(fi), p->root.hwnd, FLASHW_CAPTION, 2, 0 };
    FlashWindowEx(&fi);
}

void op_hit_result(void *ctx, uint64_t id, uint64_t surface_id)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    SurfaceWin *w = find_surface(m, id, surface_id);
    POINT pt;
    if (!w || !w->hwnd || !GetCursorPos(&pt) || WindowFromPoint(pt) != w->hwnd) return;
    LRESULT hit = SendMessageW(w->hwnd, WM_NCHITTEST, 0, MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y));
    SendMessageW(w->hwnd, WM_SETCURSOR, (WPARAM)w->hwnd, MAKELPARAM((WORD)hit, WM_MOUSEMOVE));
}

uint32_t op_input_claim(void *ctx, int renew)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    return m->config.input.claim(m->config.input.ctx, renew);
}

uint32_t op_input_send(void *ctx, uint32_t epoch, const InputPacket *packet)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    return m->config.input.send ? m->config.input.send(m->config.input.ctx, epoch, packet) : 5u;
}

void op_input_guard(void *ctx, int on)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    if (m->config.input.guard) m->config.input.guard(m->config.input.ctx, on);
}

uint32_t op_input_hold(void *ctx)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    return m->config.input.hold ? m->config.input.hold(m->config.input.ctx) : 0u;
}

void op_input_unhold(void *ctx, uint32_t hold, int ok)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    if (m->config.input.unhold) m->config.input.unhold(m->config.input.ctx, hold, ok);
}

void publish(AsbRwManager *m);

void op_drag_capture(void *ctx)
{
    (void)ctx;
    ReleaseCapture();   /* the broker's DoDragDrop takes over the physical drag */
}

void op_changed(void *ctx)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    publish(m);
}

/* ================================ manager state =========================== */

void publish(AsbRwManager *m)
{
    AsbRwManagerStatus st = {};
    std::vector<AsbRwManagerEntry> entries;
    std::vector<AsbRwProxyInfo> infos;
    st.connections = m->connections;
    st.frames_presented = m->presented_total;
    st.frames_state = ASB_RW_HOST_CLOSED;
    if (m->session) {
        AsbRwSession *s = m->session;
        st.state = s->phase == ASB_RW_SESSION_HELLO ? ASB_RW_MGR_CONNECTING :
                   s->phase == ASB_RW_SESSION_READY ? ASB_RW_MGR_READY : ASB_RW_MGR_LIMITED;
        st.negotiation_error = s->negotiated.error;
        st.guest_capabilities = s->negotiated.capabilities;
        st.missing = ASB_RW_PROXY_REQUIRED & ~s->negotiated.capabilities;
        st.guest_helper_version = s->guest_helper_version;
        st.inventory_count = s->inventory.count;
        st.input_state = s->input.state;
        st.input_guard = (uint32_t)s->input.guard;
        st.input_last_error = s->input.last_error;
        st.input_root = s->input.host_root;
        st.input_injected = s->input.injected;
        st.input_activations = s->input.activations;
        st.input_failures = s->input.failures;
        st.input_dropped_stale = s->input.dropped_stale;
        st.input_released = s->input.released;
        for (uint32_t i = 0; i < s->inventory.count; ++i) {
            AsbRwManagerEntry e = {};
            e.entry = s->inventory.entries[i];
            const AsbRwHostProxy *p = asb_rw_session_proxy(s, e.entry.id);
            if (p) { e.state = p->state; e.error = p->error; }
            entries.push_back(e);
        }
    }
    if (m->frames) {
        AsbRwFramesHostStats fs;
        asb_rw_frames_host_stats(m->frames, &fs);
        st.frames_state = fs.state;
        st.frames_gpu = fs.agreement.gpu;
    }
    for (auto &it : m->proxies) {
        Proxy *p = it.second;
        AsbRwProxyInfo info = {};
        info.id = p->id;
        info.hwnd = p->root.hwnd;
        info.presented = p->root.render.presented;
        info.content_w = p->root.render.frame_w;
        info.content_h = p->root.render.frame_h;
        info.window_flags = p->root.state.flags;
        for (SurfaceWin *w : p->popups) {
            if (info.count >= ASB_RW_MAX_SURFACES) break;
            AsbRwProxySurfaceInfo &si = info.surfaces[info.count++];
            si.surface_id = w->surface_id;
            si.kind = w->kind;
            si.hwnd = w->hwnd;
            si.presented = w->render.presented;
        }
        infos.push_back(info);
    }
    st.proxies = (uint32_t)infos.size();
    EnterCriticalSection(&m->lock);
    m->status = st;
    m->entries.swap(entries);
    m->infos.swap(infos);
    for (auto it = m->icons.begin(); it != m->icons.end();) {   /* icons of windows that went away */
        bool listed = false;
        for (const AsbRwManagerEntry &e : m->entries) listed |= e.entry.id == it->first;
        if (listed) { ++it; continue; }
        if (it->second) DestroyIcon(it->second);
        it = m->icons.erase(it);
    }
    LeaveCriticalSection(&m->lock);
    notify_changed(m);
}

void stop_frames(AsbRwManager *m)
{
    if (!m->frames) return;
    asb_rw_frames_host_stop(m->frames);
    m->frames = nullptr;
}

void start_frames_connect(AsbRwManager *m)
{
    if (m->frames || m->frames_connecting || !m->session || m->session->phase != ASB_RW_SESSION_READY ||
        GetTickCount64() < m->frames_retry_at)
        return;
    FramesConnect *fc = (FramesConnect *)malloc(sizeof(*fc));
    if (!fc) return;
    fc->m = m;
    fc->gen = m->session_generation;
    HANDLE t = CreateThread(nullptr, 0, frames_connect_thread, fc, 0, nullptr);
    if (!t) { free(fc); return; }
    CloseHandle(t);
    m->frames_connecting = true;
}

void frames_notify(void *ctx, uint64_t surface_id)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    (void)surface_id;
    if (!InterlockedExchange(&m->frames_dirty, 1)) PostMessageW(m->hwnd, WM_RW_FRAMES_DIRTY, 0, 0);
}

void frames_log(void *ctx, const char *message)
{
    AsbRwManager *m = (AsbRwManager *)ctx;
    if (m->config.log) m->config.log(m->config.ctx, message);
}

void on_frames_socket(AsbRwManager *m, LONG gen, SOCKET s)
{
    m->frames_connecting = false;
    if (gen != m->session_generation || !m->session || m->stopping) {
        if (s != INVALID_SOCKET) closesocket(s);
        return;
    }
    if (s == INVALID_SOCKET) {
        m->frames_retry_at = GetTickCount64() + 2000;
        return;
    }
    AsbRwFramesHostConfig fc = {};
    fc.vm_id = m->config.local_frames ? GUID_NULL : m->config.vm_id;
    fc.device = m->config.allow_gpu ? m->device.Get() : nullptr;
    fc.allow_gpu = m->config.allow_gpu;
    fc.slots = 3;
    fc.max_cpu_bytes = 128u << 20;
    fc.ctx = m;
    fc.log = frames_log;
    fc.notify = frames_notify;
    m->frames = asb_rw_frames_host_start(s, m->session->negotiated.session_tag, &fc);
    mlog(m, "manager: frame channel %s", m->frames ? "connected" : "failed");
}

void destroy_all_render(AsbRwManager *m)
{
    for (auto &it : m->by_surface) release_render(it.second->render);
}

void handle_device_loss(AsbRwManager *m, HRESULT hr)
{
    mlog(m, "manager: render device lost (0x%08lX); recreating", (unsigned long)hr);
    stop_frames(m);
    destroy_all_render(m);
    m->dcomp.Reset();
    m->factory.Reset();
    m->context.Reset();
    m->device.Reset();
    if (!create_device(m)) { mlog(m, "manager: device recreation failed"); return; }
    for (auto &it : m->by_surface) present(m, it.second);
    m->frames_retry_at = GetTickCount64() + 250;
}

void end_session(AsbRwManager *m)
{
    stop_frames(m);
    if (m->session) {
        asb_rw_session_destroy(m->session);   /* destroys every proxy */
        m->session = nullptr;
    }
    publish(m);
}

void on_tick(AsbRwManager *m)
{
    ULONGLONG now = GetTickCount64();
    if (m->session) asb_rw_session_tick(m->session, now);
    check_host_active(m);
    if (InterlockedExchange(&m->input_dirty, 0)) input_sync(m);
    if (m->frames) {
        AsbRwFramesHostStats fs;
        asb_rw_frames_host_stats(m->frames, &fs);
        if (fs.state == ASB_RW_HOST_DEVICE_LOST) handle_device_loss(m, DXGI_ERROR_DEVICE_REMOVED);
        else if (fs.state == ASB_RW_HOST_CLOSED) { stop_frames(m); m->frames_retry_at = now + 1000; }
    }
    if (!m->frames && !m->frames_connecting && m->session && now >= m->frames_retry_at) start_frames_connect(m);
    if (now - m->last_publish > 500) { m->last_publish = now; publish(m); }
}

LRESULT CALLBACK manager_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwManager *m = (AsbRwManager *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!m) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_RW_CONNECTED: {
        end_session(m);
        m->session_generation = (LONG)wp;
        m->connections++;
        AsbRwSessionOps ops = {};
        ops.ctx = m;
        ops.send = op_send;
        ops.log = op_log;
        ops.proxy_create = op_proxy_create;
        ops.proxy_destroy = op_proxy_destroy;
        ops.proxy_title = op_proxy_title;
        ops.proxy_icon = op_proxy_icon;
        ops.proxy_state = op_proxy_state;
        ops.proxy_size = op_proxy_size;
        ops.proxy_show = op_proxy_show;
        ops.proxy_surfaces = op_proxy_surfaces;
        ops.proxy_activate_modal = op_proxy_activate_modal;
        ops.hit_result = op_hit_result;
        ops.changed = op_changed;
        ops.inventory_icon = op_inventory_icon;
        ops.drag_capture = op_drag_capture;   /* eh6.9 */
        ops.file_caps = m->drag_ok ? (ASB_RW_CAP_FILES_TO_GUEST | ASB_RW_CAP_FILES_FROM_GUEST) : 0;
        if (m->config.input.claim) {
            ops.input_claim = op_input_claim;
            ops.input_send = op_input_send;
            ops.input_guard = op_input_guard;
            ops.input_hold = op_input_hold;
            ops.input_unhold = op_input_unhold;
        }
        m->session = asb_rw_session_create(&ops);
        input_sync(m);
        mlog(m, "manager: control channel connected (generation %ld)", (long)wp);
        publish(m);
        return 0;
    }
    case WM_RW_MESSAGE: {
        Blob *b = (Blob *)lp;
        if ((LONG)wp == m->session_generation && m->session) {
            m->session->now_ms = GetTickCount64();
            uint32_t r = asb_rw_session_receive(m->session, b->data, b->len);
            if (r) {
                mlog(m, "manager: protocol error %s; reconnecting", asb_rw_error_name(r));
                EnterCriticalSection(&m->send_lock);
                if (m->control != INVALID_SOCKET) shutdown(m->control, SD_BOTH);
                LeaveCriticalSection(&m->send_lock);
            } else if (m->session && m->session->phase == ASB_RW_SESSION_READY) {
                start_frames_connect(m);
            }
        }
        free(b);
        return 0;
    }
    case WM_RW_DISCONNECTED:
        if ((LONG)wp == m->session_generation) {
            mlog(m, "manager: control channel closed");
            end_session(m);
        }
        return 0;
    case WM_RW_FRAMES_SOCKET:
        on_frames_socket(m, (LONG)wp, (SOCKET)lp);
        return 0;
    case WM_RW_FRAMES_DIRTY:
        InterlockedExchange(&m->frames_dirty, 0);
        drain_frames(m);
        return 0;
    case WM_RW_SELECT:
        return m->session ? asb_rw_session_select(m->session, *(const uint64_t *)wp, (int)lp) : ASB_RW_ERR_BUSY;
    case WM_RW_RESTORE_ALL:
        return m->session ? asb_rw_session_restore_all(m->session) : ASB_RW_ERR_BUSY;
    case WM_RW_INPUT_CHANGED:
        InterlockedExchange(&m->input_dirty, 0);
        input_sync(m);
        return 0;
    case WM_TIMER:
        if (wp == kTickTimer) on_tick(m);
        return 0;
    case WM_RW_STOP:
        KillTimer(hwnd, kTickTimer);
        end_session(m);
        set_host_active(m, false);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI ui_thread(LPVOID arg)
{
    AsbRwManager *m = (AsbRwManager *)arg;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = manager_wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kMessageClass;
    RegisterClassExW(&wc);
    register_classes(inst);
    if (create_device(m)) {
        m->hwnd = CreateWindowExW(0, kMessageClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        if (m->hwnd) {
            SetWindowLongPtrW(m->hwnd, GWLP_USERDATA, (LONG_PTR)m);
            SetTimer(m->hwnd, kTickTimer, kTickMs, nullptr);
            m->started_ok = true;
        }
    }
    SetEvent(m->ready);
    if (m->started_ok) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    /* Drain messages that still own heap blocks. */
    MSG msg;
    while (PeekMessageW(&msg, nullptr, WM_RW_MESSAGE, WM_RW_MESSAGE, PM_REMOVE)) free((void *)msg.lParam);
    while (PeekMessageW(&msg, nullptr, WM_RW_FRAMES_SOCKET, WM_RW_FRAMES_SOCKET, PM_REMOVE))
        if ((SOCKET)msg.lParam != INVALID_SOCKET) closesocket((SOCKET)msg.lParam);
    m->dcomp.Reset();
    m->factory.Reset();
    m->context.Reset();
    m->device.Reset();
    CoUninitialize();
    return 0;
}

} // namespace

/* ================================ public API ============================== */

extern "C" AsbRwManager *asb_rw_manager_start(const AsbRwManagerConfig *config)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return nullptr;
    AsbRwManager *m = new AsbRwManager();
    m->config = *config;
    if (config->group) {
        wcsncpy_s(m->group, ARRAYSIZE(m->group), config->group, _TRUNCATE);
        m->config.group = m->group;
    }
    InitializeCriticalSection(&m->send_lock);
    InitializeCriticalSection(&m->lock);
    m->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->ui_thread = m->ready && m->stop_event ? CreateThread(nullptr, 0, ui_thread, m, 0, nullptr) : nullptr;
    if (m->ui_thread) WaitForSingleObject(m->ready, INFINITE);
    if (!m->started_ok) {
        if (m->ui_thread) { WaitForSingleObject(m->ui_thread, INFINITE); CloseHandle(m->ui_thread); }
        if (m->ready) CloseHandle(m->ready);
        if (m->stop_event) CloseHandle(m->stop_event);
        DeleteCriticalSection(&m->send_lock);
        DeleteCriticalSection(&m->lock);
        delete m;
        WSACleanup();
        return nullptr;
    }
    /* eh6.9: drag and drop runs in a broker the host app starts (medium integrity). */
    m->drag_cookie = 0;
    while (!m->drag_cookie) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        m->drag_cookie = ((uint64_t)GetCurrentProcessId() << 40) ^ (uint64_t)t.QuadPart ^ ((uint64_t)(UINT_PTR)m << 8);
    }
    if (config->drag_start) m->drag_ok = config->drag_start(config->ctx, m->drag_cookie) != 0;
    m->conn_thread = CreateThread(nullptr, 0, connection_thread, m, 0, nullptr);
    mlog(m, "manager: started (drag and drop %s)", m->drag_ok ? "on" : "off");
    return m;
}

extern "C" void asb_rw_manager_stop(AsbRwManager *m)
{
    if (!m) return;
    InterlockedExchange(&m->stopping, 1);
    SetEvent(m->stop_event);
    EnterCriticalSection(&m->send_lock);
    if (m->control != INVALID_SOCKET) shutdown(m->control, SD_BOTH);
    LeaveCriticalSection(&m->send_lock);
    if (m->conn_thread) { WaitForSingleObject(m->conn_thread, INFINITE); CloseHandle(m->conn_thread); }
    PostMessageW(m->hwnd, WM_RW_STOP, 0, 0);
    WaitForSingleObject(m->ui_thread, INFINITE);
    if (m->drag_ok && m->config.drag_stop) m->config.drag_stop(m->config.ctx);
    CloseHandle(m->ui_thread);
    CloseHandle(m->ready);
    CloseHandle(m->stop_event);
    DeleteCriticalSection(&m->send_lock);
    DeleteCriticalSection(&m->lock);
    for (auto &it : m->icons) if (it.second) DestroyIcon(it.second);
    if (m->config.log) m->config.log(m->config.ctx, "manager: stopped");
    delete m;
    WSACleanup();
}

extern "C" void asb_rw_manager_status(AsbRwManager *m, AsbRwManagerStatus *out)
{
    EnterCriticalSection(&m->lock);
    *out = m->status;
    LeaveCriticalSection(&m->lock);
}

extern "C" uint32_t asb_rw_manager_inventory(AsbRwManager *m, AsbRwManagerEntry *out, uint32_t max)
{
    EnterCriticalSection(&m->lock);
    uint32_t n = (uint32_t)(m->entries.size() < max ? m->entries.size() : max);
    for (uint32_t i = 0; i < n; ++i) out[i] = m->entries[i];
    LeaveCriticalSection(&m->lock);
    return n;
}

extern "C" uint32_t asb_rw_manager_proxies(AsbRwManager *m, AsbRwProxyInfo *out, uint32_t max)
{
    EnterCriticalSection(&m->lock);
    uint32_t n = (uint32_t)(m->infos.size() < max ? m->infos.size() : max);
    for (uint32_t i = 0; i < n; ++i) out[i] = m->infos[i];
    LeaveCriticalSection(&m->lock);
    return n;
}

extern "C" uint32_t asb_rw_manager_select(AsbRwManager *m, uint64_t id, int select)
{
    DWORD_PTR result = ASB_RW_ERR_BUSY;
    if (!SendMessageTimeoutW(m->hwnd, WM_RW_SELECT, (WPARAM)&id, select, SMTO_BLOCK, 5000, &result)) return ASB_RW_ERR_BUSY;
    return (uint32_t)result;
}

extern "C" HICON asb_rw_manager_icon(AsbRwManager *m, uint64_t id)
{
    HICON copy = nullptr;
    EnterCriticalSection(&m->lock);
    auto it = m->icons.find(id);
    if (it != m->icons.end() && it->second) copy = CopyIcon(it->second);
    LeaveCriticalSection(&m->lock);
    return copy;
}

extern "C" void asb_rw_manager_input_changed(AsbRwManager *m)
{
    if (m && m->hwnd && !InterlockedExchange(&m->input_dirty, 1)) PostMessageW(m->hwnd, WM_RW_INPUT_CHANGED, 0, 0);
}

extern "C" uint32_t asb_rw_manager_restore_all(AsbRwManager *m)
{
    DWORD_PTR result = ASB_RW_ERR_BUSY;
    if (!SendMessageTimeoutW(m->hwnd, WM_RW_RESTORE_ALL, 0, 0, SMTO_BLOCK, 5000, &result)) return ASB_RW_ERR_BUSY;
    return (uint32_t)result;
}
