/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_frames_test
 *
 * @file      remote_windows_frames_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows frame channel model tests (AppSandbox-eh6.5).
   Codec size/length validation, negotiation, stale generations, queue/memory bounds, GPU pool
   lifetimes, popup association/metadata and teardown - all through the real codec and the real
   guest (AsbRwTx) and host (AsbRwRx) state machines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../transport/asb_remote_windows.h"
#include "../transport/asb_rw_frames.h"

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static uint8_t g_buf[ASB_RW_HEADER_BYTES + 4096 + 64 * 64 * 4];
static AsbRwPayload g_msg, g_out;
static AsbRwTx g_tx;
static AsbRwRx g_rx;

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static size_t encode(uint16_t type, const AsbRwPayload *m)
{
    size_t n = 0;
    uint32_t r = asb_rw_encode(type, 0, m, g_buf, sizeof(g_buf), &n);
    if (r) printf("  encode %u -> %s\n", type, asb_rw_error_name(r));
    return r ? 0 : n;
}

static uint32_t decode(size_t n)
{
    AsbRwHeader h;
    return asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, n, &h, &g_out, NULL, NULL);
}

static AsbRwFrameHeader cpu_frame(uint64_t surface, uint64_t root, uint32_t w, uint32_t h)
{
    AsbRwFrameHeader f;
    memset(&f, 0, sizeof(f));
    f.surface_id = surface; f.root_id = root; f.generation = 1; f.frame_seq = 1;
    f.kind = surface == root ? ASB_RW_SURFACE_ROOT : ASB_RW_SURFACE_MENU;
    f.rect.left = 100; f.rect.top = 100; f.rect.right = 100 + (int32_t)w; f.rect.bottom = 100 + (int32_t)h;
    f.width = w; f.height = h; f.stride = w * 4; f.format = ASB_RW_FORMAT_BGRA8;
    f.transport = ASB_RW_TRANSPORT_CPU; f.data_bytes = w * h * 4; f.flags = ASB_RW_FRAME_PREMULTIPLIED;
    return f;
}

/* ============================================================ codec */

