#include "../transport/asb_display_resize.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static DisplayGpuPacket request(uint32_t w, uint32_t h)
{
    DisplayGpuPacket p = {0};
    p.magic=DISPLAY_GPU_MAGIC; p.type=DISPLAY_GPU_RESIZE; p.version=DISPLAY_GPU_VERSION;
    p.session=42; p.width=w; p.height=h; p.slot=DISPLAY_SIZE_GUEST;
    return p;
}
int main(void)
{
    static const uint32_t sizes[][2]={{640,480},{1280,720},{1920,1080},{7680,4320},{1365,911}};
    size_t i;
    CHECK(sizeof(DisplayGpuPacket)==68 && offsetof(DisplayGpuPacket,slot)==64);
    CHECK(DISPLAY_GPU_VERSION==3 && DISPLAY_MIN_WIDTH==1024 && DISPLAY_MIN_HEIGHT==768);
    CHECK(!asb_windows_cpu_resize_capability(3,DISPLAY_GPU_CAP_RESIZE)); /* old GPU-only driver */
    CHECK(!asb_windows_cpu_resize_capability(3,DISPLAY_GPU_CAP_CPU_RESIZE));
    CHECK(!asb_windows_cpu_resize_capability(2,DISPLAY_GPU_CAP_RESIZE|DISPLAY_GPU_CAP_CPU_RESIZE));
    CHECK(asb_windows_cpu_resize_capability(3,DISPLAY_GPU_CAP_RESIZE|DISPLAY_GPU_CAP_CPU_RESIZE));
    /* CPU control needs no FENCE/IVSHMEM/SNAPSHOT or successful GPU offer. */
    CHECK(asb_windows_cpu_resize_capability(3,DISPLAY_GPU_CAP_RESIZE|DISPLAY_GPU_CAP_CPU_RESIZE|DISPLAY_GPU_CAP_FENCE));
    for(i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        DisplayGpuPacket p=request(sizes[i][0],sizes[i][1]);
        CHECK(asb_windows_resize_packet(&p,42));
        p.slot=DISPLAY_SIZE_WINDOW; p.handle=120;
        CHECK(asb_windows_resize_packet(&p,42));
    }
    {
        DisplayGpuPacket p=request(639,480);
        CHECK(!asb_windows_resize_packet(&p,42));
        p=request(640,479); CHECK(!asb_windows_resize_packet(&p,42));
        p=request(7681,4320); CHECK(!asb_windows_resize_packet(&p,42));
        p=request(7680,4321); CHECK(!asb_windows_resize_packet(&p,42));
        p=request(0,0); CHECK(!asb_windows_resize_packet(&p,42));
        p=request(1280,720); p.version--; CHECK(!asb_windows_resize_packet(&p,42));
        p=request(1280,720); CHECK(!asb_windows_resize_packet(&p,43));
        p.session=0; CHECK(!asb_windows_resize_packet(&p,0));
        p=request(1280,720); p.slot=2; CHECK(!asb_windows_resize_packet(&p,42));
        p=request(1280,720); p.handle=23; CHECK(!asb_windows_resize_packet(&p,42));
        p.handle=121; CHECK(!asb_windows_resize_packet(&p,42));
    }
    {
        AsbDisplayResizeState s={0};
        DisplayGpuPacket p=request(640,480), reply=p, other=request(1365,911);
        CHECK(asb_resize_begin(&s,&p,100)==1);
        CHECK(asb_resize_begin(&s,&p,101)==0);
        CHECK(asb_resize_begin(&s,&other,101)==-2);
        CHECK(!asb_resize_observe(&s,640,480,DISPLAY_SIZE_GUEST));
        reply.type=DISPLAY_GPU_RESIZED; reply.session++;
        CHECK(!asb_resize_reply(&s,&reply));
        reply.session=p.session; reply.status=0x80070057u;
        CHECK(asb_resize_reply(&s,&reply));
        p.session++; /* reconnect must not clear the failed target */
        CHECK(asb_resize_begin(&s,&p,200)==-1);
        CHECK(asb_resize_begin(&s,&other,200)==1);
        reply=other; reply.type=DISPLAY_GPU_RESIZED;
        CHECK(asb_resize_reply(&s,&reply));
        CHECK(s.pending.type && s.acknowledged); /* ACK is not mode completion */
        CHECK(!asb_resize_observe(&s,1920,1080,DISPLAY_SIZE_GUEST));
        CHECK(!asb_resize_observe(&s,1365,911,DISPLAY_SIZE_WINDOW));
        CHECK(asb_resize_observe(&s,1365,911,DISPLAY_SIZE_GUEST));
        CHECK(!s.pending.type && !s.deadline);
        other=request(7680,4320);
        CHECK(asb_resize_begin(&s,&other,1000)==1);
        CHECK(!asb_resize_expire(&s,15999,0x800705b4u));
        CHECK(asb_resize_expire(&s,16000,0x800705b4u));
        CHECK(!asb_resize_expire(&s,17000,0x800705b4u));
        other.session++; CHECK(asb_resize_begin(&s,&other,18000)==-1);
        other=request(1920,1080); CHECK(asb_resize_begin(&s,&other,18000)==1);
        asb_resize_fail(&s,0x800704d4u); /* connection aborted before ACK */
        CHECK(asb_resize_begin(&s,&other,19000)==-1);
        other=request(639,480); CHECK(asb_resize_begin(&s,&other,19000)==-3);
    }
    puts("Display resize protocol, bounds, ACK/frame completion and reconnect recovery passed.");
    return 0;
}
