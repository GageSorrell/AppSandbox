#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/backend_win/fullscreen_resolution.h"
#include "../../src/backend_win/display_settings.h"
#include "../../src/backend_win/display_modes.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

typedef void *AsbVm;
typedef struct VmDisplayIdd {
    HWND hwnd;
    UINT vm_id, saved_windowed_width, saved_windowed_height, size_mode;
    UINT saved_fullscreen_width, saved_fullscreen_height;
    UINT fullscreen_target_width, fullscreen_target_height;
    UINT shared_width, shared_height, guest_size_mode, shared_refresh_rate;
    UINT frame_width, frame_height;
    volatile LONG fullscreen;
    BOOL fullscreen_override, fullscreen_pinned, fullscreen_fitting;
    BOOL input_sizing, windows_cpu_resize, windows_control_disabled;
    BOOL shared_ready, shared_resize, shared_disabled, shared_offered, shared_refresh;
    BOOL windowed_unavailable_reported, explicit_size_request;
    BOOL transmit_hotkeys, share_clipboard, size_follow_pending, size_update_pending;
    UINT64 shared_session;
    SOCKET frame_socket;
    int shared_front, shared_pending;
    wchar_t os_type[32];
    CRITICAL_SECTION frame_cs;
    DisplayGpuPacket size_request;
    AsbDisplayResizeState windows_resize_state;
    HMONITOR fullscreen_monitor;
    WINDOWPLACEMENT windowed_placement;
    LONG_PTR windowed_style;
    void *agent_overlay;
} VmDisplayIdd;

static AsbDisplaySettings preferences;
#ifdef ASB_DISPLAY_INTEGRATION
static wchar_t integration_disk[MAX_PATH];
#endif
static RECT bounds = {100,100,1100,800};
static RECT monitor_bounds[2] = {{0,0,1920,1080},{1920,0,3200,1024}};
static AsbDisplayMode modes[2][5] = {
    {{1920,1080},{3840,2160},{7680,5000},{2560,1080},{0,0}},
    {{1280,1024},{2560,1440},{1920,1920},{0,0},{0,0}}
};
static HMONITOR current_monitor = (HMONITOR)1;
static LONG_PTR style = WS_OVERLAPPEDWINDOW;
static WINDOWPLACEMENT placement;
static int logs, enumerations, frees, fits, placement_restores;
static BOOL enumerate_failure, info_failure, fit_failure, overlay_ready=TRUE;
static VmDisplayIdd *active_viewer;
static BOOL idd_fit_fullscreen(VmDisplayIdd *d,HMONITOR target);