static void test_codec(void)
{
    size_t n;
    AsbRwHeader h;
    const uint8_t *bulk = NULL;
    size_t bulk_len = 0;

    /* FRAMES_HELLO round trip; guests must name their PID, hosts must not. */
    memset(&g_msg, 0, sizeof(g_msg));
    asb_rw_frames_hello_init(&g_msg.frames_hello, ASB_RW_ROLE_GUEST, 0x1234, ASB_RW_TRANSPORT_GPU, 3, 64u << 20);
    g_msg.frames_hello.process_id = 4242; g_msg.frames_hello.vendor_id = 0x10DE; g_msg.frames_hello.device_id = 0x2684;
    n = encode(ASB_RW_FRAMES_HELLO, &g_msg);
    CHECK(n > 0);
    CHECK_EQ(decode(n), ASB_RW_OK);
    CHECK(g_out.frames_hello.process_id == 4242 && g_out.frames_hello.transports == 3 && g_out.frames_hello.slots == 3);
    g_msg.frames_hello.process_id = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAMES_HELLO, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frames_hello.process_id = 1; g_msg.frames_hello.slots = ASB_RW_MAX_FRAME_SLOTS + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAMES_HELLO, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frames_hello.slots = 2; g_msg.frames_hello.transports = ASB_RW_TRANSPORT_GPU;   /* CPU is mandatory */
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAMES_HELLO, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frames_hello.transports = 3; g_msg.frames_hello.max_cpu_bytes = 100;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAMES_HELLO, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frames_hello.max_cpu_bytes = 1 << 20;
    n = encode(ASB_RW_FRAMES_HELLO, &g_msg);
    put_u32(g_buf + 16 + 4, 0);                 /* session tag 0 on the wire */
    CHECK_EQ(decode(n), ASB_RW_ERR_MALFORMED);

    /* Pool request / slot / status / surface end. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.pool_request.surface_id = 7; g_msg.pool_request.generation = 2;
    g_msg.pool_request.width = 640; g_msg.pool_request.height = 448; g_msg.pool_request.slots = 3;
    n = encode(ASB_RW_POOL_REQUEST, &g_msg);
    CHECK_EQ(decode(n), ASB_RW_OK);
    CHECK(g_out.pool_request.width == 640 && g_out.pool_request.slots == 3);
    g_msg.pool_request.slots = 0; g_msg.pool_request.width = 0;            /* CPU announcement */
    CHECK_EQ(decode(encode(ASB_RW_POOL_REQUEST, &g_msg)), ASB_RW_OK);
    g_msg.pool_request.slots = 2;                                            /* GPU pool of size 0 */
    CHECK_EQ(asb_rw_encode(ASB_RW_POOL_REQUEST, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.pool_request.width = ASB_RW_MAX_FRAME_WIDTH + 64; g_msg.pool_request.height = 64;
    CHECK_EQ(asb_rw_encode(ASB_RW_POOL_REQUEST, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.pool_request.width = 64; g_msg.pool_request.generation = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_POOL_REQUEST, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.pool_slot.surface_id = 7; g_msg.pool_slot.generation = 2; g_msg.pool_slot.slot = 2; g_msg.pool_slot.slots = 3;
    g_msg.pool_slot.width = 640; g_msg.pool_slot.height = 448; g_msg.pool_slot.handle = 0x1C4;
    n = encode(ASB_RW_POOL_SLOT, &g_msg);
    CHECK_EQ(decode(n), ASB_RW_OK);
    CHECK(g_out.pool_slot.handle == 0x1C4);
    g_msg.pool_slot.slot = 3;                                                /* slot outside the pool */
    CHECK_EQ(asb_rw_encode(ASB_RW_POOL_SLOT, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.pool_slot.slot = 0; g_msg.pool_slot.status = 0x80004005u;          /* failure must not carry a handle */
    CHECK_EQ(asb_rw_encode(ASB_RW_POOL_SLOT, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.pool_slot.handle = 0;
    CHECK_EQ(decode(encode(ASB_RW_POOL_SLOT, &g_msg)), ASB_RW_OK);
    g_msg.pool_slot.status = 0;                                              /* success needs a handle */
    CHECK_EQ(asb_rw_encode(ASB_RW_POOL_SLOT, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.surface_end.surface_id = 7; g_msg.surface_end.generation = 2; g_msg.surface_end.reason = ASB_RW_END_CLOSED;
    CHECK_EQ(decode(encode(ASB_RW_SURFACE_END, &g_msg)), ASB_RW_OK);
    g_msg.surface_end.reason = 99;
    CHECK_EQ(asb_rw_encode(ASB_RW_SURFACE_END, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.frame_release.surface_id = 7; g_msg.frame_release.generation = 2; g_msg.frame_release.slot = 1; g_msg.frame_release.frame_seq = 9;
    CHECK_EQ(decode(encode(ASB_RW_FRAME_RELEASE, &g_msg)), ASB_RW_OK);
    CHECK(g_out.frame_release.generation == 2 && g_out.frame_release.slot == 1);
    g_msg.frame_release.slot = ASB_RW_MAX_FRAME_SLOTS;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME_RELEASE, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame_release.slot = 0; g_msg.frame_release.generation = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME_RELEASE, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    /* CPU frame: exact length, metadata round trip, pixels out of band. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.frame = cpu_frame(0x51, 0x50, 8, 4);
    g_msg.frame.z = 3; g_msg.frame.rect.left = -40;   /* popup extends left of the root */
    size_t hn = encode(ASB_RW_FRAME, &g_msg);
    CHECK(hn > 0);
    for (uint32_t i = 0; i < 128; ++i) g_buf[hn + i] = (uint8_t)i;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + 128, &h, &g_out, &bulk, &bulk_len), ASB_RW_OK);
    CHECK(bulk_len == 128 && bulk[127] == 127 && g_out.frame.kind == ASB_RW_SURFACE_MENU && g_out.frame.z == 3 &&
          g_out.frame.rect.left == -40 && g_out.frame.root_id == 0x50 && g_out.frame.flags == ASB_RW_FRAME_PREMULTIPLIED);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + 127, &h, &g_out, NULL, NULL), ASB_RW_ERR_NEED_MORE);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + 129, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    /* Declared pixels disagreeing with stride*height are rejected on both sides. */
    put_u32(g_buf + hn - 4, 127);
    put_u32(g_buf + 12, (uint32_t)(hn - 16 + 127));
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + 127, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    g_msg.frame.data_bytes = 127;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame.data_bytes = 128; g_msg.frame.stride = 30;   /* not a multiple of 4 */
    g_msg.frame.data_bytes = 120;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x51, 0x50, 8, 4);
    g_msg.frame.kind = ASB_RW_SURFACE_ROOT;                   /* a root must be its own root */
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.kind = ASB_RW_SURFACE_POPUP;                  /* a popup must not be its own root */
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.generation = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.flags = 0x80;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.z = ASB_RW_MAX_SURFACES;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.slot = ASB_RW_MAX_FRAME_SLOTS;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.rect.right = g_msg.frame.rect.left - 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.height = ASB_RW_MAX_FRAME_HEIGHT + 1;
    g_msg.frame.data_bytes = g_msg.frame.stride * g_msg.frame.height;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    /* GPU frames carry no pixels. */
    g_msg.frame = cpu_frame(0x50, 0x50, 8, 4);
    g_msg.frame.transport = ASB_RW_TRANSPORT_GPU; g_msg.frame.stride = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame.data_bytes = 0;
    CHECK_EQ(decode(encode(ASB_RW_FRAME, &g_msg)), ASB_RW_OK);

    /* Oversized frame payloads are rejected from the header, before any allocation. */
    memset(g_buf, 0, 16);
    put_u32(g_buf, ASB_RW_MAGIC); g_buf[4] = 1; g_buf[6] = ASB_RW_FRAME;
    put_u32(g_buf + 12, ASB_RW_MAX_FRAMES_PAYLOAD + 1);
    size_t total = 0;
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_FRAMES, g_buf, 16, &total), ASB_RW_ERR_TOO_LARGE);
    put_u32(g_buf + 12, ASB_RW_MAX_FRAMES_PAYLOAD);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_FRAMES, g_buf, 16, &total), ASB_RW_OK);
    CHECK_EQ(total, 16 + ASB_RW_MAX_FRAMES_PAYLOAD);
    /* Frame messages belong on channel 10; ERROR is accepted on every channel. */
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_CONTROL, g_buf, 16, &total), ASB_RW_ERR_WRONG_CHANNEL);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.error.code = ASB_RW_ERR_STALE_ID;
    n = encode(ASB_RW_ERROR, &g_msg);
    CHECK_EQ(decode(n), ASB_RW_OK);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_FILES, g_buf, n, &total), ASB_RW_OK);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_POOL_REQUEST), ASB_CH_RW_FRAMES);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_SURFACE_END), ASB_CH_RW_FRAMES);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.pool_status.surface_id = 1; g_msg.pool_status.generation = 1;
    n = encode(ASB_RW_POOL_STATUS, &g_msg);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_CONTROL, g_buf, n, &total), ASB_RW_ERR_WRONG_CHANNEL);
    /* Truncation and trailing bytes for every fixed-size frame-channel message. */
    static const uint16_t types[] = { ASB_RW_FRAME_RELEASE, ASB_RW_POOL_REQUEST, ASB_RW_POOL_STATUS, ASB_RW_SURFACE_END };
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.frame_release.surface_id = 1;   /* first u64 of each payload */
        g_msg.pool_request.generation = 1; g_msg.frame_release.generation = 1;
        g_msg.pool_status.generation = 1; g_msg.surface_end.reason = ASB_RW_END_CLOSED; g_msg.surface_end.surface_id = 1;
        if (types[t] == ASB_RW_POOL_REQUEST) { memset(&g_msg, 0, sizeof(g_msg)); g_msg.pool_request.surface_id = 1; g_msg.pool_request.generation = 1; }
        if (types[t] == ASB_RW_POOL_STATUS) { memset(&g_msg, 0, sizeof(g_msg)); g_msg.pool_status.surface_id = 1; g_msg.pool_status.generation = 1; }
        if (types[t] == ASB_RW_SURFACE_END) { memset(&g_msg, 0, sizeof(g_msg)); g_msg.surface_end.surface_id = 1; g_msg.surface_end.reason = 1; }
        if (types[t] == ASB_RW_FRAME_RELEASE) { memset(&g_msg, 0, sizeof(g_msg)); g_msg.frame_release.surface_id = 1; g_msg.frame_release.generation = 1; }
        n = encode(types[t], &g_msg);
        CHECK(n > 16);
        CHECK_EQ(decode(n), ASB_RW_OK);
        put_u32(g_buf + 12, (uint32_t)(n - 16 - 1));
        CHECK_EQ(decode(n - 1), ASB_RW_ERR_MALFORMED);
        put_u32(g_buf + 12, (uint32_t)(n - 16 + 1));
        g_buf[n] = 0;
        CHECK_EQ(decode(n + 1), ASB_RW_ERR_MALFORMED);
    }
}

