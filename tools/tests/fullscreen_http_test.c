#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#include <http.h>
#pragma warning(pop)
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static HANDLE g_req_queue;
static ULONG watch_status=ERROR_IO_PENDING;
static int calls,starts,cancels,joins;
static BOOL disconnect_during_call;
static OVERLAPPED *outstanding;
static ULONG test_wait(HANDLE queue,HTTP_CONNECTION_ID connection,OVERLAPPED *watch)
{
 CHECK(queue==g_req_queue && connection==17);++starts;
 if (watch_status==ERROR_IO_PENDING) outstanding=watch;
 return watch_status;
}
static BOOL test_cancel(HANDLE queue,OVERLAPPED *watch)
{
 CHECK(queue==g_req_queue && watch==outstanding);++cancels;
 SetEvent(watch->hEvent);return TRUE;
}
static BOOL test_join(HANDLE queue,OVERLAPPED *watch,DWORD *bytes,BOOL wait)
{
 CHECK(queue==g_req_queue && watch==outstanding && wait);++joins;*bytes=0;
 CHECK(WaitForSingleObject(watch->hEvent,0)==WAIT_OBJECT_0);outstanding=NULL;return TRUE;
}
static BOOL display_set_fullscreen(UINT64 vm_id,BOOL enabled,HANDLE cancel)
{
 CHECK(vm_id==3 && enabled);++calls;
 if (disconnect_during_call) SetEvent(cancel);
 return WaitForSingleObject(cancel,0)!=WAIT_OBJECT_0;
}
#define HttpWaitForDisconnect test_wait
#define CancelIoEx test_cancel
#define GetOverlappedResult test_join
/* ACTUAL HTTP FULLSCREEN WATCH */
int main(void)
{
 g_req_queue=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(g_req_queue);
 CHECK(display_set_fullscreen_for_request(3,TRUE,17));CHECK(calls==1 && cancels==1 && joins==1 && !outstanding);
 disconnect_during_call=TRUE;
 CHECK(!display_set_fullscreen_for_request(3,TRUE,17));CHECK(calls==2 && cancels==2 && joins==2 && !outstanding);
 disconnect_during_call=FALSE;watch_status=NO_ERROR;
 CHECK(!display_set_fullscreen_for_request(3,TRUE,17));CHECK(calls==3 && cancels==2 && joins==2);
 watch_status=ERROR_INVALID_HANDLE;
 CHECK(!display_set_fullscreen_for_request(3,TRUE,17));CHECK(calls==3 && starts==4);
 CloseHandle(g_req_queue);
 puts("PASS HTTP fullscreen abandonment: disconnect event, completed disconnect, setup failure, overlapped cleanup");
 return 0;
}
