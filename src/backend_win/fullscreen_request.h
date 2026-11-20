#pragma once
#include <windows.h>

/* Caller and viewer each own a reference. Cancellation and committing a
   transition compete atomically; a caller waits for any committed work. */
enum { ASB_FS_PENDING, ASB_FS_RUNNING, ASB_FS_COMMITTING, ASB_FS_CANCELLED };
typedef struct AsbFullscreenRequest {
    LONG refs;
    volatile LONG state;
    HANDLE done, cancel;
    volatile LONG result;
} AsbFullscreenRequest;

static void asb_fullscreen_request_release(AsbFullscreenRequest *r)
{
    if (!InterlockedDecrement(&r->refs)) {
        CloseHandle(r->done);
        if (r->cancel) CloseHandle(r->cancel);
        HeapFree(GetProcessHeap(),0,r);
    }
}

static void asb_fullscreen_request_cancel(AsbFullscreenRequest *r)
{
    LONG state=InterlockedCompareExchange(&r->state,0,0);
    while (state==ASB_FS_PENDING || state==ASB_FS_RUNNING) {
        LONG previous=InterlockedCompareExchange(&r->state,ASB_FS_CANCELLED,state);
        if (previous==state) {
            /* No handler has started; its retained reference protects late dispatch. */
            if (state==ASB_FS_PENDING) SetEvent(r->done);
            return;
        }
        state=previous;
    }
}

static BOOL asb_fullscreen_request_cancelled(AsbFullscreenRequest *r)
{
    if (!r) return FALSE;
    if (r->cancel && WaitForSingleObject(r->cancel,0)==WAIT_OBJECT_0)
        asb_fullscreen_request_cancel(r);
    return InterlockedCompareExchange(&r->state,0,0)==ASB_FS_CANCELLED;
}

static BOOL asb_fullscreen_request_begin(AsbFullscreenRequest *r)
{
    return !asb_fullscreen_request_cancelled(r) &&
        InterlockedCompareExchange(&r->state,ASB_FS_RUNNING,ASB_FS_PENDING)==ASB_FS_PENDING;
}

static BOOL asb_fullscreen_request_commit(AsbFullscreenRequest *r)
{
    return !r || (!asb_fullscreen_request_cancelled(r) &&
        InterlockedCompareExchange(&r->state,ASB_FS_COMMITTING,ASB_FS_RUNNING)==ASB_FS_RUNNING);
}

static void asb_fullscreen_request_finish(AsbFullscreenRequest *r,BOOL result)
{
    InterlockedExchange(&r->result,result);
    SetEvent(r->done);
    asb_fullscreen_request_release(r);
}

/* Call after destroying the viewer window, before its thread exits. */
static void asb_fullscreen_discard_requests(UINT message)
{
    MSG msg;
    while (PeekMessageW(&msg,NULL,message,message,PM_REMOVE))
        if (msg.lParam) asb_fullscreen_request_finish((AsbFullscreenRequest*)msg.lParam,FALSE);
}

static BOOL asb_fullscreen_call(HWND hwnd,UINT message,BOOL enabled,HANDLE cancel)
{
    AsbFullscreenRequest *r;
    BOOL result=FALSE,quit=FALSE,cancelling=FALSE;
    int quit_code=0;
    if (!IsWindow(hwnd)) return FALSE;
    r=(AsbFullscreenRequest*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*r));
    if (!r) return FALSE;
    r->refs=2;
    r->done=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!r->done || (cancel && !DuplicateHandle(GetCurrentProcess(),cancel,
            GetCurrentProcess(),&r->cancel,SYNCHRONIZE,FALSE,0))) {
        if (r->done) CloseHandle(r->done);
        HeapFree(GetProcessHeap(),0,r);
        return FALSE;
    }
    if (GetWindowThreadProcessId(hwnd,NULL)==GetCurrentThreadId()) {
        SendMessageW(hwnd,message,!!enabled,(LPARAM)r);
    } else if (!PostMessageW(hwnd,message,!!enabled,(LPARAM)r)) {
        asb_fullscreen_request_release(r);
        asb_fullscreen_request_release(r);
        return FALSE;
    }
    for (;;) {
        HANDLE handles[2]={r->done,r->cancel};
        DWORD count=r->cancel && !cancelling?2:1;
        DWORD wait=MsgWaitForMultipleObjectsEx(count,handles,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if (wait==WAIT_OBJECT_0) { result=InterlockedCompareExchange(&r->result,0,0); break; }
        if (wait==WAIT_OBJECT_0+1 && count==2) {
            asb_fullscreen_request_cancel(r);
            /* Avoid spinning on a permanently signaled cancellation handle. */
            cancelling=TRUE;
        } else if (wait==WAIT_OBJECT_0+count) {
            MSG msg;
            while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
                if (msg.message==WM_QUIT) {
                    quit=TRUE;quit_code=(int)msg.wParam;
                    asb_fullscreen_request_cancel(r);
                    break;
                }
                TranslateMessage(&msg);DispatchMessageW(&msg);
                if (WaitForSingleObject(r->done,0)==WAIT_OBJECT_0) break;
            }
        } else if (wait==WAIT_FAILED) {
            asb_fullscreen_request_cancel(r);
            /* Running handlers must finish before their caller abandons them. */
            WaitForSingleObject(r->done,INFINITE);
            result=InterlockedCompareExchange(&r->result,0,0);break;
        }
    }
    asb_fullscreen_request_release(r);
    if (quit) PostQuitMessage(quit_code);
    return result;
}
