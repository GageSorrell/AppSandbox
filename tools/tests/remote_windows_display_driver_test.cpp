/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_display_driver_test
 *
 * @file      remote_windows_display_driver_test.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Real driver arrival/departure/mode/control code, mocked IddCx boundary. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <stdio.h>
#include <stdlib.h>
#include "../transport/asb_private_display.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define NT_SUCCESS(x) ((NTSTATUS)(x) >= 0)
#define VDD_MAX_MODES 64
#define VDD_EDID_BYTES 2048
#define VDD_WIDTH 1920
#define VDD_HEIGHT 1080
struct ResolutionEntry { UINT width=0, height=0, refresh=60; };
struct VDD_DEVICE_CONTEXT;
struct MonitorContextWrapper {
    VDD_DEVICE_CONTEXT* pContext;
    ResolutionEntry modes[VDD_MAX_MODES];
    UINT modeCount;
    BOOL matchWindow, isPrivate;
};
using IDDCX_MONITOR = MonitorContextWrapper*;
struct VDD_PRIVATE_SWAP {};
struct VDD_DEVICE_CONTEXT {
    HANDLE hAdapter;
    IDDCX_MONITOR hMonitor, hPrivateMonitor;
    VDD_PRIVATE_SWAP* pPrivateSwap;
    SRWLOCK privateLock, swapLock;
    GUID privateContainerId;
    LONG stopping, fullModes, gpuModes;
    UINT64 privateToken;
    UINT32 privateState, privateWidth, privateHeight;
};
struct WDF_OBJECT_ATTRIBUTES {};
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a,b) ZeroMemory(a,sizeof(*a))
#define IDDCX_MONITOR_DESCRIPTION_TYPE_EDID 1
struct IDDCX_MONITOR_INFO {
    UINT Size, MonitorType, ConnectorIndex;
    struct { UINT Size, Type, DataSize; BYTE* pData; } MonitorDescription;
    GUID MonitorContainerId;
};
struct IDARG_IN_MONITORCREATE { WDF_OBJECT_ATTRIBUTES* ObjectAttributes; IDDCX_MONITOR_INFO* pMonitorInfo; };
struct IDARG_OUT_MONITORCREATE { IDDCX_MONITOR MonitorObject; };
struct IDARG_OUT_MONITORARRIVAL {};
static NTSTATUS createStatus, arrivalStatus, departureStatus;
static UINT created, deleted, arrived, departed;
static BOOL memoryTransport;
static void VddLog(const char*, ...) {}
struct IDDCX_MONITOR_DESCRIPTION { UINT Size, Type, DataSize; const void* pData; };
#include "vdd_edid.inc"   /* actual VDD_EDID template, VddChecksum, VddBuildEdid, VddReadModes */
static BYTE lastEdid[VDD_EDID_BYTES];
static UINT lastEdidBytes;
static GUID lastContainer;
static BOOL EdidValid(const BYTE* edid, UINT bytes)
{
    for (UINT block=0; block*128<bytes; ++block) {
        BYTE sum=0; for (UINT i=0;i<128;++i) sum=(BYTE)(sum+edid[block*128+i]);
        if (sum) return FALSE;
    }
    return TRUE;
}
static NTSTATUS IddCxMonitorCreate(HANDLE,IDARG_IN_MONITORCREATE* in,IDARG_OUT_MONITORCREATE* out) {
    CHECK(in->pMonitorInfo->ConnectorIndex==1);
    CHECK(in->pMonitorInfo->MonitorDescription.Type==IDDCX_MONITOR_DESCRIPTION_TYPE_EDID);
    lastEdidBytes=in->pMonitorInfo->MonitorDescription.DataSize;
    CHECK(lastEdidBytes>=256 && lastEdidBytes<=VDD_EDID_BYTES);
    memcpy(lastEdid,in->pMonitorInfo->MonitorDescription.pData,lastEdidBytes);
    lastContainer=in->pMonitorInfo->MonitorContainerId;
    if (!NT_SUCCESS(createStatus)) return createStatus;
    ++created; out->MonitorObject=new MonitorContextWrapper{}; return STATUS_SUCCESS;
}
static NTSTATUS IddCxMonitorArrival(IDDCX_MONITOR,IDARG_OUT_MONITORARRIVAL*) { ++arrived; return arrivalStatus; }
static NTSTATUS IddCxMonitorDeparture(IDDCX_MONITOR m) {
    ++departed; if (NT_SUCCESS(departureStatus)) { delete m; ++deleted; } return departureStatus;
}
static void WdfObjectDelete(IDDCX_MONITOR m) { delete m; ++deleted; }
static MonitorContextWrapper* WdfObjectGet_MonitorContextWrapper(IDDCX_MONITOR m) { return m; }
static void VddDestroyPrivateSwap(VDD_PRIVATE_SWAP*) {}
static BOOL asb_transport_is_ivshmem() { return memoryTransport; }
#include "vdd_private_monitor.inc"
#include "vdd_target_modes.inc"
#include "../vdd/private_control.inl"
static void CheckPrivateEdid(UINT width, UINT height)
{
    /* Distinct PnP identity: manufacturer ASB, product 0x0002 (desktop is 0x0001),
       serial 2; same monitor name so the guest display watcher keeps it. */
    CHECK(EdidValid(lastEdid,lastEdidBytes));
    CHECK(lastEdid[8]==0x06 && lastEdid[9]==0x62 && lastEdid[10]==0x02 && lastEdid[11]==0x00);
    CHECK(lastEdid[12]==0x02 && VDD_EDID[10]==0x01 && VDD_EDID[11]==0x00);
    CHECK(!memcmp(lastEdid+72,VDD_EDID+72,18)); /* "AppSandboxVDD" name descriptor */
    IDDCX_MONITOR_DESCRIPTION description={sizeof(description),IDDCX_MONITOR_DESCRIPTION_TYPE_EDID,lastEdidBytes,lastEdid};
    ResolutionEntry parsed[VDD_MAX_MODES];
    CHECK(VddReadModes(&description,parsed)==1);
    CHECK(parsed[0].width==width && parsed[0].height==height && parsed[0].refresh==60);
}
int main()
{
    static_assert(sizeof(AsbPrivateDisplayRequest)==32 && sizeof(AsbPrivateDisplayReply)==32);
    CHECK(asb_display_role(L"\\\\?\\DISPLAY#ASB0001#abc")==ASB_DISPLAY_DESKTOP);
    CHECK(asb_display_role(L"MONITOR\\asb0002\\abc")==ASB_DISPLAY_PRIVATE);
    CHECK(!asb_display_role(L"MONITOR\\ASB00021\\abc"));
    CHECK(!asb_display_role(L"ASB"));
    VDD_DEVICE_CONTEXT ctx={};
    ctx.hAdapter=(HANDLE)1;
    ctx.privateContainerId={0x12345678,1,2,{3,4,5,6,7,8,9,10}};
    MonitorContextWrapper desktop={}; desktop.pContext=&ctx;
    desktop.modeCount=2; desktop.modes[0]={1920,1080,60}; desktop.modes[1]={3840,2160,120};
    ctx.hMonitor=&desktop;
    AsbPrivateDisplayRequest command={sizeof(command),1,ASB_PRIVATE_ACTIVATE,2560,1440,0,42};
    AsbPrivateDisplayReply reply={};
    createStatus=STATUS_UNSUCCESSFUL;
    CHECK(VddPrivateCommand(&ctx,&command,&reply)==createStatus && !ctx.privateToken && !ctx.hPrivateMonitor);
    createStatus=STATUS_SUCCESS; arrivalStatus=STATUS_UNSUCCESSFUL;
    CHECK(VddPrivateCommand(&ctx,&command,&reply)==arrivalStatus && !ctx.privateToken && !ctx.hPrivateMonitor);
    CHECK(created==deleted);
    arrivalStatus=STATUS_SUCCESS;
    for (int cycle=0;cycle<20;++cycle) {
        CHECK(NT_SUCCESS(VddPrivateCommand(&ctx,&command,&reply)) && reply.state==ASB_PRIVATE_READY);
        IDDCX_MONITOR monitor=ctx.hPrivateMonitor;
        CHECK(monitor && ctx.hMonitor==&desktop && monitor->isPrivate && monitor->pContext==&ctx);
        CheckPrivateEdid(command.width,command.height);
        CHECK(IsEqualGUID(lastContainer,ctx.privateContainerId));
        CHECK(monitor->modes[0].width==command.width && monitor->modes[0].height==command.height);
        ResolutionEntry modes[VDD_MAX_MODES];
        for (LONG full=0;full<=1;++full) for (LONG gpu=0;gpu<=1;++gpu) {
            ctx.fullModes=full; ctx.gpuModes=gpu;
            CHECK(VddTargetResolutions(monitor,modes)==1 && modes[0].width==command.width);
            CHECK(VddTargetResolutions(&desktop,modes)==(UINT)(full && gpu ? 2 : 1));
        }
        UINT before=created;
        CHECK(NT_SUCCESS(VddPrivateCommand(&ctx,&command,&reply)) && created==before);
        command.token=43; CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_ACCESS_DENIED); command.token=42;
        command.operation=ASB_PRIVATE_REMOVE;
        CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_DEVICE_BUSY && ctx.hPrivateMonitor==monitor);
        command.operation=ASB_PRIVATE_RESTORE;
        CHECK(NT_SUCCESS(VddPrivateCommand(&ctx,&command,&reply)));
        command.operation=ASB_PRIVATE_ACTIVATE;
        CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_DEVICE_BUSY);
        command.operation=ASB_PRIVATE_REMOVE; departureStatus=STATUS_UNSUCCESSFUL;
        CHECK(VddPrivateCommand(&ctx,&command,&reply)==departureStatus && ctx.hPrivateMonitor==monitor && ctx.privateToken==42);
        departureStatus=STATUS_SUCCESS;
        CHECK(NT_SUCCESS(VddPrivateCommand(&ctx,&command,&reply)) && !ctx.hPrivateMonitor && !ctx.privateToken);
        CHECK(ctx.hMonitor==&desktop && desktop.modes[1].width==3840);
        command.operation=ASB_PRIVATE_ACTIVATE;
        command.width=cycle%2 ? 2560 : 1920; command.height=cycle%2 ? 1440 : 1080;
    }
    command.width=7681; CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_NOT_SUPPORTED);
    command.width=639; command.height=480; CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_NOT_SUPPORTED);
    CHECK(!ctx.hPrivateMonitor && !ctx.privateToken);
    command.width=1920; command.height=1080; memoryTransport=TRUE;
    CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_NOT_SUPPORTED);
    memoryTransport=FALSE; command.version=2;
    CHECK(VddPrivateCommand(&ctx,&command,&reply)==STATUS_INVALID_PARAMETER);
    CHECK(created==deleted && arrived>20 && departed==40);
    puts("PASS actual VDD private lifecycle, identity, mode isolation, failure/retry and restoration gate");
}