/* ============================================================ negotiation */

static AsbRwFramesHello hello(uint32_t role, uint32_t tag, uint32_t transports, uint32_t vendor, uint32_t device)
{
    AsbRwFramesHello h;
    asb_rw_frames_hello_init(&h, role, tag, transports, role == ASB_RW_ROLE_GUEST ? 3 : 2, 64u << 20);
    h.process_id = role == ASB_RW_ROLE_GUEST ? 900 : 0;
    h.vendor_id = vendor;
    h.device_id = device;
    return h;
}

static void test_negotiation(void)
{
    AsbRwFramesHello g = hello(ASB_RW_ROLE_GUEST, 5, ASB_RW_TRANSPORT_GPU, 0x10DE, 0x2684);
    AsbRwFramesHello h = hello(ASB_RW_ROLE_HOST, 5, ASB_RW_TRANSPORT_GPU, 0x10DE, 0x2684);
    AsbRwFramesAgreement a = asb_rw_frames_negotiate(&h, &g, 5), b = asb_rw_frames_negotiate(&g, &h, 5);
    CHECK(a.error == ASB_RW_OK && a.gpu == 1 && a.slots == 2 && a.guest_pid == 900);
    CHECK(!memcmp(&a, &b, sizeof(a)));                      /* both ends agree */
    /* Forced CPU on the host, adapter mismatch, Basic Render Driver: CPU. */
    h.transports = ASB_RW_TRANSPORT_CPU;
    CHECK(asb_rw_frames_negotiate(&h, &g, 5).gpu == 0);
    h = hello(ASB_RW_ROLE_HOST, 5, ASB_RW_TRANSPORT_GPU, 0x10DE, 0x2204);
    CHECK(asb_rw_frames_negotiate(&h, &g, 5).gpu == 0);
    g = hello(ASB_RW_ROLE_GUEST, 5, ASB_RW_TRANSPORT_GPU, 0x1414, 0x008C);
    h = hello(ASB_RW_ROLE_HOST, 5, ASB_RW_TRANSPORT_GPU, 0x1414, 0x008C);
    CHECK(asb_rw_frames_negotiate(&h, &g, 5).gpu == 0);
    g = hello(ASB_RW_ROLE_GUEST, 5, ASB_RW_TRANSPORT_GPU, 0, 0);
    h = hello(ASB_RW_ROLE_HOST, 5, ASB_RW_TRANSPORT_GPU, 0, 0);
    CHECK(asb_rw_frames_negotiate(&h, &g, 5).gpu == 0);
    /* A frame channel from a previous control session (or for another one) is refused. */
    g = hello(ASB_RW_ROLE_GUEST, 6, ASB_RW_TRANSPORT_CPU, 0, 0);
    h = hello(ASB_RW_ROLE_HOST, 5, ASB_RW_TRANSPORT_CPU, 0, 0);
    CHECK_EQ(asb_rw_frames_negotiate(&g, &h, 6).error, ASB_RW_ERR_STALE_ID);
    g.session_tag = 5;
    CHECK_EQ(asb_rw_frames_negotiate(&g, &h, 6).error, ASB_RW_ERR_STALE_ID);
    CHECK_EQ(asb_rw_frames_negotiate(&g, &h, 5).error, ASB_RW_OK);
    CHECK_EQ(asb_rw_frames_negotiate(&g, &g, 5).error, ASB_RW_ERR_MALFORMED);   /* same role */
}

/* ============================================================ end to end */

/* Both state machines connected through the real codec. Releases flow host -> guest. */
static AsbRwFramesAgreement agreement(int gpu, uint32_t slots, uint32_t max_cpu)
{
    AsbRwFramesAgreement a;
    memset(&a, 0, sizeof(a));
    a.gpu = (uint32_t)gpu; a.slots = slots; a.max_cpu_bytes = max_cpu; a.guest_pid = 900;
    return a;
}

static uint32_t deliver_releases(const AsbRwRxReleases *r)
{
    uint32_t result = ASB_RW_OK;
    for (uint32_t i = 0; i < r->count; ++i) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.frame_release = r->items[i];
        size_t n = encode(ASB_RW_FRAME_RELEASE, &g_msg);
        if (!n || decode(n) != ASB_RW_OK) return ASB_RW_ERR_INTERNAL;
        uint32_t x = asb_rw_tx_release(&g_tx, &g_out.frame_release);
        if (x == ASB_RW_ERR_MALFORMED) result = x;
    }
    return result;
}

