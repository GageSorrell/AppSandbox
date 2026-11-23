/**
 *
 *
 * @module AppSandbox/tools/remote-windows-spike/rw_spike_guest
 *
 * @file      rw_spike_guest.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows feasibility spike (AppSandbox-eh6.1) - guest helper.
 *
 * Runs as the signed-in standard user (medium integrity) in the active console session.
 * - Listens on AF_HYPERV (AppSandbox channel 7, wildcard VM) for one host proxy.
 * - Parks selected roots on the private AppSandboxVDD display (PnP ASB0002), never minimizing
 *   or hiding them, and suppresses their guest taskbar/Alt+Tab presence through reversible
 *   extended-style changes plus ITaskbarList::DeleteTab. Original placement/style are journaled
 *   first; restoration reverts only the bits this helper changed, after re-validating identity.
 * - Captures each root and each same-process popup on the private display separately with
 *   Windows Graphics Capture (CPU BGRA readback, latest-frame-only per surface).
 * - Injects host keyboard/mouse with SendInput (scan codes / virtual-desktop absolute mouse).
 * - Native OLE drag bridges: host->guest stages files and runs a genuine DoDragDrop to the
 *   target; guest->host catches the guest app's own drag on an IDropTarget catcher window on
 *   the private display, ships the CF_HDROP files, then ends the guest drag with no effect.
 */
#include "rw_spike_common.h"   /* winsock2.h before windows.h */
#include <unknwn.h>           /* before C++/WinRT: classic COM interop */
#include <dwmapi.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <shobjidl.h>
#include <map>
#include <set>
#include <thread>
#include <condition_variable>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

#define WM_APP_SELECT    (WM_APP + 1)
#define WM_APP_UNSELECT  (WM_APP + 2)
#define WM_APP_DROP      (WM_APP + 3)
#define WM_APP_PROBE     (WM_APP + 4)
#define WM_APP_DRAGDONE  (WM_APP + 5)
#define WM_APP_RESTORE   (WM_APP + 6)
#define WM_APP_ACTIVATE  (WM_APP + 7)
#define WM_APP_CLOSE     (WM_APP + 8)
#define WM_APP_SHELL     (WM_APP + 9)
#define TIMER_TRACK      1
#define TIMER_PROBE      2

static RwConn g_conn;
static HWND g_ui, g_catcher;
static RECT g_private, g_desktop;
static UINT g_privateDpi;
static std::wstring g_stateDir;

static void Remote(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    RwLog("%s", buf);
    if (g_conn.s != INVALID_SOCKET) g_conn.Send(RW_LOG, buf, (uint32_t)strlen(buf));
}

/* ---------------- private display discovery ---------------- */

static BOOL CALLBACK FindPrivateMonitor(HMONITOR mon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (mi.dwFlags & MONITORINFOF_PRIMARY) g_desktop = mi.rcMonitor;
    DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(mi.szDevice, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
        if (wcsstr(dd.DeviceID, L"ASB0002")) {
            g_private = mi.rcMonitor;
            UINT x = 96, y = 96;
            typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
            auto fn = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
            if (fn) fn(mon, 0, &x, &y);
            g_privateDpi = x;
        }
    }
    return TRUE;
}

static bool RefreshDisplays()
{
    SetRectEmpty(&g_private);
    EnumDisplayMonitors(nullptr, nullptr, FindPrivateMonitor, 0);
    return !IsRectEmpty(&g_private);
}

/* ---------------- window identity / eligibility ---------------- */

static bool Cloaked(HWND h)
{
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c;
}

static RECT FrameRect(HWND h)
{
    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || IsRectEmpty(&r))
        GetWindowRect(h, &r);
    return r;
}

static uint64_t ProcessStart(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    FILETIME c, e, k, u;
    uint64_t v = GetProcessTimes(p, &c, &e, &k, &u) ? ((uint64_t)c.dwHighDateTime << 32 | c.dwLowDateTime) : 0;
    CloseHandle(p);
    return v;
}

/* Elevated (or unqueryable) processes are ineligible: a standard-user helper must not drive them. */
static bool Elevated(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), t = nullptr;
    bool elevated = true;
    if (p && OpenProcessToken(p, TOKEN_QUERY, &t)) {
        TOKEN_ELEVATION e = {}; DWORD n = 0;
        if (GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n)) elevated = e.TokenIsElevated != 0;
        CloseHandle(t);
    }
    if (p) CloseHandle(p);
    return elevated;
}

static std::wstring ExeName(DWORD pid)
{
    wchar_t path[MAX_PATH] = L""; DWORD n = MAX_PATH;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) { QueryFullProcessImageNameW(p, 0, path, &n); CloseHandle(p); }
    const wchar_t* s = wcsrchr(path, L'\\');
    return s ? s + 1 : path;
}

