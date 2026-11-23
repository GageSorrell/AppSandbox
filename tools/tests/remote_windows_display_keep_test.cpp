/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_display_keep_test
 *
 * @file      remote_windows_display_keep_test.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Real VDD host-connection continuity code (VddKeepClient/VddAdoptKeptClient/
   VddDropKeptClient) against a minimal swap-proc model. The private monitor's
   arrival/departure re-commits modes and IddCx reassigns the desktop swap chain; the
   host frame connection must survive that without a reconnect, but only on the CPU path. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/core/protocol.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define DISPLAY_GPU_SLOTS_TEST DISPLAY_GPU_SLOTS
struct AsbConn { unsigned long long socket; BOOL closed; };
using IDDCX_MONITOR = void*;
struct VDD_GPU_SLOT { void* pTexture; void* pMutex; HANDLE hTexture; BOOL bOfferSeen; UINT64 sequence; };
struct VDD_SWAP_PROC {
    IDDCX_MONITOR hMonitor;
    AsbConn* volatile hClientConn;
    AsbConn* volatile hPendingConn;
    AsbConn* hGpuIoConn;
    BOOL bGpuPoolReady, bGpuPoolFailed, bGpuCapsSent;
    volatile LONG gpuIoFailed;
    VDD_GPU_SLOT gpuSlots[DISPLAY_GPU_SLOTS];
    UINT64 gpuSession;
    BOOL gpuResizeSupported, cpuResizeSupported, gpuScreenshotSupported;
    DisplayGpuPacket gpuControl;
    UINT gpuControlBytes;
};
struct VDD_KEPT_CLIENT {
    AsbConn* conn; IDDCX_MONITOR monitor; ULONGLONG tick; UINT64 gpuSession;
    BOOL bGpuCapsSent, gpuResizeSupported, cpuResizeSupported, gpuScreenshotSupported;
    DisplayGpuPacket gpuControl; UINT gpuControlBytes;
};
struct VDD_DEVICE_CONTEXT { VDD_KEPT_CLIENT keptClient; };
static ULONGLONG g_now = 100000;
#define GetTickCount64() g_now
static int g_closed;
static void VddLog(const char*, ...) {}
static unsigned long long asb_conn_socket_u64(AsbConn* c) { return c->socket; }
static void VddCloseConn(AsbConn* volatile* pc) { if (*pc) { (*pc)->closed = TRUE; ++g_closed; } *pc = NULL; }
#include "vdd_keep_client.inc"

static VDD_SWAP_PROC Proc(AsbConn* conn, IDDCX_MONITOR monitor)
{
    VDD_SWAP_PROC proc = {};
    proc.hMonitor = monitor;
    proc.hClientConn = conn;
    proc.gpuSession = 77;
    proc.bGpuCapsSent = TRUE;
    proc.cpuResizeSupported = TRUE;
    proc.gpuScreenshotSupported = TRUE;
    proc.gpuControlBytes = 5;
    proc.gpuControl.magic = DISPLAY_GPU_MAGIC;
    return proc;
}

int main()
{
    IDDCX_MONITOR desktop = (void*)0x10, recreated = (void*)0x20;
    VDD_DEVICE_CONTEXT ctx = {};

    /* CPU path: kept with its negotiated state and adopted by the next swap chain. */
    AsbConn conn = {42, FALSE};
    VDD_SWAP_PROC old = Proc(&conn, desktop);
    VDD_KEPT_CLIENT keep = {};
    CHECK(VddKeepClient(&old, &keep) && !old.hClientConn && keep.conn == &conn && keep.monitor == desktop);
    ctx.keptClient = keep;
    VDD_SWAP_PROC next = {};
    g_now += 250;
    VddAdoptKeptClient(&ctx, &next, desktop);
    CHECK(next.hClientConn == &conn && !conn.closed && !ctx.keptClient.conn);
    CHECK(next.gpuSession == 77 && next.bGpuCapsSent && next.cpuResizeSupported && next.gpuScreenshotSupported);
    CHECK(next.gpuControlBytes == 5 && next.gpuControl.magic == DISPLAY_GPU_MAGIC);

    /* Not kept: GPU texture pool/offers/I-O, failed pool, newer pending host connection,
       ivshmem slot, or no connection. Those keep today's close-and-reconnect. */
    for (int scenario = 0; scenario < 8; ++scenario) {
        AsbConn c = {7, FALSE}, pending = {8, FALSE}, io = {9, FALSE};
        VDD_SWAP_PROC p = Proc(&c, desktop);
        switch (scenario) {
        case 0: p.bGpuPoolReady = TRUE; break;
        case 1: p.bGpuPoolFailed = TRUE; break;
        case 2: p.gpuSlots[1].bOfferSeen = TRUE; break;
        case 3: p.gpuSlots[0].hTexture = (HANDLE)4; break;
        case 4: p.hPendingConn = &pending; break;
        case 5: p.hGpuIoConn = &io; break;
        case 6: c.socket = ~0ull; break;          /* ivshmem: single shared slot */
        case 7: p.hClientConn = NULL; break;
        }
        VDD_KEPT_CLIENT k = {};
        CHECK(!VddKeepClient(&p, &k) && !k.conn);
        CHECK(scenario == 7 || p.hClientConn == &c);
    }
    VDD_SWAP_PROC failedIo = Proc(&conn, desktop);
    InterlockedExchange(&failedIo.gpuIoFailed, 1);
    CHECK(!VddKeepClient(&failedIo, &keep));
    CHECK(!VddKeepClient(&old, nullptr));

    /* A recreated (different) monitor or an expired keep closes the connection. */
    AsbConn a = {1, FALSE}, b = {2, FALSE};
    VDD_SWAP_PROC pa = Proc(&a, desktop), pb = Proc(&b, desktop), fresh = {};
    g_closed = 0;
    CHECK(VddKeepClient(&pa, &ctx.keptClient));
    VddAdoptKeptClient(&ctx, &fresh, recreated);
    CHECK(!fresh.hClientConn && a.closed && g_closed == 1 && !ctx.keptClient.conn);
    CHECK(VddKeepClient(&pb, &ctx.keptClient));
    g_now += 10001;
    VddAdoptKeptClient(&ctx, &fresh, desktop);
    CHECK(!fresh.hClientConn && b.closed && g_closed == 2);

    /* Explicit drop (monitor recreated / stopping) closes; dropping nothing is a no-op. */
    AsbConn d = {3, FALSE};
    VDD_SWAP_PROC pd = Proc(&d, desktop);
    CHECK(VddKeepClient(&pd, &ctx.keptClient));
    VddDropKeptClient(&ctx, "monitor recreated");
    CHECK(d.closed && !ctx.keptClient.conn && g_closed == 3);
    VddDropKeptClient(&ctx, "stopping");
    CHECK(g_closed == 3);
    puts("PASS VDD host frame connection kept across desktop swap-chain reassignment (CPU path only)");
    return 0;
}
