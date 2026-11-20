#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "api_json.h"
#define ASB_CONTROL_AGENT 1
#define json_get_int api_json_get_int
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct FakeView FakeView;
typedef struct { void (*PostWebMessageAsJson)(FakeView *,const wchar_t *); } FakeVtbl;
struct FakeView { FakeVtbl *lpVtbl; };
typedef struct AgentOverlay {
    BOOL closed,ready,finished,waiting,fullscreen,pinned;
    int state,layout_revision,fullscreen_zone;
    RECT bounds,menu_bounds,exit_bounds,options_bounds,button_bounds;
    double dpi;
    UINT mouse_buttons;
    HWND render;
    FakeView *webview;
} AgentOverlay;
static int cancel_revision,updates;
static void post(FakeView *view,const wchar_t *json) { (void)view; CHECK(json_get_int(json,L"layoutRevision",&cancel_revision)); }
static void fullscreen_pointer(AgentOverlay *s,POINT p,BOOL down) { (void)s;(void)p;(void)down; }
static void leave(AgentOverlay *s) { (void)s; }
static void update(AgentOverlay *s) { (void)s; ++updates; }
/* ACTUAL OVERLAY FUNCTIONS */
int main(void)
{
    FakeVtbl vtable={post};FakeView view={&vtable};
    AgentOverlay s={0};s.ready=TRUE;s.fullscreen=TRUE;s.bounds=(RECT){0,0,1600,1200};s.dpi=2;s.webview=&view;
    const wchar_t *visible=L"{\"layoutRevision\":0,\"fx\":404,\"fy\":8,\"fw\":36,\"fh\":36,\"bx\":360,\"by\":8,\"bw\":36,\"bh\":36,\"ox\":360,\"oy\":52,\"ow\":250,\"oh\":184}";
    layout(&s,visible);
    CHECK(agent_overlay_hit_test(&s,(POINT){820,30}));
    CHECK(agent_overlay_hit_test(&s,(POINT){730,30}));
    CHECK(agent_overlay_hit_test(&s,(POINT){740,120}));
    CHECK(!agent_overlay_hit_test(&s,(POINT){800,30})); /* gap */
    CHECK(!agent_overlay_hit_test(&s,(POINT){200,600}));
    agent_overlay_cancel_input(&s);
    CHECK(cancel_revision==1 && !s.mouse_buttons);
    CHECK(!agent_overlay_hit_test(&s,(POINT){820,30}));
    layout(&s,visible);CHECK(IsRectEmpty(&s.exit_bounds)); /* delayed old layout */
    layout(&s,L"{\"layoutRevision\":1,\"fx\":404,\"fy\":8,\"fw\":36,\"fh\":36}");
    CHECK(agent_overlay_hit_test(&s,(POINT){820,30}));
    CHECK(IsRectEmpty(&s.options_bounds) && IsRectEmpty(&s.button_bounds));
    agent_overlay_set_pinned(&s,TRUE);CHECK(s.pinned && updates==1 && s.layout_revision==3);
    layout(&s,L"{\"layoutRevision\":2,\"fx\":404,\"fy\":8,\"fw\":36,\"fh\":36}");
    CHECK(IsRectEmpty(&s.exit_bounds)); /* intermediate cancel reply */
    agent_overlay_set_fullscreen(&s,FALSE);CHECK(!s.fullscreen && updates==2);
    layout(&s,L"{\"layoutRevision\":5,\"fx\":404,\"fy\":8,\"fw\":36,\"fh\":36}");
    CHECK(!agent_overlay_hit_test(&s,(POINT){820,30}));
    agent_overlay_set_fullscreen(&s,TRUE);CHECK(s.fullscreen && s.pinned);
    s.closed=TRUE;CHECK(!agent_overlay_hit_test(&s,(POINT){820,30}));
    puts("PASS native overlay: visible-only rectangles, gap, DPI, cancel, stale layouts, pin, fullscreen transitions");
    return 0;
}