/* Guest announces a generation if needed (and the host allocates/accepts a pool when asked). */
static uint32_t guest_prepare(int index, uint32_t w, uint32_t h, int pool_ok)
{
    AsbRwPoolRequest req;
    AsbRwRxReleases rel = { 0 };
    if (!asb_rw_tx_prepare(&g_tx, index, w, h, &req)) return ASB_RW_OK;
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.pool_request = req;
    size_t n = encode(ASB_RW_POOL_REQUEST, &g_msg);
    if (!n || decode(n)) return ASB_RW_ERR_INTERNAL;
    int ri, allocate;
    uint32_t r = asb_rw_rx_pool_request(&g_rx, &g_out.pool_request, &ri, &allocate, &rel);
    if (r) return r;
    deliver_releases(&rel);
    if (allocate) {
        /* POOL_SLOTs reach the guest; it opens them (or fails) and reports POOL_STATUS. */
        asb_rw_tx_pool_result(&g_tx, index, req.generation, pool_ok);
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.pool_status.surface_id = req.surface_id; g_msg.pool_status.generation = req.generation;
        g_msg.pool_status.status = pool_ok ? 0 : 0x80070057u;
        n = encode(ASB_RW_POOL_STATUS, &g_msg);
        if (!n || decode(n)) return ASB_RW_ERR_INTERNAL;
        int ready = 0;
        r = asb_rw_rx_pool_status(&g_rx, &g_out.pool_status, &ri, &ready);
        if (r) return r;
        if (ready != pool_ok) return ASB_RW_ERR_INTERNAL;
    }
    return ASB_RW_OK;
}

/* Guest sends its pending frame if a slot is free. Returns 1 sent, 0 kept pending, <0 error. */
static int guest_send(int index, uint32_t kind, uint32_t z, AsbRwRxReleases *rel, uint32_t *rx_result)
{
    AsbRwTxSurface *s = &g_tx.surfaces[index];
    uint32_t bytes = asb_rw_tx_transport(&g_tx, index) == ASB_RW_TRANSPORT_CPU ? s->width * s->height * 4u : 0u;
    int slot = asb_rw_tx_slot(&g_tx, index, bytes);
    if (slot < 0) return 0;
    memset(&g_msg, 0, sizeof(g_msg));
    AsbRwFrameHeader *f = &g_msg.frame;
    f->kind = kind; f->z = z;
    f->rect.left = 2000; f->rect.top = 300; f->rect.right = 2000 + (int32_t)s->width; f->rect.bottom = 300 + (int32_t)s->height;
    f->stride = s->width * 4u; f->data_bytes = bytes; f->flags = ASB_RW_FRAME_PREMULTIPLIED;
    asb_rw_tx_commit(&g_tx, index, slot, bytes, f);
    size_t hn = 0;
    if (asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &hn)) return -1;
    /* The codec only needs the header to validate lengths; pixels are not materialized here. */
    AsbRwHeader h;
    memset(g_buf + hn, 0x5A, bytes <= sizeof(g_buf) - hn ? bytes : 0);
    if (bytes <= sizeof(g_buf) - hn) {
        if (asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + bytes, &h, &g_out, NULL, NULL)) return -2;
    } else {
        g_out.frame = *f;
    }
    int ri;
    memset(rel, 0, sizeof(*rel));
    *rx_result = asb_rw_rx_frame(&g_rx, &g_out.frame, &ri, rel);
    return 1;
}