static bool EligibleRoot(HWND h)
{
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || Cloaked(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE), st = GetWindowLongPtrW(h, GWL_STYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (!(ex & WS_EX_APPWINDOW) && !(st & WS_CAPTION)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"WorkerW")) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    DWORD session = 0, mine = 0;
    ProcessIdToSessionId(pid, &session); ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    return session == mine && !Elevated(pid);
}

/* ---------------- journal ---------------- */

struct Journal {
    HWND hwnd; DWORD pid; uint64_t start;
    WINDOWPLACEMENT wp; LONG_PTR exOriginal;
    bool addedTool, removedApp, deletedTab;
    uint32_t mode; HWND ownerOriginal; bool setOwner;
    RwRect lastRoot; std::vector<RwSurface> lastSurfaces;
};
static std::map<HWND, Journal> g_selected;

static void JournalWrite(const char* what, const Journal& j)
{
    std::wstring path = g_stateDir + L"\\journal.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d %s hwnd=%p pid=%lu start=%llu show=%u normal=(%ld,%ld,%ld,%ld) ex=0x%llx mode=%u addedTool=%d removedApp=%d setOwner=%d owner=%p deletedTab=%d\n",
        st.wHour, st.wMinute, st.wSecond, what, (void*)j.hwnd, j.pid, (unsigned long long)j.start, j.wp.showCmd,
        j.wp.rcNormalPosition.left, j.wp.rcNormalPosition.top, j.wp.rcNormalPosition.right, j.wp.rcNormalPosition.bottom,
        (unsigned long long)j.exOriginal, j.mode, j.addedTool, j.removedApp, j.setOwner, (void*)j.ownerOriginal, j.deletedTab);
    fclose(f);
}

static bool SameIdentity(const Journal& j)
{
    DWORD pid = 0;
    return IsWindow(j.hwnd) && GetWindowThreadProcessId(j.hwnd, &pid) && pid == j.pid && ProcessStart(pid) == j.start;
}

static ITaskbarList* Taskbar()
{
    static ITaskbarList* tb;
    if (!tb && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tb))))
        tb->HrInit();
    return tb;
}

/* ---------------- capture ---------------- */

static com_ptr<ID3D11Device> g_d3d;
static com_ptr<ID3D11DeviceContext> g_ctx;
static std::mutex g_d3dLock;
static IDirect3DDevice g_rtDevice{ nullptr };

struct Surface {
    HWND hwnd, root; uint32_t kind;
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    com_ptr<ID3D11Texture2D> staging;
    UINT sw = 0, sh = 0;
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    uint32_t seq = 0;
    /* latest-frame-only mailbox (bounded: one frame per surface) */
    std::vector<uint8_t> pixels; UINT pw = 0, ph = 0; bool pending = false;
};
static std::mutex g_surfLock;
static std::map<HWND, Surface*> g_surfaces;
static std::condition_variable g_frameCv;
static bool g_quit;

static void OnFrame(Surface* s, Direct3D11CaptureFramePool const& pool)
{
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    auto size = frame.ContentSize();
    if (size.Width != s->poolSize.Width || size.Height != s->poolSize.Height) {
        s->poolSize = size;
        pool.Recreate(g_rtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    }
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(tex.put())))) return;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    UINT w = std::min<UINT>((UINT)size.Width, d.Width), h = std::min<UINT>((UINT)size.Height, d.Height);
    if (!w || !h) return;
    std::lock_guard<std::mutex> g(g_d3dLock);
    if (!s->staging || s->sw != w || s->sh != h) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s->staging = nullptr;
        if (FAILED(g_d3d->CreateTexture2D(&sd, nullptr, s->staging.put()))) return;
        s->sw = w; s->sh = h;
    }
    D3D11_BOX box = { 0, 0, 0, w, h, 1 };
    g_ctx->CopySubresourceRegion(s->staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(s->staging.get(), 0, D3D11_MAP_READ, 0, &m))) return;
    {
        std::lock_guard<std::mutex> sg(g_surfLock);
        s->pixels.resize((size_t)w * h * 4);
        for (UINT y = 0; y < h; ++y) memcpy(&s->pixels[(size_t)y * w * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        s->pw = w; s->ph = h; s->pending = true; s->seq++;
    }
    g_ctx->Unmap(s->staging.get(), 0);
    g_frameCv.notify_one();
}

static HRESULT StartCapture(HWND hwnd, HWND root, uint32_t kind)
{
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item));
    if (FAILED(hr) || !item) return FAILED(hr) ? hr : E_FAIL;
    Surface* s = new Surface();
    s->hwnd = hwnd; s->root = root; s->kind = kind; s->item = item;
    s->poolSize = item.Size();
    try {
        s->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g_rtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, s->poolSize);
        s->session = s->pool.CreateCaptureSession(item);
        s->session.IsCursorCaptureEnabled(false);
        try { s->session.IsBorderRequired(false); } catch (...) { Remote("capture: IsBorderRequired(false) unavailable"); }
        s->pool.FrameArrived([s](Direct3D11CaptureFramePool const& p, auto&&) { OnFrame(s, p); });
        s->session.StartCapture();
    } catch (hresult_error const& e) {
        hr = e.code();
        delete s;
        return hr;
    }
    std::lock_guard<std::mutex> g(g_surfLock);
    g_surfaces[hwnd] = s;
    return S_OK;
}

