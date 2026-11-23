/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_proxy_native_test
 *
 * @file      remote_windows_proxy_native_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows native host proxies, native (AppSandbox-eh6.6).
 *
 * The real guest side - the helper control session (remote_windows_helper.c), parking
 * (remote_windows_share*.c), window control (remote_windows_control_win32.c) and the capture
 * engine - runs in this process against a separate application process
 * (remote-windows-proxy-test-app.exe). It talks over loopback TCP to the real host manager
 * (remote_windows_proxy.cpp + remote_windows_session.c), which creates real proxy windows on this
 * Windows 11 desktop. The "private display" is the right half of the primary monitor's work area
 * (no VDD on the host), so parked sources stay on a real monitor (menus position as on a guest).
 *
 * Checked natively: two roots shared at once; shell entries (unowned, APPWINDOW, visible, guest
 * title + icon, per-application AUMID); no host caption (client = visible frame, guest pixels at
 * the client origin, guest title band at the top for custom chrome); guest hit testing (custom
 * caption/buttons/top edge, standard caption and maximize button for Snap Layouts); acknowledged
 * resize including the application's minimum; host maximize/restore (source sized, never
 * maximized) and minimize (source untouched); application-initiated maximize/minimize replayed on
 * the proxy; an overflowing menu as an owned non-activating popup outside the root; a modal
 * dialog as an owned framed proxy with the root disabled, host close refused while it is up and
 * the dialog moved through the host; unsaved-close prompt and cancellation (proxy survives);
 * unselect restores without closing; real close removes the proxy; stopping the manager restores
 * everything; no proxy windows left. With --snap (foreground input), Win+Left snaps a proxy.
 *
 *   remote-windows-proxy-native-test.exe [--cpu] [--snap] [--evidence <dir>]
 */
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../agent/remote_windows_capture.h"
#include "../agent/remote_windows_control_win32.h"
#include "../agent/remote_windows_helper.h"
#include "../agent/remote_windows_inventory.h"
#include "../agent/remote_windows_share_win32.h"
#include "../../src/backend_win/remote_windows_proxy.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")

#define MAX_PROXIES 32

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; tlog("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

static FILE *g_log;
static const char *g_evidence;
static int g_cpu, g_snap;

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

static void quiet_log(const char *message) { (void)message; }
static void tagged_log(void *ctx, const char *message)
{
    if (strstr(message, "geometry") || strstr(message, "close") || strstr(message, "proxy") || strstr(message, "session") ||
        strstr(message, "application") || strstr(message, "manager") || strstr(message, "select") || strstr(message, "frame channel"))
        tlog("  [%s] %s", (const char *)ctx, message);
}
static void helper_log_line(const char *message) { tagged_log("guest", message); }

/* ============================================================ loopback transport */

static SOCKET g_listen[2];
static USHORT g_port[2];   /* 0: control (9), 1: frames (10) */

static SOCKET make_listener(USHORT *port)
{
    struct sockaddr_in a;
    int len = sizeof(a);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4) || getsockname(s, (struct sockaddr *)&a, &len)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    *port = ntohs(a.sin_port);
    return s;
}

static SOCKET accept_timeout(SOCKET l, DWORD ms)
{
    fd_set set;
    struct timeval tv = { (long)(ms / 1000), (long)((ms % 1000) * 1000) };
    FD_ZERO(&set);
    FD_SET(l, &set);
    if (select(0, &set, NULL, NULL, &tv) != 1) return INVALID_SOCKET;
    SOCKET s = accept(l, NULL, NULL);
    if (s != INVALID_SOCKET) { BOOL one = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)); }
    return s;
}

static SOCKET host_connect(void *ctx, int channel, DWORD timeout_ms)
{
    struct sockaddr_in a;
    (void)ctx; (void)timeout_ms;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(g_port[channel == ASB_CH_RW_CONTROL ? 0 : 1]);
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { closesocket(s); return INVALID_SOCKET; }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return s;
}

/* ============================================================ guest side */

static HWND g_owner;
static RECT g_desk, g_priv;
static AsbRwCapture *g_capture;
static volatile LONG g_guest_stop, g_guest_sessions;
static HANDLE g_guest_thread;