static void test_cpu_bounds(void)
{
    AsbRwFramesAgreement a = agreement(0, 2, 64u * 48u * 4u * 3u);   /* room for 3 frames */
    AsbRwRxReleases rel;
    uint32_t rr;
    asb_rw_tx_init(&g_tx, &a);
    asb_rw_rx_init(&g_rx, &a);
    int i = asb_rw_tx_add(&g_tx, 0x50, 0x50);
    CHECK(i >= 0);
    /* A frame before the generation is announced has no slot. */
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(asb_rw_tx_slot(&g_tx, i, 64 * 48 * 4), -1);
    CHECK_EQ(guest_prepare(i, 64, 48, 0), ASB_RW_OK);
    CHECK_EQ(g_tx.surfaces[i].generation, 1);
    CHECK_EQ(g_rx.surfaces[asb_rw_rx_find(&g_rx, 0x50)].generation, 1);

    /* The host never consumes: capture 100 frames. In flight stays within the agreed slots,
       the host keeps one pending frame and releases every superseded one; the guest discards
       captures that arrive while all slots are busy. */
    uint32_t sent = 0, max_in_flight = 0;
    for (int k = 0; k < 100; ++k) {
        asb_rw_tx_captured(&g_tx, i);
        int r = guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr);
        if (r == 1) {
            ++sent;
            CHECK_EQ(rr, ASB_RW_OK);
            /* Delay delivery of releases for every other frame to exercise the slot bound. */
            if (k % 3) CHECK_EQ(deliver_releases(&rel), ASB_RW_OK);
            else {
                AsbRwRxReleases later = rel;
                asb_rw_tx_captured(&g_tx, i);
                int again = guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr);
                CHECK(again >= 0);
                if (again == 1) { ++sent; CHECK_EQ(rr, ASB_RW_OK); CHECK_EQ(deliver_releases(&rel), ASB_RW_OK); }
                CHECK_EQ(deliver_releases(&later), ASB_RW_OK);
            }
        }
        uint32_t bits = g_tx.surfaces[i].in_flight, n = 0;
        while (bits) { n += bits & 1u; bits >>= 1; }
        if (n > max_in_flight) max_in_flight = n;
        CHECK(g_rx.cpu_bytes <= a.max_cpu_bytes && g_tx.cpu_bytes <= a.max_cpu_bytes);
    }
    CHECK(max_in_flight <= 2 && sent >= 50);
    AsbRwRxSurface *rs = &g_rx.surfaces[asb_rw_rx_find(&g_rx, 0x50)];
    CHECK(rs->pending >= 0 && rs->held < 0 && rs->superseded + 1 == rs->received);
    CHECK_EQ(g_rx.cpu_bytes, 64 * 48 * 4);                 /* only the pending frame is retained */

    /* Consumer: acquire newest, then done -> everything released. */
    AsbRwFrameHeader f;
    memset(&rel, 0, sizeof(rel));
    int ri = asb_rw_rx_find(&g_rx, 0x50);
    CHECK_EQ(asb_rw_rx_acquire(&g_rx, ri, &f, &rel), 1);
    CHECK_EQ(rel.count, 0);
    CHECK_EQ(asb_rw_rx_acquire(&g_rx, ri, &f, &rel), 0);   /* nothing newer */
    CHECK_EQ(asb_rw_rx_done(&g_rx, ri, f.generation, f.slot + 1, &rel), 0);   /* wrong slot */
    CHECK_EQ(asb_rw_rx_done(&g_rx, ri, f.generation, f.slot, &rel), 1);
    CHECK_EQ(deliver_releases(&rel), ASB_RW_OK);
    CHECK_EQ(g_rx.cpu_bytes, 0);
    CHECK_EQ(g_tx.cpu_bytes, 0);
    CHECK_EQ(g_tx.surfaces[i].in_flight, 0);

    /* The guest's CPU budget: a second surface cannot exceed max_cpu_bytes in flight. */
    int j = asb_rw_tx_add(&g_tx, 0x61, 0x50);
    CHECK_EQ(guest_prepare(j, 64, 48, 0), ASB_RW_OK);
    CHECK_EQ(guest_prepare(i, 64, 48, 0), ASB_RW_OK);
    asb_rw_tx_captured(&g_tx, i); CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
    asb_rw_tx_captured(&g_tx, j); CHECK_EQ(guest_send(j, ASB_RW_SURFACE_MENU, 1, &rel, &rr), 1);
    asb_rw_tx_captured(&g_tx, i); CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
    CHECK_EQ(rel.count, 1);   /* host superseded i's first frame ... */
    AsbRwRxReleases held_back = rel;   /* ... but that release has not reached the guest yet */
    asb_rw_tx_captured(&g_tx, j);
    CHECK_EQ(asb_rw_tx_slot(&g_tx, j, 64 * 48 * 4), -1);   /* 3 frames in flight = budget */
    CHECK_EQ(deliver_releases(&held_back), ASB_RW_OK);
    CHECK(asb_rw_tx_slot(&g_tx, j, 64 * 48 * 4) >= 0);

    /* Release validation: a release for a slot that is not in flight is a protocol error,
       a release for an older generation is ignored, a newer one is an error. */
    AsbRwFrameRelease bogus = { 0x50, 99, g_tx.surfaces[i].generation, 3 };
    CHECK_EQ(asb_rw_tx_release(&g_tx, &bogus), ASB_RW_ERR_MALFORMED);
    bogus.generation = g_tx.surfaces[i].generation + 1;
    CHECK_EQ(asb_rw_tx_release(&g_tx, &bogus), ASB_RW_ERR_MALFORMED);
    bogus.generation = g_tx.surfaces[i].generation - 1;
    CHECK_EQ(asb_rw_tx_release(&g_tx, &bogus), ASB_RW_ERR_STALE_ID);
    bogus.surface_id = 0x9999;
    CHECK_EQ(asb_rw_tx_release(&g_tx, &bogus), ASB_RW_ERR_STALE_ID);
}

static void test_generations(void)
{
    AsbRwFramesAgreement a = agreement(0, 2, 64u << 20);
    AsbRwRxReleases rel;
    AsbRwFrameHeader f;
    uint32_t rr;
    asb_rw_tx_init(&g_tx, &a);
    asb_rw_rx_init(&g_rx, &a);
    int i = asb_rw_tx_add(&g_tx, 0x70, 0x70);
    CHECK_EQ(guest_prepare(i, 40, 30, 0), ASB_RW_OK);
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
    int ri = asb_rw_rx_find(&g_rx, 0x70);
    CHECK_EQ(asb_rw_rx_acquire(&g_rx, ri, &f, &rel), 1);   /* consumer displays the 40x30 frame */
    CHECK_EQ(f.generation, 1);
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);   /* a second 40x30 frame pending */

    /* Resize: a new generation starts; the pending old-size frame is released at once, the held
       one stays with the consumer, the guest stops counting both. */
    CHECK_EQ(asb_rw_tx_prepare(&g_tx, i, 40, 30, NULL), 0);          /* same size: same generation */
    CHECK_EQ(guest_prepare(i, 52, 30, 0), ASB_RW_OK);
    CHECK_EQ(g_tx.surfaces[i].generation, 2);
    CHECK_EQ(g_tx.surfaces[i].in_flight, 0);
    AsbRwRxSurface *rs = &g_rx.surfaces[ri];
    CHECK(rs->generation == 2 && rs->pending < 0 && rs->held >= 0 && rs->held_frame.generation == 1);
    CHECK_EQ(g_tx.surfaces[i].stale_releases, 1);                   /* that release was stale */
    /* Old-generation frames that still arrive are released, never displayed. */
    AsbRwFrameHeader old = cpu_frame(0x70, 0x70, 40, 30);
    old.generation = 1; old.slot = 1;
    memset(&rel, 0, sizeof(rel));
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &old, &ri, &rel), ASB_RW_ERR_STALE_ID);
    CHECK(rel.count == 1 && rel.items[0].generation == 1);
    CHECK(rs->pending < 0);
    /* Frames from an unannounced generation are a protocol error. */
    old.generation = 3;
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &old, &ri, &rel), ASB_RW_ERR_MALFORMED);
    /* New-size frame arrives; acquiring it retires the old held frame (stale release). */
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
    CHECK_EQ(rr, ASB_RW_OK);
    memset(&rel, 0, sizeof(rel));
    CHECK_EQ(asb_rw_rx_acquire(&g_rx, ri, &f, &rel), 1);
    CHECK(f.generation == 2 && f.width == 52);
    CHECK(rel.count == 1 && rel.items[0].generation == 1);
    uint32_t stale_before = g_tx.surfaces[i].stale_releases;
    CHECK_EQ(deliver_releases(&rel), ASB_RW_OK);
    CHECK_EQ(g_tx.surfaces[i].stale_releases, stale_before + 1);
    CHECK(g_rx.cpu_bytes == 52u * 30u * 4u);
    /* Generations must only grow. */
    AsbRwPoolRequest again = { 0x70, 2, 0, 0, 0 };
    int idx, alloc;
    CHECK_EQ(asb_rw_rx_pool_request(&g_rx, &again, &idx, &alloc, &rel), ASB_RW_ERR_MALFORMED);
    /* Frame for a surface the host never heard of. */
    AsbRwFrameHeader unknown = cpu_frame(0x71, 0x70, 4, 4);
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &unknown, &idx, &rel), ASB_RW_ERR_MALFORMED);
    /* Root identity of a surface is fixed. */
    AsbRwFrameHeader wrong_root = cpu_frame(0x70, 0x70, 52, 30);
    wrong_root.generation = 2; wrong_root.root_id = 0x70; wrong_root.slot = 1;
    wrong_root.kind = ASB_RW_SURFACE_ROOT;
    AsbRwFrameHeader popup = wrong_root;
    popup.root_id = 0x99; popup.kind = ASB_RW_SURFACE_POPUP;
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &popup, &idx, &rel), ASB_RW_ERR_MALFORMED);
}