static void StopCapture(HWND hwnd)
{
    Surface* s = nullptr;
    {
        std::lock_guard<std::mutex> g(g_surfLock);
        auto it = g_surfaces.find(hwnd);
        if (it == g_surfaces.end()) return;
        s = it->second; g_surfaces.erase(it);
    }
    try { s->session.Close(); s->pool.Close(); } catch (...) {}
    std::lock_guard<std::mutex> g(g_d3dLock);   /* no OnFrame still using s->staging */
    delete s;
}

static void FrameSender()
{
    std::vector<uint8_t> px;
    while (!g_quit) {
        RwFrame f = {};
        {
            std::unique_lock<std::mutex> g(g_surfLock);
            g_frameCv.wait_for(g, std::chrono::milliseconds(200));
            Surface* pick = nullptr;
            for (auto& kv : g_surfaces) if (kv.second->pending) { pick = kv.second; break; }
            if (!pick) continue;
            pick->pending = false;
            px.swap(pick->pixels);
            RECT r = FrameRect(pick->hwnd);
            f = { (uint64_t)pick->root, (uint64_t)pick->hwnd, { r.left, r.top, r.right, r.bottom }, pick->pw, pick->ph, pick->kind, pick->seq };
        }
        if (!g_conn.Send(RW_FRAME, &f, sizeof(f), px.data(), (uint32_t)px.size())) return;
        Sleep(8);   /* ~cap send rate; superseded frames are dropped by the mailbox */
    }
}

/* ---------------- select / restore ---------------- */

static void SendSurfaces(Journal& j, bool force)
{
    RECT r = FrameRect(j.hwnd);
    RwRect rr = { r.left, r.top, r.right, r.bottom };
    std::vector<RwSurface> list;
    {
        std::lock_guard<std::mutex> g(g_surfLock);
        for (auto& kv : g_surfaces)
            if (kv.second->root == j.hwnd && kv.second->kind == RW_SURFACE_POPUP) {
                RECT pr = FrameRect(kv.first);
                list.push_back({ (uint64_t)kv.first, { pr.left, pr.top, pr.right, pr.bottom }, RW_SURFACE_POPUP });
            }
    }
    bool same = !force && list.size() == j.lastSurfaces.size() && !memcmp(&rr, &j.lastRoot, sizeof(rr)) &&
        (list.empty() || !memcmp(list.data(), j.lastSurfaces.data(), list.size() * sizeof(RwSurface)));
    if (same) return;
    j.lastRoot = rr; j.lastSurfaces = list;
    RwSurfaces h = { (uint64_t)j.hwnd, rr, (uint32_t)list.size() };
    g_conn.Send(RW_SURFACES, &h, sizeof(h), list.data(), (uint32_t)(list.size() * sizeof(RwSurface)));
}

static HWND g_owner;   /* hidden, never-shown owner for RW_SUPPRESS_OWNER */

static int Select(HWND h, uint32_t mode, RwSelectReply& reply)
{
    reply.hwnd = (uint64_t)h;
    if (g_selected.count(h)) return 0;
    if (!IsWindow(h) || !EligibleRoot(h)) return -1;
    if (!RefreshDisplays()) return -2;
    Journal j = {};
    j.hwnd = h; GetWindowThreadProcessId(h, &j.pid); j.start = ProcessStart(j.pid);
    j.wp.length = sizeof(j.wp); GetWindowPlacement(h, &j.wp);
    j.exOriginal = GetWindowLongPtrW(h, GWL_EXSTYLE);
    j.mode = mode;
    j.ownerOriginal = GetWindow(h, GW_OWNER);
    JournalWrite("journal", j);   /* journal BEFORE modification */

    /* Park on the private display: normal show state, same size, never minimized or hidden. */
    if (IsIconic(h) || IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    RECT fr; GetWindowRect(h, &fr);
    SetWindowPos(h, nullptr, g_private.left + 64, g_private.top + 64, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    /* Shell suppression: tool-window bit (taskbar + Alt+Tab exclusion) and removal of the tab. */
    LONG_PTR ex = j.exOriginal;
    j.removedApp = (ex & WS_EX_APPWINDOW) != 0;
    if (mode == RW_SUPPRESS_OWNER) {
        /* An owned, non-APPWINDOW window has no taskbar button or Alt+Tab entry; the frame is untouched.
           (Cross-process ownership implicitly attaches input queues - evaluated by this spike.) */
        if (j.removedApp) SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_APPWINDOW);
        SetLastError(0);
        SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)g_owner);
        j.setOwner = GetWindow(h, GW_OWNER) == g_owner;
    } else {
        j.addedTool = !(ex & WS_EX_TOOLWINDOW);
        SetWindowLongPtrW(h, GWL_EXSTYLE, (ex | WS_EX_TOOLWINDOW) & ~(LONG_PTR)WS_EX_APPWINDOW);
    }
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (ITaskbarList* tb = Taskbar()) j.deletedTab = SUCCEEDED(tb->DeleteTab(h));
    JournalWrite("modified", j);
    g_selected[h] = j;

    HRESULT hr = StartCapture(h, h, RW_SURFACE_ROOT);
    Remote("select hwnd=%p pid=%lu capture hr=0x%08lX parked at %ld,%ld", (void*)h, j.pid, hr, g_private.left + 64, g_private.top + 64);
    RECT r = FrameRect(h);
    reply.frame = { r.left, r.top, r.right, r.bottom };
    reply.dpi = GetDpiForWindow(h);
    GetWindowTextW(h, reply.title, 128);
    SendSurfaces(g_selected[h], true);
    return SUCCEEDED(hr) ? 0 : (int)hr;
}

