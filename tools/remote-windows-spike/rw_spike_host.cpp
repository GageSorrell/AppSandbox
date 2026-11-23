/**
 *
 *
 * @module AppSandbox/tools/remote-windows-spike/rw_spike_host
 *
 * @file      rw_spike_host.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows feasibility spike (AppSandbox-eh6.1) - host proxy.
 *
 * Runs at MEDIUM integrity on the Windows 11 host (no elevation, no host policy changes).
 * Connects with AF_HYPERV to the running HCS VM (channel 7 service; Everyone connect SDDL),
 * selects one guest root, and shows it as a native top-level host window with taskbar and
 * Alt+Tab presence. Popups (menus/dialogs/tooltips) arrive as separate surfaces and are shown
 * as owned no-activate host windows at their guest offsets - including outside the root.
 * Keyboard/mouse are forwarded; Close is forwarded as SC_CLOSE. The proxy is an OLE drop
 * target (host files -> guest app) and an OLE drag source (guest files -> host apps).
 *
 * usage: rw_spike_host.exe <vm-runtime-guid> [title-substring | list] [hostX,hostY] [tool|owner]
 */
#include "rw_spike_common.h"
#include <windowsx.h>
#include <map>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_APP_SELECTED (WM_APP + 1)
#define WM_APP_SURFACES (WM_APP + 2)
#define WM_APP_FRAME    (WM_APP + 3)
#define WM_APP_DRAGOUT  (WM_APP + 4)
#define WM_APP_GONE     (WM_APP + 5)

static RwConn g_conn;
static HWND g_proxy;
static uint64_t g_root;
static RwRect g_rootRect;               /* guest frame rect of the root (physical px) */
static POINT g_anchorHost;              /* host screen position of the proxy for g_anchorGuest */
static POINT g_anchorGuest;
static std::wstring g_title;

struct HostSurface {
    HWND wnd = nullptr; RwRect rect = {}; uint32_t kind = 0;
    std::vector<uint8_t> px; UINT w = 0, h = 0;
};
static std::mutex g_lock;
static std::map<uint64_t, HostSurface> g_surf;      /* guest hwnd -> surface */
static std::atomic<bool> g_dragProbe;
static RwStager* g_dragStager;
static std::vector<std::wstring> g_dragPaths;

/* ---------------- geometry ---------------- */

static POINT HostFromGuest(int gx, int gy) { return { g_anchorHost.x + gx - g_anchorGuest.x, g_anchorHost.y + gy - g_anchorGuest.y }; }

static void SendMouse(uint32_t kind, uint32_t button, int gx, int gy, int wheel = 0)
{
    RwMouse m = { kind, button, gx, gy, wheel };
    g_conn.Send(RW_MOUSE, &m, sizeof(m));
}

static uint64_t SurfaceOf(HWND w)
{
    std::lock_guard<std::mutex> g(g_lock);
    for (auto& kv : g_surf) if (kv.second.wnd == w) return kv.first;
    return 0;
}

/* ---------------- OLE drop target: host files -> guest ---------------- */

struct ProxyDropTarget : IDropTarget {
    LONG ref = 1; bool files = false;
    STDMETHODIMP QueryInterface(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&ref); }
    STDMETHODIMP DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        files = d->QueryGetData(&fe) == S_OK;
        *eff = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* eff) override { *eff = files ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
    STDMETHODIMP DragLeave() override { return S_OK; }
    STDMETHODIMP Drop(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        *eff = DROPEFFECT_NONE;
        std::vector<std::wstring> paths = RwHdropPaths(d);
        std::vector<RwEntry> entries; std::wstring err;
        bool ok = !paths.empty();
        for (size_t i = 0; ok && i < paths.size(); ++i) {
            const wchar_t* name = wcsrchr(paths[i].c_str(), L'\\');
            ok = RwCollect(paths[i], name ? name + 1 : paths[i], entries, err);
        }
        if (!ok) { RwLog("drop rejected: %ls", err.empty() ? L"no filesystem files" : err.c_str()); return S_OK; }
        int gx = g_anchorGuest.x + pt.x - g_anchorHost.x, gy = g_anchorGuest.y + pt.y - g_anchorHost.y;
        RwLog("host drop: %zu item(s) (%zu entries) at host %ld,%ld -> guest %d,%d; first=%ls", paths.size(), entries.size(), pt.x, pt.y, gx, gy, paths[0].c_str());
        std::thread([entries, gx, gy, n = (uint32_t)paths.size()]() {
            RwDropBegin b = { g_root, gx, gy, n };
            if (g_conn.Send(RW_DROP_BEGIN, &b, sizeof(b)) && RwSendEntries(g_conn, entries)) g_conn.Send(RW_DROP_END);
        }).detach();
        *eff = DROPEFFECT_COPY;   /* copy only: the host source is never asked to delete */
        return S_OK;
    }
} g_dropTarget;