static LRESULT CALLBACK owner_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return m == WM_CLOSE ? 0 : DefWindowProcW(h, m, w, l); }
static DWORD WINAPI owner_thread(LPVOID ready)
{
    WNDCLASSW wc;
    MSG msg;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = owner_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = ASB_RW_OWNER_CLASS;
    RegisterClassW(&wc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"owner", WS_POPUP, 0, 0, 0, 0,
                              NULL, NULL, wc.hInstance, NULL);
    SetEvent((HANDLE)ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

static SOCKET frame_accept(void *ctx, DWORD timeout_ms) { (void)ctx; return accept_timeout(g_listen[1], timeout_ms); }
static void frame_close(void *ctx, SOCKET s) { (void)ctx; closesocket(s); }
static void capture_log(void *ctx, const char *message) { (void)ctx; (void)message; }

typedef struct GuestConn { SOCKET s; } GuestConn;

static BOOL g_send(void *ctx, const uint8_t *bytes, size_t len)
{
    GuestConn *c = (GuestConn *)ctx;
    while (len) {
        int n = send(c->s, (const char *)bytes, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return FALSE;
        bytes += n;
        len -= (size_t)n;
    }
    return TRUE;
}
static uint32_t g_enumerate(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx;
    return asb_rw_enumerate_windows(out, max, GetCurrentProcessId(), count);
}
static BOOL g_facts(void *ctx, HWND hwnd, AsbRwWindowFacts *f)
{
    (void)ctx;
    return asb_rw_collect_facts(hwnd, GetCurrentProcessId(), asb_rw_active_session(), f);
}
static int g_probe(void *ctx, uint64_t hwnd, AsbRwIdentity *live) { (void)ctx; return asb_rw_probe_identity(NULL, hwnd, live); }
static void g_hlog(void *ctx, const char *message) { (void)ctx; helper_log_line(message); }
static uint32_t g_scan(void *ctx, AsbRwWindowFacts *out, uint32_t max, uint32_t *count)
{
    (void)ctx;
    return asb_rw_scan_visible_windows(out, max, count);
}
static void g_capture_set(void *ctx, uint64_t root_id, const AsbRwCaptureTarget *t, uint32_t n)
{
    (void)ctx;
    asb_rw_capture_set(g_capture, root_id, t, n);
}

static BOOL recv_exact(SOCKET s, uint8_t *p, size_t len)
{
    while (len) {
        int n = recv(s, (char *)p, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= (size_t)n;
    }
    return TRUE;
}

static AsbRwShare g_share;

static void guest_serve(SOCKET s)
{
    GuestConn conn = { s };
    AsbRwHelperOps ops;
    static AsbRwHelper helper;
    static uint8_t buffer[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD];
    ULONGLONG last_inventory = 0;
    memset(&ops, 0, sizeof(ops));
    ops.ctx = &conn; ops.send = g_send; ops.enumerate = g_enumerate; ops.facts = g_facts; ops.probe = g_probe;
    ops.log = g_hlog; ops.scan = g_scan; ops.capture = g_capture_set;
    asb_rw_ctl_ops(&ops);
    uint32_t caps = ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_PRIVATE_DISPLAY | ASB_RW_CAP_FRAMES_CPU |
                    ASB_RW_CAP_POPUPS | (g_cpu ? 0u : ASB_RW_CAP_FRAMES_GPU);
    caps = asb_rw_helper_capabilities(&ops, caps);
    uint32_t tag = 0x6E660000u | (uint32_t)(GetTickCount() & 0xFFFF) | 1u;
    if (asb_rw_helper_open(&helper, &ops, &g_share, caps, 1, tag)) return;
    asb_rw_capture_session(g_capture, tag);
    InterlockedIncrement(&g_guest_sessions);
    while (!g_guest_stop) {
        fd_set set;
        struct timeval tv = { 0, 100000 };
        FD_ZERO(&set);
        FD_SET(s, &set);
        int ready = select(0, &set, NULL, NULL, &tv);
        if (ready < 0) break;
        if (ready > 0) {
            size_t total = 0;
            if (!recv_exact(s, buffer, ASB_RW_HEADER_BYTES)) break;
            if (asb_rw_frame_check(ASB_CH_RW_CONTROL, buffer, ASB_RW_HEADER_BYTES, &total) ||
                !recv_exact(s, buffer + ASB_RW_HEADER_BYTES, total - ASB_RW_HEADER_BYTES))
                break;
            if (asb_rw_helper_receive(&helper, buffer, total)) break;
        }
        ULONGLONG now = GetTickCount64();
        int inventory = now - last_inventory >= 500;
        if (inventory) last_inventory = now;
        asb_rw_helper_tick(&helper, inventory);
        if (helper.send_failed) break;
    }
    asb_rw_helper_close(&helper);   /* selections end with the session: everything is restored */
    asb_rw_capture_session(g_capture, 0);
}

static DWORD WINAPI guest_thread(LPVOID arg)
{
    AsbRwWin32 w;
    AsbRwShareOps ops;
    WCHAR journal[MAX_PATH];
    (void)arg;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetTempPathW(MAX_PATH, journal);
    wcscat_s(journal, MAX_PATH, L"asb-eh66-journal.bin");
    DeleteFileW(journal);
    asb_rw_win32_init(&w, journal, quiet_log);
    w.fake_display = TRUE;
    w.fake_desktop = g_desk;
    w.fake_private = g_priv;
    asb_rw_win32_ops(&w, &ops);
    asb_rw_share_init(&g_share, &ops, g_owner, 0x4548362E36ull, 2560, 1440);
    while (!g_guest_stop) {
        SOCKET s = accept_timeout(g_listen[0], 200);
        if (s == INVALID_SOCKET) continue;
        guest_serve(s);
        closesocket(s);
    }
    asb_rw_share_restore_all(&g_share);
    asb_rw_win32_close(&w);
    CoUninitialize();
    return 0;
}

/* ============================================================ helpers */

static AsbRwManager *g_mgr;
static HWND g_src_std, g_src_custom;

typedef BOOL (*Cond)(void *arg);
static BOOL wait_for(Cond cond, void *arg, DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        if (cond(arg)) return TRUE;
        if (GetTickCount64() > end) return FALSE;
        Sleep(40);
    }
}

static void visible_frame(HWND h, RECT *r)
{
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)))) GetWindowRect(h, r);
}
static int rw_(const RECT *r) { return r->right - r->left; }
static int rh_(const RECT *r) { return r->bottom - r->top; }

static uint64_t id_by_title(const wchar_t *title)
{
    static AsbRwManagerEntry entries[ASB_RW_MAX_INVENTORY];
    uint32_t n = asb_rw_manager_inventory(g_mgr, entries, ASB_RW_MAX_INVENTORY);
    size_t len = wcslen(title);
    for (uint32_t i = 0; i < n; ++i) {
        const AsbRwInventoryEntry *e = &entries[i].entry;
        if (e->title_len != len) continue;
        BOOL same = TRUE;
        for (size_t k = 0; k < len; ++k) same &= e->title[k] == (uint16_t)title[k];
        if (same) return e->id;
    }
    return 0;
}

static BOOL find_proxy(uint64_t id, AsbRwProxyInfo *out)
{
    static AsbRwProxyInfo infos[MAX_PROXIES];
    uint32_t n = asb_rw_manager_proxies(g_mgr, infos, MAX_PROXIES);
    for (uint32_t i = 0; i < n; ++i) if (infos[i].id == id) { if (out) *out = infos[i]; return TRUE; }
    return FALSE;
}