static void *asb_find_vm_by_id(UINT id) { (void)id; return &preferences; }
static HRESULT asb_vm_get_display_settings(AsbVm vm, AsbDisplaySettings *out)
{
    (void)vm;
#ifdef ASB_DISPLAY_INTEGRATION
    return asb_display_settings_read(integration_disk,out);
#else
    *out=preferences; return S_OK;
#endif
}
static void idd_log(VmDisplayIdd *d, const wchar_t *format, ...)
{ (void)d; (void)format; ++logs; }
static HRESULT device(void *ctx, size_t index, wchar_t out[CCHDEVICENAME])
{
    (void)ctx;
    if(index>=2) return S_FALSE;
    wcscpy_s(out,CCHDEVICENAME,index==0?L"FIRST":L"SECOND");
    return S_OK;
}
static HRESULT mode(void *ctx,const wchar_t *name,size_t index,AsbDisplayMode *out)
{
    size_t monitor=wcscmp(name,L"FIRST")?1:0;
    (void)ctx;
    if(index>=5 || !modes[monitor][index].width) return S_FALSE;
    *out=modes[monitor][index]; return S_OK;
}
static HRESULT asb_get_host_display_capabilities(AsbDisplayCapabilities *out)
{
    AsbDisplayModeSource source={NULL,device,mode};
    ++enumerations;
    if(enumerate_failure) { ZeroMemory(out,sizeof(*out)); out->status=E_FAIL; return E_FAIL; }
    return asb_display_capabilities_build(&source,out);
}
static void asb_free_host_display_capabilities(AsbDisplayCapabilities *out)
{ ++frees; asb_display_capabilities_free(out); }
static HRESULT asb_get_host_monitor_maximum(const AsbDisplayCapabilities *caps,HMONITOR monitor,AsbDisplayMode *out)
{
    if(monitor!=(HMONITOR)1 && monitor!=(HMONITOR)2) return E_FAIL;
    return asb_display_capabilities_maximum(caps,monitor==(HMONITOR)1?L"FIRST":L"SECOND",out);
}
static BOOL test_GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO out)
{
    if(info_failure || (monitor!=(HMONITOR)1 && monitor!=(HMONITOR)2)) return FALSE;
    out->rcMonitor=monitor_bounds[monitor==(HMONITOR)1?0:1];
    out->rcWork=out->rcMonitor;
    return TRUE;
}
static HMONITOR test_MonitorFromWindow(HWND hwnd,DWORD flags)
{ (void)hwnd; (void)flags; return current_monitor; }
static BOOL test_GetClientRect(HWND hwnd,RECT *out)
{
    (void)hwnd;
    *out=(RECT){0,0,bounds.right-bounds.left,bounds.bottom-bounds.top};
    return TRUE;
}
static BOOL test_GetWindowPlacement(HWND hwnd,WINDOWPLACEMENT *out)
{ (void)hwnd; *out=placement; return TRUE; }
static BOOL test_SetWindowPlacement(HWND hwnd,const WINDOWPLACEMENT *value)
{ (void)hwnd; placement=*value; bounds=value->rcNormalPosition; ++placement_restores; return TRUE; }
static LONG_PTR test_GetWindowLongPtrW(HWND hwnd,int index)
{ (void)hwnd; CHECK(index==GWL_STYLE); return style; }
static LONG_PTR test_SetWindowLongPtrW(HWND hwnd,int index,LONG_PTR value)
{ LONG_PTR old=style; (void)hwnd; CHECK(index==GWL_STYLE); style=value; return old; }
static BOOL test_SetWindowPos(HWND hwnd,HWND after,int x,int y,int w,int h,UINT flags)
{
    (void)hwnd; (void)after;
    if(flags&SWP_NOSIZE) return TRUE;
    if(fit_failure) return FALSE;
    bounds=(RECT){x,y,x+w,y+h}; ++fits;
    /* Exercise the guard against synchronous SetWindowPos reentrancy. */
    if(active_viewer && active_viewer->fullscreen_fitting)
        CHECK(idd_fit_fullscreen(active_viewer,current_monitor));
    return TRUE;
}
static BOOL test_IsIconic(HWND hwnd) { (void)hwnd; return FALSE; }
static BOOL test_IsZoomed(HWND hwnd) { (void)hwnd; return FALSE; }
static LONG test_GetWindowLongW(HWND hwnd,int index)
{ (void)hwnd; return index==GWL_STYLE?(LONG)style:0; }
static UINT test_GetDpiForWindow(HWND hwnd) { (void)hwnd; return 144; }
static BOOL test_ShowWindow(HWND hwnd,int command) { (void)hwnd; (void)command; return TRUE; }
static UINT idd_monitor_refresh(HWND hwnd) { (void)hwnd; return 60; }
static BOOL agent_overlay_is_ready(void *overlay) { (void)overlay; return overlay_ready; }
static void agent_overlay_cancel_input(void *overlay) { (void)overlay; }
static void agent_overlay_set_fullscreen(void *overlay,BOOL full) { (void)overlay; (void)full; }
static void idd_update_relative_mouse(VmDisplayIdd *d) { (void)d; }
static void asb_events_changed(void) {}

