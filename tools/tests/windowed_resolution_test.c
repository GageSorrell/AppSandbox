#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/backend_win/windowed_resolution.h"
#include "../../src/backend_win/display_settings.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

/* Compile actual viewer functions with only platform/VM boundaries substituted. */
typedef void *AsbVm;
typedef struct VmDisplayIdd {
    HWND hwnd;
    UINT vm_id, saved_windowed_width, saved_windowed_height, size_mode;
    UINT shared_width, shared_height, guest_size_mode;
    BOOL fullscreen, input_sizing, windows_cpu_resize, windows_control_disabled;
    BOOL shared_ready, shared_resize, shared_disabled, shared_offered;
    BOOL windowed_unavailable_reported, explicit_size_request;
    BOOL transmit_hotkeys, share_clipboard, fullscreen_pinned;
    BOOL fullscreen_override;
    UINT saved_fullscreen_width, saved_fullscreen_height;
    UINT fullscreen_target_width, fullscreen_target_height;
    UINT64 shared_session;
    SOCKET frame_socket;
    int shared_front, shared_pending;
    CRITICAL_SECTION frame_cs;
    DisplayGpuPacket size_request;
    AsbDisplayResizeState windows_resize_state;
} VmDisplayIdd;
static AsbDisplaySettings preferences;
static HRESULT settings_result;
static RECT test_client_rect;
static UINT dpi = 96;
static int logs, sizes;
static BOOL iconic, zoomed;
static void *asb_find_vm_by_id(UINT id) { (void)id; return &preferences; }
static HRESULT asb_vm_get_display_settings(AsbVm vm, AsbDisplaySettings *out)
{ (void)vm; *out = preferences; return settings_result; }
static void idd_log(VmDisplayIdd *d, const wchar_t *format, ...)
{ (void)d; (void)format; ++logs; }
static BOOL test_GetClientRect(HWND hwnd, RECT *out) { (void)hwnd; *out=test_client_rect; return TRUE; }
static BOOL test_IsIconic(HWND hwnd) { (void)hwnd; return iconic; }
static BOOL test_IsZoomed(HWND hwnd) { (void)hwnd; return zoomed; }
static UINT test_GetDpiForWindow(HWND hwnd) { (void)hwnd; return dpi; }
static LONG test_GetWindowLongW(HWND hwnd, int index)
{ (void)hwnd; return index==GWL_STYLE ? WS_OVERLAPPEDWINDOW : 0; }
static BOOL test_ShowWindow(HWND hwnd, int command)
{ (void)hwnd; CHECK(command==SW_RESTORE); zoomed=FALSE; return TRUE; }
static BOOL test_SetWindowPos(HWND hwnd, HWND after, int x, int y, int width, int height, UINT flags)
{
    RECT frame={0};
    (void)hwnd; (void)after; (void)x; (void)y;
    CHECK(flags==(SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    CHECK(AdjustWindowRectExForDpi(&frame,WS_OVERLAPPEDWINDOW,FALSE,0,dpi));
    test_client_rect.right=width-(frame.right-frame.left);
    test_client_rect.bottom=height-(frame.bottom-frame.top);
    ++sizes;
    return TRUE;
}
#define GetClientRect test_GetClientRect
#define IsIconic test_IsIconic
#define IsZoomed test_IsZoomed
#define GetDpiForWindow test_GetDpiForWindow
#define GetWindowLongW test_GetWindowLongW
#define ShowWindow test_ShowWindow
#define SetWindowPos test_SetWindowPos
#include "windowed_viewer_functions.inc"

static void reset(VmDisplayIdd *d)
{
    ZeroMemory(d,sizeof(*d));
    InitializeCriticalSection(&d->frame_cs);
    idd_display_settings_load(d);
    d->hwnd=(HWND)1;
    d->frame_socket=INVALID_SOCKET;
    d->shared_front=d->shared_pending=-1;
    test_client_rect.left=test_client_rect.top=0;
    idd_size_window_content(d,d->saved_windowed_width,d->saved_windowed_height);
}
static void ready(VmDisplayIdd *d)
{
    d->shared_offered=TRUE;
    d->windows_cpu_resize=TRUE;
    d->shared_session=42;
    d->frame_socket=1;
    d->shared_width=1920; d->shared_height=1080;
}
static void send_request(VmDisplayIdd *d)
{
    CHECK(d->size_request.type);
    CHECK(asb_resize_begin(&d->windows_resize_state,&d->size_request,100)==1);
    ZeroMemory(&d->size_request,sizeof(d->size_request));
}
static void ack(VmDisplayIdd *d)
{
    DisplayGpuPacket reply=d->windows_resize_state.pending;
    reply.type=DISPLAY_GPU_RESIZED;
    CHECK(asb_resize_reply(&d->windows_resize_state,&reply));
    d->guest_size_mode=reply.slot;
}
static void frame(VmDisplayIdd *d, UINT w, UINT h)
{
    d->shared_width=w; d->shared_height=h;
    asb_resize_observe(&d->windows_resize_state,w,h,d->guest_size_mode);
    idd_update_windowed_resolution(d);
}
int main(void)
{
    VmDisplayIdd d;
    UINT x,y,i;
    int old_sizes, old_logs;
    /* Defaults/read failure never rewrite preferences. */
    settings_result=E_FAIL;
    reset(&d);
    CHECK(d.saved_windowed_width==1920 && d.saved_windowed_height==1080);
    CHECK(d.size_mode==DISPLAY_SIZE_GUEST && test_client_rect.right==1920 && test_client_rect.bottom==1080);
    DeleteCriticalSection(&d.frame_cs);
    settings_result=S_OK;
    preferences.resolution.windowed_width=1365;
    preferences.resolution.windowed_height=911;
    reset(&d);
    for (i=96;i<=192;i+=48) {
        dpi=i; zoomed=TRUE;
        idd_size_window_content(&d,1365,911);
        CHECK(test_client_rect.right==1365 && test_client_rect.bottom==911 && !zoomed);
    }
    old_sizes=sizes;
    idd_update_windowed_resolution(&d); /* transport not ready */
    CHECK(!d.size_request.type);
    ready(&d); /* CPU fallback, no GPU textures */
    idd_update_windowed_resolution(&d);
    CHECK(d.size_request.width==1365 && d.size_request.height==911);
    send_request(&d);
    frame(&d,1920,1080); /* old frame and repeated notifications */
    CHECK(!d.size_request.type && sizes==old_sizes);
    ack(&d);
    frame(&d,1920,1080); /* ACK alone is not completion */
    CHECK(d.windows_resize_state.pending.type && !d.size_request.type);
    frame(&d,1365,911);
    CHECK(!d.windows_resize_state.pending.type && !d.size_request.type);
    test_client_rect.right=1000; test_client_rect.bottom=700; /* resize fixed viewer */
    idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type && sizes==old_sizes);
    idd_toggle_windowed_match(&d);
    CHECK(d.size_request.width==1000 && d.size_request.height==700 && d.size_request.slot==DISPLAY_SIZE_WINDOW);
    send_request(&d); ack(&d); frame(&d,1000,700);
    CHECK(!d.size_request.type);
    idd_toggle_windowed_match(&d); /* Match off restores saved policy */
    CHECK(test_client_rect.right==1365 && test_client_rect.bottom==911 && d.size_request.width==1365);
    send_request(&d);
    asb_resize_fail(&d.windows_resize_state,(UINT32)E_FAIL);
    old_sizes=sizes;
    for(i=0;i<100;++i) idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type && sizes==old_sizes && d.shared_width==1000);
    CHECK(preferences.resolution.windowed_width==1365 && preferences.resolution.windowed_height==911);
    d.shared_session=99; /* reconnect keeps suppression and session choice */
    idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type && d.size_mode==DISPLAY_SIZE_GUEST);
    d.size_mode=DISPLAY_SIZE_WINDOW;
    test_client_rect.right=800; test_client_rect.bottom=600;
    idd_update_windowed_resolution(&d);
    CHECK(d.size_request.width==800 && d.size_request.height==600);
    ZeroMemory(&d.size_request,sizeof(d.size_request));
    d.fullscreen=TRUE;
    idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type); /* windowed override is suspended */
    d.fullscreen=FALSE;
    d.input_sizing=TRUE;
    idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type);
    d.input_sizing=FALSE; iconic=TRUE;
    idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type);
    iconic=FALSE;
    d.windows_cpu_resize=FALSE;
    old_logs=logs;
    for(i=0;i<10;++i) idd_update_windowed_resolution(&d);
    CHECK(logs==old_logs+1 && !d.size_request.type);
    /* Input follows actual 1000x700 frame in a 1365x911 viewer. */
    test_client_rect.right=1365; test_client_rect.bottom=911;
    window_to_vm_coords(d.hwnd,682,455,1000,700,FALSE,&x,&y);
    CHECK(x>=499 && x<=501 && y>=349 && y<=351);
    window_to_vm_coords(d.hwnd,0,0,1000,700,FALSE,&x,&y);
    CHECK(x==0 && y==0);
    window_to_vm_coords(d.hwnd,1365,911,1000,700,FALSE,&x,&y);
    CHECK(x==999 && y==699);
    /* Unsent Match target is superseded even if the connection disappears. */
    CHECK(d.size_mode==DISPLAY_SIZE_WINDOW);
    d.windows_cpu_resize=TRUE;
    idd_update_windowed_resolution(&d);
    CHECK(d.size_request.type);
    d.frame_socket=INVALID_SOCKET;
    d.windows_cpu_resize=FALSE;
    CHECK(idd_windowed_match_available(&d));
    idd_toggle_windowed_match(&d);
    CHECK(d.size_mode==DISPLAY_SIZE_GUEST && !d.size_request.type);
    CHECK(!idd_windowed_match_available(&d));
    CHECK(test_client_rect.right==1365 && test_client_rect.bottom==911);
    /* A stale in-flight Match ACK cannot undo a newer fixed session choice. */
    ZeroMemory(&d.windows_resize_state,sizeof(d.windows_resize_state));
    ready(&d);
    d.guest_size_mode=DISPLAY_SIZE_GUEST;
    idd_toggle_windowed_match(&d);
    send_request(&d);
    idd_toggle_windowed_match(&d);
    CHECK(!d.size_request.type && d.size_mode==DISPLAY_SIZE_GUEST);
    ack(&d); frame(&d,1365,911);
    CHECK(d.size_mode==DISPLAY_SIZE_GUEST && d.size_request.type && d.size_request.slot==DISPLAY_SIZE_GUEST);
    send_request(&d);
    CHECK(asb_resize_expire(&d.windows_resize_state,15100,(UINT32)HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
    for(i=0;i<100;++i) idd_update_windowed_resolution(&d);
    CHECK(!d.size_request.type && d.saved_windowed_width==1365);
    DeleteCriticalSection(&d.frame_cs);
    reset(&d); /* reopening discards Match and reloads saved custom pair */
    CHECK(d.size_mode==DISPLAY_SIZE_GUEST && test_client_rect.right==1365 && test_client_rect.bottom==911);
    ready(&d); idd_update_windowed_resolution(&d);
    CHECK(d.size_request.width==1365 && d.size_request.slot==DISPLAY_SIZE_GUEST);
    DeleteCriticalSection(&d.frame_cs);
    {
        DisplayGpuPacket p=asb_windowed_target(1365,911,DISPLAY_SIZE_WINDOW,0,0);
        CHECK(p.width==640 && p.height==480);
        p=asb_windowed_target(1365,911,DISPLAY_SIZE_WINDOW,99999,99999);
        CHECK(p.width==7680 && p.height==4320);
    }
    puts("PASS windowed viewer policy, DPI sizing, transport and actual-frame input mapping");
    return 0;
}
