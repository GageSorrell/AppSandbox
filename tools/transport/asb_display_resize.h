#pragma once
#include "../../src/core/protocol.h"
#include <string.h>

/* Windows-only rules. Legacy DISPLAY_MIN_* remain unchanged for Linux/macOS. */
#define ASB_WINDOWS_MIN_WIDTH 640u
#define ASB_WINDOWS_MIN_HEIGHT 480u
#define ASB_RESIZE_TIMEOUT_MS 15000u
static inline int asb_windows_cpu_resize_capability(uint32_t version, uint64_t flags)
{
    return version == DISPLAY_GPU_VERSION &&
        (flags & (DISPLAY_GPU_CAP_RESIZE | DISPLAY_GPU_CAP_CPU_RESIZE)) ==
        (DISPLAY_GPU_CAP_RESIZE | DISPLAY_GPU_CAP_CPU_RESIZE);
}
static inline int asb_windows_display_dimensions(uint32_t width, uint32_t height)
{
    return width >= ASB_WINDOWS_MIN_WIDTH && width <= DISPLAY_MAX_WIDTH &&
        height >= ASB_WINDOWS_MIN_HEIGHT && height <= DISPLAY_MAX_HEIGHT;
}
static inline int asb_windows_resize_packet(const DisplayGpuPacket *p, uint64_t session)
{
    return p && session && p->session == session && p->magic == DISPLAY_GPU_MAGIC &&
        p->version == DISPLAY_GPU_VERSION && p->type == DISPLAY_GPU_RESIZE &&
        p->slot <= DISPLAY_SIZE_WINDOW && asb_windows_display_dimensions(p->width,p->height) &&
        (!p->handle || (p->handle >= DISPLAY_MIN_REFRESH && p->handle <= DISPLAY_MAX_REFRESH));
}
static inline int asb_resize_same(const DisplayGpuPacket *a, const DisplayGpuPacket *b)
{
    return a->width == b->width && a->height == b->height && a->slot == b->slot && a->handle == b->handle;
}
typedef struct AsbDisplayResizeState {
    DisplayGpuPacket pending, failed;
    uint64_t deadline;
    uint32_t failure;
    int acknowledged;
} AsbDisplayResizeState;
static inline void asb_resize_fail(AsbDisplayResizeState *s, uint32_t status)
{
    if (s->pending.type) s->failed = s->pending;
    s->failure = status;
    memset(&s->pending,0,sizeof(s->pending));
    s->deadline = 0;
    s->acknowledged = 0;
}
/* 1=new attempt, 0=already in flight, -1=blocked failed target, -2=busy.
 * Failed targets survive reconnects. A different target permits another attempt. */
static inline int asb_resize_begin(AsbDisplayResizeState *s, const DisplayGpuPacket *p, uint64_t now)
{
    if (!asb_windows_resize_packet(p,p->session)) return -3;
    if (s->pending.type) return asb_resize_same(&s->pending,p) ? 0 : -2;
    if (s->failed.type && asb_resize_same(&s->failed,p)) return -1;
    s->pending = *p;
    s->deadline = now + ASB_RESIZE_TIMEOUT_MS;
    s->acknowledged = 0;
    return 1;
}
static inline int asb_resize_reply(AsbDisplayResizeState *s, const DisplayGpuPacket *p)
{
    if (!s->pending.type || p->type != DISPLAY_GPU_RESIZED || p->session != s->pending.session ||
        !asb_resize_same(&s->pending,p)) return 0;
    if (p->status) asb_resize_fail(s,p->status);
    else s->acknowledged = 1; /* ACK means queued, never proof of a working mode. */
    return 1;
}
static inline int asb_resize_observe(AsbDisplayResizeState *s, uint32_t width, uint32_t height, uint32_t mode)
{
    if (!s->pending.type || !s->acknowledged || s->pending.width != width ||
        s->pending.height != height || s->pending.slot != mode) return 0;
    memset(&s->pending,0,sizeof(s->pending));
    s->deadline = 0;
    s->acknowledged = 0;
    return 1;
}
static inline int asb_resize_expire(AsbDisplayResizeState *s, uint64_t now, uint32_t status)
{
    if (!s->pending.type || now < s->deadline) return 0;
    asb_resize_fail(s,status);
    return 1;
}