#define GetMonitorInfoW test_GetMonitorInfoW
#define MonitorFromWindow test_MonitorFromWindow
#define GetClientRect test_GetClientRect
#define GetWindowPlacement test_GetWindowPlacement
#define SetWindowPlacement test_SetWindowPlacement
#define GetWindowLongPtrW test_GetWindowLongPtrW
#define SetWindowLongPtrW test_SetWindowLongPtrW
#define SetWindowPos test_SetWindowPos
#define IsIconic test_IsIconic
#define IsZoomed test_IsZoomed
#define GetWindowLongW test_GetWindowLongW
#define GetDpiForWindow test_GetDpiForWindow
#define ShowWindow test_ShowWindow
#include "fullscreen_viewer_functions.inc"

static void settle(VmDisplayIdd *d)
{
    DisplayGpuPacket reply=d->size_request;
    CHECK(reply.type);
    CHECK(asb_resize_begin(&d->windows_resize_state,&reply,100)==1);
    ZeroMemory(&d->size_request,sizeof(d->size_request));
    reply.type=DISPLAY_GPU_RESIZED;
    CHECK(asb_resize_reply(&d->windows_resize_state,&reply));
    d->guest_size_mode=reply.slot;
    d->shared_width=reply.width; d->shared_height=reply.height;
    CHECK(asb_resize_observe(&d->windows_resize_state,reply.width,reply.height,reply.slot));
    idd_update_display_size(d,FALSE);
    CHECK(!d->size_request.type);
}
static void reset(VmDisplayIdd *d)
{
    ZeroMemory(d,sizeof(*d));
    InitializeCriticalSection(&d->frame_cs);
    wcscpy_s(d->os_type,32,L"windows");
    idd_display_settings_load(d);
    d->hwnd=(HWND)1; d->frame_socket=1;
    d->shared_session=42; d->windows_cpu_resize=TRUE; d->shared_offered=TRUE;
    d->shared_width=1365; d->shared_height=911;
    d->shared_front=d->shared_pending=-1;
    bounds=(RECT){100,100,1100,800}; style=WS_OVERLAPPEDWINDOW;
    placement=(WINDOWPLACEMENT){sizeof(placement)};
    placement.showCmd=SW_SHOWNORMAL;
    placement.rcNormalPosition=bounds;
    current_monitor=(HMONITOR)1;
    active_viewer=d;
}
#ifndef ASB_DISPLAY_INTEGRATION
int main(void)
{
    VmDisplayIdd d;
    RECT original;
    int old_enumerations,old_logs,old_fits;
    float x,y,w,h;
    UINT vx,vy;
    preferences.resolution=(AsbDisplayResolution){1365,911,FALSE,4096,2160};
    reset(&d); original=bounds;
    CHECK(idd_set_fullscreen(&d,TRUE));
    CHECK(d.fullscreen_target_width==3840 && d.fullscreen_target_height==2160);
    CHECK(d.size_request.width==3840 && d.size_request.slot==DISPLAY_SIZE_GUEST);
    CHECK(EqualRect(&bounds,&monitor_bounds[0]) && style&WS_POPUP);
    CHECK(d.saved_fullscreen_width==4096 && !d.fullscreen_override);
    settle(&d);
    old_enumerations=enumerations; old_fits=fits;
    idd_update_display_size(&d,FALSE); /* frames/timer never re-enumerate or refit */
    CHECK(!d.size_request.type && enumerations==old_enumerations && fits==old_fits);
    current_monitor=(HMONITOR)2;
    CHECK(idd_fit_fullscreen(&d,current_monitor));
    CHECK(d.size_request.width==2560 && d.size_request.height==1440);
    CHECK(EqualRect(&bounds,&monitor_bounds[1]));
    settle(&d);
    compute_letterbox(1280,1024,2560,1440,&x,&y,&w,&h);
    CHECK(x==0 && y==152 && w==1280 && h==720);
    window_to_vm_coords(d.hwnd,640,512,2560,1440,FALSE,&vx,&vy);
    CHECK(vx==1280 && vy==720);
    /* Same-monitor hotplug/DPI refresh uses a fresh catalog, including portrait. */
    modes[1][3]=(AsbDisplayMode){2160,3840};
    monitor_bounds[1]=(RECT){1920,0,3000,1920};
    CHECK(idd_fit_fullscreen(&d,current_monitor));
    CHECK(d.size_request.width==2160 && d.size_request.height==3840);
    settle(&d);
    CHECK(idd_set_fullscreen(&d,FALSE));
    CHECK(EqualRect(&bounds,&original) && style==WS_OVERLAPPEDWINDOW && placement_restores==1);
    CHECK(d.size_mode==DISPLAY_SIZE_GUEST && d.size_request.width==1365 && d.size_request.height==911);
    settle(&d);
    DeleteCriticalSection(&d.frame_cs);

    /* Fullscreen takes priority over Match, then restores the session choice. */
    reset(&d); d.size_mode=DISPLAY_SIZE_WINDOW;
    idd_update_display_size(&d,FALSE); CHECK(d.size_request.width==1000);
    CHECK(idd_set_fullscreen(&d,TRUE)); /* supersedes unsent Match target */
    CHECK(d.size_mode==DISPLAY_SIZE_WINDOW && d.size_request.width==3840 && d.size_request.slot==DISPLAY_SIZE_GUEST);
    settle(&d);
    CHECK(idd_set_fullscreen(&d,FALSE));
    CHECK(EqualRect(&bounds,&original) && d.size_mode==DISPLAY_SIZE_WINDOW);
    CHECK(d.size_request.width==1000 && d.size_request.height==700 && d.size_request.slot==DISPLAY_SIZE_WINDOW);
    settle(&d);
    DeleteCriticalSection(&d.frame_cs);

    /* Custom mode persists across monitors/config changes without enumeration. */
    preferences.resolution.fullscreen_override=TRUE;
    reset(&d); old_enumerations=enumerations;
    CHECK(idd_set_fullscreen(&d,TRUE));
    CHECK(d.size_request.width==4096 && d.size_request.height==2160);
    settle(&d); current_monitor=(HMONITOR)2;
    CHECK(idd_fit_fullscreen(&d,current_monitor));
    CHECK(!d.size_request.type && enumerations==old_enumerations);
    compute_letterbox(1080,1920,4096,2160,&x,&y,&w,&h);
    CHECK(w==1080 && h<570 && y>675); /* aspect mismatch retains black bars */
    CHECK(preferences.resolution.fullscreen_width==4096 && d.saved_fullscreen_width==4096);
    DeleteCriticalSection(&d.frame_cs);

    /* Automatic failure: bounded current desktop pixels, then default pixels. */
    preferences.resolution.fullscreen_override=FALSE;
    enumerate_failure=TRUE;
    reset(&d); old_logs=logs;
    CHECK(idd_set_fullscreen(&d,TRUE));
    CHECK(d.size_request.width==1920 && d.size_request.height==1080 && logs==old_logs+1);
    settle(&d);
    monitor_bounds[0]=(RECT){-10000,0,0,6000};
    CHECK(idd_fit_fullscreen(&d,(HMONITOR)1));
    CHECK(d.fullscreen_target_width==7680 && d.fullscreen_target_height==4320);
    CHECK(EqualRect(&bounds,&monitor_bounds[0])); /* viewer bounds never use the cap */
    info_failure=TRUE;
    idd_select_fullscreen_resolution(&d,(HMONITOR)1);
    CHECK(d.fullscreen_target_width==1920 && d.fullscreen_target_height==1080);
    info_failure=FALSE; enumerate_failure=FALSE;
    CHECK(enumerations==frees);
    DeleteCriticalSection(&d.frame_cs);

    /* Successful enumeration without an in-cap maximum also uses the fallback. */
    reset(&d);
    modes[0][0]=(AsbDisplayMode){8000,6000};
    modes[0][1]=(AsbDisplayMode){0,0};
    monitor_bounds[0]=(RECT){0,0,500,400};
    CHECK(idd_set_fullscreen(&d,TRUE));
    CHECK(d.fullscreen_target_width==640 && d.fullscreen_target_height==480);
    CHECK(EqualRect(&bounds,&monitor_bounds[0]));
    DeleteCriticalSection(&d.frame_cs);
    modes[0][0]=(AsbDisplayMode){1920,1080};
    modes[0][1]=(AsbDisplayMode){3840,2160};
    monitor_bounds[0]=(RECT){0,0,1920,1080};

    /* Rejection and reconnect preserve both target and the last actual frame. */
    reset(&d); CHECK(idd_set_fullscreen(&d,TRUE));
    CHECK(asb_resize_begin(&d.windows_resize_state,&d.size_request,100)==1);
    ZeroMemory(&d.size_request,sizeof(d.size_request));
    asb_resize_fail(&d.windows_resize_state,(UINT32)E_FAIL);
    d.shared_session=99;
    for(int i=0;i<100;++i) idd_update_display_size(&d,FALSE);
    CHECK(!d.size_request.type && d.shared_width==1365 && d.saved_fullscreen_width==4096);
    CHECK(idd_fit_fullscreen(&d,(HMONITOR)1));
    CHECK(!d.size_request.type); /* identical display notification cannot retry */
    CHECK(idd_set_fullscreen(&d,FALSE));
    CHECK(!d.size_request.type && EqualRect(&bounds,&original));
    DeleteCriticalSection(&d.frame_cs);

    /* Late transport readiness and in-flight windowed/fullscreen transitions. */
    reset(&d); d.windows_cpu_resize=FALSE; d.shared_offered=FALSE;
    CHECK(idd_set_fullscreen(&d,TRUE)); CHECK(!d.size_request.type);
    d.windows_cpu_resize=TRUE; idd_update_display_size(&d,FALSE);
    CHECK(d.size_request.width==3840);
    CHECK(asb_resize_begin(&d.windows_resize_state,&d.size_request,100)==1);
    ZeroMemory(&d.size_request,sizeof(d.size_request));
    CHECK(idd_set_fullscreen(&d,FALSE)); CHECK(!d.size_request.type);
    {
        DisplayGpuPacket reply=d.windows_resize_state.pending;
        reply.type=DISPLAY_GPU_RESIZED;
        CHECK(asb_resize_reply(&d.windows_resize_state,&reply));
        d.guest_size_mode=reply.slot; d.shared_width=reply.width; d.shared_height=reply.height;
        CHECK(asb_resize_observe(&d.windows_resize_state,reply.width,reply.height,reply.slot));
        idd_update_display_size(&d,FALSE);
        CHECK(d.size_request.width==1365 && !d.fullscreen && EqualRect(&bounds,&original));
    }
    DeleteCriticalSection(&d.frame_cs);

    /* Failed borderless entry restores placement and the windowed target. */
    reset(&d); fit_failure=TRUE;
    CHECK(!idd_set_fullscreen(&d,TRUE) && !d.fullscreen);
    CHECK(EqualRect(&bounds,&original) && style==WS_OVERLAPPEDWINDOW && !d.size_request.type);
    fit_failure=FALSE; overlay_ready=FALSE;
    CHECK(!idd_set_fullscreen(&d,TRUE) && !d.fullscreen);
    DeleteCriticalSection(&d.frame_cs);
    /* Preserve maximized placement metadata and the original window style. */
    overlay_ready=TRUE;
    reset(&d);
    placement.showCmd=SW_SHOWMAXIMIZED;
    placement.ptMaxPosition=(POINT){-8,-8};
    style=WS_OVERLAPPEDWINDOW|WS_MAXIMIZE;
    CHECK(idd_set_fullscreen(&d,TRUE)); settle(&d);
    CHECK(!(style&WS_MAXIMIZE));
    CHECK(idd_set_fullscreen(&d,FALSE));
    CHECK(placement.showCmd==SW_SHOWMAXIMIZED && placement.ptMaxPosition.x==-8);
    CHECK(style==(WS_OVERLAPPEDWINDOW|WS_MAXIMIZE) && EqualRect(&bounds,&original));
    DeleteCriticalSection(&d.frame_cs);
    CHECK(preferences.resolution.windowed_width==1365 && preferences.resolution.fullscreen_width==4096);
    puts("PASS fullscreen maximum/custom/fallback, monitor refresh, aspect/input, transport and placement restoration");
    return 0;
}

#endif
