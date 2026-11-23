/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_frames_host
 *
 * @file      remote_windows_frames_host.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows host frame receiver (AppSandbox-eh6.5). See remote_windows_frames_host.h. */
#define COBJMACROS
#include "remote_windows_frames_host.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <dxgi1_2.h>
#pragma warning(pop)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hcs_services.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dxguid.lib")

#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#define HV_PROTOCOL_RAW 1

typedef struct HvSockAddr { ADDRESS_FAMILY family; USHORT reserved; GUID vm_id; GUID service_id; } HvSockAddr;

/* D3DKMTDuplicateHandle arguments, as used by the Windows-guest display path (vm_display_idd.c). */
typedef struct DuplicateArgs {
    HANDLE resource;
    GUID vm_id;
    UINT64 process_id;
    UINT64 guest_handle;
    ACCESS_MASK access;
    UINT flags;
} DuplicateArgs;
typedef LONG (WINAPI *DuplicateFn)(DuplicateArgs *);

#define SMALL_MESSAGE    4096u
#define FREE_BUFFERS     4u
#define HELLO_TIMEOUT_MS 10000u

typedef struct FrameBuffer { uint8_t *data; size_t cap; } FrameBuffer;

typedef struct PoolTexture {
    ID3D11Texture2D *texture;
    IDXGIKeyedMutex *mutex;
} PoolTexture;

typedef struct SurfaceRes {
    uint32_t pool_generation, pool_slots;
    PoolTexture pool[ASB_RW_MAX_FRAME_SLOTS];
    FrameBuffer pending;            /* CPU: whole message of the pending frame */
    uint32_t pending_offset;        /* pixel offset inside it */
    int consumer;                   /* the consumer holds this surface's frame */
    AsbRwHostSurfaceInfo info;
} SurfaceRes;

struct AsbRwFramesHost {
    SOCKET socket;
    HANDLE thread;
    CRITICAL_SECTION lock, send_lock;
    AsbRwFramesHostConfig config;
    uint32_t session_tag;
    volatile LONG stop, device_lost;
    AsbRwRx rx;
    SurfaceRes res[ASB_RW_MAX_FRAME_SURFACES];
    FrameBuffer free_buffers[FREE_BUFFERS];
    HANDLE guest_process;           /* local mode only */
    DuplicateFn duplicate;
    uint32_t vendor_id, device_id;
    AsbRwFramesHostStats stats;
    uint8_t msgbuf[ASB_RW_HEADER_BYTES + SMALL_MESSAGE];
};

static volatile LONG g_textures_live;

static void hlog(AsbRwFramesHost *h, const char *fmt, ...)
{
    char text[512];
    va_list args;
    if (!h->config.log) return;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    h->config.log(h->config.ctx, text);
}