static void Restore(HWND h, bool alive)
{
    auto it = g_selected.find(h);
    if (it == g_selected.end()) return;
    Journal j = it->second;
    g_selected.erase(it);
    std::vector<HWND> mine;
    {
        std::lock_guard<std::mutex> g(g_surfLock);
        for (auto& kv : g_surfaces) if (kv.second->root == h) mine.push_back(kv.first);
    }
    for (HWND s : mine) StopCapture(s);
    if (!alive || !SameIdentity(j)) { JournalWrite("gone-or-reused (no restore)", j); return; }
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (j.addedTool) ex &= ~(LONG_PTR)WS_EX_TOOLWINDOW;
    if (j.removedApp) ex |= WS_EX_APPWINDOW;
    if (j.setOwner && GetWindow(h, GW_OWNER) == g_owner) SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)j.ownerOriginal);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    WINDOWPLACEMENT wp = j.wp;
    if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_SHOWMINNOACTIVE;
    /* Mixed DPI: the first placement moves the window back to its original monitor, where it gets
       WM_DPICHANGED and rescales; the second re-applies the journaled rectangle at that DPI so the
       size does not drift on every share/unshare cycle. */
    SetWindowPlacement(h, &wp);
    SetWindowPlacement(h, &wp);
    if (j.deletedTab) if (ITaskbarList* tb = Taskbar()) tb->AddTab(h);
    JournalWrite("restored", j);
    Remote("restored hwnd=%p", (void*)h);
}

static void RestoreAll()
{
    while (!g_selected.empty()) Restore(g_selected.begin()->first, true);
}

/* ---------------- popup tracking ---------------- */

struct PopupScan { HWND root; DWORD pid; std::vector<HWND> found; };
static BOOL CALLBACK ScanPopup(HWND h, LPARAM lp)
{
    PopupScan* s = (PopupScan*)lp;
    if (h == s->root || g_selected.count(h) || h == g_catcher || !IsWindowVisible(h) || Cloaked(h)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid) return TRUE;
    RECT r = FrameRect(h), x;
    if (IsRectEmpty(&r) || !IntersectRect(&x, &r, &g_private)) return TRUE;
    s->found.push_back(h);
    return TRUE;
}

static void Track()
{
    std::vector<HWND> roots;
    for (auto& kv : g_selected) roots.push_back(kv.first);
    for (HWND root : roots) {
        Journal& j = g_selected[root];
        if (!SameIdentity(j)) {
            RwSurfaces gone = { (uint64_t)root, {}, 0 };
            g_conn.Send(RW_SURFACES, &gone, sizeof(gone));
            Remote("root %p destroyed; capture ended", (void*)root);
            Restore(root, false);
            continue;
        }
        PopupScan scan = { root, j.pid };
        EnumWindows(ScanPopup, (LPARAM)&scan);
        std::set<HWND> want(scan.found.begin(), scan.found.end()), have;
        {
            std::lock_guard<std::mutex> g(g_surfLock);
            for (auto& kv : g_surfaces) if (kv.second->root == root && kv.second->kind == RW_SURFACE_POPUP) have.insert(kv.first);
        }
        for (HWND h : want) if (!have.count(h)) {
            wchar_t cls[64]; GetClassNameW(h, cls, 64);
            RECT r = FrameRect(h), rr = FrameRect(root);
            HRESULT hr = StartCapture(h, root, RW_SURFACE_POPUP);
            Remote("popup %p class=%ls owner=%p rect=(%ld,%ld,%ld,%ld) outsideRoot=%d capture hr=0x%08lX", (void*)h, cls,
                (void*)GetWindow(h, GW_OWNER), r.left, r.top, r.right, r.bottom,
                (r.left < rr.left || r.top < rr.top || r.right > rr.right || r.bottom > rr.bottom), hr);
        }
        for (HWND h : have) if (!want.count(h)) StopCapture(h);
        SendSurfaces(j, false);
    }
}