static uint32_t proxy_count(void)
{
    static AsbRwProxyInfo infos[MAX_PROXIES];
    return asb_rw_manager_proxies(g_mgr, infos, MAX_PROXIES);
}

static HWND surface_window(uint64_t id, uint32_t kind)
{
    AsbRwProxyInfo info;
    if (!find_proxy(id, &info)) return NULL;
    for (uint32_t i = 0; i < info.count; ++i) if (info.surfaces[i].kind == kind) return info.surfaces[i].hwnd;
    return NULL;
}

static void client_screen(HWND h, RECT *r)
{
    POINT o = { 0, 0 };
    GetClientRect(h, r);
    ClientToScreen(h, &o);
    OffsetRect(r, o.x, o.y);
}

/* Guest client origin inside its visible frame (both measured live on this machine). */
static POINT client_offset(HWND src)
{
    RECT vis;
    POINT o = { 0, 0 };
    visible_frame(src, &vis);
    ClientToScreen(src, &o);
    o.x -= vis.left;
    o.y -= vis.top;
    return o;
}

static LRESULT hit(HWND proxy, int cx, int cy)
{
    POINT pt = { cx, cy };
    ClientToScreen(proxy, &pt);
    return SendMessageW(proxy, WM_NCHITTEST, 0, MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y));
}

/* Polls the proxy's hit test until the guest answer arrives. */
static LRESULT hit_settled(HWND proxy, int cx, int cy, LRESULT want)
{
    LRESULT r = 0;
    for (int i = 0; i < 60; ++i) {
        r = hit(proxy, cx, cy);
        if (r == want) return r;
        Sleep(30);
    }
    return r;
}

/* Screen pixels (composed by DWM) of a rectangle; caller frees. */
static uint32_t *grab(const RECT *r)
{
    int w = rw_(r), h = rh_(r);
    BITMAPINFO bi;
    void *bits = NULL;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, r->left, r->top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    uint32_t *copy = (uint32_t *)malloc((size_t)w * h * 4);
    if (copy && bits) memcpy(copy, bits, (size_t)w * h * 4);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return copy;
}

static void save_bmp(const char *name, const uint32_t *px, int w, int h)
{
    char path[MAX_PATH];
    FILE *f = NULL;
    if (!g_evidence || !px) return;
    sprintf_s(path, sizeof(path), "%s\\%s.bmp", g_evidence, name);
    if (fopen_s(&f, path, "wb") || !f) return;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)(w * h * 4);
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
    tlog("  evidence: %s", path);
}

static int near_rgb(uint32_t p, int r, int g, int b)
{
    int pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
    return abs(pr - r) <= 24 && abs(pg - g) <= 24 && abs(pb - b) <= 24;
}

/* Pixel at a proxy-client position, with the proxy (and its owned windows) temporarily topmost. */
static uint32_t proxy_pixel(HWND proxy, int cx, int cy, const char *evidence)
{
    RECT c;
    client_screen(proxy, &c);
    SetWindowPos(proxy, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Sleep(350);
    RECT r = c;
    InflateRect(&r, 40, 40);
    if (evidence) r.left -= 360;   /* room for popups outside the root (menus open to the left) */
    uint32_t *px = grab(&r);
    SetWindowPos(proxy, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    uint32_t v = 0;
    int x = cx + (c.left - r.left), y = cy + (c.top - r.top);
    if (px && x >= 0 && y >= 0 && x < rw_(&r) && y < rh_(&r)) v = px[y * rw_(&r) + x];
    if (evidence) save_bmp(evidence, px, rw_(&r), rh_(&r));
    free(px);
    return v;
}

/* proxy_pixel until the expected colour shows (DWM may need a few frames after a z change). */
static uint32_t proxy_pixel_expect(HWND proxy, int cx, int cy, int r, int g, int b, const char *evidence)
{
    uint32_t v = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        v = proxy_pixel(proxy, cx, cy, evidence);
        if (near_rgb(v, r, g, b)) {
            if (attempt) tlog("  (pixel settled after %d retries)", attempt);
            return v;
        }
        Sleep(300 * (attempt + 1));
    }
    return v;
}

/* ---- conditions -------------------------------------------------------------------------- */

static BOOL c_ready(void *a) { AsbRwManagerStatus st; (void)a; asb_rw_manager_status(g_mgr, &st); return st.state == ASB_RW_MGR_READY; }
static BOOL c_listed(void *a) { return id_by_title((const wchar_t *)a) != 0; }
typedef struct ProxyWait { uint64_t id; uint64_t min_presented; } ProxyWait;
static BOOL c_proxy_drawn(void *a)
{
    ProxyWait *w = (ProxyWait *)a;
    AsbRwProxyInfo info;
    return find_proxy(w->id, &info) && info.presented > w->min_presented && info.content_w > 0;
}
static BOOL c_proxy_gone(void *a) { return !find_proxy(*(uint64_t *)a, NULL); }
typedef struct SizeWait { HWND src, proxy; int w, h; } SizeWait;
static BOOL c_source_size(void *a)
{
    SizeWait *s = (SizeWait *)a;
    RECT vis;
    visible_frame(s->src, &vis);
    return rw_(&vis) == s->w && rh_(&vis) == s->h;
}
static BOOL c_proxy_matches_source(void *a)
{
    SizeWait *s = (SizeWait *)a;
    RECT vis, c;
    visible_frame(s->src, &vis);
    GetClientRect(s->proxy, &c);
    return rw_(&vis) == rw_(&c) && rh_(&vis) == rh_(&c);
}
typedef struct SurfWait { uint64_t id; uint32_t kind; int present; } SurfWait;
static BOOL c_surface(void *a) { SurfWait *s = (SurfWait *)a; return (surface_window(s->id, s->kind) != NULL) == s->present; }
static BOOL c_disabled(void *a) { return !IsWindowEnabled((HWND)a); }
static BOOL c_enabled(void *a) { return IsWindowEnabled((HWND)a); }
static BOOL c_zoomed(void *a) { return IsZoomed((HWND)a); }
static BOOL c_not_zoomed(void *a) { return !IsZoomed((HWND)a) && !IsIconic((HWND)a); }
static BOOL c_iconic(void *a) { return IsIconic((HWND)a); }
static BOOL c_icon(void *a) { return SendMessageW((HWND)a, WM_GETICON, ICON_BIG, 0) != 0; }
static BOOL c_window_gone(void *a) { return !IsWindow((HWND)a); }
typedef struct RectWait { HWND h; RECT r; } RectWait;
static BOOL c_rect(void *a) { RectWait *w = (RectWait *)a; RECT r; GetWindowRect(w->h, &r); return EqualRect(&r, &w->r); }

static BOOL proxy_aumid_ok(HWND h)
{
    IPropertyStore *store = NULL;
    PROPVARIANT v;
    BOOL ok = FALSE;
    PropVariantInit(&v);
    if (FAILED(SHGetPropertyStoreForWindow(h, &IID_IPropertyStore, (void **)&store))) return FALSE;
    if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        ok = wcsncmp(v.pwszVal, L"AppSandbox.RemoteWindows.", 25) == 0;
    PropVariantClear(&v);
    IPropertyStore_Release(store);
    return ok;
}

static void manager_log(void *ctx, const char *message) { tagged_log(ctx, message); }

/* ============================================================ test */

static BOOL CALLBACK count_ours(HWND h, LPARAM lp)
{
    WCHAR cls[64];
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && GetClassNameW(h, cls, 64) &&
        (!wcscmp(cls, L"AppSandbox.RemoteWindows.Proxy") || !wcscmp(cls, L"AppSandbox.RemoteWindows.Popup")))
        ++*(int *)lp;
    return TRUE;
}

