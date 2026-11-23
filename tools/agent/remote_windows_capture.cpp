/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_capture
 *
 * @file      remote_windows_capture.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows guest capture engine (AppSandbox-eh6.5). See remote_windows_capture.h. */
#include "remote_windows_capture.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "../transport/asb_rw_frames.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
namespace WGI = ::Windows::Graphics::DirectX::Direct3D11;

namespace {

constexpr DWORD HELLO_TIMEOUT_MS = 5000;
constexpr UINT KEY_GUEST = 0, KEY_HOST = 1;

bool device_lost_hr(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

struct Surface {
    AsbRwCaptureTarget target{};
    bool failed = false;           /* could not start: not retried while still requested */
    bool ended = false;            /* SURFACE_END sent (closed); not restarted */
    std::atomic<bool> closed{ false };   /* capture item closed (window destroyed) */
    std::atomic<bool> stopping{ false };
    std::atomic<int> active{ 0 };       /* frame callbacks running for this surface */
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    event_token frame_token{}, closed_token{};
    winrt::Windows::Graphics::SizeInt32 pool_size{};
    /* mailbox (frame_lock) */
    com_ptr<ID3D11Texture2D> latest;
    UINT lw = 0, lh = 0;
    bool has_frame = false, pending = false;
    uint64_t captured = 0, captured_at_send = 0;
    /* frame channel (engine thread) */
    int tx = -1;
    uint32_t pool_gen = 0, pool_slots = 0, pool_w = 0, pool_h = 0, opened = 0;
    com_ptr<ID3D11Texture2D> slot_tex[ASB_RW_MAX_FRAME_SLOTS];
    com_ptr<IDXGIKeyedMutex> slot_mutex[ASB_RW_MAX_FRAME_SLOTS];
    com_ptr<ID3D11Texture2D> staging;
    UINT sw = 0, sh = 0;
};

} // namespace

struct AsbRwCapture {
    AsbRwCaptureConfig config{};
    HANDLE thread = nullptr, wake = nullptr, ready = nullptr, loss_event = nullptr;
    std::atomic<bool> stop{ false }, device_lost{ false };
    std::atomic<int> callbacks{ 0 };
    bool recovering = false;
    ULONGLONG next_recovery = 0;
    /* requests from the control thread */
    std::mutex request_lock;
    uint32_t session_tag = 0;
    std::map<uint64_t, std::vector<AsbRwCaptureTarget>> wanted;   /* root -> surfaces */
    /* engine thread */
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11Device1> device1;
    com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice rt_device{ nullptr };
    std::mutex d3d_lock;           /* immediate context */
    std::mutex frame_lock;         /* mailboxes */
    std::map<uint64_t, std::shared_ptr<Surface>> surfaces;
    SOCKET sock = INVALID_SOCKET;
    uint32_t conn_tag = 0;
    AsbRwTx tx;
    std::vector<uint8_t> packed;
    /* stats */
    std::mutex stats_lock;
    AsbRwCaptureStats stats{};
};

static void clog(AsbRwCapture *c, const char *fmt, ...)
{
    char text[512];
    va_list args;
    if (!c->config.log) return;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    c->config.log(c->config.ctx, text);
}

template <typename F> static void with_stats(AsbRwCapture *c, F f)
{
    std::lock_guard<std::mutex> g(c->stats_lock);
    f(c->stats);
}

/* ---- device ----------------------------------------------------------------------------- */

static HRESULT create_device(AsbRwCapture *c)
{
    com_ptr<IDXGIFactory1> factory;
    com_ptr<IDXGIAdapter1> chosen;
    DXGI_ADAPTER_DESC1 desc{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    for (UINT i = 0; SUCCEEDED(hr) && !chosen; ++i) {
        com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (d.VendorId == 0x1414 && d.DeviceId == 0x008C)) continue;
        chosen = adapter;
        desc = d;
    }
    c->device = nullptr;
    c->context = nullptr;
    c->device1 = nullptr;
    c->rt_device = nullptr;
    hr = E_FAIL;
    if (chosen)
        hr = D3D11CreateDevice(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                               c->device.put(), nullptr, c->context.put());
    if (FAILED(hr)) {
        memset(&desc, 0, sizeof(desc));
        desc.VendorId = 0x1414;
        desc.DeviceId = 0x008C;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                               c->device.put(), nullptr, c->context.put());
    }
    if (SUCCEEDED(hr)) hr = c->device->QueryInterface(IID_PPV_ARGS(c->device1.put()));
    if (SUCCEEDED(hr)) {
        com_ptr<ID3D10Multithread> mt;
        if (SUCCEEDED(c->device->QueryInterface(IID_PPV_ARGS(mt.put())))) mt->SetMultithreadProtected(TRUE);
        com_ptr<IDXGIDevice> dxgi = c->device.as<IDXGIDevice>();
        com_ptr<IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put());
        if (SUCCEEDED(hr)) c->rt_device = inspectable.as<IDirect3DDevice>();
    }
    bool hardware = SUCCEEDED(hr) && !(desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
    with_stats(c, [&](AsbRwCaptureStats &s) {
        s.vendor_id = SUCCEEDED(hr) ? desc.VendorId : 0;
        s.device_id = SUCCEEDED(hr) ? desc.DeviceId : 0;
        s.gpu_available = hardware && c->config.allow_gpu;
    });
    if (SUCCEEDED(hr))
        clog(c, "capture: device on adapter %04x:%04x (%ls)%s", desc.VendorId, desc.DeviceId, desc.Description,
             hardware ? "" : " - software, CPU frames only");
    else
        clog(c, "capture: no D3D11 device (0x%08lX)", (unsigned long)hr);
    return hr;
}