static int is_device_loss(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

static void mark_device_lost(AsbRwFramesHost *h, HRESULT hr, const char *where)
{
    if (InterlockedExchange(&h->device_lost, 1)) return;
    hlog(h, "frames: render device lost in %s (0x%08lX); closing the frame channel", where, (unsigned long)hr);
    shutdown(h->socket, SD_BOTH);
}

/* ---- sockets -------------------------------------------------------------------------- */

SOCKET asb_rw_frames_connect_vm(const GUID *vm_id, DWORD timeout_ms)
{
    HvSockAddr addr;
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return s;
    memset(&addr, 0, sizeof(addr));
    addr.family = AF_HYPERV;
    addr.vm_id = *vm_id;
    hcs_service_guid(L"windows", ASB_CH_RW_FRAMES, &addr.service_id);
    DWORD t = timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
    if (connect(s, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static BOOL send_all(AsbRwFramesHost *h, const uint8_t *p, size_t len)
{
    while (len) {
        int n = send(h->socket, (const char *)p, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= (size_t)n;
    }
    return TRUE;
}

static BOOL send_msg(AsbRwFramesHost *h, uint16_t type, const AsbRwPayload *m)
{
    uint8_t out[ASB_RW_HEADER_BYTES + 128];
    size_t n = 0;
    if (asb_rw_encode(type, 0, m, out, sizeof(out), &n)) return FALSE;
    EnterCriticalSection(&h->send_lock);
    BOOL ok = send_all(h, out, n);
    LeaveCriticalSection(&h->send_lock);
    return ok;
}

static void send_releases(AsbRwFramesHost *h, const AsbRwRxReleases *r)
{
    AsbRwPayload m;
    for (uint32_t i = 0; i < r->count; ++i) {
        memset(&m, 0, sizeof(m));
        m.frame_release = r->items[i];
        if (send_msg(h, ASB_RW_FRAME_RELEASE, &m)) InterlockedIncrement64((volatile LONG64 *)&h->stats.releases_sent);
    }
}

/* Receives exactly len bytes, waking every 250 ms to honour stop. */
static BOOL recv_exact(AsbRwFramesHost *h, uint8_t *p, size_t len, DWORD idle_limit_ms)
{
    DWORD idle = 0;
    while (len) {
        if (h->stop) return FALSE;
        fd_set set;
        struct timeval tv = { 0, 250000 };
        FD_ZERO(&set);
        FD_SET(h->socket, &set);
        int ready = select(0, &set, NULL, NULL, &tv);
        if (ready < 0) return FALSE;
        if (!ready) {
            idle += 250;
            if (idle_limit_ms && idle >= idle_limit_ms) return FALSE;
            continue;
        }
        int n = recv(h->socket, (char *)p, len > 1048576 ? 1048576 : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= (size_t)n;
        idle = 0;
    }
    return TRUE;
}

/* ---- buffers -------------------------------------------------------------------------- */

static FrameBuffer take_buffer(AsbRwFramesHost *h, size_t size)
{
    FrameBuffer b = { NULL, 0 };
    for (uint32_t i = 0; i < FREE_BUFFERS; ++i) {
        if (h->free_buffers[i].data && h->free_buffers[i].cap >= size && h->free_buffers[i].cap <= size * 2) {
            b = h->free_buffers[i];
            h->free_buffers[i].data = NULL;
            h->free_buffers[i].cap = 0;
            return b;
        }
    }
    b.data = (uint8_t *)malloc(size);
    b.cap = b.data ? size : 0;
    return b;
}

static void give_buffer(AsbRwFramesHost *h, FrameBuffer *b)
{
    if (!b->data) return;
    for (uint32_t i = 0; i < FREE_BUFFERS; ++i) {
        if (!h->free_buffers[i].data) {
            h->free_buffers[i] = *b;
            b->data = NULL;
            b->cap = 0;
            return;
        }
    }
    free(b->data);
    b->data = NULL;
    b->cap = 0;
}

/* ---- pools ---------------------------------------------------------------------------- */

static void free_pool(AsbRwFramesHost *h, SurfaceRes *r)
{
    int had = 0;
    for (uint32_t k = 0; k < ASB_RW_MAX_FRAME_SLOTS; ++k) {
        if (r->pool[k].mutex) IDXGIKeyedMutex_Release(r->pool[k].mutex);
        if (r->pool[k].texture) {
            ID3D11Texture2D_Release(r->pool[k].texture);
            InterlockedDecrement(&g_textures_live);
            had = 1;
        }
        r->pool[k].mutex = NULL;
        r->pool[k].texture = NULL;
    }
    if (had) h->stats.pools_retired++;
    r->pool_generation = 0;
    r->pool_slots = 0;
}

static void free_surface(AsbRwFramesHost *h, SurfaceRes *r)
{
    free_pool(h, r);
    give_buffer(h, &r->pending);
    memset(r, 0, sizeof(*r));
}

static HRESULT duplicate_into_guest(AsbRwFramesHost *h, HANDLE shared, uint64_t *guest_handle)
{
    static const GUID zero = { 0 };
    *guest_handle = 0;
    if (!memcmp(&h->config.vm_id, &zero, sizeof(zero))) {
        HANDLE out = NULL;
        if (!h->guest_process) {
            h->guest_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, h->rx.agreement.guest_pid);
            if (!h->guest_process) return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!DuplicateHandle(GetCurrentProcess(), shared, h->guest_process, &out, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        *guest_handle = (uint64_t)(ULONG_PTR)out;
        return S_OK;
    }
    if (!h->duplicate) return E_NOTIMPL;
    DuplicateArgs args;
    memset(&args, 0, sizeof(args));
    args.resource = shared;
    args.vm_id = h->config.vm_id;
    args.process_id = h->rx.agreement.guest_pid;
    args.flags = 1;
    LONG status = h->duplicate(&args);
    if (status < 0) return HRESULT_FROM_NT(status);
    if (!args.guest_handle) return E_FAIL;
    *guest_handle = args.guest_handle;
    return S_OK;
}

/* Creates the textures of a requested pool and sends one POOL_SLOT per slot (then a failure
   POOL_SLOT if anything failed). Runs on the receive thread without the lock held. */
static void create_pool(AsbRwFramesHost *h, int index, const AsbRwPoolRequest *req)
{
    PoolTexture made[ASB_RW_MAX_FRAME_SLOTS];
    uint64_t handles[ASB_RW_MAX_FRAME_SLOTS];
    HRESULT hr = S_OK;
    uint32_t created = 0;
    memset(made, 0, sizeof(made));
    memset(handles, 0, sizeof(handles));
    if (!h->config.device) hr = E_NOINTERFACE;
    for (uint32_t k = 0; SUCCEEDED(hr) && k < req->slots; ++k) {
        D3D11_TEXTURE2D_DESC desc;
        IDXGIResource1 *resource = NULL;
        HANDLE shared = NULL;
        memset(&desc, 0, sizeof(desc));
        desc.Width = req->width;
        desc.Height = req->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = ID3D11Device_CreateTexture2D(h->config.device, &desc, NULL, &made[k].texture);
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_textures_live);
            hr = ID3D11Texture2D_QueryInterface(made[k].texture, &IID_IDXGIKeyedMutex, (void **)&made[k].mutex);
        }
        if (SUCCEEDED(hr)) hr = ID3D11Texture2D_QueryInterface(made[k].texture, &IID_IDXGIResource1, (void **)&resource);
        if (SUCCEEDED(hr))
            hr = IDXGIResource1_CreateSharedHandle(resource, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   NULL, &shared);
        if (SUCCEEDED(hr)) hr = duplicate_into_guest(h, shared, &handles[k]);
        if (shared) CloseHandle(shared);
        if (resource) IDXGIResource1_Release(resource);
        if (SUCCEEDED(hr)) created++;
    }
    if (FAILED(hr) && is_device_loss(hr)) mark_device_lost(h, hr, "pool creation");
    if (SUCCEEDED(hr)) {
        HRESULT removed = ID3D11Device_GetDeviceRemovedReason(h->config.device);
        if (removed != S_OK) { hr = removed; mark_device_lost(h, hr, "pool creation"); }
    }

    EnterCriticalSection(&h->lock);
    AsbRwRxSurface *s = &h->rx.surfaces[index];
    BOOL current = s->in_use && s->surface_id == req->surface_id && s->generation == req->generation &&
                   s->pool_state == ASB_RW_POOL_REQUESTED;
    if (SUCCEEDED(hr) && current) {
        SurfaceRes *r = &h->res[index];
        free_pool(h, r);
        memcpy(r->pool, made, sizeof(made));
        memset(made, 0, sizeof(made));
        r->pool_generation = req->generation;
        r->pool_slots = req->slots;
        h->stats.pools_created++;
    } else {
        if (current) asb_rw_rx_pool_failed(&h->rx, index, req->generation);
        if (SUCCEEDED(hr)) hr = E_ABORT;
        h->stats.pools_failed++;
    }
    LeaveCriticalSection(&h->lock);
    for (uint32_t k = 0; k < ASB_RW_MAX_FRAME_SLOTS; ++k) {
        if (made[k].mutex) IDXGIKeyedMutex_Release(made[k].mutex);
        if (made[k].texture) { ID3D11Texture2D_Release(made[k].texture); InterlockedDecrement(&g_textures_live); }
    }

    /* Slots that reached the guest are opened there (and their handles closed); a trailing
       failure tells it to drop the partial pool and use CPU frames for this generation. */
    AsbRwPayload m;
    uint32_t sendable = SUCCEEDED(hr) ? req->slots : created;
    for (uint32_t k = 0; k < sendable; ++k) {
        memset(&m, 0, sizeof(m));
        m.pool_slot.surface_id = req->surface_id; m.pool_slot.generation = req->generation;
        m.pool_slot.slot = k; m.pool_slot.slots = req->slots;
        m.pool_slot.width = req->width; m.pool_slot.height = req->height; m.pool_slot.handle = handles[k];
        send_msg(h, ASB_RW_POOL_SLOT, &m);
    }
    if (FAILED(hr)) {
        memset(&m, 0, sizeof(m));
        m.pool_slot.surface_id = req->surface_id; m.pool_slot.generation = req->generation;
        m.pool_slot.slot = created < req->slots ? created : 0; m.pool_slot.slots = req->slots;
        m.pool_slot.width = req->width; m.pool_slot.height = req->height;
        m.pool_slot.status = (uint32_t)hr;
        send_msg(h, ASB_RW_POOL_SLOT, &m);
        hlog(h, "frames: pool for surface %016llx gen %u failed (0x%08lX); CPU frames", (unsigned long long)req->surface_id,
             req->generation, (unsigned long)hr);
    }
}

/* ---- receive thread ------------------------------------------------------------------- */

static uint32_t handshake(AsbRwFramesHost *h)
{
    AsbRwPayload m;
    AsbRwFramesHello local;
    AsbRwHeader header;
    size_t total = 0;
    uint32_t transports = ASB_RW_TRANSPORT_CPU;
    if (h->config.device && h->config.allow_gpu) transports |= ASB_RW_TRANSPORT_GPU;
    asb_rw_frames_hello_init(&local, ASB_RW_ROLE_HOST, h->session_tag, transports, h->config.slots, h->config.max_cpu_bytes);
    local.vendor_id = h->vendor_id;
    local.device_id = h->device_id;
    memset(&m, 0, sizeof(m));
    m.frames_hello = local;
    if (!send_msg(h, ASB_RW_FRAMES_HELLO, &m)) return ASB_RW_ERR_INTERNAL;
    if (!recv_exact(h, h->msgbuf, ASB_RW_HEADER_BYTES, HELLO_TIMEOUT_MS)) return ASB_RW_ERR_INTERNAL;
    uint32_t r = asb_rw_frame_check(ASB_CH_RW_FRAMES, h->msgbuf, ASB_RW_HEADER_BYTES, &total);
    if (r) return r;
    if (total > sizeof(h->msgbuf)) return ASB_RW_ERR_TOO_LARGE;
    if (!recv_exact(h, h->msgbuf + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES, HELLO_TIMEOUT_MS))
        return ASB_RW_ERR_INTERNAL;
    r = asb_rw_decode(ASB_CH_RW_FRAMES, h->msgbuf, total, &header, &m, NULL, NULL);
    if (r) return r;
    if (header.type == ASB_RW_ERROR) {
        hlog(h, "frames: guest refused the frame channel (%s)", asb_rw_error_name(m.error.code));
        return m.error.code ? m.error.code : ASB_RW_ERR_INTERNAL;
    }
    if (header.type != ASB_RW_FRAMES_HELLO) return ASB_RW_ERR_MALFORMED;
    AsbRwFramesAgreement a = asb_rw_frames_negotiate(&local, &m.frames_hello, h->session_tag);
    if (a.error) return a.error;
    EnterCriticalSection(&h->lock);
    asb_rw_rx_init(&h->rx, &a);
    h->stats.agreement = a;
    h->stats.state = ASB_RW_HOST_RUNNING;
    LeaveCriticalSection(&h->lock);
    hlog(h, "frames: connected, guest pid %u adapter %04x:%04x, %s, %u slot(s), %u CPU bytes", a.guest_pid,
         m.frames_hello.vendor_id, m.frames_hello.device_id, a.gpu ? "GPU pools" : "CPU frames", a.slots, a.max_cpu_bytes);
    return ASB_RW_OK;
}

static uint32_t handle_frame(AsbRwFramesHost *h, FrameBuffer *buf, size_t total)
{
    AsbRwPayload m;
    AsbRwHeader header;
    const uint8_t *bulk = NULL;
    size_t bulk_len = 0;
    AsbRwRxReleases rel = { 0 };
    int index = -1;
    uint32_t r = asb_rw_decode(ASB_CH_RW_FRAMES, buf->data, total, &header, &m, &bulk, &bulk_len);
    if (r) return r;
    EnterCriticalSection(&h->lock);
    r = asb_rw_rx_frame(&h->rx, &m.frame, &index, &rel);
    if (r == ASB_RW_OK) {
        SurfaceRes *res = &h->res[index];
        if (m.frame.transport == ASB_RW_TRANSPORT_GPU && (res->pool_generation != m.frame.generation ||
                                                          !res->pool[m.frame.slot].texture)) {
            r = ASB_RW_ERR_MALFORMED;   /* the host failed this pool; the guest must not use it */
        } else {
            give_buffer(h, &res->pending);   /* the superseded frame (released via rel) */
            if (m.frame.transport == ASB_RW_TRANSPORT_CPU) {
                res->pending = *buf;
                res->pending_offset = (uint32_t)(bulk - buf->data);
                buf->data = NULL;
                buf->cap = 0;
                h->stats.frames_cpu++;
            } else {
                h->stats.frames_gpu++;
            }
            res->info.surface_id = m.frame.surface_id;
            res->info.root_id = m.frame.root_id;
            res->info.generation = m.frame.generation;
            res->info.transport = m.frame.transport;
            res->info.kind = m.frame.kind;
            res->info.z = m.frame.z;
            res->info.rect = m.frame.rect;
            res->info.width = m.frame.width;
            res->info.height = m.frame.height;
            res->info.frames++;
        }
    } else if (r == ASB_RW_ERR_STALE_ID) {
        h->stats.stale++;
        r = ASB_RW_OK;
    }
    h->stats.superseded = 0;
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i) h->stats.superseded += h->rx.surfaces[i].superseded;
    LeaveCriticalSection(&h->lock);
    send_releases(h, &rel);
    if (r == ASB_RW_OK && index >= 0 && h->config.notify) h->config.notify(h->config.ctx, m.frame.surface_id);
    return r;
}

static uint32_t handle_small(AsbRwFramesHost *h, size_t total)
{
    AsbRwPayload m;
    AsbRwHeader header;
    AsbRwRxReleases rel = { 0 };
    int index = -1, allocate = 0, ready = 0;
    uint32_t r = asb_rw_decode(ASB_CH_RW_FRAMES, h->msgbuf, total, &header, &m, NULL, NULL);
    if (r) return r;
    switch (header.type) {
    case ASB_RW_POOL_REQUEST:
        EnterCriticalSection(&h->lock);
        r = asb_rw_rx_pool_request(&h->rx, &m.pool_request, &index, &allocate, &rel);
        if (r == ASB_RW_OK) {
            SurfaceRes *res = &h->res[index];
            if (res->info.surface_id != m.pool_request.surface_id) {
                memset(&res->info, 0, sizeof(res->info));
                res->info.surface_id = m.pool_request.surface_id;
            }
            give_buffer(h, &res->pending);      /* pending frame of the old generation (in rel) */
            free_pool(h, res);                  /* a held GPU frame keeps its own references */
            res->info.generation = m.pool_request.generation;
        }
        LeaveCriticalSection(&h->lock);
        send_releases(h, &rel);
        if (r == ASB_RW_OK && allocate) create_pool(h, index, &m.pool_request);
        return r;
    case ASB_RW_POOL_STATUS:
        EnterCriticalSection(&h->lock);
        r = asb_rw_rx_pool_status(&h->rx, &m.pool_status, &index, &ready);
        if (r == ASB_RW_OK && !ready && h->res[index].pool_generation == m.pool_status.generation) {
            free_pool(h, &h->res[index]);
            h->stats.pools_failed++;
        }
        LeaveCriticalSection(&h->lock);
        if (r == ASB_RW_OK && !ready)
            hlog(h, "frames: guest could not open pool %016llx gen %u (0x%08lX); CPU frames",
                 (unsigned long long)m.pool_status.surface_id, m.pool_status.generation, (unsigned long)m.pool_status.status);
        return r == ASB_RW_ERR_STALE_ID ? ASB_RW_OK : r;
    case ASB_RW_SURFACE_END:
        EnterCriticalSection(&h->lock);
        index = asb_rw_rx_end(&h->rx, &m.surface_end);
        if (index >= 0) {
            free_surface(h, &h->res[index]);
            h->stats.surfaces_ended++;
        }
        LeaveCriticalSection(&h->lock);
        if (h->config.notify) h->config.notify(h->config.ctx, m.surface_end.surface_id);
        if (m.surface_end.reason == ASB_RW_END_SESSION) return ASB_RW_ERR_CANCELLED;
        return index >= 0 ? ASB_RW_OK : ASB_RW_ERR_MALFORMED;
    case ASB_RW_ERROR:
        hlog(h, "frames: guest error %s (detail %u)", asb_rw_error_name(m.error.code), m.error.detail);
        return m.error.code ? m.error.code : ASB_RW_ERR_INTERNAL;
    default:
        return ASB_RW_ERR_MALFORMED;   /* host -> guest types or a second HELLO */
    }
}

static DWORD WINAPI receive_thread(LPVOID arg)
{
    AsbRwFramesHost *h = (AsbRwFramesHost *)arg;
    uint32_t r = handshake(h);
    while (r == ASB_RW_OK && !h->stop) {
        size_t total = 0;
        if (!recv_exact(h, h->msgbuf, ASB_RW_HEADER_BYTES, 0)) { r = ASB_RW_ERR_CANCELLED; break; }
        r = asb_rw_frame_check(ASB_CH_RW_FRAMES, h->msgbuf, ASB_RW_HEADER_BYTES, &total);
        if (r) break;
        uint16_t type = (uint16_t)(h->msgbuf[6] | (h->msgbuf[7] << 8));
        if (type == ASB_RW_FRAME) {
            EnterCriticalSection(&h->lock);
            FrameBuffer buf = take_buffer(h, total);
            LeaveCriticalSection(&h->lock);
            if (!buf.data) { r = ASB_RW_ERR_INTERNAL; break; }
            memcpy(buf.data, h->msgbuf, ASB_RW_HEADER_BYTES);
            if (!recv_exact(h, buf.data + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES, 30000)) {
                free(buf.data);
                r = ASB_RW_ERR_CANCELLED;
                break;
            }
            r = handle_frame(h, &buf, total);
            EnterCriticalSection(&h->lock);
            give_buffer(h, &buf);
            LeaveCriticalSection(&h->lock);
        } else {
            if (total > sizeof(h->msgbuf)) { r = ASB_RW_ERR_TOO_LARGE; break; }
            if (!recv_exact(h, h->msgbuf + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES, 30000)) {
                r = ASB_RW_ERR_CANCELLED;
                break;
            }
            r = handle_small(h, total);
        }
    }
    if (r && r != ASB_RW_ERR_CANCELLED && !h->device_lost) {
        AsbRwPayload m;
        memset(&m, 0, sizeof(m));
        m.error.code = r;
        send_msg(h, ASB_RW_ERROR, &m);
    }
    shutdown(h->socket, SD_BOTH);
    EnterCriticalSection(&h->lock);
    h->stats.state = h->device_lost ? ASB_RW_HOST_DEVICE_LOST : ASB_RW_HOST_CLOSED;
    h->stats.error = r == ASB_RW_ERR_CANCELLED ? 0 : r;
    /* Free everything not held by a consumer; held frames carry their own references. */
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i) free_surface(h, &h->res[i]);
    asb_rw_rx_init(&h->rx, &h->stats.agreement);
    LeaveCriticalSection(&h->lock);
    hlog(h, "frames: closed (%s)%s", asb_rw_error_name(r == ASB_RW_ERR_CANCELLED ? 0 : r),
         h->device_lost ? ", device lost" : "");
    if (h->config.notify) h->config.notify(h->config.ctx, 0);
    return 0;
}

/* ---- public --------------------------------------------------------------------------- */

AsbRwFramesHost *asb_rw_frames_host_start(SOCKET connected, uint32_t session_tag, const AsbRwFramesHostConfig *config)
{
    AsbRwFramesHost *h = (AsbRwFramesHost *)calloc(1, sizeof(*h));
    if (!h) {
        closesocket(connected);
        return NULL;
    }
    h->socket = connected;
    h->session_tag = session_tag;
    h->config = *config;
    if (!h->config.slots || h->config.slots > ASB_RW_MAX_FRAME_SLOTS) h->config.slots = 3;
    if (h->config.max_cpu_bytes < 4096u) h->config.max_cpu_bytes = 128u << 20;
    InitializeCriticalSection(&h->lock);
    InitializeCriticalSection(&h->send_lock);
    h->stats.state = ASB_RW_HOST_CONNECTING;
    h->stats.agreement.slots = h->config.slots;
    h->stats.agreement.max_cpu_bytes = h->config.max_cpu_bytes;
    asb_rw_rx_init(&h->rx, &h->stats.agreement);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    h->duplicate = gdi ? (DuplicateFn)GetProcAddress(gdi, "D3DKMTDuplicateHandle") : NULL;
    if (h->config.device) {
        IDXGIDevice *dxgi = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC desc;
        ID3D11Device_AddRef(h->config.device);
        if (SUCCEEDED(ID3D11Device_QueryInterface(h->config.device, &IID_IDXGIDevice, (void **)&dxgi)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
            h->vendor_id = desc.VendorId;
            h->device_id = desc.DeviceId;
        }
        if (adapter) IDXGIAdapter_Release(adapter);
        if (dxgi) IDXGIDevice_Release(dxgi);
    }
    DWORD timeout = 10000;
    setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    h->thread = CreateThread(NULL, 0, receive_thread, h, 0, NULL);
    if (!h->thread) {
        h->stop = 1;
        asb_rw_frames_host_stop(h);
        return NULL;
    }
    return h;
}

int asb_rw_frames_host_acquire(AsbRwFramesHost *h, uint64_t surface_id, AsbRwHostFrame *frame)
{
    AsbRwRxReleases rel = { 0 };
    int result = 0;
    memset(frame, 0, sizeof(*frame));
    EnterCriticalSection(&h->lock);
    int index = asb_rw_rx_find(&h->rx, surface_id);
    if (index < 0 || h->res[index].consumer) {
        LeaveCriticalSection(&h->lock);
        return -1;
    }
    SurfaceRes *res = &h->res[index];
    AsbRwRxSurface *s = &h->rx.surfaces[index];
    if (s->pending >= 0 && asb_rw_rx_acquire(&h->rx, index, &frame->header, &rel)) {
        if (frame->header.transport == ASB_RW_TRANSPORT_GPU) {
            PoolTexture *t = &res->pool[frame->header.slot];
            HRESULT hr = t->mutex ? IDXGIKeyedMutex_AcquireSync(t->mutex, 1, 100) : E_POINTER;
            if (hr == S_OK) {
                frame->texture = t->texture;
                frame->mutex = t->mutex;
                ID3D11Texture2D_AddRef(frame->texture);
                IDXGIKeyedMutex_AddRef(frame->mutex);
                InterlockedIncrement(&g_textures_live);
                res->consumer = 1;
                result = 1;
            } else {
                /* Nothing to show; give the slot back to the guest. */
                asb_rw_rx_done(&h->rx, index, frame->header.generation, frame->header.slot, &rel);
                if (is_device_loss(hr) || (hr != (HRESULT)WAIT_TIMEOUT && hr != (HRESULT)WAIT_ABANDONED &&
                                           ID3D11Device_GetDeviceRemovedReason(h->config.device) != S_OK))
                    mark_device_lost(h, hr, "keyed mutex acquire");
                else
                    hlog(h, "frames: keyed mutex of %016llx slot %u not available (0x%08lX); frame skipped",
                         (unsigned long long)surface_id, frame->header.slot, (unsigned long)hr);
            }
        } else {
            frame->buffer = res->pending.data;
            frame->buffer_cap = res->pending.cap;
            frame->pixels = res->pending.data + res->pending_offset;
            res->pending.data = NULL;
            res->pending.cap = 0;
            res->consumer = 1;
            result = 1;
        }
    }
    LeaveCriticalSection(&h->lock);
    send_releases(h, &rel);
    return result;
}

void asb_rw_frames_host_frame_free(AsbRwHostFrame *frame)
{
    if (frame->mutex) {
        IDXGIKeyedMutex_ReleaseSync(frame->mutex, 0);
        IDXGIKeyedMutex_Release(frame->mutex);
    }
    if (frame->texture) {
        ID3D11Texture2D_Release(frame->texture);
        InterlockedDecrement(&g_textures_live);
    }
    free(frame->buffer);
    memset(frame, 0, sizeof(*frame));
}

void asb_rw_frames_host_release(AsbRwFramesHost *h, AsbRwHostFrame *frame)
{
    AsbRwRxReleases rel = { 0 };
    if (!frame->mutex && !frame->buffer) return;
    if (frame->mutex) IDXGIKeyedMutex_ReleaseSync(frame->mutex, 0);
    EnterCriticalSection(&h->lock);
    int index = asb_rw_rx_find(&h->rx, frame->header.surface_id);
    if (index >= 0) {
        asb_rw_rx_done(&h->rx, index, frame->header.generation, frame->header.slot, &rel);
        h->res[index].consumer = 0;
    }
    if (frame->buffer) {
        FrameBuffer b = { (uint8_t *)frame->buffer, frame->buffer_cap };
        give_buffer(h, &b);
        frame->buffer = NULL;
    }
    LeaveCriticalSection(&h->lock);
    if (frame->mutex) {
        IDXGIKeyedMutex_Release(frame->mutex);
        frame->mutex = NULL;
    }
    if (frame->texture) {
        ID3D11Texture2D_Release(frame->texture);
        InterlockedDecrement(&g_textures_live);
    }
    memset(frame, 0, sizeof(*frame));
    send_releases(h, &rel);
}

uint32_t asb_rw_frames_host_surfaces(AsbRwFramesHost *h, AsbRwHostSurfaceInfo *out, uint32_t max)
{
    uint32_t n = 0;
    EnterCriticalSection(&h->lock);
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES && n < max; ++i)
        if (h->rx.surfaces[i].in_use && h->res[i].info.surface_id) out[n++] = h->res[i].info;
    LeaveCriticalSection(&h->lock);
    return n;
}

void asb_rw_frames_host_stats(AsbRwFramesHost *h, AsbRwFramesHostStats *stats)
{
    EnterCriticalSection(&h->lock);
    *stats = h->stats;
    stats->surfaces = h->rx.count;
    stats->cpu_bytes = h->rx.cpu_bytes;
    stats->textures_live = (uint64_t)g_textures_live;
    LeaveCriticalSection(&h->lock);
}

void asb_rw_frames_host_simulate_device_loss(AsbRwFramesHost *h)
{
    mark_device_lost(h, DXGI_ERROR_DEVICE_REMOVED, "test hook");
}

void asb_rw_frames_host_stop(AsbRwFramesHost *h)
{
    if (!h) return;
    InterlockedExchange(&h->stop, 1);
    shutdown(h->socket, SD_BOTH);
    if (h->thread) {
        WaitForSingleObject(h->thread, INFINITE);
        CloseHandle(h->thread);
    }
    closesocket(h->socket);
    for (uint32_t i = 0; i < ASB_RW_MAX_FRAME_SURFACES; ++i) free_surface(h, &h->res[i]);
    for (uint32_t i = 0; i < FREE_BUFFERS; ++i) free(h->free_buffers[i].data);
    if (h->guest_process) CloseHandle(h->guest_process);
    if (h->config.device) ID3D11Device_Release(h->config.device);
    DeleteCriticalSection(&h->lock);
    DeleteCriticalSection(&h->send_lock);
    free(h);
}