static void send_keys_win_left(void)
{
    INPUT in[4];
    memset(in, 0, sizeof(in));
    for (int i = 0; i < 4; ++i) in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = VK_LEFT;
    in[2].ki.wVk = VK_LEFT; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR app[MAX_PATH], *slash;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu")) g_cpu = 1;
        else if (!strcmp(argv[i], "--snap")) g_snap = 1;
        else if (!strcmp(argv[i], "--evidence") && i + 1 < argc) g_evidence = argv[++i];
    }
    if (g_evidence) {
        char path[MAX_PATH];
        CreateDirectoryA(g_evidence, NULL);
        sprintf_s(path, sizeof(path), "%s\\proxy-native%s.log", g_evidence, g_cpu ? "-cpu" : "");
        fopen_s(&g_log, path, "w");
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    tlog("eh6.6 proxy native test (%s frames)", g_cpu ? "CPU" : "GPU-allowed");

    /* Regions: "desktop" = left half of the primary work area, "private display" = right half. */
    MONITORINFO mi = { sizeof(mi) };
    POINT origin = { 0, 0 };
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_desk = mi.rcWork;
    g_desk.right = mi.rcWork.left + rw_(&mi.rcWork) / 2;
    g_priv = mi.rcWork;
    g_priv.left = g_desk.right;
    tlog("desktop (%ld,%ld)-(%ld,%ld), private (%ld,%ld)-(%ld,%ld)", g_desk.left, g_desk.top, g_desk.right, g_desk.bottom,
         g_priv.left, g_priv.top, g_priv.right, g_priv.bottom);

    /* The guest application, as its own process. */
    GetModuleFileNameW(NULL, app, MAX_PATH);
    slash = wcsrchr(app, L'\\');
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - app), L"remote-windows-proxy-test-app.exe");
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(app, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { tlog("cannot start the test app"); return 2; }
    WaitForInputIdle(pi.hProcess, 5000);
    for (int i = 0; i < 100 && (!g_src_std || !g_src_custom); ++i) {
        g_src_std = FindWindowW(L"AsbEh66Standard", NULL);
        g_src_custom = FindWindowW(L"AsbEh66Custom", NULL);
        Sleep(50);
    }
    CHECK(g_src_std && g_src_custom);
    RECT orig_std, orig_custom;
    GetWindowRect(g_src_std, &orig_std);
    GetWindowRect(g_src_custom, &orig_custom);

    /* Guest side. */
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, owner_thread, ready, 0, NULL));
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    g_listen[0] = make_listener(&g_port[0]);
    g_listen[1] = make_listener(&g_port[1]);
    AsbRwCaptureConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.accept = frame_accept;
    cc.close = frame_close;
    cc.log = capture_log;
    cc.allow_gpu = g_cpu ? 0u : 1u;
    cc.slots = 3;
    cc.max_cpu_bytes = 128u << 20;
    g_capture = asb_rw_capture_start(&cc);
    CHECK(g_capture != NULL);
    g_guest_thread = CreateThread(NULL, 0, guest_thread, NULL, 0, NULL);

    /* Host manager. */
    AsbRwManagerConfig mc;
    memset(&mc, 0, sizeof(mc));
    mc.connect = host_connect;
    mc.ctx = "host";
    mc.log = manager_log;
    mc.allow_gpu = g_cpu ? 0u : 1u;
    mc.local_frames = 1;
    mc.no_activate = 1;
    mc.group = L"NativeTest";
    g_mgr = asb_rw_manager_start(&mc);
    CHECK(g_mgr != NULL);
    if (!g_mgr) return 3;
    CHECK(wait_for(c_ready, NULL, 8000));
    CHECK(wait_for(c_listed, (void *)L"eh6.6 standard", 5000));
    CHECK(wait_for(c_listed, (void *)L"eh6.6 custom", 5000));
    uint64_t id_std = id_by_title(L"eh6.6 standard"), id_custom = id_by_title(L"eh6.6 custom");

    /* ---- multiple roots, shell entries ---- */
    CHECK(asb_rw_manager_select(g_mgr, id_std, 1) == ASB_RW_OK);
    CHECK(asb_rw_manager_select(g_mgr, id_custom, 1) == ASB_RW_OK);
    ProxyWait pw_std = { id_std, 0 }, pw_custom = { id_custom, 0 };
    CHECK(wait_for(c_proxy_drawn, &pw_std, 8000));
    CHECK(wait_for(c_proxy_drawn, &pw_custom, 8000));
    AsbRwProxyInfo pstd, pcus;
    memset(&pstd, 0, sizeof(pstd));
    memset(&pcus, 0, sizeof(pcus));
    find_proxy(id_std, &pstd);
    find_proxy(id_custom, &pcus);
    HWND proxy_std = pstd.hwnd, proxy_custom = pcus.hwnd;
    CHECK(proxy_std && proxy_custom && proxy_std != proxy_custom);
    CHECK(proxy_count() == 2);
    {
        AsbRwManagerStatus st;
        asb_rw_manager_status(g_mgr, &st);
        tlog("frame channel: state %u gpu %u", st.frames_state, st.frames_gpu);
        CHECK(st.frames_state == 1u);
        if (!g_cpu) CHECK(st.frames_gpu == 1u);
    }
    for (int k = 0; k < 2; ++k) {
        HWND p = k ? proxy_custom : proxy_std;
        WCHAR title[128];
        LONG ex = GetWindowLongW(p, GWL_EXSTYLE), style = GetWindowLongW(p, GWL_STYLE);
        GetWindowTextW(p, title, 128);
        CHECK(IsWindowVisible(p));
        CHECK(GetWindow(p, GW_OWNER) == NULL);                 /* taskbar + Alt+Tab eligible */
        CHECK((ex & WS_EX_APPWINDOW) && !(ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_NOACTIVATE));
        CHECK((style & WS_CAPTION) == WS_CAPTION && (style & WS_SYSMENU) && (style & WS_THICKFRAME));
        CHECK(!wcscmp(title, k ? L"eh6.6 custom" : L"eh6.6 standard"));
        CHECK(wait_for(c_icon, p, 4000));
        CHECK(proxy_aumid_ok(p));
        /* sources disappeared from the guest shell (owned by the helper, no APPWINDOW) */
        HWND src = k ? g_src_custom : g_src_std;
        CHECK(GetWindow(src, GW_OWNER) == g_owner);
        CHECK(!IsIconic(src) && IsWindowVisible(src));
    }

    /* ---- no double non-client frame ---- */
    for (int k = 0; k < 2; ++k) {
        HWND p = k ? proxy_custom : proxy_std, src = k ? g_src_custom : g_src_std;
        RECT pvis, pc, svis;
        visible_frame(p, &pvis);
        client_screen(p, &pc);
        visible_frame(src, &svis);
        { RECT wr; GetWindowRect(p, &wr); tlog("proxy %d: window (%ld,%ld)-(%ld,%ld) style %08lx", k, wr.left, wr.top, wr.right, wr.bottom, (unsigned long)GetWindowLongW(p, GWL_STYLE)); }
        tlog("proxy %d: visible (%ld,%ld)-(%ld,%ld) client (%ld,%ld)-(%ld,%ld); source visible %dx%d", k, pvis.left, pvis.top,
             pvis.right, pvis.bottom, pc.left, pc.top, pc.right, pc.bottom, rw_(&svis), rh_(&svis));
        CHECK(pc.top == pvis.top);                             /* no host caption above the guest content */
        CHECK(rw_(&pc) == rw_(&svis) && rh_(&pc) == rh_(&svis));
        /* DWM keeps its border strip (2 px at 144 DPI) inside the side/bottom margin; the proxy
           makes it transparent, so the guest frame's own border is the only one drawn. */
        int strip = MulDiv(2, (int)GetDpiForWindow(p), 144) + 1;
        CHECK(pvis.left >= pc.left - strip && pvis.right <= pc.right + strip && pvis.bottom <= pc.bottom + strip);
        DWORD thickness = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(p, 37 /* DWMWA_VISIBLE_FRAME_BORDER_THICKNESS */, &thickness, sizeof(thickness))))
            tlog("proxy %d: DWM border thickness %lu (drawn transparent)", k, thickness);
    }
    {
        POINT off = client_offset(g_src_std);
        uint32_t marker = proxy_pixel_expect(proxy_std, off.x + 18, off.y + 18, 0xFF, 0x00, 0xFF, "proxy-standard");
        tlog("standard proxy marker pixel %08x (guest client offset %ld,%ld)", marker, off.x, off.y);
        {
            AsbRwProxyInfo di;
            RECT dr;
            find_proxy(id_std, &di);
            GetWindowRect(proxy_std, &dr);
            tlog("  standard proxy: presented %llu content %ux%u visible %d iconic %d rect (%ld,%ld)-(%ld,%ld)",
                 (unsigned long long)di.presented, di.content_w, di.content_h, IsWindowVisible(proxy_std), IsIconic(proxy_std),
                 dr.left, dr.top, dr.right, dr.bottom);
        }
        CHECK(near_rgb(marker, 0xFF, 0x00, 0xFF));
        POINT coff = client_offset(g_src_custom);
        uint32_t band = proxy_pixel_expect(proxy_custom, coff.x + 60, coff.y + 16, 0x10, 0x30, 0x90, "proxy-custom");
        uint32_t body = proxy_pixel_expect(proxy_custom, coff.x + 60, coff.y + 120, 0xE0, 0xC0, 0x20, NULL);
        tlog("custom proxy title band %08x body %08x", band, body);
        CHECK(near_rgb(band, 0x10, 0x30, 0x90));               /* the guest's own title bar is the top */
        CHECK(near_rgb(body, 0xE0, 0xC0, 0x20));
    }

    /* ---- guest hit testing (custom chrome, standard caption, Snap Layouts) ---- */
    {
        POINT o = client_offset(g_src_custom);
        RECT c;
        GetClientRect(proxy_custom, &c);
        int right = o.x + (rw_(&c) - 2 * o.x);   /* guest client right edge in proxy space */
        LRESULT h1 = hit_settled(proxy_custom, o.x + 80, o.y + 16, HTCAPTION);
        LRESULT h2 = hit_settled(proxy_custom, right - 20, o.y + 16, HTCLOSE);
        LRESULT h3 = hit_settled(proxy_custom, right - 46 - 20, o.y + 16, HTMAXBUTTON);
        LRESULT h4 = hit_settled(proxy_custom, right - 92 - 20, o.y + 16, HTMINBUTTON);
        LRESULT h5 = hit_settled(proxy_custom, o.x + 80, o.y + 1, HTTOP);
        LRESULT h6 = hit_settled(proxy_custom, o.x + 80, o.y + 150, HTCLIENT);
        tlog("custom hits: caption %ld close %ld max %ld min %ld top %ld body %ld", (long)h1, (long)h2, (long)h3, (long)h4,
             (long)h5, (long)h6);
        CHECK(h1 == HTCAPTION && h2 == HTCLOSE && h3 == HTMAXBUTTON && h4 == HTMINBUTTON && h5 == HTTOP && h6 == HTCLIENT);
        RECT vis;
        visible_frame(g_src_std, &vis);
        POINT so = client_offset(g_src_std);
        /* Find the application's own maximize button, then ask the proxy at the same place. */
        int mx = -1, my = so.y / 2;
        for (int x = rw_(&vis) - 2; x > rw_(&vis) / 2 && mx < 0; x -= 2) {
            LRESULT own = SendMessageW(g_src_std, WM_NCHITTEST, 0, MAKELPARAM((WORD)(SHORT)(vis.left + x), (WORD)(SHORT)(vis.top + my)));
            if (own == HTMAXBUTTON) mx = x - 6;
        }
        CHECK(mx > 0);
        if (mx > 0) {
            LRESULT hm = hit_settled(proxy_std, mx, my, HTMAXBUTTON);
            tlog("standard max button at (%d,%d) -> %ld", mx, my, (long)hm);
            CHECK(hm == HTMAXBUTTON);                          /* Snap Layouts flyout on hover */
        }
        LRESULT hc = hit_settled(proxy_std, 60, so.y / 2, HTCAPTION);
        CHECK(hc == HTCAPTION);
        CHECK(hit_settled(proxy_std, so.x + 60, so.y + 60, HTCLIENT) == HTCLIENT);
        LRESULT edge = hit(proxy_std, -3, 100);              /* host invisible border: native resize */
        CHECK(edge == HTLEFT);
    }

    /* ---- acknowledged resize, application minimum ---- */
    {
        RECT c;
        SizeWait sw = { g_src_std, proxy_std, 900, 600 };
        GetClientRect(proxy_std, &c);
        RECT wr;
        GetWindowRect(proxy_std, &wr);
        SetWindowPos(proxy_std, NULL, 0, 0, rw_(&wr) + (900 - rw_(&c)), rh_(&wr) + (600 - rh_(&c)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CHECK(wait_for(c_source_size, &sw, 4000));
        ProxyWait pw = { id_std, 0 };
        AsbRwProxyInfo info;
        Sleep(400);
        CHECK(wait_for(c_proxy_drawn, &pw, 3000) && find_proxy(id_std, &info) && info.content_w == 900 && info.content_h == 600);
        /* Below the application's minimum: the guest applies its minimum and the proxy adopts it. */
        GetWindowRect(proxy_std, &wr);
        GetClientRect(proxy_std, &c);
        SetWindowPos(proxy_std, NULL, 0, 0, rw_(&wr) - rw_(&c) + 200, rh_(&wr) - rh_(&c) + 150, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(300);
        CHECK(wait_for(c_proxy_matches_source, &sw, 4000));
        RECT svis;
        visible_frame(g_src_std, &svis);
        GetClientRect(proxy_std, &c);
        tlog("minimum: proxy client %dx%d, source visible %dx%d", rw_(&c), rh_(&c), rw_(&svis), rh_(&svis));
        CHECK(rw_(&c) > 200 && rh_(&c) > 150);
        MINMAXINFO mmi;
        memset(&mmi, 0, sizeof(mmi));
        SendMessageW(proxy_std, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
        GetWindowRect(proxy_std, &wr);
        tlog("proxy min track %ldx%ld (window %dx%d)", mmi.ptMinTrackSize.x, mmi.ptMinTrackSize.y, rw_(&wr), rh_(&wr));
        CHECK(mmi.ptMinTrackSize.x == rw_(&wr) && mmi.ptMinTrackSize.y == rh_(&wr));   /* the app's minimum, mirrored */
        /* back to a comfortable size */
        SetWindowPos(proxy_std, NULL, 0, 0, rw_(&wr) - rw_(&c) + 700, rh_(&wr) - rh_(&c) + 460, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SizeWait back = { g_src_std, proxy_std, 700, 460 };
        CHECK(wait_for(c_source_size, &back, 4000));
    }

    /* ---- host maximize / restore / minimize ---- */
    {
        RECT before;
        visible_frame(g_src_std, &before);
        ShowWindow(proxy_std, SW_MAXIMIZE);
        SizeWait sw = { g_src_std, proxy_std, 0, 0 };
        CHECK(IsZoomed(proxy_std));
        CHECK(wait_for(c_proxy_matches_source, &sw, 5000));
        CHECK(!IsZoomed(g_src_std) && !IsIconic(g_src_std));   /* source sized, never maximized */
        RECT c, pvis, work;
        client_screen(proxy_std, &c);
        visible_frame(proxy_std, &pvis);
        MONITORINFO pm = { sizeof(pm) };
        GetMonitorInfoW(MonitorFromWindow(proxy_std, MONITOR_DEFAULTTONEAREST), &pm);
        work = pm.rcWork;
        tlog("maximized client (%ld,%ld)-(%ld,%ld) work (%ld,%ld)-(%ld,%ld)", c.left, c.top, c.right, c.bottom, work.left,
             work.top, work.right, work.bottom);
        CHECK(EqualRect(&c, &work));                         /* fills the work area, no caption */
        ShowWindow(proxy_std, SW_RESTORE);
        SizeWait restored = { g_src_std, proxy_std, rw_(&before), rh_(&before) };
        CHECK(wait_for(c_source_size, &restored, 4000));
        Sleep(400);
        visible_frame(g_src_std, &before);
        ShowWindow(proxy_std, SW_MINIMIZE);
        Sleep(600);
        RECT after;
        visible_frame(g_src_std, &after);
        CHECK(IsIconic(proxy_std) && !IsIconic(g_src_std) && IsWindowVisible(g_src_std));
        CHECK(EqualRect(&after, &before));                   /* host minimization leaves the source alone */
        ShowWindow(proxy_std, SW_RESTORE);
        CHECK(wait_for(c_not_zoomed, proxy_std, 3000));
    }

    /* ---- application-initiated maximize/minimize replayed on the proxy ---- */
    {
        PostMessageW(g_src_std, WM_APP + 7, 0, 0);
        CHECK(wait_for(c_zoomed, proxy_std, 4000));
        Sleep(300);
        CHECK(!IsZoomed(g_src_std));
        PostMessageW(g_src_std, WM_APP + 7, 0, 0);           /* maximize again = restore (toggle) */
        CHECK(wait_for(c_not_zoomed, proxy_std, 4000));
        PostMessageW(g_src_std, WM_APP + 8, 0, 0);
        CHECK(wait_for(c_iconic, proxy_std, 4000));
        Sleep(300);
        CHECK(!IsIconic(g_src_std));
        ShowWindow(proxy_std, SW_RESTORE);
        CHECK(wait_for(c_not_zoomed, proxy_std, 3000));
    }

    /* ---- Snap (opt-in: needs foreground input) ---- */
    if (g_snap) {
        DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
        AttachThreadInput(GetCurrentThreadId(), fg_thread, TRUE);
        SetForegroundWindow(proxy_std);
        AttachThreadInput(GetCurrentThreadId(), fg_thread, FALSE);
        Sleep(300);
        if (GetForegroundWindow() == proxy_std) {
            send_keys_win_left();
            Sleep(1200);
            RECT pvis, work;
            visible_frame(proxy_std, &pvis);
            MONITORINFO pm = { sizeof(pm) };
            GetMonitorInfoW(MonitorFromWindow(proxy_std, MONITOR_DEFAULTTONEAREST), &pm);
            work = pm.rcWork;
            keybd_event(VK_ESCAPE, 0, 0, 0);                  /* dismiss Snap Assist */
            keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            tlog("snap: proxy visible (%ld,%ld)-(%ld,%ld) work (%ld,%ld)-(%ld,%ld)", pvis.left, pvis.top, pvis.right, pvis.bottom,
                 work.left, work.top, work.right, work.bottom);
            CHECK(abs(pvis.left - work.left) <= 8 && abs(pvis.top - work.top) <= 8 && abs(pvis.bottom - work.bottom) <= 8 &&
                  abs(rw_(&pvis) - rw_(&work) / 2) <= 16);
            SizeWait sw = { g_src_std, proxy_std, 0, 0 };
            CHECK(wait_for(c_proxy_matches_source, &sw, 4000));   /* the guest follows the snapped size */
            if (g_evidence) proxy_pixel(proxy_std, 10, 10, "proxy-snapped");
            ShowWindow(proxy_std, SW_RESTORE);
            Sleep(500);
        } else {
            tlog("snap: SKIPPED (could not take the foreground)");
        }
    }

    /* ---- overflowing menu: owned, non-activating popup outside the root ---- */
    {
        PostMessageW(g_src_std, WM_APP + 1, 0, 0);
        SurfWait open = { id_std, ASB_RW_SURFACE_MENU, 1 };
        CHECK(wait_for(c_surface, &open, 5000));
        HWND menu = surface_window(id_std, ASB_RW_SURFACE_MENU);
        RECT mr, pc;
        GetWindowRect(menu, &mr);
        client_screen(proxy_std, &pc);
        tlog("menu popup (%ld,%ld)-(%ld,%ld), root client (%ld,%ld)-(%ld,%ld)", mr.left, mr.top, mr.right, mr.bottom, pc.left,
             pc.top, pc.right, pc.bottom);
        CHECK(menu && GetWindow(menu, GW_OWNER) == proxy_std);
        CHECK(GetWindowLongW(menu, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
        CHECK(mr.left < pc.left);                             /* outside the root's bounds */
        CHECK(IsWindowVisible(menu));
        Sleep(400);
        AsbRwProxyInfo info;
        find_proxy(id_std, &info);
        uint64_t presented = 0;
        for (uint32_t i = 0; i < info.count; ++i) if (info.surfaces[i].hwnd == menu) presented = info.surfaces[i].presented;
        CHECK(presented > 0);
        if (g_evidence) proxy_pixel(proxy_std, 10, 10, "proxy-menu");
        PostMessageW(g_src_std, WM_APP + 2, 0, 0);
        SurfWait closed = { id_std, ASB_RW_SURFACE_MENU, 0 };
        CHECK(wait_for(c_surface, &closed, 5000));
    }

    /* ---- modal dialog: owned framed proxy, root disabled, host close refused, host move ---- */
    {
        PostMessageW(g_src_std, WM_APP + 3, 0, 0);
        SurfWait open = { id_std, ASB_RW_SURFACE_OWNED, 1 };
        CHECK(wait_for(c_surface, &open, 5000));
        HWND dlg = surface_window(id_std, ASB_RW_SURFACE_OWNED);
        CHECK(dlg && GetWindow(dlg, GW_OWNER) == proxy_std);
        CHECK(!(GetWindowLongW(dlg, GWL_EXSTYLE) & WS_EX_APPWINDOW));
        CHECK(wait_for(c_disabled, proxy_std, 4000));          /* system modal rules apply on the host */
        CHECK(IsWindowEnabled(dlg));
        PostMessageW(proxy_std, WM_SYSCOMMAND, SC_CLOSE, 0);
        Sleep(600);
        CHECK(IsWindow(g_src_std) && find_proxy(id_std, NULL));
        HWND src_dlg = FindWindowW(L"AsbEh66Dialog", L"eh6.6 modal");
        RECT before, after, d;
        visible_frame(src_dlg, &before);
        GetWindowRect(dlg, &d);
        SetWindowPos(dlg, NULL, d.left + 40, d.top + 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(700);
        visible_frame(src_dlg, &after);
        tlog("dialog moved on host: guest (%ld,%ld) -> (%ld,%ld)", before.left, before.top, after.left, after.top);
        CHECK(after.left - before.left == 40 && after.top - before.top == 30);
        if (g_evidence) proxy_pixel(proxy_std, 10, 10, "proxy-modal");
        PostMessageW(g_src_std, WM_APP + 4, 0, 0);
        SurfWait closed = { id_std, ASB_RW_SURFACE_OWNED, 0 };
        CHECK(wait_for(c_surface, &closed, 5000));
        CHECK(wait_for(c_enabled, proxy_std, 4000));
    }

    /* ---- unsaved close: prompt, cancellation keeps the window and the proxy ---- */
    {
        PostMessageW(g_src_std, WM_APP + 5, 1, 0);
        Sleep(100);
        PostMessageW(proxy_std, WM_SYSCOMMAND, SC_CLOSE, 0);
        SurfWait prompt = { id_std, ASB_RW_SURFACE_OWNED, 1 };
        CHECK(wait_for(c_surface, &prompt, 5000));
        CHECK(FindWindowW(L"AsbEh66Dialog", L"eh6.6 save changes?") != NULL);
        PostMessageW(g_src_std, WM_APP + 6, 0, 0);            /* the user cancels */
        SurfWait gone = { id_std, ASB_RW_SURFACE_OWNED, 0 };
        CHECK(wait_for(c_surface, &gone, 5000));
        Sleep(500);
        AsbRwProxyInfo info;
        CHECK(IsWindow(g_src_std) && find_proxy(id_std, &info) && info.hwnd == proxy_std && IsWindow(proxy_std));
        PostMessageW(g_src_std, WM_APP + 5, 0, 0);
    }

    /* ---- unselect restores without closing ---- */
    {
        CHECK(asb_rw_manager_select(g_mgr, id_custom, 0) == ASB_RW_OK);
        CHECK(wait_for(c_proxy_gone, &id_custom, 4000));
        RectWait rwt = { g_src_custom, orig_custom };
        CHECK(wait_for(c_rect, &rwt, 4000));
        CHECK(IsWindow(g_src_custom) && GetWindow(g_src_custom, GW_OWNER) == NULL);
        CHECK(!IsWindow(proxy_custom));
    }

    /* ---- real close: the guest window goes, then the proxy ---- */
    {
        PostMessageW(proxy_std, WM_SYSCOMMAND, SC_CLOSE, 0);
        CHECK(wait_for(c_window_gone, g_src_std, 5000));
        CHECK(wait_for(c_proxy_gone, &id_std, 5000));
        CHECK(!IsWindow(proxy_std));
    }

    /* ---- stopping the manager (host app exit) ends sharing and restores ---- */
    {
        CHECK(wait_for(c_listed, (void *)L"eh6.6 custom", 3000));
        id_custom = id_by_title(L"eh6.6 custom");
        CHECK(asb_rw_manager_select(g_mgr, id_custom, 1) == ASB_RW_OK);
        ProxyWait pw = { id_custom, 0 };
        CHECK(wait_for(c_proxy_drawn, &pw, 6000));
        CHECK(GetWindow(g_src_custom, GW_OWNER) == g_owner);
        asb_rw_manager_stop(g_mgr);
        g_mgr = NULL;
        RectWait rwt = { g_src_custom, orig_custom };
        CHECK(wait_for(c_rect, &rwt, 5000));
        CHECK(GetWindow(g_src_custom, GW_OWNER) == NULL && IsWindow(g_src_custom));
        int ours = 0;
        EnumWindows(count_ours, (LPARAM)&ours);
        CHECK(ours == 0);                                     /* no proxy windows left */
    }

    InterlockedExchange(&g_guest_stop, 1);
    WaitForSingleObject(g_guest_thread, 10000);
    asb_rw_capture_stop(g_capture);
    PostMessageW(g_src_custom, WM_APP + 9, 0, 0);
    if (IsWindow(g_src_std)) PostMessageW(g_src_std, WM_APP + 9, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    tlog("%s: %d checks, %d failures", g_failures ? "RESULT FAIL" : "RESULT PASS", g_checks, g_failures);
    if (g_log) fclose(g_log);
    return g_failures ? 1 : 0;
}