static void check_device(AsbRwCapture *c, HRESULT hr, const char *where)
{
    HRESULT reason = c->device ? c->device->GetDeviceRemovedReason() : S_OK;
    if (!device_lost_hr(hr) && reason == S_OK) return;
    if (!c->device_lost.exchange(true)) {
        clog(c, "capture: device lost in %s (0x%08lX, reason 0x%08lX)", where, (unsigned long)hr, (unsigned long)reason);
        SetEvent(c->wake);
    }
}

/* ---- capture sessions ------------------------------------------------------------------- */

static void on_frame(AsbRwCapture *c, const std::weak_ptr<Surface> &weak, Direct3D11CaptureFramePool const &pool)
{
    c->callbacks++;
    struct Leave { AsbRwCapture *c; ~Leave() { c->callbacks--; } } leave{ c };
    auto s = weak.lock();
    if (!s) return;
    s->active++;
    struct LeaveSurface { Surface *s; ~LeaveSurface() { s->active--; } } leave_surface{ s.get() };
    if (s->stopping || c->stop || c->device_lost) return;
    try {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        auto size = frame.ContentSize();
        if (size.Width <= 0 || size.Height <= 0) return;
        auto access = frame.Surface().as<WGI::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        UINT w = (UINT)size.Width < d.Width ? (UINT)size.Width : d.Width;
        UINT h = (UINT)size.Height < d.Height ? (UINT)size.Height : d.Height;
        if (w > ASB_RW_MAX_FRAME_WIDTH) w = ASB_RW_MAX_FRAME_WIDTH;
        if (h > ASB_RW_MAX_FRAME_HEIGHT) h = ASB_RW_MAX_FRAME_HEIGHT;
        HRESULT hr = S_OK;
        {
            std::lock_guard<std::mutex> fl(c->frame_lock);
            std::lock_guard<std::mutex> dl(c->d3d_lock);
            if (!s->latest || s->lw != w || s->lh != h) {
                D3D11_TEXTURE2D_DESC m{};
                m.Width = w; m.Height = h; m.MipLevels = 1; m.ArraySize = 1;
                m.Format = DXGI_FORMAT_B8G8R8A8_UNORM; m.SampleDesc.Count = 1; m.Usage = D3D11_USAGE_DEFAULT;
                s->latest = nullptr;
                hr = c->device->CreateTexture2D(&m, nullptr, s->latest.put());
                if (SUCCEEDED(hr)) { s->lw = w; s->lh = h; }
            }
            if (SUCCEEDED(hr)) {
                D3D11_BOX box{ 0, 0, 0, w, h, 1 };
                c->context->CopySubresourceRegion(s->latest.get(), 0, 0, 0, 0, texture.get(), 0, &box);
                hr = c->device->GetDeviceRemovedReason();
            }
            if (SUCCEEDED(hr)) {
                s->has_frame = true;
                s->pending = true;
                s->captured++;
            }
        }
        frame.Close();
        if (size.Width != s->pool_size.Width || size.Height != s->pool_size.Height) {
            s->pool_size = size;
            pool.Recreate(c->rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
        if (FAILED(hr)) check_device(c, hr, "frame copy");
        else with_stats(c, [](AsbRwCaptureStats &st) { st.captured++; });
        SetEvent(c->wake);
    } catch (hresult_error const &e) {
        check_device(c, e.code(), "frame arrival");
    } catch (...) {
    }
}

static void stop_capture(AsbRwCapture *c, Surface *s)
{
    (void)c;
    s->stopping = true;
    try {
        if (s->pool) s->pool.FrameArrived(s->frame_token);
        if (s->item) s->item.Closed(s->closed_token);
    } catch (...) {
    }
    try { if (s->session) s->session.Close(); } catch (...) {}
    try { if (s->pool) s->pool.Close(); } catch (...) {}
    /* A callback that already started finishes before the objects go away. */
    for (int i = 0; i < 400 && s->active > 0; ++i) Sleep(5);
    s->session = nullptr;
    s->pool = nullptr;
    s->item = nullptr;
}

static HRESULT start_capture(AsbRwCapture *c, const std::shared_ptr<Surface> &s)
{
    if (!c->rt_device) return E_NOT_VALID_STATE;
    HWND hwnd = (HWND)(ULONG_PTR)s->target.hwnd;
    try {
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{ nullptr };
        check_hresult(interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item)));
        s->item = item;
        s->pool_size = item.Size();
        if (s->pool_size.Width <= 0 || s->pool_size.Height <= 0) s->pool_size = { 1, 1 };
        s->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(c->rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                 2, s->pool_size);
        s->session = s->pool.CreateCaptureSession(item);
        s->session.IsCursorCaptureEnabled(false);
        try { s->session.IsBorderRequired(false); } catch (...) {}
        /* Popups are surfaces of their own; the root frame must not contain clipped copies. */
        try { s->session.IncludeSecondaryWindows(false); } catch (...) {}
        std::weak_ptr<Surface> weak = s;
        s->frame_token = s->pool.FrameArrived(
            [c, weak](Direct3D11CaptureFramePool const &sender, auto &&) { on_frame(c, weak, sender); });
        s->closed_token = item.Closed([c, weak](GraphicsCaptureItem const &, auto &&) {
            if (auto p = weak.lock()) { p->closed = true; SetEvent(c->wake); }
        });
        s->stopping = false;
        s->session.StartCapture();
        return S_OK;
    } catch (hresult_error const &e) {
        stop_capture(c, s.get());
        return e.code();
    }
}

