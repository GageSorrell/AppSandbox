/**
 *
 *
 * @module AppSandbox/src/backend_win/vm_integration
 *
 * @file      vm_integration.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Per-VM clipboard coordinator and audio playback (AppSandbox-eh6.7). See vm_integration.h. */
#include "vm_integration.h"

#define COBJMACROS
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef VM_INTEGRATION_NO_DEFAULTS
#include "vm_clipboard.h"
#include "asb_core.h"
#include "display_settings.h"
#include "hcs_services.h"
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

#define VI_MAX_INSTANCES 64
#define WM_VI_CLIP_APPLY (WM_APP + 11)   /* vm_clipboard posts guest data to its window with this */

struct VmIntegration {
    UINT64 vm_id;
    GUID runtime;
    wchar_t os_type[32], name[128];
    LONG refs;                       /* g_lock */
    SRWLOCK lock;                    /* everything below except the audio state */
    uint32_t clients[VM_INTEGRATION_MAX_CLIENTS];
    uint8_t focused[VM_INTEGRATION_MAX_CLIENTS];
    uint32_t next_client;
    BOOL sharing, sync;
    void *clipboard;
    uint32_t clipboard_creates, audio_starts;
    BOOL audio_running;
    /* The log sink has its own lock: clipboard calls made under `lock` log synchronously, and
       SRW locks are not recursive. */
    SRWLOCK log_lock;
    VmIntegrationLog log;
    void *log_ctx;
    volatile LONG muted, stopping;
    HANDLE thread, ready;
    DWORD thread_id;
    HWND hwnd;
    /* default audio */
    HANDLE audio_thread;
    CRITICAL_SECTION audio_cs;
    SOCKET audio_socket;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static VmIntegration *g_items[VI_MAX_INSTANCES];
static VmIntegrationBackend g_backend;
static BOOL g_backend_set;

static void vi_logf(VmIntegration *vi, const wchar_t *fmt, ...)
{
    wchar_t text[600];
    va_list args;
    int n = swprintf_s(text, ARRAYSIZE(text), L"%s: ", vi && vi->name[0] ? vi->name : L"VM");
    if (n < 0) n = 0;
    va_start(args, fmt);
    vswprintf_s(text + n, ARRAYSIZE(text) - (size_t)n, fmt, args);
    va_end(args);
    VmIntegrationLog log = NULL;
    void *ctx = NULL;
    if (vi) {
        AcquireSRWLockShared(&vi->log_lock);
        log = vi->log;
        ctx = vi->log_ctx;
        ReleaseSRWLockShared(&vi->log_lock);
    }
    if (log) log(ctx, text);
    else if (g_backend.log) g_backend.log(text);
}

void vm_integration_log(VmIntegration *vi, const wchar_t *message)
{
    vi_logf(vi, L"%s", message);
}

/* ================================ default services ======================== */

#ifndef VM_INTEGRATION_NO_DEFAULTS

#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#define HV_PROTOCOL_RAW 1
#define HVSOCKET_CONNECT_TIMEOUT 1

#pragma pack(push, 1)
typedef struct { USHORT family; USHORT reserved; GUID vm_id; GUID service_id; } ViSockAddrHv;
#pragma pack(pop)

static void clip_log(const wchar_t *msg, void *ud) { vm_integration_log((VmIntegration *)ud, msg); }

static void *def_clipboard_create(const GUID *runtime, const wchar_t *os_type, HWND hwnd, VmIntegration *vi)
{
    return vm_clipboard_create(runtime, os_type, hwnd, clip_log, vi);
}
static void def_clipboard_destroy(void *c) { vm_clipboard_destroy((VmClipboard)c); }
static void def_clipboard_sync(void *c, BOOL on) { vm_clipboard_set_sync_enabled((VmClipboard)c, on); }
static void def_clipboard_sharing(void *c, BOOL on) { vm_clipboard_set_sharing_enabled((VmClipboard)c, on); }
static void def_clipboard_update(void *c) { vm_clipboard_on_clipboard_update((VmClipboard)c); }
static void def_clipboard_apply(void *c) { vm_clipboard_on_reader_apply((VmClipboard)c); }

static BOOL def_load_sharing(UINT64 vm_id)
{
    AsbDisplaySettings settings;
    AsbVm vm = (AsbVm)asb_find_vm_by_id(vm_id);
    return !vm || FAILED(asb_vm_get_display_settings(vm, &settings)) || settings.share_clipboard;
}

static void def_log(const wchar_t *message)
{
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}

/* ---- audio playback: guest -> host (moved from the desktop viewer) ---- */

#define AUDIO_HEADER_MAGIC 0x31415341u   /* 'ASA1'; mirror of tools/agent/appsandbox-audio.c */
#pragma pack(push, 1)
typedef struct { UINT32 magic, sample_rate; UINT16 channels, bits_per_sample, format_tag, block_align; } ViAudioHeader;
typedef struct { UINT32 bytes; } ViAudioFrame;
#pragma pack(pop)

static const CLSID VI_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID VI_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID VI_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID VI_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

static SOCKET audio_connect(VmIntegration *vi, DWORD timeout_ms)
{
    static const GUID zero = { 0 };
    ViSockAddrHv addr;
    if (!memcmp(&vi->runtime, &zero, sizeof(zero))) return INVALID_SOCKET;
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return s;
    setsockopt(s, HV_PROTOCOL_RAW, HVSOCKET_CONNECT_TIMEOUT, (const char *)&timeout_ms, sizeof(timeout_ms));
    memset(&addr, 0, sizeof(addr));
    addr.family = AF_HYPERV;
    addr.vm_id = vi->runtime;
    hcs_service_guid(vi->os_type, 4, &addr.service_id);
    if (connect(s, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static BOOL audio_recv(VmIntegration *vi, SOCKET s, void *buffer, int len)
{
    char *p = (char *)buffer;
    while (len > 0 && !vi->stopping) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return len == 0;
}

static void audio_wait(VmIntegration *vi, int ms)
{
    for (int waited = 0; waited < ms && !vi->stopping; waited += 100) Sleep(100);
}

static DWORD WINAPI audio_thread_proc(LPVOID param)
{
    VmIntegration *vi = (VmIntegration *)param;
    BOOL com_ok = FALSE;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    DWORD task_index = 0;
    int header_misses = 0;
    WSADATA wsa;
    BOOL wsa_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) com_ok = SUCCEEDED(hr);
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

    while (!vi->stopping) {
        IMMDeviceEnumerator *enumerator = NULL;
        IMMDevice *device = NULL;
        IAudioClient *client = NULL;
        IAudioRenderClient *render = NULL;
        WAVEFORMATEX format;
        HANDLE event = NULL;
        UINT32 buffer_frames = 0, block = 0;
        BOOL started = FALSE, logged = FALSE;
        ViAudioHeader header;

        SOCKET s = audio_connect(vi, 1000);
        if (s == INVALID_SOCKET) { audio_wait(vi, 2000); continue; }
        DWORD no_timeout = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&no_timeout, sizeof(no_timeout));
        EnterCriticalSection(&vi->audio_cs);
        if (vi->stopping) { LeaveCriticalSection(&vi->audio_cs); closesocket(s); break; }
        vi->audio_socket = s;
        LeaveCriticalSection(&vi->audio_cs);

        /* The guest closes without a header while its audio endpoint is not ready yet. */
        if (!audio_recv(vi, s, &header, sizeof(header)) || header.magic != AUDIO_HEADER_MAGIC) {
            if (!header_misses++) vi_logf(vi, L"Audio header bad/absent - guest not ready, retrying quietly.");
            goto session_end;
        }
        header_misses = 0;
        logged = TRUE;
        vi_logf(vi, L"Audio connected: %lu Hz, %u ch, %u bits, tag=%u.", header.sample_rate, header.channels,
                header.bits_per_sample, header.format_tag);

        hr = CoCreateInstance(&VI_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &VI_IID_IMMDeviceEnumerator,
                              (void **)&enumerator);
        if (FAILED(hr)) { vi_logf(vi, L"Audio: CoCreateInstance failed 0x%08lX.", hr); goto session_end; }
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
        if (FAILED(hr)) { vi_logf(vi, L"Audio: GetDefaultAudioEndpoint failed 0x%08lX.", hr); goto session_end; }
        hr = IMMDevice_Activate(device, &VI_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
        if (FAILED(hr)) { vi_logf(vi, L"Audio: Activate failed 0x%08lX.", hr); goto session_end; }
        /* The guest's own format; AUTOCONVERTPCM converts it to the endpoint's (GetMixFormat is
           deliberately not used: some endpoints fail it for their configured format). */
        memset(&format, 0, sizeof(format));
        format.wFormatTag = header.format_tag;
        format.nChannels = header.channels;
        format.nSamplesPerSec = header.sample_rate;
        format.wBitsPerSample = header.bits_per_sample;
        format.nBlockAlign = header.block_align ? header.block_align : (WORD)((header.channels * header.bits_per_sample) / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        block = format.nBlockAlign;
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     400000, 0, &format, NULL);
        if (FAILED(hr)) { vi_logf(vi, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr); goto session_end; }
        if (FAILED(IAudioClient_GetBufferSize(client, &buffer_frames))) goto session_end;
        if (FAILED(IAudioClient_GetService(client, &VI_IID_IAudioRenderClient, (void **)&render))) goto session_end;
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!event || FAILED(IAudioClient_SetEventHandle(client, event))) goto session_end;

        while (!vi->stopping) {
            ViAudioFrame frame;
            UINT32 frames, offset = 0;
            if (!audio_recv(vi, s, &frame, sizeof(frame))) break;
            if (!frame.bytes || frame.bytes > 4u * 1024u * 1024u || !block || frame.bytes % block) break;
            if (frame.bytes > scratch_cap) {
                BYTE *grown = scratch ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, frame.bytes)
                                      : (BYTE *)HeapAlloc(GetProcessHeap(), 0, frame.bytes);
                if (!grown) break;
                scratch = grown;
                scratch_cap = frame.bytes;
            }
            if (!audio_recv(vi, s, scratch, (int)frame.bytes)) break;
            if (vi->muted) continue;   /* keep draining the stream, play nothing */
            frames = frame.bytes / block;
            while (offset < frames && !vi->stopping && !vi->muted) {
                UINT32 padding, room, count;
                BYTE *dst;
                hr = IAudioClient_GetCurrentPadding(client, &padding);
                if (FAILED(hr) || padding > buffer_frames) goto session_end;
                if (!padding && buffer_frames > 1) {
                    UINT32 lead = buffer_frames / 2;
                    if (FAILED(IAudioRenderClient_GetBuffer(render, lead, &dst))) goto session_end;
                    if (FAILED(IAudioRenderClient_ReleaseBuffer(render, lead, AUDCLNT_BUFFERFLAGS_SILENT))) goto session_end;
                    padding = lead;
                }
                room = buffer_frames - padding;
                if (!room) {
                    if (WaitForSingleObject(event, 100) == WAIT_FAILED) goto session_end;
                    continue;
                }
                count = frames - offset < room ? frames - offset : room;
                if (FAILED(IAudioRenderClient_GetBuffer(render, count, &dst))) goto session_end;
                memcpy(dst, scratch + (size_t)offset * block, (size_t)count * block);
                if (FAILED(IAudioRenderClient_ReleaseBuffer(render, count, 0))) goto session_end;
                offset += count;
                if (!started) {
                    ResetEvent(event);
                    if (FAILED(IAudioClient_Start(client))) goto session_end;
                    started = TRUE;
                }
            }
        }
session_end:
        if (started) IAudioClient_Stop(client);
        if (render) IAudioRenderClient_Release(render);
        if (client) IAudioClient_Release(client);
        if (event) CloseHandle(event);
        if (device) IMMDevice_Release(device);
        if (enumerator) IMMDeviceEnumerator_Release(enumerator);
        EnterCriticalSection(&vi->audio_cs);
        vi->audio_socket = INVALID_SOCKET;
        LeaveCriticalSection(&vi->audio_cs);
        closesocket(s);
        if (logged) vi_logf(vi, L"Audio session ended.");
        if (vi->stopping) break;
        audio_wait(vi, header_misses > 3 ? 5000 : 500);   /* back off while the guest refuses */
    }
    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (task) AvRevertMmThreadCharacteristics(task);
    if (com_ok) CoUninitialize();
    if (wsa_ok) WSACleanup();
    return 0;
}

static BOOL def_audio_start(VmIntegration *vi)
{
    vi->audio_thread = CreateThread(NULL, 0, audio_thread_proc, vi, 0, NULL);
    return vi->audio_thread != NULL;
}

static void def_audio_stop(VmIntegration *vi)
{
    EnterCriticalSection(&vi->audio_cs);
    if (vi->audio_socket != INVALID_SOCKET) shutdown(vi->audio_socket, SD_BOTH);
    LeaveCriticalSection(&vi->audio_cs);
    if (vi->audio_thread) {
        WaitForSingleObject(vi->audio_thread, INFINITE);
        CloseHandle(vi->audio_thread);
        vi->audio_thread = NULL;
    }
}

#endif /* VM_INTEGRATION_NO_DEFAULTS */

static void backend_defaults(void)
{
    if (g_backend_set) return;
    g_backend_set = TRUE;
#ifndef VM_INTEGRATION_NO_DEFAULTS
    g_backend.clipboard_create = def_clipboard_create;
    g_backend.clipboard_destroy = def_clipboard_destroy;
    g_backend.clipboard_sync = def_clipboard_sync;
    g_backend.clipboard_sharing = def_clipboard_sharing;
    g_backend.clipboard_update = def_clipboard_update;
    g_backend.clipboard_apply = def_clipboard_apply;
    g_backend.audio_start = def_audio_start;
    g_backend.audio_stop = def_audio_stop;
    g_backend.load_sharing = def_load_sharing;
    g_backend.log = def_log;
#endif
}

void vm_integration_set_backend(const VmIntegrationBackend *backend)
{
    AcquireSRWLockExclusive(&g_lock);
    g_backend_set = FALSE;
    memset(&g_backend, 0, sizeof(g_backend));
    backend_defaults();
    if (backend) {
        const void *const *src = (const void *const *)backend;
        void **dst = (void **)&g_backend;
        for (size_t i = 0; i < sizeof(g_backend) / sizeof(void *); ++i) if (src[i]) dst[i] = (void *)src[i];
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* ================================ clipboard owner thread ================== */

static void apply_sync_locked(VmIntegration *vi)
{
    BOOL any = FALSE;
    for (uint32_t i = 0; i < VM_INTEGRATION_MAX_CLIENTS; ++i) any |= vi->clients[i] && vi->focused[i];
    BOOL sync = vi->sharing && any;
    if (sync == vi->sync) return;
    vi->sync = sync;
    if (vi->clipboard && g_backend.clipboard_sync) g_backend.clipboard_sync(vi->clipboard, sync);
}

static LRESULT CALLBACK vi_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmIntegration *vi = (VmIntegration *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    void *clip = NULL;
    if (vi && (msg == WM_CLIPBOARDUPDATE || msg == WM_VI_CLIP_APPLY)) {
        AcquireSRWLockShared(&vi->lock);
        clip = vi->clipboard;
        ReleaseSRWLockShared(&vi->lock);
    }
    switch (msg) {
    case WM_CLIPBOARDUPDATE:
        if (clip && g_backend.clipboard_update) g_backend.clipboard_update(clip);
        return 0;
    case WM_VI_CLIP_APPLY:
        if (clip && g_backend.clipboard_apply) g_backend.clipboard_apply(clip);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI vi_thread_proc(LPVOID param)
{
    static const wchar_t cls[] = L"AppSandbox.VmIntegration";
    VmIntegration *vi = (VmIntegration *)param;
    WNDCLASSEXW wc;
    MSG msg;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = vi_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);   /* ERROR_CLASS_ALREADY_EXISTS after the first instance */
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);   /* create the queue before signalling */
    vi->hwnd = CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (vi->hwnd) {
        SetWindowLongPtrW(vi->hwnd, GWLP_USERDATA, (LONG_PTR)vi);
        AddClipboardFormatListener(vi->hwnd);
    }
    SetEvent(vi->ready);
    if (!vi->hwnd) return 1;
    /* The writer connect may take a moment; acquire does not wait for it. */
    void *clip = g_backend.clipboard_create && !vi->stopping
                     ? g_backend.clipboard_create(&vi->runtime, vi->os_type, vi->hwnd, vi) : NULL;
    AcquireSRWLockExclusive(&vi->lock);
    vi->clipboard = clip;
    if (clip) {
        vi->clipboard_creates++;
        if (g_backend.clipboard_sharing) g_backend.clipboard_sharing(clip, vi->sharing);
        if (g_backend.clipboard_sync) g_backend.clipboard_sync(clip, vi->sync);
    }
    ReleaseSRWLockExclusive(&vi->lock);
    if (!clip && !vi->stopping) vi_logf(vi, L"Clipboard module could not be created.");
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveClipboardFormatListener(vi->hwnd);
    AcquireSRWLockExclusive(&vi->lock);
    clip = vi->clipboard;
    vi->clipboard = NULL;
    ReleaseSRWLockExclusive(&vi->lock);
    if (clip && g_backend.clipboard_destroy) g_backend.clipboard_destroy(clip);
    DestroyWindow(vi->hwnd);
    vi->hwnd = NULL;
    return 0;
}

/* ================================ lifetime ================================ */

static void join_pumping(HANDLE thread)
{
    while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    }
}

static void destroy(VmIntegration *vi)
{
    InterlockedExchange(&vi->stopping, 1);
    if (vi->audio_running && g_backend.audio_stop) g_backend.audio_stop(vi);
    vi->audio_running = FALSE;
    if (vi->thread) {
        PostThreadMessageW(vi->thread_id, WM_QUIT, 0, 0);
        join_pumping(vi->thread);
        CloseHandle(vi->thread);
    }
    if (vi->ready) CloseHandle(vi->ready);
    DeleteCriticalSection(&vi->audio_cs);
    HeapFree(GetProcessHeap(), 0, vi);
}

VmIntegration *vm_integration_acquire(UINT64 vm_id, const GUID *runtime, const wchar_t *os_type, const wchar_t *name)
{
    VmIntegration *vi = NULL;
    int free_slot = -1;
    if (!runtime) return NULL;
    AcquireSRWLockExclusive(&g_lock);
    backend_defaults();
    for (int i = 0; i < VI_MAX_INSTANCES; ++i) {
        if (!g_items[i]) { if (free_slot < 0) free_slot = i; continue; }
        if (g_items[i]->vm_id == vm_id && IsEqualGUID(&g_items[i]->runtime, runtime)) { vi = g_items[i]; break; }
    }
    if (vi) {
        vi->refs++;
        ReleaseSRWLockExclusive(&g_lock);
        return vi;
    }
    if (free_slot < 0) { ReleaseSRWLockExclusive(&g_lock); return NULL; }
    vi = (VmIntegration *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*vi));
    if (!vi) { ReleaseSRWLockExclusive(&g_lock); return NULL; }
    vi->vm_id = vm_id;
    vi->runtime = *runtime;
    if (os_type) wcsncpy_s(vi->os_type, ARRAYSIZE(vi->os_type), os_type, _TRUNCATE);
    if (name) wcsncpy_s(vi->name, ARRAYSIZE(vi->name), name, _TRUNCATE);
    vi->refs = 1;
    InitializeSRWLock(&vi->lock);
    InitializeSRWLock(&vi->log_lock);
    InitializeCriticalSection(&vi->audio_cs);
    vi->audio_socket = INVALID_SOCKET;
    vi->sharing = g_backend.load_sharing ? g_backend.load_sharing(vm_id) : TRUE;
    vi->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    vi->thread = vi->ready ? CreateThread(NULL, 0, vi_thread_proc, vi, 0, &vi->thread_id) : NULL;
    if (!vi->thread) {
        ReleaseSRWLockExclusive(&g_lock);
        destroy(vi);
        return NULL;
    }
    WaitForSingleObject(vi->ready, INFINITE);
    if (g_backend.audio_start && g_backend.audio_start(vi)) {
        vi->audio_running = TRUE;
        vi->audio_starts++;
    }
    g_items[free_slot] = vi;
    ReleaseSRWLockExclusive(&g_lock);
    return vi;
}

void vm_integration_release(VmIntegration *vi)
{
    BOOL last = FALSE;
    if (!vi) return;
    AcquireSRWLockExclusive(&g_lock);
    if (--vi->refs == 0) {
        last = TRUE;
        for (int i = 0; i < VI_MAX_INSTANCES; ++i) if (g_items[i] == vi) g_items[i] = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (last) destroy(vi);
}

uint32_t vm_integration_count(void)
{
    uint32_t n = 0;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < VI_MAX_INSTANCES; ++i) n += g_items[i] != NULL;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

/* ================================ clients ================================= */

uint32_t vm_integration_client_add(VmIntegration *vi)
{
    uint32_t id = 0;
    if (!vi) return 0;
    AcquireSRWLockExclusive(&vi->lock);
    for (uint32_t i = 0; i < VM_INTEGRATION_MAX_CLIENTS; ++i) {
        if (vi->clients[i]) continue;
        if (!++vi->next_client) ++vi->next_client;
        id = vi->clients[i] = vi->next_client;
        vi->focused[i] = 0;
        break;
    }
    ReleaseSRWLockExclusive(&vi->lock);
    return id;
}

void vm_integration_client_focus(VmIntegration *vi, uint32_t client, BOOL focused)
{
    if (!vi || !client) return;
    AcquireSRWLockExclusive(&vi->lock);
    for (uint32_t i = 0; i < VM_INTEGRATION_MAX_CLIENTS; ++i)
        if (vi->clients[i] == client) vi->focused[i] = focused ? 1u : 0u;
    apply_sync_locked(vi);
    ReleaseSRWLockExclusive(&vi->lock);
}

void vm_integration_client_remove(VmIntegration *vi, uint32_t client)
{
    if (!vi || !client) return;
    AcquireSRWLockExclusive(&vi->lock);
    for (uint32_t i = 0; i < VM_INTEGRATION_MAX_CLIENTS; ++i)
        if (vi->clients[i] == client) { vi->clients[i] = 0; vi->focused[i] = 0; }
    apply_sync_locked(vi);
    ReleaseSRWLockExclusive(&vi->lock);
}

void vm_integration_set_clipboard_sharing(VmIntegration *vi, BOOL enabled)
{
    if (!vi) return;
    AcquireSRWLockExclusive(&vi->lock);
    enabled = enabled != FALSE;
    if (vi->sharing != enabled) {
        vi->sharing = enabled;
        if (vi->clipboard && g_backend.clipboard_sharing) g_backend.clipboard_sharing(vi->clipboard, enabled);
    }
    apply_sync_locked(vi);
    ReleaseSRWLockExclusive(&vi->lock);
}

BOOL vm_integration_clipboard_sharing(VmIntegration *vi)
{
    BOOL on;
    if (!vi) return FALSE;
    AcquireSRWLockShared(&vi->lock);
    on = vi->sharing;
    ReleaseSRWLockShared(&vi->lock);
    return on;
}

void vm_integration_set_audio_muted(VmIntegration *vi, BOOL muted)
{
    if (vi) InterlockedExchange(&vi->muted, muted ? 1 : 0);
}

BOOL vm_integration_audio_muted(VmIntegration *vi)
{
    return vi && InterlockedCompareExchange(&vi->muted, 0, 0) != 0;
}

BOOL vm_integration_audio_stopping(VmIntegration *vi)
{
    return !vi || vi->stopping;
}

void vm_integration_set_log(VmIntegration *vi, VmIntegrationLog log, void *ctx)
{
    if (!vi) return;
    AcquireSRWLockExclusive(&vi->log_lock);
    vi->log = log;
    vi->log_ctx = ctx;
    ReleaseSRWLockExclusive(&vi->log_lock);
}

void vm_integration_info(VmIntegration *vi, VmIntegrationInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (!vi) return;
    AcquireSRWLockShared(&g_lock);
    out->refs = (uint32_t)vi->refs;
    ReleaseSRWLockShared(&g_lock);
    AcquireSRWLockShared(&vi->lock);
    for (uint32_t i = 0; i < VM_INTEGRATION_MAX_CLIENTS; ++i) {
        out->clients += vi->clients[i] != 0;
        out->focused += vi->clients[i] && vi->focused[i];
    }
    out->sharing = vi->sharing;
    out->sync = vi->sync;
    out->clipboard = vi->clipboard;
    out->clipboard_creates = vi->clipboard_creates;
    out->audio_starts = vi->audio_starts;
    out->audio_running = vi->audio_running;
    ReleaseSRWLockShared(&vi->lock);
    out->muted = vm_integration_audio_muted(vi);
}
