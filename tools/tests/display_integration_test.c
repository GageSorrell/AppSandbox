/* Cross-component fixture: real editor command/storage/catalog/viewer policy.
   Reuse the focused harness's simulated monitor/window/transport boundaries. */
#define ASB_DISPLAY_INTEGRATION
#include "fullscreen_resolution_test.c"
#include "../../src/app_win/display_resolution_command.h"

static wchar_t *read_command(const wchar_t *path)
{
    FILE *file;CHECK(!_wfopen_s(&file,path,L"rb"));
    CHECK(!fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0 && size<65536);rewind(file);
    char *bytes=calloc((size_t)size+1,1);CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size);CHECK(!fclose(file));
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,(int)size,NULL,0);CHECK(length>0);
    wchar_t *wide=calloc((size_t)length+1,sizeof(wchar_t));CHECK(wide);
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,(int)size,wide,length)==length);
    free(bytes);return wide;
}
int wmain(int argc,wchar_t **argv)
{
    CHECK(argc>=3);CHECK(!wcscpy_s(integration_disk,MAX_PATH,argv[2]));
    HRESULT hr=S_OK;
    if (!wcscmp(argv[1],L"edit")) {
        CHECK(argc==4);wchar_t *json=read_command(argv[3]);
        AsbDisplayResolution value,expected;
        hr=parse_display_resolution_command(json,&value,&expected);free(json);
        if (SUCCEEDED(hr)) hr=asb_display_resolution_edit(integration_disk,FALSE,FALSE,FALSE,&value,&expected);
    } else if (!wcscmp(argv[1],L"preferences")) {
        AsbDisplaySettings patch={0};patch.fullscreen_pinned=patch.suppress_fullscreen_reminder=TRUE;
        hr=asb_display_settings_update(integration_disk,ASB_DISPLAY_PIN,&patch);
        if (SUCCEEDED(hr)) hr=asb_display_settings_update(integration_disk,ASB_DISPLAY_REMINDER,&patch);
    } else CHECK(!wcscmp(argv[1],L"read"));
    if (FAILED(hr)) { printf("{\"ok\":false,\"errorCode\":%ld}\n",hr);return 0; }
    AsbDisplaySettings saved;CHECK(SUCCEEDED(asb_display_settings_read(integration_disk,&saved)));
    VmDisplayIdd d;reset(&d);
    idd_update_display_size(&d,FALSE);
    UINT windowed_width=d.size_request.width,windowed_height=d.size_request.height;
    if (d.size_request.type) settle(&d);
    CHECK(idd_set_fullscreen(&d,TRUE));
    UINT full_width=d.fullscreen_target_width,full_height=d.fullscreen_target_height;
    CHECK(d.size_request.slot==DISPLAY_SIZE_GUEST);settle(&d);
    CHECK(idd_set_fullscreen(&d,FALSE));
    CHECK(d.size_request.width==(UINT)saved.resolution.windowed_width && d.size_request.height==(UINT)saved.resolution.windowed_height);
    settle(&d);
    /* The session Match choice survives fullscreen and restores its mode. */
    d.size_mode=DISPLAY_SIZE_WINDOW;idd_update_display_size(&d,FALSE);
    if (d.size_request.type) settle(&d);
    CHECK(idd_set_fullscreen(&d,TRUE));settle(&d);
    CHECK(idd_set_fullscreen(&d,FALSE));CHECK(d.size_mode==DISPLAY_SIZE_WINDOW);
    CHECK(d.size_request.slot==DISPLAY_SIZE_WINDOW);
    float x,y,w,h;UINT vx,vy;
    compute_letterbox(1600,900,640,480,&x,&y,&w,&h);CHECK(w==1200 && x==200);
    window_to_vm_coords(d.hwnd,500,350,640,480,FALSE,&vx,&vy);CHECK(vx<=640 && vy<=480);
    printf("{\"ok\":true,\"resolution\":{\"windowedWidth\":%d,\"windowedHeight\":%d,\"fullscreenOverride\":%s,\"fullscreenWidth\":%d,\"fullscreenHeight\":%d},"
        "\"pin\":%s,\"reminder\":%s,\"hotkeys\":%s,\"clipboard\":%s,\"windowed\":[%u,%u],\"fullscreen\":[%u,%u]}\n",
        saved.resolution.windowed_width,saved.resolution.windowed_height,saved.resolution.fullscreen_override?"true":"false",
        saved.resolution.fullscreen_width,saved.resolution.fullscreen_height,
        d.fullscreen_pinned?"true":"false",saved.suppress_fullscreen_reminder?"true":"false",
        saved.transmit_keyboard_hotkeys?"true":"false",saved.share_clipboard?"true":"false",
        windowed_width,windowed_height,full_width,full_height);
    DeleteCriticalSection(&d.frame_cs);return 0;
}