/* ---------------- input ---------------- */

static void InjectMouse(const RwMouse& m)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in = {}; in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((int64_t)(m.x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = (LONG)(((int64_t)(m.y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    static const DWORD down[3] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
    static const DWORD up[3] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
    /* Host sends LOGICAL buttons (0 = primary). SendInput flags are physical, so honor the guest's
       own swap setting. */
    uint32_t b = m.button;
    if (b < 2 && GetSystemMetrics(SM_SWAPBUTTON)) b ^= 1;
    if (m.kind == RW_MOUSE_DOWN && b < 3) in.mi.dwFlags |= down[b];
    if (m.kind == RW_MOUSE_UP && b < 3) in.mi.dwFlags |= up[b];
    if (m.kind == RW_MOUSE_WHEEL) { in.mi.dwFlags |= MOUSEEVENTF_WHEEL; in.mi.mouseData = (DWORD)m.wheel; }
    SendInput(1, &in, sizeof(in));
}

static void MouseTo(int x, int y, uint32_t kind = RW_MOUSE_MOVE, uint32_t button = 0)
{
    RwMouse m = { kind, button, x, y, 0 };
    InjectMouse(m);
}

static void InjectKey(const RwKey& k)
{
    INPUT in = {}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = k.scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | ((k.flags & 1) ? KEYEVENTF_EXTENDEDKEY : 0) | ((k.flags & 2) ? KEYEVENTF_KEYUP : 0);
    SendInput(1, &in, sizeof(in));
}

/* ---------------- OLE: catcher drop target ---------------- */

static bool g_selfDrag;                 /* host->guest drag in progress (ignore our own data) */
static bool g_probing;                  /* waiting for a guest-originated drag over the catcher */
static IDataObject* g_probeData;
static std::vector<std::wstring> g_probePaths;

struct Catcher : IDropTarget {
    LONG ref = 1;
    STDMETHODIMP QueryInterface(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&ref); }
    STDMETHODIMP DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        if (g_selfDrag) { *eff = DROPEFFECT_NONE; return S_OK; }
        *eff = DROPEFFECT_COPY;
        if (g_probing && !g_probeData) {
            g_probeData = d; d->AddRef();
            g_probePaths = RwHdropPaths(d);
            PostMessageW(g_ui, WM_TIMER, TIMER_PROBE, 0);   /* resolve the probe now */
        }
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* eff) override { *eff = g_selfDrag ? DROPEFFECT_NONE : DROPEFFECT_COPY; return S_OK; }
    STDMETHODIMP DragLeave() override { return S_OK; }
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL, DWORD* eff) override {
        /* The guest drag was handed to the host: end it here with no effect so the guest source
           keeps its files (copy-only semantics). */
        *eff = DROPEFFECT_NONE;
        Remote("dragout: guest drag ended on catcher with DROPEFFECT_NONE (source preserved)");
        return S_OK;
    }
} g_catcherTarget;

static POINT CatcherCenter()
{
    RECT r; GetWindowRect(g_catcher, &r);
    return { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
}

static void PlaceCatcher()
{
    SetWindowPos(g_catcher, HWND_TOPMOST, g_private.right - 96, g_private.bottom - 96, 64, 64, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* host -> guest: run a genuine OLE drag of staged files onto the target point */
struct DropJob { RwStager* stager; int x, y; };

static void RunDropToGuest(DropJob* job)
{
    RwStager* st = job->stager;
    HRESULT hr = E_FAIL; DWORD effect = DROPEFFECT_NONE;
    if (!st->failed && !st->topLevel.empty()) {
        IDataObject* data = nullptr;
        hr = RwShellDataObject(st->topLevel, &data);
        if (SUCCEEDED(hr)) {
            RwSetPreferredCopy(data);
            PlaceCatcher();
            POINT c = CatcherCenter();
            int tx = job->x, ty = job->y;
            MouseTo(c.x, c.y);
            MouseTo(c.x, c.y, RW_MOUSE_DOWN, 0);
            /* OLE's drag loop reads this thread's key state (GetKeyState), which only changes once the
               injected button-down message has been retrieved by this thread - pump until it has. */
            for (int i = 0; i < 100 && !(GetKeyState(VK_LBUTTON) & 0x8000); ++i) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) DispatchMessageW(&msg);
                Sleep(5);
            }
            Remote("drop: thread sees primary button down=%d", (GetKeyState(VK_LBUTTON) & 0x8000) != 0);
            std::thread mover([c, tx, ty]() {
                Sleep(150);
                for (int i = 1; i <= 12; ++i) { MouseTo(c.x + (tx - c.x) * i / 12, c.y + (ty - c.y) * i / 12); Sleep(25); }
                Sleep(350);
                MouseTo(tx, ty, RW_MOUSE_UP, 0);
            });
            g_selfDrag = true;
            RwDropSource* src = new RwDropSource();
            hr = DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
            g_selfDrag = false;
            HWND under = WindowFromPoint({ tx, ty });
            wchar_t cls[64] = L""; GetClassNameW(under, cls, 64);
            Remote("drop: target point %d,%d window=%p class=%ls lastFeedbackEffect=%lu feedbackCalls=%lu", tx, ty, (void*)under, cls,
                   src->lastEffect, src->feedbackCount);
            src->Release();
            mover.join();
            data->Release();
        }
    } else {
        Remote("drop: staging failed: %ls", st->error.c_str());
    }
    Remote("drop: guest DoDragDrop hr=0x%08lX effect=%lu staged=%ls", hr, effect, st->root.c_str());
    RwStatus r = { (int32_t)effect, (int32_t)hr };
    g_conn.Send(RW_DROP_RESULT, &r, sizeof(r));
    delete st; delete job;
}

/* guest -> host: probe, ship files, and end the guest drag after the host drop */
static void BeginProbe()
{
    PlaceCatcher();
    g_probing = true;
    if (g_probeData) { g_probeData->Release(); g_probeData = nullptr; }
    g_probePaths.clear();
    POINT c = CatcherCenter();
    MouseTo(c.x - 8, c.y - 8); Sleep(30); MouseTo(c.x, c.y);
    SetTimer(g_ui, TIMER_PROBE, 1500, nullptr);
}

static void ResolveProbe()
{
    KillTimer(g_ui, TIMER_PROBE);
    if (!g_probing) return;
    g_probing = false;
    std::vector<RwEntry> entries; std::wstring err;
    bool ok = g_probeData && !g_probePaths.empty();
    if (!g_probeData) err = L"no guest OLE drag reached the catcher";
    else if (g_probePaths.empty()) err = L"guest drag has no CF_HDROP filesystem files (virtual/generated data unsupported)";
    for (size_t i = 0; ok && i < g_probePaths.size(); ++i) {
        const wchar_t* name = wcsrchr(g_probePaths[i].c_str(), L'\\');
        ok = RwCollect(g_probePaths[i], name ? name + 1 : g_probePaths[i], entries, err);
    }
    if (!ok) {
        Remote("dragout: none (%ls)", err.c_str());
        RwStatus s = { -1, 0 };
        g_conn.Send(RW_DRAGOUT_NONE, &s, sizeof(s));
        return;
    }
    Remote("dragout: guest drag carries %zu top-level item(s), %zu entries; first=%ls", g_probePaths.size(), entries.size(), g_probePaths[0].c_str());
    std::thread([entries]() {
        RwStatus s = { 0, (int32_t)entries.size() };
        if (g_conn.Send(RW_DRAGOUT_BEGIN, &s, sizeof(s)) && RwSendEntries(g_conn, entries)) g_conn.Send(RW_DRAGOUT_READY);
    }).detach();
}

static void FinishDragOut(DWORD hostEffect)
{
    POINT c = CatcherCenter();
    MouseTo(c.x, c.y, RW_MOUSE_UP, 0);   /* the guest source's DoDragDrop ends over the catcher */
    if (g_probeData) { g_probeData->Release(); g_probeData = nullptr; }
    Remote("dragout: host drop effect=%lu; guest button released over catcher", hostEffect);
}

/* ---------------- shell state (evidence) ---------------- */

static void ShellState(HWND h)
{
    RwShellState s = {};
    s.hwnd = (uint64_t)h;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    s.toolWindow = (ex & WS_EX_TOOLWINDOW) != 0; s.appWindow = (ex & WS_EX_APPWINDOW) != 0;
    s.visible = IsWindowVisible(h); s.cloaked = Cloaked(h);
    s.onTaskbarCandidate = s.visible && !GetWindow(h, GW_OWNER) && (!s.toolWindow || s.appWindow);
    RECT r = FrameRect(h), x; s.rect = { r.left, r.top, r.right, r.bottom };
    s.onPrivate = IntersectRect(&x, &r, &g_private) && EqualRect(&x, &r);
    g_conn.Send(RW_SHELL_STATE, &s, sizeof(s));
}

/* ---------------- UI thread ---------------- */

static LRESULT CALLBACK UiProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_APP_SELECT: {
        RwSelectReply r = {};
        r.status = Select((HWND)wp, (uint32_t)lp, r);
        g_conn.Send(RW_SELECT, &r, sizeof(r));
        return 0;
    }
    case WM_APP_UNSELECT: {
        Restore((HWND)wp, true);
        RwStatus s = { 0, 0 };
        g_conn.Send(RW_UNSELECT, &s, sizeof(s));
        return 0;
    }
    case WM_APP_RESTORE: {
        RestoreAll();
        RwStatus s = { 0, 0 };
        g_conn.Send(RW_RESTORE_ALL, &s, sizeof(s));
        return 0;
    }
    case WM_APP_ACTIVATE: {
        HWND h = (HWND)wp;
        if (!g_selected.count(h)) return 0;
        BOOL ok = GetForegroundWindow() == h || SetForegroundWindow(h);
        if (!ok) { SwitchToThisWindow(h, TRUE); ok = GetForegroundWindow() == h; }
        Remote("activate %p -> foreground=%d", (void*)h, ok);
        return 0;
    }
    case WM_APP_CLOSE:
        if (g_selected.count((HWND)wp)) PostMessageW((HWND)wp, WM_SYSCOMMAND, SC_CLOSE, 0);
        return 0;
    case WM_APP_SHELL: ShellState((HWND)wp); return 0;
    case WM_APP_DROP: RunDropToGuest((DropJob*)lp); return 0;
    case WM_APP_PROBE: BeginProbe(); return 0;
    case WM_APP_DRAGDONE: FinishDragOut((DWORD)wp); return 0;
    case WM_TIMER:
        if (wp == TIMER_TRACK) Track();
        else if (wp == TIMER_PROBE) ResolveProbe();
        return 0;
    case WM_DISPLAYCHANGE:
        RefreshDisplays();
        if (IsRectEmpty(&g_private)) { Remote("private display gone: restoring all"); RestoreAll(); }
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* ---------------- network ---------------- */

static void Serve()
{
    RwHello hello = { RW_VERSION, { g_private.left, g_private.top, g_private.right, g_private.bottom }, g_privateDpi,
                      { g_desktop.left, g_desktop.top, g_desktop.right, g_desktop.bottom } };
    g_conn.Send(RW_HELLO, &hello, sizeof(hello));
    std::thread sender(FrameSender);
    RwHdr h; std::vector<uint8_t> p;
    RwStager* stager = nullptr; RwDropBegin drop = {};
    while (g_conn.Recv(h, p)) {
        switch (h.type) {
        case RW_LIST: {
            struct Ctx { std::vector<RwListEntry> v; } ctx;
            EnumWindows([](HWND w, LPARAM lp) -> BOOL {
                if (!EligibleRoot(w) && !g_selected.count(w)) return TRUE;
                RwListEntry e = {}; e.hwnd = (uint64_t)w; DWORD pid = 0; GetWindowThreadProcessId(w, &pid); e.pid = pid;
                RECT r = FrameRect(w); e.rect = { r.left, r.top, r.right, r.bottom };
                GetWindowTextW(w, e.title, 128); GetClassNameW(w, e.cls, 64);
                wcsncpy_s(e.exe, ExeName(e.pid).c_str(), _TRUNCATE);
                ((Ctx*)lp)->v.push_back(e); return TRUE; }, (LPARAM)&ctx);
            RwListReply r = { (uint32_t)ctx.v.size() };
            g_conn.Send(RW_LIST, &r, sizeof(r), ctx.v.data(), (uint32_t)(ctx.v.size() * sizeof(RwListEntry)));
            break;
        }
        case RW_SELECT:
            if (p.size() >= 8)
                PostMessageW(g_ui, WM_APP_SELECT, (WPARAM)((RwHwnd*)p.data())->hwnd,
                             p.size() >= sizeof(RwSelect) ? ((RwSelect*)p.data())->mode : RW_SUPPRESS_TOOLWINDOW);
            break;
        case RW_UNSELECT: if (p.size() >= 8) PostMessageW(g_ui, WM_APP_UNSELECT, (WPARAM)((RwHwnd*)p.data())->hwnd, 0); break;
        case RW_ACTIVATE: if (p.size() >= 8) PostMessageW(g_ui, WM_APP_ACTIVATE, (WPARAM)((RwHwnd*)p.data())->hwnd, 0); break;
        case RW_CLOSE: if (p.size() >= 8) PostMessageW(g_ui, WM_APP_CLOSE, (WPARAM)((RwHwnd*)p.data())->hwnd, 0); break;
        case RW_SHELL_STATE: if (p.size() >= 8) PostMessageW(g_ui, WM_APP_SHELL, (WPARAM)((RwHwnd*)p.data())->hwnd, 0); break;
        case RW_RESTORE_ALL: PostMessageW(g_ui, WM_APP_RESTORE, 0, 0); break;
        case RW_MOUSE: if (p.size() >= sizeof(RwMouse)) InjectMouse(*(RwMouse*)p.data()); break;
        case RW_KEY: if (p.size() >= sizeof(RwKey)) InjectKey(*(RwKey*)p.data()); break;
        case RW_DROP_BEGIN:
            if (p.size() >= sizeof(RwDropBegin)) {
                drop = *(RwDropBegin*)p.data();
                delete stager; stager = new RwStager();
                if (!stager->Begin((g_stateDir + L"\\drop").c_str())) stager->Fail(L"staging dir");
                Remote("drop: begin %u top-level item(s) at %d,%d", drop.count, drop.x, drop.y);
            }
            break;
        case RW_FILE_ENTRY: if (stager && p.size() >= sizeof(RwFileEntry)) stager->Entry(*(RwFileEntry*)p.data()); break;
        case RW_FILE_DATA: if (stager) stager->Data(p.data(), p.size()); break;
        case RW_DROP_END:
            if (stager) {
                stager->End();
                PostMessageW(g_ui, WM_APP_DROP, 0, (LPARAM)new DropJob{ stager, drop.x, drop.y });
                stager = nullptr;
            }
            break;
        case RW_DRAGOUT_PROBE: PostMessageW(g_ui, WM_APP_PROBE, 0, 0); break;
        case RW_DRAGOUT_DONE: PostMessageW(g_ui, WM_APP_DRAGDONE, p.size() >= 4 ? *(uint32_t*)p.data() : 0, 0); break;
        }
    }
    Remote("host disconnected");
    g_quit = true; g_frameCv.notify_all();
    sender.join();
    g_quit = false;
    delete stager;
}

static DWORD WINAPI NetThread(LPVOID)
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    RW_SOCKADDR_HV a = {}; a.Family = AF_HYPERV;
    RwParseGuid(RW_SPIKE_SERVICE_GUID, &a.ServiceId);   /* VmId = HV_GUID_WILDCARD (zero) */
    if (ls == INVALID_SOCKET || bind(ls, (sockaddr*)&a, sizeof(a)) || listen(ls, 1)) {
        RwLog("hvsocket listen failed: %d", WSAGetLastError());
        PostMessageW(g_ui, WM_CLOSE, 0, 0);
        return 1;
    }
    RwLog("listening on AF_HYPERV service %ls", RW_SPIKE_SERVICE_GUID);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        RwLog("host connected");
        g_conn.s = c;
        Serve();
        closesocket(c);
        g_conn.s = INVALID_SOCKET;
        /* Selections end on disconnect: restore every surviving window. */
        PostMessageW(g_ui, WM_APP_RESTORE, 0, 0);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(apartment_type::single_threaded);
    OleInitialize(nullptr);
    wchar_t base[MAX_PATH]; ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\AppSandbox\\RemoteWindowsSpike", base, MAX_PATH);
    g_stateDir = base;
    SHCreateDirectoryExW(nullptr, base, nullptr);
    g_rwLog = _wfsopen((g_stateDir + L"\\helper.log").c_str(), L"a", _SH_DENYNO);
    RefreshDisplays();
    RwLog("rw_spike_guest start pid=%lu private=(%ld,%ld,%ld,%ld) dpi=%u desktop=(%ld,%ld,%ld,%ld)", GetCurrentProcessId(),
        g_private.left, g_private.top, g_private.right, g_private.bottom, g_privateDpi,
        g_desktop.left, g_desktop.top, g_desktop.right, g_desktop.bottom);

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
                                   D3D11_SDK_VERSION, g_d3d.put(), nullptr, g_ctx.put());
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
                                           D3D11_SDK_VERSION, g_d3d.put(), nullptr, g_ctx.put());
    if (FAILED(hr)) { RwLog("D3D11CreateDevice failed 0x%08lX", hr); return 1; }
    if (auto mt = g_d3d.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);
    com_ptr<::IInspectable> insp;
    CreateDirect3D11DeviceFromDXGIDevice(g_d3d.as<IDXGIDevice>().get(), insp.put());
    g_rtDevice = insp.as<IDirect3DDevice>();
    RwLog("GraphicsCaptureSession::IsSupported=%d", (int)GraphicsCaptureSession::IsSupported());

    WNDCLASSW c = {}; c.lpfnWndProc = UiProc; c.hInstance = inst; c.lpszClassName = L"RwSpikeGuestUi";
    RegisterClassW(&c);
    g_ui = CreateWindowExW(0, c.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    WNDCLASSW cc = {}; cc.lpfnWndProc = DefWindowProcW; cc.hInstance = inst; cc.lpszClassName = L"RwSpikeCatcher";
    cc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&cc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, cc.lpszClassName, L"RwSpikeOwner", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, inst, nullptr);
    g_catcher = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, cc.lpszClassName, L"",
                                WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_catcher, 0, 1, LWA_ALPHA);   /* alpha 1: hit-testable, visually empty */
    if (!IsRectEmpty(&g_private)) PlaceCatcher();
    RegisterDragDrop(g_catcher, &g_catcherTarget);
    SetTimer(g_ui, TIMER_TRACK, 50, nullptr);
    CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    RestoreAll();
    RevokeDragDrop(g_catcher);
    return 0;
}