/* ---- frame channel ---------------------------------------------------------------------- */

static bool send_all(AsbRwCapture *c, const uint8_t *p, size_t len)
{
    while (len) {
        int n = send(c->sock, (const char *)p, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_msg(AsbRwCapture *c, uint16_t type, const AsbRwPayload &m)
{
    uint8_t out[ASB_RW_HEADER_BYTES + 256];
    size_t n = 0;
    uint32_t r = asb_rw_encode(type, 0, &m, out, sizeof(out), &n);
    if (r) {
        clog(c, "capture: encode %u failed (%s)", type, asb_rw_error_name(r));
        return false;
    }
    return send_all(c, out, n);
}

static bool recv_exact(AsbRwCapture *c, uint8_t *p, size_t len, DWORD timeout_ms)
{
    DWORD waited = 0;
    while (len) {
        fd_set set;
        timeval tv{ 0, 100000 };
        FD_ZERO(&set);
        FD_SET(c->sock, &set);
        int ready = select(0, &set, nullptr, nullptr, &tv);
        if (ready < 0 || c->stop) return false;
        if (!ready) {
            waited += 100;
            if (waited >= timeout_ms) return false;
            continue;
        }
        int n = recv(c->sock, (char *)p, (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void release_pool(Surface *s)
{
    for (uint32_t k = 0; k < ASB_RW_MAX_FRAME_SLOTS; ++k) {
        s->slot_tex[k] = nullptr;
        s->slot_mutex[k] = nullptr;
    }
    s->pool_gen = s->pool_slots = s->pool_w = s->pool_h = s->opened = 0;
}

static void drop_connection(AsbRwCapture *c, const char *why)
{
    if (c->sock == INVALID_SOCKET) return;
    clog(c, "capture: frame channel closed (%s)", why);
    if (c->config.close) c->config.close(c->config.ctx, c->sock);
    else closesocket(c->sock);
    c->sock = INVALID_SOCKET;
    c->conn_tag = 0;
    asb_rw_tx_init(&c->tx, nullptr);
    std::lock_guard<std::mutex> fl(c->frame_lock);
    for (auto &kv : c->surfaces) {
        Surface *s = kv.second.get();
        release_pool(s);
        s->tx = -1;
        if (s->has_frame) s->pending = true;   /* resend the newest frame on the next connection */
    }
    with_stats(c, [](AsbRwCaptureStats &st) { st.connected = 0; st.gpu_agreed = 0; });
}

static void accept_connection(AsbRwCapture *c, uint32_t tag)
{
    SOCKET s = c->config.accept(c->config.ctx, 50);
    if (s == INVALID_SOCKET) return;
    c->sock = s;
    DWORD timeout = 10000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    uint8_t buf[ASB_RW_HEADER_BYTES + 128];
    size_t total = 0;
    AsbRwHeader header;
    AsbRwPayload m{};
    if (!recv_exact(c, buf, ASB_RW_HEADER_BYTES, HELLO_TIMEOUT_MS) ||
        asb_rw_frame_check(ASB_CH_RW_FRAMES, buf, ASB_RW_HEADER_BYTES, &total) || total > sizeof(buf) ||
        !recv_exact(c, buf + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES, HELLO_TIMEOUT_MS) ||
        asb_rw_decode(ASB_CH_RW_FRAMES, buf, total, &header, &m, nullptr, nullptr) || header.type != ASB_RW_FRAMES_HELLO) {
        drop_connection(c, "no valid FRAMES_HELLO");
        return;
    }
    AsbRwFramesHello host = m.frames_hello, local;
    AsbRwCaptureStats st;
    asb_rw_capture_stats(c, &st);
    asb_rw_frames_hello_init(&local, ASB_RW_ROLE_GUEST, tag, st.gpu_available ? ASB_RW_TRANSPORT_GPU : 0,
                             c->config.slots, c->config.max_cpu_bytes);
    local.process_id = GetCurrentProcessId();
    local.vendor_id = st.vendor_id;
    local.device_id = st.device_id;
    AsbRwFramesAgreement a = asb_rw_frames_negotiate(&local, &host, tag);
    if (a.error) {
        memset(&m, 0, sizeof(m));
        m.error.code = a.error;
        send_msg(c, ASB_RW_ERROR, m);
        drop_connection(c, a.error == ASB_RW_ERR_STALE_ID ? "host is on another control session" : "bad FRAMES_HELLO");
        return;
    }
    memset(&m, 0, sizeof(m));
    m.frames_hello = local;
    if (!send_msg(c, ASB_RW_FRAMES_HELLO, m)) {
        drop_connection(c, "send failed");
        return;
    }
    c->conn_tag = tag;
    asb_rw_tx_init(&c->tx, &a);
    with_stats(c, [&](AsbRwCaptureStats &s2) { s2.connected = 1; s2.gpu_agreed = a.gpu; s2.connections++; });
    clog(c, "capture: frame channel connected (%s, %u slot(s), %u CPU bytes)", a.gpu ? "GPU pools" : "CPU frames",
         a.slots, a.max_cpu_bytes);
}

static void send_pool_status(AsbRwCapture *c, Surface *s, uint32_t generation, HRESULT hr)
{
    AsbRwPayload m{};
    m.pool_status.surface_id = s->target.surface_id;
    m.pool_status.generation = generation;
    m.pool_status.status = (uint32_t)hr;
    send_msg(c, ASB_RW_POOL_STATUS, m);
}

static void pool_failed(AsbRwCapture *c, Surface *s, uint32_t generation, HRESULT hr)
{
    if (s->tx >= 0 && asb_rw_tx_pool_result(&c->tx, s->tx, generation, 0)) {
        send_pool_status(c, s, generation, hr);
        with_stats(c, [](AsbRwCaptureStats &st) { st.pool_failures++; });
        clog(c, "capture: pool %016llx gen %u unusable (0x%08lX); CPU frames for it",
             (unsigned long long)s->target.surface_id, generation, (unsigned long)hr);
    }
    release_pool(s);
}

static void handle_pool_slot(AsbRwCapture *c, const AsbRwPoolSlot &p)
{
    auto it = c->surfaces.find(p.surface_id);
    Surface *s = it == c->surfaces.end() ? nullptr : it->second.get();
    bool current = s && s->tx >= 0 && p.generation == s->pool_gen && p.slots == s->pool_slots &&
                   p.width == s->pool_w && p.height == s->pool_h;
    com_ptr<ID3D11Texture2D> texture;
    HRESULT hr = p.status ? (HRESULT)p.status : S_OK;
    if (p.handle) {
        /* Only a handle that opens as a shared resource is ours to close. */
        HANDLE handle = (HANDLE)(ULONG_PTR)p.handle;
        hr = c->device1 ? c->device1->OpenSharedResource1(handle, IID_PPV_ARGS(texture.put())) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) CloseHandle(handle);
        else check_device(c, hr, "pool open");
    }
    if (!current) return;   /* stale generation: the texture (if any) is dropped here */
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        if (d.Width != p.width || d.Height != p.height || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.SampleDesc.Count != 1 ||
            d.MipLevels != 1 || d.ArraySize != 1 || !(d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
            hr = E_INVALIDARG;
    }
    com_ptr<IDXGIKeyedMutex> mutex;
    if (SUCCEEDED(hr)) hr = texture->QueryInterface(IID_PPV_ARGS(mutex.put()));
    if (SUCCEEDED(hr) && (s->opened & (1u << p.slot))) hr = E_INVALIDARG;   /* slot offered twice */
    if (FAILED(hr)) {
        pool_failed(c, s, p.generation, hr);
        return;
    }
    s->slot_tex[p.slot] = texture;
    s->slot_mutex[p.slot] = mutex;
    s->opened |= 1u << p.slot;
    if (s->opened == (1u << s->pool_slots) - 1u && asb_rw_tx_pool_result(&c->tx, s->tx, p.generation, 1)) {
        send_pool_status(c, s, p.generation, S_OK);
        with_stats(c, [](AsbRwCaptureStats &st) { st.pools_opened++; });
        SetEvent(c->wake);
    }
}

/* Reads every complete message that is already available. False: drop the connection. */
static bool pump_incoming(AsbRwCapture *c)
{
    uint8_t buf[ASB_RW_HEADER_BYTES + 256];
    for (int count = 0; count < 256; ++count) {
        fd_set set;
        timeval tv{ 0, 0 };
        FD_ZERO(&set);
        FD_SET(c->sock, &set);
        int ready = select(0, &set, nullptr, nullptr, &tv);
        if (ready < 0) return false;
        if (!ready) return true;
        size_t total = 0;
        AsbRwHeader header;
        AsbRwPayload m{};
        if (!recv_exact(c, buf, ASB_RW_HEADER_BYTES, 10000)) return false;
        uint32_t r = asb_rw_frame_check(ASB_CH_RW_FRAMES, buf, ASB_RW_HEADER_BYTES, &total);
        if (r || total > sizeof(buf)) return false;
        if (!recv_exact(c, buf + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES, 10000)) return false;
        r = asb_rw_decode(ASB_CH_RW_FRAMES, buf, total, &header, &m, nullptr, nullptr);
        if (r) return false;
        switch (header.type) {
        case ASB_RW_FRAME_RELEASE:
            if (asb_rw_tx_release(&c->tx, &m.frame_release) == ASB_RW_ERR_MALFORMED) {
                clog(c, "capture: bad FRAME_RELEASE %016llx gen %u slot %u", (unsigned long long)m.frame_release.surface_id,
                     m.frame_release.generation, m.frame_release.slot);
                return false;
            }
            break;
        case ASB_RW_POOL_SLOT:
            handle_pool_slot(c, m.pool_slot);
            break;
        case ASB_RW_ERROR:
            clog(c, "capture: host error %s", asb_rw_error_name(m.error.code));
            return false;
        default:
            return false;
        }
    }
    return true;
}

static bool send_surface_end(AsbRwCapture *c, Surface *s, uint32_t reason)
{
    if (s->tx < 0) return true;
    AsbRwSurfaceEnd end;
    int announced = asb_rw_tx_remove(&c->tx, s->tx, reason, &end);
    s->tx = -1;
    release_pool(s);
    with_stats(c, [](AsbRwCaptureStats &st) { st.surfaces_ended++; });
    if (!announced || c->sock == INVALID_SOCKET) return true;
    AsbRwPayload m{};
    m.surface_end = end;
    return send_msg(c, ASB_RW_SURFACE_END, m);
}

/* Sends the newest frame of one surface if it has a free slot. False: drop the connection. */
static bool send_frame(AsbRwCapture *c, Surface *s)
{
    UINT w, h;
    uint64_t captured;
    {
        std::lock_guard<std::mutex> fl(c->frame_lock);
        if (!s->pending || !s->latest) return true;
        w = s->lw;
        h = s->lh;
        captured = s->captured;
    }
    if (s->tx < 0) {
        s->tx = asb_rw_tx_add(&c->tx, s->target.surface_id, s->target.root_id);
        if (s->tx < 0) return true;   /* surface table full; keep the newest frame */
    }
    AsbRwPoolRequest req;
    if (asb_rw_tx_prepare(&c->tx, s->tx, w, h, &req)) {
        release_pool(s);
        s->pool_gen = req.generation;
        s->pool_slots = req.slots;
        s->pool_w = req.width;
        s->pool_h = req.height;
        AsbRwPayload m{};
        m.pool_request = req;
        if (!send_msg(c, ASB_RW_POOL_REQUEST, m)) return false;
    }
    uint32_t transport = asb_rw_tx_transport(&c->tx, s->tx);
    uint32_t bytes = transport == ASB_RW_TRANSPORT_CPU ? w * h * 4u : 0u;
    int slot = asb_rw_tx_slot(&c->tx, s->tx, bytes);
    if (slot < 0) return true;
    AsbRwPayload m{};
    AsbRwFrameHeader &f = m.frame;
    f.kind = s->target.kind;
    f.z = s->target.z;
    f.rect = s->target.rect;
    f.flags = ASB_RW_FRAME_PREMULTIPLIED;
    HRESULT hr = S_OK;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (transport == ASB_RW_TRANSPORT_GPU) {
        IDXGIKeyedMutex *mutex = s->slot_mutex[slot].get();
        hr = mutex ? mutex->AcquireSync(KEY_GUEST, 0) : E_POINTER;
        /* A slot the host released without reading it (superseded or stale) still has key 1. */
        if (hr == (HRESULT)WAIT_TIMEOUT) hr = mutex->AcquireSync(KEY_HOST, 0);
        if (hr == (HRESULT)WAIT_TIMEOUT) return true;   /* host still finishing; try again */
        if (hr == S_OK) {
            {
                std::lock_guard<std::mutex> fl(c->frame_lock);
                std::lock_guard<std::mutex> dl(c->d3d_lock);
                D3D11_BOX box{ 0, 0, 0, w, h, 1 };
                c->context->CopySubresourceRegion(s->slot_tex[slot].get(), 0, 0, 0, 0, s->latest.get(), 0, &box);
                s->pending = false;
            }
            hr = mutex->ReleaseSync(KEY_HOST);
            if (SUCCEEDED(hr)) hr = c->device->GetDeviceRemovedReason();
        }
        if (FAILED(hr) || hr == (HRESULT)WAIT_ABANDONED) {
            check_device(c, hr, "pool publish");
            if (!c->device_lost) {
                /* Pool unusable: retire it and continue on the CPU. */
                asb_rw_tx_invalidate(&c->tx, s->tx, 1);
                release_pool(s);
                with_stats(c, [](AsbRwCaptureStats &st) { st.pool_failures++; });
                clog(c, "capture: publish to pool slot failed (0x%08lX); CPU frames", (unsigned long)hr);
                std::lock_guard<std::mutex> fl(c->frame_lock);
                s->pending = true;
            }
            return true;
        }
    } else {
        std::lock_guard<std::mutex> fl(c->frame_lock);
        std::lock_guard<std::mutex> dl(c->d3d_lock);
        if (!s->staging || s->sw != w || s->sh != h) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            s->staging = nullptr;
            hr = c->device->CreateTexture2D(&d, nullptr, s->staging.put());
            if (SUCCEEDED(hr)) { s->sw = w; s->sh = h; }
        }
        if (SUCCEEDED(hr)) {
            c->context->CopyResource(s->staging.get(), s->latest.get());
            hr = c->context->Map(s->staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (SUCCEEDED(hr)) {
            c->packed.resize((size_t)w * h * 4u);
            for (UINT y = 0; y < h; ++y)
                memcpy(&c->packed[(size_t)y * w * 4u], (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, (size_t)w * 4u);
            c->context->Unmap(s->staging.get(), 0);
            s->pending = false;
        }
        if (FAILED(hr)) {
            check_device(c, hr, "CPU readback");
            return true;
        }
        f.stride = w * 4u;
        f.data_bytes = bytes;
    }
    asb_rw_tx_captured(&c->tx, s->tx);
    asb_rw_tx_commit(&c->tx, s->tx, slot, bytes, &f);
    uint64_t superseded = captured > s->captured_at_send + 1 ? captured - s->captured_at_send - 1 : 0;
    s->captured_at_send = captured;
    uint8_t out[ASB_RW_HEADER_BYTES + 256];
    size_t n = 0;
    if (asb_rw_encode(ASB_RW_FRAME, 0, &m, out, sizeof(out), &n)) return false;
    if (!send_all(c, out, n)) return false;
    if (bytes && !send_all(c, c->packed.data(), bytes)) return false;
    with_stats(c, [&](AsbRwCaptureStats &st) {
        if (transport == ASB_RW_TRANSPORT_GPU) st.sent_gpu++; else st.sent_cpu++;
        st.superseded += superseded;
    });
    return true;
}

/* ---- reconciliation --------------------------------------------------------------------- */

static void reconcile(AsbRwCapture *c)
{
    std::map<uint64_t, AsbRwCaptureTarget> want;
    {
        std::lock_guard<std::mutex> g(c->request_lock);
        for (auto &kv : c->wanted)
            for (auto &t : kv.second) want[t.surface_id] = t;
    }
    /* Removed surfaces, and surfaces whose window was destroyed. */
    for (auto it = c->surfaces.begin(); it != c->surfaces.end();) {
        Surface *s = it->second.get();
        auto w = want.find(it->first);
        if (w == want.end() || w->second.hwnd != s->target.hwnd) {
            stop_capture(c, s);
            if (!send_surface_end(c, s, ASB_RW_END_UNSELECTED)) drop_connection(c, "send failed");
            it = c->surfaces.erase(it);
            continue;
        }
        if (s->closed && !s->ended) {
            stop_capture(c, s);
            s->ended = true;
            if (!send_surface_end(c, s, ASB_RW_END_CLOSED)) drop_connection(c, "send failed");
        }
        s->target = w->second;   /* kind, z and rect follow the control thread */
        ++it;
    }
    bool can_start = c->rt_device && !c->device_lost;
    for (auto &kv : want) {
        if (c->surfaces.count(kv.first) || !can_start) continue;
        auto s = std::make_shared<Surface>();
        s->target = kv.second;
        HRESULT hr = start_capture(c, s);
        if (FAILED(hr)) {
            s->failed = true;
            with_stats(c, [](AsbRwCaptureStats &st) { st.capture_failures++; });
            clog(c, "capture: surface %016llx (hwnd %p kind %u) could not be captured (0x%08lX)",
                 (unsigned long long)kv.first, (void *)(ULONG_PTR)kv.second.hwnd, kv.second.kind, (unsigned long)hr);
            check_device(c, hr, "capture start");
        }
        c->surfaces[kv.first] = s;
    }
    uint32_t capturing = 0;
    for (auto &kv : c->surfaces) capturing += kv.second->session ? 1u : 0u;
    uint32_t total = (uint32_t)c->surfaces.size();
    with_stats(c, [&](AsbRwCaptureStats &st) { st.surfaces = total; st.capturing = capturing; });
}

static void recover_device(AsbRwCapture *c)
{
    ULONGLONG now = GetTickCount64();
    if (now < c->next_recovery) return;
    c->next_recovery = now + 1000;      /* retry a failed device creation once per second */
    if (!c->recovering && c->device) with_stats(c, [](AsbRwCaptureStats &st) { st.device_losses++; });
    c->recovering = true;
    for (auto &kv : c->surfaces) stop_capture(c, kv.second.get());
    /* Wait for frame callbacks that may still run on the old device. */
    for (int i = 0; i < 200 && c->callbacks > 0; ++i) Sleep(5);
    {
        std::lock_guard<std::mutex> fl(c->frame_lock);
        for (auto &kv : c->surfaces) {
            Surface *s = kv.second.get();
            release_pool(s);
            s->latest = nullptr;
            s->staging = nullptr;
            s->lw = s->lh = s->sw = s->sh = 0;
            s->has_frame = s->pending = false;
            if (s->tx >= 0) asb_rw_tx_invalidate(&c->tx, s->tx, 0);   /* next frame: new generation */
        }
    }
    {
        std::lock_guard<std::mutex> dl(c->d3d_lock);
        c->context = nullptr;
        c->device1 = nullptr;
        c->device = nullptr;
        c->rt_device = nullptr;
    }
    HRESULT hr = create_device(c);
    if (FAILED(hr)) return;   /* retried on a later loop */
    c->device_lost = false;
    c->recovering = false;
    uint32_t restarted = 0;
    for (auto &kv : c->surfaces) {
        Surface *s = kv.second.get();
        if (s->ended || s->failed) continue;
        s->closed = false;
        if (SUCCEEDED(start_capture(c, kv.second))) restarted++;
    }
    with_stats(c, [](AsbRwCaptureStats &st) { st.device_recoveries++; });
    clog(c, "capture: device recovered; %u surface(s) restarted with new generations", restarted);
}

static DWORD WINAPI engine_thread(LPVOID arg)
{
    AsbRwCapture *c = (AsbRwCapture *)arg;
    init_apartment(apartment_type::multi_threaded);
    bool supported = false;
    try { supported = GraphicsCaptureSession::IsSupported(); } catch (...) {}
    if (supported && FAILED(create_device(c))) c->next_recovery = GetTickCount64() + 1000;
    if (!supported) clog(c, "capture: Windows Graphics Capture is not supported");
    SetEvent(c->ready);
    if (!supported) return 1;
    while (!c->stop) {
        if (c->loss_event && WaitForSingleObject(c->loss_event, 0) == WAIT_OBJECT_0) {
            clog(c, "capture: device loss requested by test event");
            c->device_lost = true;
        }
        if (c->device_lost || !c->device) recover_device(c);
        if (c->device_lost) {
            /* Device still down: keep the connection (surfaces resume with new generations). */
            if (c->sock != INVALID_SOCKET && !pump_incoming(c)) drop_connection(c, "transport failure or protocol error");
            WaitForSingleObject(c->wake, 50);
            continue;
        }
        uint32_t tag;
        {
            std::lock_guard<std::mutex> g(c->request_lock);
            tag = c->session_tag;
        }
        if (c->sock != INVALID_SOCKET && c->conn_tag != tag) drop_connection(c, "control session ended");
        reconcile(c);
        if (c->sock == INVALID_SOCKET && tag && c->device) accept_connection(c, tag);
        if (c->sock != INVALID_SOCKET) {
            bool ok = pump_incoming(c);
            if (ok && !c->device_lost) {
                for (auto &kv : c->surfaces) {
                    if (!kv.second->session && !kv.second->has_frame) continue;
                    if (!send_frame(c, kv.second.get())) { ok = false; break; }
                }
            }
            if (!ok) drop_connection(c, "transport failure or protocol error");
        }
        WaitForSingleObject(c->wake, c->sock != INVALID_SOCKET ? 4 : 50);
    }
    drop_connection(c, "engine stopping");
    for (auto &kv : c->surfaces) stop_capture(c, kv.second.get());
    for (int i = 0; i < 400 && c->callbacks > 0; ++i) Sleep(5);
    c->surfaces.clear();
    c->rt_device = nullptr;
    c->context = nullptr;
    c->device1 = nullptr;
    c->device = nullptr;
    uninit_apartment();
    return 0;
}

/* ---- public ----------------------------------------------------------------------------- */

extern "C" AsbRwCapture *asb_rw_capture_start(const AsbRwCaptureConfig *config)
{
    if (!config || !config->accept) return nullptr;
    AsbRwCapture *c = new (std::nothrow) AsbRwCapture();
    if (!c) return nullptr;
    c->config = *config;
    if (!c->config.slots || c->config.slots > ASB_RW_MAX_FRAME_SLOTS) c->config.slots = 3;
    if (c->config.max_cpu_bytes < 4096u) c->config.max_cpu_bytes = 128u << 20;
    asb_rw_tx_init(&c->tx, nullptr);
    c->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (config->device_loss_event && config->device_loss_event[0])
        c->loss_event = CreateEventW(nullptr, FALSE, FALSE, config->device_loss_event);
    if (c->wake && c->ready) c->thread = CreateThread(nullptr, 0, engine_thread, c, 0, nullptr);
    if (!c->thread) {
        asb_rw_capture_stop(c);
        return nullptr;
    }
    WaitForSingleObject(c->ready, 10000);
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeThread(c->thread, &code) && code != STILL_ACTIVE) {
        asb_rw_capture_stop(c);
        return nullptr;
    }
    return c;
}

extern "C" void asb_rw_capture_session(AsbRwCapture *c, uint32_t session_tag)
{
    if (!c) return;
    {
        std::lock_guard<std::mutex> g(c->request_lock);
        c->session_tag = session_tag;
        if (!session_tag) c->wanted.clear();
    }
    SetEvent(c->wake);
}

extern "C" void asb_rw_capture_set(AsbRwCapture *c, uint64_t root_id, const AsbRwCaptureTarget *targets, uint32_t n)
{
    if (!c || !root_id) return;
    {
        std::lock_guard<std::mutex> g(c->request_lock);
        if (!n) c->wanted.erase(root_id);
        else c->wanted[root_id].assign(targets, targets + n);
    }
    SetEvent(c->wake);
}

extern "C" void asb_rw_capture_simulate_device_loss(AsbRwCapture *c)
{
    if (!c) return;
    c->device_lost = true;
    SetEvent(c->wake);
}

extern "C" void asb_rw_capture_stats(AsbRwCapture *c, AsbRwCaptureStats *stats)
{
    std::lock_guard<std::mutex> g(c->stats_lock);
    *stats = c->stats;
}

extern "C" void asb_rw_capture_stop(AsbRwCapture *c)
{
    if (!c) return;
    c->stop = true;
    if (c->wake) SetEvent(c->wake);
    if (c->thread) {
        WaitForSingleObject(c->thread, INFINITE);
        CloseHandle(c->thread);
    }
    if (c->wake) CloseHandle(c->wake);
    if (c->ready) CloseHandle(c->ready);
    if (c->loss_event) CloseHandle(c->loss_event);
    delete c;
}
