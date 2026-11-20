/* Runs the actual VddHandleResize source against a minimal WDF/transport harness.
   This checks dispatch/validation, not monitor arrival or native rendering. */
#include <windows.h>
#include "../transport/asb_display_resize.h"
#include <stdio.h>
#include <stdlib.h>
struct AsbConn {};
struct VDD_DEVICE_CONTEXT {
    HANDLE hDisplayWork;
    LONG stopping;
    SRWLOCK displayLock;
    DisplayGpuPacket displayRequest;
};
struct VDD_SWAP_PROC { VDD_DEVICE_CONTEXT *pOwner; UINT64 gpuSession; BOOL gpuResizeSupported; };
static BOOL memoryTransport;
static int queued;
static DisplayGpuPacket capturedReply;
static BOOL asb_transport_is_ivshmem() { return memoryTransport; }
static int VddSendAll(AsbConn*, const char *data, int bytes) {
    if (bytes != sizeof(capturedReply)) abort();
    memcpy(&capturedReply,data,sizeof(capturedReply)); return 0;
}
static void WdfWorkItemEnqueue(HANDLE) { ++queued; }
#include "vdd_resize_handler.inc"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
int main()
{
    VDD_DEVICE_CONTEXT ctx={}; ctx.hDisplayWork=(HANDLE)1;
    InitializeSRWLock(&ctx.displayLock);
    VDD_SWAP_PROC proc={&ctx,42,TRUE}; AsbConn conn;
    DisplayGpuPacket p={}; p.magic=DISPLAY_GPU_MAGIC; p.version=DISPLAY_GPU_VERSION;
    p.type=DISPLAY_GPU_RESIZE; p.session=42;
    const UINT sizes[][2]={{640,480},{1280,720},{1920,1080},{7680,4320},{1365,911}};
    for (const auto &size:sizes) for (UINT mode=0;mode<=1;++mode) {
        p.width=size[0]; p.height=size[1]; p.slot=mode;
        int before=queued;
        CHECK(VddHandleResize(&proc,&conn,p));
        CHECK(capturedReply.status==S_OK && queued==before+1);
        CHECK(ctx.displayRequest.width==p.width && ctx.displayRequest.height==p.height);
    }
    const UINT invalid[][2]={{639,480},{640,479},{7681,4320},{7680,4321},{0,0},{0xffffffff,480}};
    for (const auto &size:invalid) {
        p.width=size[0]; p.height=size[1]; int before=queued;
        DisplayGpuPacket previous=ctx.displayRequest;
        CHECK(VddHandleResize(&proc,&conn,p));
        CHECK(capturedReply.status==E_INVALIDARG && queued==before);
        CHECK(!memcmp(&previous,&ctx.displayRequest,sizeof(previous)));
    }
    p.width=1920; p.height=1080;
    proc.gpuResizeSupported=FALSE;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==E_INVALIDARG);
    proc.gpuResizeSupported=TRUE; p.version--;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==E_INVALIDARG);
    p.version=DISPLAY_GPU_VERSION; p.session++;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==E_INVALIDARG);
    p.session=42; ctx.stopping=TRUE;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==E_INVALIDARG);
    ctx.stopping=FALSE; ctx.hDisplayWork=NULL;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==E_INVALIDARG);
    ctx.hDisplayWork=(HANDLE)1; memoryTransport=TRUE;
    p.slot=DISPLAY_SIZE_WINDOW; p.width=640; p.height=480;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==E_INVALIDARG);
    p.width=1024; p.height=768;
    CHECK(VddHandleResize(&proc,&conn,p) && capturedReply.status==S_OK);
    puts("Actual VDD resize handler bounds, custom modes, rejection preservation and legacy IVSHMEM checks passed.");
}