static void test_gpu_pools(void)
{
    AsbRwFramesAgreement a = agreement(1, 3, 32u << 20);
    AsbRwRxReleases rel;
    AsbRwFrameHeader f;
    uint32_t rr;
    asb_rw_tx_init(&g_tx, &a);
    asb_rw_rx_init(&g_rx, &a);
    int i = asb_rw_tx_add(&g_tx, 0x80, 0x80);
    AsbRwPoolRequest req;
    CHECK_EQ(asb_rw_tx_prepare(&g_tx, i, 1000, 700, &req), 1);
    CHECK(req.slots == 3 && req.width == 1024 && req.height == 704 && req.generation == 1);
    /* No slot until the pool is ready. */
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(asb_rw_tx_slot(&g_tx, i, 0), -1);
    int ri, alloc;
    memset(&rel, 0, sizeof(rel));
    CHECK_EQ(asb_rw_rx_pool_request(&g_rx, &req, &ri, &alloc, &rel), ASB_RW_OK);
    CHECK_EQ(alloc, 1);
    /* A GPU frame before POOL_STATUS is a protocol error. */
    AsbRwFrameHeader early = cpu_frame(0x80, 0x80, 1000, 700);
    early.transport = ASB_RW_TRANSPORT_GPU; early.stride = 0; early.data_bytes = 0;
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &early, &ri, &rel), ASB_RW_ERR_MALFORMED);
    /* ... so is a CPU frame while a GPU pool is pending. */
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &(AsbRwFrameHeader){ 0 }, &ri, &rel) != ASB_RW_OK, 1);
    AsbRwPoolStatus st = { 0x80, 1, 0 };
    int ready;
    asb_rw_tx_pool_result(&g_tx, i, 1, 1);
    CHECK_EQ(asb_rw_rx_pool_status(&g_rx, &st, &ri, &ready), ASB_RW_OK);
    CHECK_EQ(ready, 1);
    CHECK_EQ(asb_rw_tx_transport(&g_tx, i), ASB_RW_TRANSPORT_GPU);
    /* Three frames fill the pool; a fourth waits. */
    for (int k = 0; k < 3; ++k) {
        asb_rw_tx_captured(&g_tx, i);
        CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
        CHECK_EQ(rr, ASB_RW_OK);
        if (k == 0) {
            memset(&rel, 0, sizeof(rel));
            CHECK_EQ(asb_rw_rx_acquire(&g_rx, ri, &f, &rel), 1);   /* consumer holds slot 0 */
        }
        /* superseded releases are withheld from the guest to fill the pool */
    }
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(asb_rw_tx_slot(&g_tx, i, 0), -1);
    CHECK_EQ(g_tx.cpu_bytes, 0);
    /* Slot reuse before its release is rejected by the host. */
    AsbRwFrameHeader reuse = early;
    reuse.generation = 1; reuse.slot = 0; reuse.frame_seq = 50;
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &reuse, &ri, &rel), ASB_RW_ERR_MALFORMED);
    /* Content larger than the pool, or a slot outside it, is rejected. */
    reuse.slot = 1; reuse.width = 1025;
    AsbRwRxSurface *rs = &g_rx.surfaces[ri];
    rs->slot_state[1] = ASB_RW_RX_FREE;
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &reuse, &ri, &rel), ASB_RW_ERR_MALFORMED);
    /* Shrinking a little keeps the pool; shrinking to less than half starts a new generation. */
    CHECK_EQ(asb_rw_tx_prepare(&g_tx, i, 900, 600, NULL), 0);
    CHECK_EQ(g_tx.surfaces[i].generation, 1);
    CHECK_EQ(asb_rw_tx_prepare(&g_tx, i, 300, 200, &req), 1);
    CHECK(req.generation == 2 && req.width == 320 && req.height == 256 && req.slots == 3);
    /* Growing beyond the pool also starts a new generation; the generation-2 pool result that
       arrives afterwards is stale. */
    CHECK_EQ(asb_rw_tx_prepare(&g_tx, i, 1400, 900, &req), 1);
    CHECK_EQ(asb_rw_tx_pool_result(&g_tx, i, 2, 1), 0);
    CHECK_EQ(asb_rw_tx_pool_result(&g_tx, i, 3, 1), 1);
    /* Host: generation 3 pool; frames of generation 1 the consumer still holds are released as stale. */
    memset(&rel, 0, sizeof(rel));
    CHECK_EQ(asb_rw_rx_pool_request(&g_rx, &req, &ri, &alloc, &rel), ASB_RW_OK);
    CHECK(alloc == 1 && rs->held >= 0 && rs->held_frame.generation == 1 && rs->pending < 0);
    CHECK_EQ(rel.count, 1);   /* the pending generation-1 frame */
    /* Host-side pool failure: this generation is CPU. A late POOL_STATUS(ok) does not revive it. */
    asb_rw_rx_pool_failed(&g_rx, ri, 3);
    st.generation = 3; st.status = 0;
    CHECK_EQ(asb_rw_rx_pool_status(&g_rx, &st, &ri, &ready), ASB_RW_OK);
    CHECK_EQ(ready, 0);
    CHECK_EQ(rs->pool_state, ASB_RW_POOL_FAILED);
    /* Guest learns of the failure (POOL_SLOT with a status) and switches to CPU frames. */
    asb_rw_tx_invalidate(&g_tx, i, 1);
    /* Releases for the invalidated generation (e.g. the consumer's held frame) are stale now. */
    AsbRwFrameRelease old_gen = { 0x80, 1, 3, 0 };
    CHECK_EQ(asb_rw_tx_release(&g_tx, &old_gen), ASB_RW_ERR_STALE_ID);
    CHECK_EQ(guest_prepare(i, 1400, 900, 0), ASB_RW_OK);
    CHECK_EQ(g_tx.surfaces[i].generation, 5);
    CHECK_EQ(asb_rw_tx_transport(&g_tx, i), ASB_RW_TRANSPORT_CPU);
    asb_rw_tx_captured(&g_tx, i);
    CHECK_EQ(guest_send(i, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
    CHECK_EQ(rr, ASB_RW_OK);
    /* Device recovery re-enables GPU pools for the next generation. */
    asb_rw_tx_invalidate(&g_tx, i, 0);
    CHECK_EQ(asb_rw_tx_prepare(&g_tx, i, 1400, 900, &req), 1);
    CHECK(req.generation == 7 && req.slots == 3);
    CHECK_EQ(g_tx.cpu_bytes, 0);
    /* GPU slots without a GPU agreement, or more slots than agreed, are refused by the host. */
    AsbRwRx cpu_only;
    AsbRwFramesAgreement c = agreement(0, 2, 1 << 20);
    asb_rw_rx_init(&cpu_only, &c);
    AsbRwPoolRequest gpu_req = { 0x90, 1, 64, 64, 2 };
    CHECK_EQ(asb_rw_rx_pool_request(&cpu_only, &gpu_req, &ri, &alloc, &rel), ASB_RW_ERR_MALFORMED);
    gpu_req.slots = 4;
    CHECK_EQ(asb_rw_rx_pool_request(&g_rx, &gpu_req, &ri, &alloc, &rel), ASB_RW_ERR_MALFORMED);
}

static void test_teardown_and_limits(void)
{
    AsbRwFramesAgreement a = agreement(0, 2, 64u << 20);
    AsbRwRxReleases rel;
    AsbRwFrameHeader f;
    uint32_t rr;
    asb_rw_tx_init(&g_tx, &a);
    asb_rw_rx_init(&g_rx, &a);
    int root = asb_rw_tx_add(&g_tx, 0xA0, 0xA0), menu = asb_rw_tx_add(&g_tx, 0xA1, 0xA0);
    CHECK_EQ(asb_rw_tx_add(&g_tx, 0xA1, 0xA0), menu);    /* idempotent */
    CHECK_EQ(asb_rw_tx_add(&g_tx, 0, 0xA0), -1);
    CHECK_EQ(guest_prepare(root, 32, 32, 0), ASB_RW_OK);
    CHECK_EQ(guest_prepare(menu, 16, 40, 0), ASB_RW_OK);
    asb_rw_tx_captured(&g_tx, root); CHECK_EQ(guest_send(root, ASB_RW_SURFACE_ROOT, 0, &rel, &rr), 1);
    asb_rw_tx_captured(&g_tx, menu); CHECK_EQ(guest_send(menu, ASB_RW_SURFACE_MENU, 1, &rel, &rr), 1);
    int rm = asb_rw_rx_find(&g_rx, 0xA1);
    CHECK_EQ(asb_rw_rx_acquire(&g_rx, rm, &f, &rel), 1);
    CHECK(f.kind == ASB_RW_SURFACE_MENU && f.z == 1 && f.root_id == 0xA0);
    asb_rw_tx_captured(&g_tx, menu); CHECK_EQ(guest_send(menu, ASB_RW_SURFACE_MENU, 1, &rel, &rr), 1);

    /* The menu closes: SURFACE_END frees everything the host held for it; late releases the
       guest receives for it are ignored; frames for it afterwards are protocol errors. */
    AsbRwSurfaceEnd end;
    CHECK_EQ(asb_rw_tx_remove(&g_tx, menu, ASB_RW_END_CLOSED, &end), 1);
    CHECK(end.surface_id == 0xA1 && end.generation == 1);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.surface_end = end;
    CHECK_EQ(decode(encode(ASB_RW_SURFACE_END, &g_msg)), ASB_RW_OK);
    CHECK_EQ(asb_rw_rx_end(&g_rx, &g_out.surface_end), rm);
    CHECK_EQ(asb_rw_rx_end(&g_rx, &g_out.surface_end), -1);
    CHECK_EQ(g_rx.cpu_bytes, 32u * 32u * 4u);                /* only the root's pending frame */
    CHECK_EQ(g_tx.cpu_bytes, 32u * 32u * 4u);
    AsbRwFrameRelease late = { 0xA1, 1, 1, 0 };
    CHECK_EQ(asb_rw_tx_release(&g_tx, &late), ASB_RW_ERR_STALE_ID);
    AsbRwFrameHeader after = cpu_frame(0xA1, 0xA0, 16, 40);
    CHECK_EQ(asb_rw_rx_frame(&g_rx, &after, &rm, &rel), ASB_RW_ERR_MALFORMED);
    /* A surface that never sent anything needs no SURFACE_END. */
    int quiet = asb_rw_tx_add(&g_tx, 0xA2, 0xA0);
    CHECK_EQ(asb_rw_tx_remove(&g_tx, quiet, ASB_RW_END_UNSELECTED, &end), 0);

    /* Disconnect: both ends start over; nothing stays accounted. */
    asb_rw_tx_init(&g_tx, &a);
    asb_rw_rx_init(&g_rx, &a);
    CHECK(g_tx.count == 0 && g_tx.cpu_bytes == 0 && g_rx.count == 0 && g_rx.cpu_bytes == 0);

    /* Surface tables are bounded identically on both ends. */
    for (uint32_t k = 0; k < ASB_RW_MAX_FRAME_SURFACES; ++k) {
        int t = asb_rw_tx_add(&g_tx, 0x1000 + k, 0x1000);
        CHECK(t >= 0);
        AsbRwPoolRequest req = { 0x1000 + k, 1, 0, 0, 0 };
        int ri, alloc;
        CHECK_EQ(asb_rw_rx_pool_request(&g_rx, &req, &ri, &alloc, &rel), ASB_RW_OK);
    }
    CHECK_EQ(asb_rw_tx_add(&g_tx, 0x9000, 0x1000), -1);
    AsbRwPoolRequest more = { 0x9000, 1, 0, 0, 0 };
    int ri, alloc;
    CHECK_EQ(asb_rw_rx_pool_request(&g_rx, &more, &ri, &alloc, &rel), ASB_RW_ERR_LIMIT);
    CHECK_EQ(g_rx.count, ASB_RW_MAX_FRAME_SURFACES);
}

/* ============================================================ popup association */

static AsbRwWindowFacts window(uint64_t hwnd, uint32_t pid, uint64_t start, const char *cls)
{
    AsbRwWindowFacts w;
    memset(&w, 0, sizeof(w));
    w.identity.hwnd = hwnd; w.identity.pid = pid; w.identity.thread_id = pid + 1; w.identity.process_start = start;
    w.visible = 1;
    w.rect.left = 2100; w.rect.top = 200; w.rect.right = 2400; w.rect.bottom = 500;
    for (uint16_t i = 0; cls[i]; ++i) w.class_name[w.class_len++] = (uint16_t)cls[i];
    w.style = 0x80000000u;   /* WS_POPUP */
    return w;
}

static void test_popup_association(void)
{
    AsbRwRect priv = { 1920, 0, 1920 + 2560, 1440 };
    AsbRwWindowFacts root = window(0x100, 40, 777, "Notepad");
    root.style = 0x14CF0000u;   /* WS_OVERLAPPEDWINDOW | visible */
    root.rect.left = 2000; root.rect.top = 100; root.rect.right = 2640; root.rect.bottom = 520;

    /* Owned menu/tooltip/dialog/popup by ownership, also beyond the root bounds. */
    AsbRwWindowFacts tip = window(0x200, 40, 777, "tooltips_class32");
    tip.owner = 0x100; tip.owner_chain[0] = 0x100; tip.owner_depth = 1;
    tip.rect.left = 2600; tip.rect.right = 2900;              /* overflows the root's right edge */
    CHECK_EQ(asb_rw_surface_association(&tip, &root, &priv), ASB_RW_SURFACE_TOOLTIP);
    AsbRwWindowFacts dialog = window(0x201, 40, 777, "#32770");
    dialog.style = 0x94C80000u; dialog.owner = 0x100; dialog.owner_chain[0] = 0x100; dialog.owner_depth = 1;
    CHECK_EQ(asb_rw_surface_association(&dialog, &root, &priv), ASB_RW_SURFACE_OWNED);
    AsbRwWindowFacts nested = window(0x202, 40, 777, "Popup");
    nested.owner = 0x201; nested.owner_chain[0] = 0x201; nested.owner_chain[1] = 0x100; nested.owner_depth = 2;
    CHECK_EQ(asb_rw_surface_association(&nested, &root, &priv), ASB_RW_SURFACE_POPUP);
    /* Menus are associated through their thread's menu owner even when unowned. */
    AsbRwWindowFacts menu = window(0x203, 40, 777, "#32768");
    menu.menu_owner = 0x100; menu.owner_chain[0] = 0x100; menu.owner_depth = 1;
    CHECK_EQ(asb_rw_surface_association(&menu, &root, &priv), ASB_RW_SURFACE_MENU);
    /* Unowned popup bridges of the root's process on the private display. */
    AsbRwWindowFacts bridge = window(0x204, 40, 777, "Microsoft.UI.Content.PopupWindowSiteBridge");
    CHECK_EQ(asb_rw_surface_association(&bridge, &root, &priv), ASB_RW_SURFACE_POPUP);
    AsbRwWindowFacts umenu = window(0x205, 40, 777, "#32768");   /* no menu owner known */
    CHECK_EQ(asb_rw_surface_association(&umenu, &root, &priv), ASB_RW_SURFACE_MENU);
    /* ... but not when they are on the visible desktop, in another process (or a process that
       reused the PID), owned by something else, captioned/APPWINDOW (independent roots),
       hidden, cloaked, minimized, empty, shadow-only, or the root itself. */
    AsbRwWindowFacts w = bridge;
    w.rect.left = 100; w.rect.right = 400;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.identity.pid = 41;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.identity.process_start = 778;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.owner = 0x999; w.owner_chain[0] = 0x999; w.owner_depth = 1;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.style = 0x14CF0000u;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.ex_style = ASB_RW_WS_EX_APPWINDOW;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.visible = 0;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.cloaked = 1;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.minimized = 1;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = bridge; w.rect.right = w.rect.left;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    w = window(0x206, 40, 777, "SysShadow"); w.owner = 0x100; w.owner_chain[0] = 0x100; w.owner_depth = 1;
    CHECK_EQ(asb_rw_surface_association(&w, &root, &priv), 0);
    CHECK_EQ(asb_rw_surface_association(&root, &root, &priv), 0);
    CHECK_EQ(asb_rw_surface_association(&bridge, &root, NULL), 0);   /* no private display known */
}

int main(void)
{
    test_codec();
    test_negotiation();
    test_cpu_bounds();
    test_generations();
    test_gpu_pools();
    test_teardown_and_limits();
    test_popup_association();
    if (g_failures) {
        printf("FAILED remote windows frames: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS remote windows frames: codec size/length validation, negotiation, queue and memory bounds, "
           "stale generations, GPU pool lifetimes, teardown and limits, popup association (%d checks)\n", g_checks);
    return 0;
}