/* ---------------- windows ---------------- */

static void PaintSurface(HWND w)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
    uint64_t id = SurfaceOf(w);
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_surf.find(id);
        if (it != g_surf.end() && !it->second.px.empty()) {
            HostSurface& s = it->second;
            BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = (LONG)s.w; bi.bmiHeader.biHeight = -(LONG)s.h;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, 0, 0, s.w, s.h, 0, 0, 0, s.h, s.px.data(), &bi, DIB_RGB_COLORS);
        }
    }
    EndPaint(w, &ps);
}

static bool g_captured;

static LRESULT HandleMouse(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    uint64_t id = SurfaceOf(w);
    RwRect r;
    { std::lock_guard<std::mutex> g(g_lock); auto it = g_surf.find(id); if (it == g_surf.end()) return 0; r = it->second.rect; }
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    if (m == WM_MOUSEWHEEL) {
        POINT p = { x, y }; ScreenToClient(w, &p); x = p.x; y = p.y;
        SendMouse(RW_MOUSE_WHEEL, 0, r.l + x, r.t + y, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    RECT cr; GetClientRect(w, &cr);
    bool outside = x < 0 || y < 0 || x >= cr.right || y >= cr.bottom;
    if (m == WM_MOUSEMOVE && g_captured && (wp & MK_LBUTTON) && outside && w == g_proxy) {
        if (!g_dragProbe.exchange(true)) {
            RwLog("pointer left proxy with button held: probing guest drag");
            g_conn.Send(RW_DRAGOUT_PROBE);
        }
        return 0;
    }
    if (g_dragProbe) return 0;
    switch (m) {
    case WM_MOUSEMOVE: SendMouse(RW_MOUSE_MOVE, 0, r.l + x, r.t + y); break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        RwLog("mouse down msg=0x%x at surface %llx client %d,%d -> guest %d,%d", m, (unsigned long long)id, x, y, r.l + x, r.t + y);
        SetCapture(w); g_captured = true;
        SendMouse(RW_MOUSE_DOWN, m == WM_LBUTTONDOWN ? 0 : m == WM_RBUTTONDOWN ? 1 : 2, r.l + x, r.t + y); break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        SendMouse(RW_MOUSE_UP, m == WM_LBUTTONUP ? 0 : m == WM_RBUTTONUP ? 1 : 2, r.l + x, r.t + y);
        ReleaseCapture(); g_captured = false; break;
    }
    return 0;
}

static LRESULT CALLBACK SurfaceProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: PaintSurface(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return w == g_proxy ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        return HandleMouse(w, m, wp, lp);
    case WM_CAPTURECHANGED: g_captured = false; return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        if (m == WM_SYSKEYDOWN && wp == VK_F4) break;   /* Alt+F4 -> WM_CLOSE -> forwarded SC_CLOSE */
        RwKey k = { (uint16_t)((lp >> 16) & 0xFF), (uint16_t)(((lp >> 24) & 1) | ((m == WM_KEYUP || m == WM_SYSKEYUP) ? 2 : 0)) };
        /* Synthesized input may flag every key as extended; keep E0 only for keys that have it. */
        switch (wp) {
        case VK_RCONTROL: case VK_CONTROL: case VK_MENU: case VK_RMENU: case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP:
        case VK_DOWN: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_NUMLOCK: case VK_RETURN:
            break;
        default: k.flags &= ~1u;
        }
        if (!k.scan) {   /* synthesized VK-only input carries no scan code: derive it */
            UINT sc = MapVirtualKeyW((UINT)wp, MAPVK_VK_TO_VSC_EX);
            k.scan = (uint16_t)(sc & 0xFF);
            if ((sc & 0xFF00) == 0xE000) k.flags |= 1;
        }
        RwLog("key msg=0x%x vk=0x%llx -> scan=0x%x flags=%u", m, (unsigned long long)wp, k.scan, k.flags);
        g_conn.Send(RW_KEY, &k, sizeof(k));
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && w == g_proxy) { RwHwnd h = { g_root }; g_conn.Send(RW_ACTIVATE, &h, sizeof(h)); }
        break;
    case WM_CLOSE:
        if (w == g_proxy) { RwHwnd h = { g_root }; g_conn.Send(RW_CLOSE, &h, sizeof(h)); RwLog("host Close -> guest SC_CLOSE"); }
        return 0;
    case WM_DESTROY:
        if (w == g_proxy) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static void PlaceSurface(HostSurface& s, uint64_t id)
{
    POINT p = HostFromGuest(s.rect.l, s.rect.t);
    int w = s.rect.r - s.rect.l, h = s.rect.b - s.rect.t;
    if (!s.wnd) {
        s.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"RwSpikeSurface", L"",
                                WS_POPUP, p.x, p.y, w, h, g_proxy, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(s.wnd, SW_SHOWNOACTIVATE);
        RwLog("host popup window for guest %llx at host %ld,%ld %dx%d", (unsigned long long)id, p.x, p.y, w, h);
    } else {
        SetWindowPos(s.wnd, HWND_TOPMOST, p.x, p.y, w, h, SWP_NOACTIVATE);
    }
}

/* ---------------- drag-out: guest files -> host ---------------- */

static void RunDragOut()
{
    std::vector<std::wstring> paths;
    { std::lock_guard<std::mutex> g(g_lock); paths = g_dragPaths; }
    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = E_FAIL;
    if (!paths.empty()) {
        IDataObject* data = nullptr;
        hr = RwShellDataObject(paths, &data);
        if (SUCCEEDED(hr)) {
            RwSetPreferredCopy(data);
            ReleaseCapture();
            RwDropSource* src = new RwDropSource();
            hr = DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
            src->Release(); data->Release();
        }
    }
    RwLog("dragout: host DoDragDrop hr=0x%08lX effect=%lu (%zu item(s), first=%ls)", hr, effect, paths.size(), paths.empty() ? L"" : paths[0].c_str());
    RwStatus s = { (int32_t)effect, (int32_t)hr };
    g_conn.Send(RW_DRAGOUT_DONE, &s, sizeof(s));
    g_dragProbe = false;
}

static LRESULT CALLBACK MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (w != g_proxy) return SurfaceProc(w, m, wp, lp);
    switch (m) {
    case WM_APP_FRAME: {
        HWND target = (HWND)lp;
        if (target) InvalidateRect(target, nullptr, FALSE);
        return 0;
    }
    case WM_APP_SURFACES: {
        std::lock_guard<std::mutex> g(g_lock);
        for (auto it = g_surf.begin(); it != g_surf.end();) {
            HostSurface& s = it->second;
            if (s.kind == RW_SURFACE_ROOT) {
                POINT p = HostFromGuest(g_rootRect.l, g_rootRect.t);
                s.rect = g_rootRect;
                SetWindowPos(g_proxy, nullptr, p.x, p.y, g_rootRect.r - g_rootRect.l, g_rootRect.b - g_rootRect.t, SWP_NOZORDER | SWP_NOACTIVATE);
                ++it;
            } else if (s.kind == 0) {     /* marked gone */
                if (s.wnd) DestroyWindow(s.wnd);
                it = g_surf.erase(it);
            } else {
                PlaceSurface(s, it->first);
                ++it;
            }
        }
        return 0;
    }
    case WM_APP_DRAGOUT: RunDragOut(); return 0;
    case WM_APP_GONE: RwLog("guest root destroyed: closing proxy"); DestroyWindow(g_proxy); return 0;
    }
    return SurfaceProc(w, m, wp, lp);
}

/* ---------------- network ---------------- */

static void Receiver()
{
    RwHdr h; std::vector<uint8_t> p;
    while (g_conn.Recv(h, p)) {
        switch (h.type) {
        case RW_LOG: RwLog("guest: %.*s", (int)p.size(), (const char*)p.data()); break;
        case RW_FRAME: {
            if (p.size() < sizeof(RwFrame)) break;
            RwFrame f = *(RwFrame*)p.data();
            size_t need = (size_t)f.width * f.height * 4;
            if (p.size() < sizeof(RwFrame) + need) break;
            HWND target = nullptr;
            {
                std::lock_guard<std::mutex> g(g_lock);
                HostSurface& s = g_surf[f.hwnd];
                if (!s.kind) { s.kind = f.kind; s.rect = f.rect; }
                s.px.assign(p.begin() + sizeof(RwFrame), p.begin() + sizeof(RwFrame) + need);
                s.w = f.width; s.h = f.height;
                if (f.kind == RW_SURFACE_ROOT) s.wnd = g_proxy;
                target = s.wnd;
                static uint32_t logged;
                if (logged < 3 || (f.seq % 300) == 0) { ++logged; RwLog("frame %llx kind=%u %ux%u seq=%u", (unsigned long long)f.hwnd, f.kind, f.width, f.height, f.seq); }
            }
            if (f.kind == RW_SURFACE_POPUP && !target) PostMessageW(g_proxy, WM_APP_SURFACES, 0, 0);
            PostMessageW(g_proxy, WM_APP_FRAME, 0, (LPARAM)target);
            break;
        }
        case RW_SURFACES: {
            if (p.size() < sizeof(RwSurfaces)) break;
            RwSurfaces s = *(RwSurfaces*)p.data();
            RwSurface* list = (RwSurface*)(p.data() + sizeof(RwSurfaces));
            if (s.root == g_root && s.rootRect.r == 0 && s.rootRect.b == 0 && s.count == 0) { PostMessageW(g_proxy, WM_APP_GONE, 0, 0); break; }
            {
                std::lock_guard<std::mutex> g(g_lock);
                g_rootRect = s.rootRect;
                for (auto& kv : g_surf) if (kv.second.kind == RW_SURFACE_POPUP) {
                    bool keep = false;
                    for (uint32_t i = 0; i < s.count; ++i) if (list[i].hwnd == kv.first) { kv.second.rect = list[i].rect; keep = true; }
                    if (!keep) kv.second.kind = 0;
                }
                for (uint32_t i = 0; i < s.count; ++i) {
                    HostSurface& hs = g_surf[list[i].hwnd];
                    if (!hs.kind) hs.kind = RW_SURFACE_POPUP;
                    hs.rect = list[i].rect;
                }
            }
            PostMessageW(g_proxy, WM_APP_SURFACES, 0, 0);
            break;
        }
        case RW_DROP_RESULT: {
            RwStatus* s = (RwStatus*)p.data();
            RwLog("guest drop result: effect=%d hr=0x%08X", s->status, s->detail);
            break;
        }
        case RW_DRAGOUT_NONE: RwLog("guest reports no file drag"); g_dragProbe = false; break;
        case RW_DRAGOUT_BEGIN: {
            delete g_dragStager; g_dragStager = new RwStager();
            wchar_t base[MAX_PATH]; ExpandEnvironmentStringsW(L"%TEMP%\\AppSandboxRemoteWindowsSpike\\dragout", base, MAX_PATH);
            SHCreateDirectoryExW(nullptr, base, nullptr);
            if (!g_dragStager->Begin(base)) g_dragStager->Fail(L"staging dir");
            RwLog("dragout: receiving %d entries", ((RwStatus*)p.data())->detail);
            break;
        }
        case RW_FILE_ENTRY: if (g_dragStager && p.size() >= sizeof(RwFileEntry)) g_dragStager->Entry(*(RwFileEntry*)p.data()); break;
        case RW_FILE_DATA: if (g_dragStager) g_dragStager->Data(p.data(), p.size()); break;
        case RW_DRAGOUT_READY:
            if (g_dragStager) {
                bool ok = g_dragStager->End();
                RwLog("dragout: staged ok=%d at %ls %ls", ok, g_dragStager->root.c_str(), g_dragStager->error.c_str());
                { std::lock_guard<std::mutex> g(g_lock); g_dragPaths = ok ? g_dragStager->topLevel : std::vector<std::wstring>(); }
                PostMessageW(g_proxy, WM_APP_DRAGOUT, 0, 0);
            }
            break;
        case RW_SHELL_STATE: {
            RwShellState* s = (RwShellState*)p.data();
            RwLog("shell state %llx: visible=%u cloaked=%u toolWindow=%u appWindow=%u taskbarCandidate=%u onPrivate=%u rect=(%d,%d,%d,%d)",
                (unsigned long long)s->hwnd, s->visible, s->cloaked, s->toolWindow, s->appWindow, s->onTaskbarCandidate, s->onPrivate,
                s->rect.l, s->rect.t, s->rect.r, s->rect.b);
            break;
        }
        case RW_UNSELECT: RwLog("guest acknowledged unselect (restored)"); break;
        case RW_RESTORE_ALL: RwLog("guest acknowledged restore-all"); break;
        }
    }
    RwLog("guest connection closed");
    PostMessageW(g_proxy, WM_APP_GONE, 0, 0);
}

/* Operator commands (one per line) are read from %TEMP%\AppSandboxRemoteWindowsSpike\cmd.txt:
   shell, unselect, restore, close, quit. */
static void Console()
{
    wchar_t path[MAX_PATH]; ExpandEnvironmentStringsW(L"%TEMP%\\AppSandboxRemoteWindowsSpike\\cmd.txt", path, MAX_PATH);
    char line[256];
    for (;;) {
        Sleep(200);
        FILE* f = _wfsopen(path, L"r", _SH_DENYNO);
        if (!f) continue;
        std::vector<std::string> lines;
        while (fgets(line, sizeof(line), f)) lines.push_back(line);
        fclose(f); DeleteFileW(path);
        for (auto& l : lines) {
        strcpy_s(line, l.c_str());
        RwLog("operator command: %s", line);
        RwHwnd h = { g_root };
        if (!strncmp(line, "shell", 5)) g_conn.Send(RW_SHELL_STATE, &h, sizeof(h));
        else if (!strncmp(line, "unselect", 8)) g_conn.Send(RW_UNSELECT, &h, sizeof(h));
        else if (!strncmp(line, "restore", 7)) g_conn.Send(RW_RESTORE_ALL);
        else if (!strncmp(line, "close", 5)) g_conn.Send(RW_CLOSE, &h, sizeof(h));
        else if (!strncmp(line, "type ", 5)) {
            /* Real host keyboard input (SendInput -> proxy WM_KEYDOWN -> guest), refused unless the
               proxy is the host foreground window so test text can never reach another host app. */
            if (GetForegroundWindow() != g_proxy) { RwLog("type refused: proxy is not the foreground window"); continue; }
            std::vector<INPUT> in;
            for (const char* c = line + 5; *c && *c != '\r' && *c != '\n'; ++c) {
                SHORT vk = VkKeyScanA(*c);
                if (vk == -1) continue;
                bool shift = (vk >> 8) & 1;
                auto key = [&](WORD v, bool up) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = v;
                    i.ki.wScan = (WORD)MapVirtualKeyW(v, MAPVK_VK_TO_VSC); i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; in.push_back(i); };
                if (shift) key(VK_SHIFT, false);
                key(vk & 0xFF, false); key(vk & 0xFF, true);
                if (shift) key(VK_SHIFT, true);
            }
            for (INPUT& i : in) { if (GetForegroundWindow() != g_proxy) break; SendInput(1, &i, sizeof(i)); Sleep(4); }
        }
        else if (!strncmp(line, "front", 5)) {
            SetWindowPos(g_proxy, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(g_proxy, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        else if (!strncmp(line, "quit", 4)) { shutdown(g_conn.s, SD_BOTH); return; }
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { wprintf(L"usage: rw_spike_host <vm-runtime-guid> [title-substring|list]\n"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    wchar_t logPath[MAX_PATH]; ExpandEnvironmentStringsW(L"%TEMP%\\AppSandboxRemoteWindowsSpike\\host.log", logPath, MAX_PATH);
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, logPath); *wcsrchr(dir, L'\\') = 0; SHCreateDirectoryExW(nullptr, dir, nullptr);
    g_rwLog = _wfsopen(logPath, L"a", _SH_DENYNO);

    HANDLE token = nullptr; TOKEN_ELEVATION e = {}; DWORD n = 0;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n);
    RwLog("rw_spike_host start pid=%lu elevated=%lu", GetCurrentProcessId(), e.TokenIsElevated);

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    RW_SOCKADDR_HV a = {}; a.Family = AF_HYPERV;
    if (!RwParseGuid(argv[1], &a.VmId) || !RwParseGuid(RW_SPIKE_SERVICE_GUID, &a.ServiceId)) { RwLog("bad guid"); return 2; }
    g_conn.s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (g_conn.s == INVALID_SOCKET || connect(g_conn.s, (sockaddr*)&a, sizeof(a))) { RwLog("AF_HYPERV connect failed: %d", WSAGetLastError()); return 1; }
    RwLog("AF_HYPERV connected to VM %ls service %ls", argv[1], RW_SPIKE_SERVICE_GUID);

    RwHdr h; std::vector<uint8_t> p;
    if (!g_conn.Recv(h, p) || h.type != RW_HELLO) { RwLog("no hello"); return 1; }
    RwHello hello = *(RwHello*)p.data();
    RwLog("guest hello v%u private=(%d,%d,%d,%d) dpi=%u desktop=(%d,%d,%d,%d)", hello.version, hello.privateDisplay.l, hello.privateDisplay.t,
        hello.privateDisplay.r, hello.privateDisplay.b, hello.privateDpi, hello.desktop.l, hello.desktop.t, hello.desktop.r, hello.desktop.b);

    g_conn.Send(RW_LIST);
    RwListEntry pick = {};
    for (;;) {
        if (!g_conn.Recv(h, p)) return 1;
        if (h.type == RW_LOG) { RwLog("guest: %.*s", (int)p.size(), (const char*)p.data()); continue; }
        if (h.type != RW_LIST) continue;
        uint32_t count = ((RwListReply*)p.data())->count;
        RwListEntry* list = (RwListEntry*)(p.data() + sizeof(RwListReply));
        for (uint32_t i = 0; i < count; ++i) {
            RwLog("  eligible root %llx pid=%u exe=%ls class=%ls title=%ls", (unsigned long long)list[i].hwnd, list[i].pid, list[i].exe, list[i].cls, list[i].title);
            if (!pick.hwnd && argc > 2 && wcscmp(argv[2], L"list") && wcsstr(list[i].title, argv[2])) pick = list[i];
        }
        break;
    }
    if (!pick.hwnd) { RwLog("no root selected"); return 0; }

    RwSelect sel = { pick.hwnd, (argc > 4 && !wcscmp(argv[4], L"owner")) ? RW_SUPPRESS_OWNER : RW_SUPPRESS_TOOLWINDOW };
    RwLog("selecting %llx with suppression mode %u", (unsigned long long)sel.hwnd, sel.mode);
    g_conn.Send(RW_SELECT, &sel, sizeof(sel));
    RwSelectReply reply = {};
    for (;;) {
        if (!g_conn.Recv(h, p)) return 1;
        if (h.type == RW_LOG) { RwLog("guest: %.*s", (int)p.size(), (const char*)p.data()); continue; }
        if (h.type == RW_SELECT) { reply = *(RwSelectReply*)p.data(); break; }
    }
    RwLog("selected %llx status=%d frame=(%d,%d,%d,%d) dpi=%u title=%ls", (unsigned long long)reply.hwnd, reply.status,
        reply.frame.l, reply.frame.t, reply.frame.r, reply.frame.b, reply.dpi, reply.title);
    if (reply.status) return 1;
    g_root = reply.hwnd; g_rootRect = reply.frame; g_title = std::wstring(reply.title) + L" (Main)";

    WNDCLASSW c = {}; c.lpfnWndProc = MainProc; c.hInstance = GetModuleHandleW(nullptr); c.lpszClassName = L"RwSpikeProxy";
    c.hCursor = LoadCursor(nullptr, IDC_ARROW); c.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&c);
    WNDCLASSW sc = c; sc.lpfnWndProc = SurfaceProc; sc.lpszClassName = L"RwSpikeSurface";
    RegisterClassW(&sc);
    g_anchorHost = { 120, 120 };
    if (argc > 3) swscanf_s(argv[3], L"%ld,%ld", &g_anchorHost.x, &g_anchorHost.y);
    g_anchorGuest = { g_rootRect.l, g_rootRect.t };
    g_proxy = CreateWindowExW(WS_EX_APPWINDOW, L"RwSpikeProxy", g_title.c_str(), WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                              g_anchorHost.x, g_anchorHost.y, g_rootRect.r - g_rootRect.l, g_rootRect.b - g_rootRect.t,
                              nullptr, nullptr, c.hInstance, nullptr);
    {
        std::lock_guard<std::mutex> g(g_lock);
        HostSurface& s = g_surf[g_root]; s.kind = RW_SURFACE_ROOT; s.rect = g_rootRect; s.wnd = g_proxy;
    }
    RegisterDragDrop(g_proxy, &g_dropTarget);
    ShowWindow(g_proxy, SW_SHOW);
    SetForegroundWindow(g_proxy);
    RwLog("host proxy window %p created at %ld,%ld", (void*)g_proxy, g_anchorHost.x, g_anchorHost.y);

    std::thread rx(Receiver); rx.detach();
    std::thread con(Console); con.detach();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    RevokeDragDrop(g_proxy);
    RwLog("host exit");
    return 0;
}
