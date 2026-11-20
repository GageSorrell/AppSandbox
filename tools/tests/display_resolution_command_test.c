#include "../../src/app_win/display_resolution_command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK(SUCCEEDED(x))
static const wchar_t *baseline = L"{\"windowedWidth\":1920,\"windowedHeight\":1080,\"fullscreenOverride\":false,\"fullscreenWidth\":1920,\"fullscreenHeight\":1080}";
static wchar_t disk[MAX_PATH], path[MAX_PATH];
static void command(const wchar_t *width, const wchar_t *height, const wchar_t *toggle, wchar_t *out, size_t size)
{
    CHECK(swprintf_s(out,size,L"{\"resolution\":{\"windowedWidth\":%s,\"windowedHeight\":%s,\"fullscreenOverride\":%s,\"fullscreenWidth\":2560,\"fullscreenHeight\":1440},\"expected\":%s}",width,height,toggle,baseline)>0);
}
static char *contents(void)
{
    FILE *f; CHECK(!_wfopen_s(&f,path,L"rb")); CHECK(!fseek(f,0,SEEK_END));
    long size=ftell(f);CHECK(size>=0);rewind(f);
    char *text=calloc((size_t)size+1,1);CHECK(text && fread(text,1,size,f)==(size_t)size);CHECK(!fclose(f));return text;
}
static AsbDisplaySettings read_settings(void) {AsbDisplaySettings s;OK(asb_display_settings_read(disk,&s));return s;}
typedef struct Writer {HANDLE start; AsbDisplayResolution value,expected; HRESULT result;} Writer;
static DWORD WINAPI writer(void *context)
{
    Writer *w=context;CHECK(WaitForSingleObject(w->start,10000)==WAIT_OBJECT_0);
    w->result=asb_display_resolution_edit(disk,FALSE,FALSE,FALSE,&w->value,&w->expected);return 0;
}
int wmain(void)
{
    wchar_t json[2048];AsbDisplayResolution value,expected;
    command(L"2048",L"1152",L"true",json,ARRAYSIZE(json));
    OK(parse_display_resolution_command(json,&value,&expected));
    CHECK(value.windowed_width==2048 && value.windowed_height==1152 && value.fullscreen_override);
    CHECK(expected.windowed_width==1920 && expected.fullscreen_width==1920 && !expected.fullscreen_override);
    const wchar_t *bad_width[]={L"\"1920\"",L"\"\"",L"1920.5",L"1920.0",L"1e3",L"null",L"true",L"639",L"7681",L"2147483648",L"-1"};
    for(size_t i=0;i<ARRAYSIZE(bad_width);++i) {command(bad_width[i],L"1080",L"false",json,ARRAYSIZE(json));CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);}
    const wchar_t *bad_height[]={L"479",L"4321",L"1080.5",L"\"1080\"",L"null"};
    for(size_t i=0;i<ARRAYSIZE(bad_height);++i) {command(L"1920",bad_height[i],L"false",json,ARRAYSIZE(json));CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);}
    const wchar_t *bad_toggle[]={L"1",L"0",L"\"false\"",L"null"};
    for(size_t i=0;i<ARRAYSIZE(bad_toggle);++i) {command(L"1920",L"1080",bad_toggle[i],json,ARRAYSIZE(json));CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);}
    const wchar_t *bad[]={L"{}",L"{\"resolution\":[]}",L"{\"resolution\":{}}",L"{\"resolution\":true}",L"{broken"};
    for(size_t i=0;i<ARRAYSIZE(bad);++i)CHECK(parse_display_resolution_command(bad[i],&value,&expected)==E_INVALIDARG);
    swprintf_s(json,ARRAYSIZE(json),L"{\"resolution\":%s}",baseline);CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);
    swprintf_s(json,ARRAYSIZE(json),L"{\"resolution\":%s,\"expected\":%s,\"resolution\":%s}",baseline,baseline,baseline);CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);
    command(L"1920,\"windowedWidth\":2560",L"1080",L"false",json,ARRAYSIZE(json));CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);
    swprintf_s(json,ARRAYSIZE(json),L"{\"resolution\":%s,\"expected\":{}}",baseline);CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);
    command(L"640",L"480",L"false",json,ARRAYSIZE(json));OK(parse_display_resolution_command(json,&value,&expected));
    command(L"7680",L"4320",L"true",json,ARRAYSIZE(json));OK(parse_display_resolution_command(json,&value,&expected));
    swprintf_s(json,ARRAYSIZE(json),L"{\"resolution\":{\"windowedWidth\":1920,\"windowedHeight\":1080,\"fullscreenOverride\":false,\"fullscreenWidth\":1920.5,\"fullscreenHeight\":1080},\"expected\":%s}",baseline);
    CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);
    swprintf_s(json,ARRAYSIZE(json),L"{\"resolution\":{\"windowedWidth\":1920,\"windowedHeight\":1080,\"fullscreenOverride\":false,\"fullscreenWidth\":1920,\"fullscreenHeight\":4321},\"expected\":%s}",baseline);
    CHECK(parse_display_resolution_command(json,&value,&expected)==E_INVALIDARG);
    wchar_t temp[MAX_PATH],directory[MAX_PATH];CHECK(GetTempPathW(MAX_PATH,temp));CHECK(GetTempFileNameW(temp,L"arc",0,directory));CHECK(DeleteFileW(directory));CHECK(CreateDirectoryW(directory,NULL));
    swprintf_s(disk,MAX_PATH,L"%s\\disk.vhdx",directory);swprintf_s(path,MAX_PATH,L"%s\\display_settings.json",directory);
    FILE *f;CHECK(!_wfopen_s(&f,path,L"wb"));CHECK(fputs("{\"shareClipboard\":0,\"transmitKeyboardHotkeys\":1,\"future\":{\"keep\":true}}",f)>=0);CHECK(!fclose(f));
    command(L"2048",L"1152",L"true",json,ARRAYSIZE(json));OK(parse_display_resolution_command(json,&value,&expected));
    char *before=contents();
    for(unsigned flags=1;flags<8;++flags)CHECK(asb_display_resolution_edit(disk,flags&1,flags&2,flags&4,&value,&expected)==E_ACCESSDENIED);
    char *after=contents();CHECK(!strcmp(before,after));free(after);
    AsbDisplayResolution invalid=value;invalid.fullscreen_height=4321;
    CHECK(asb_display_resolution_edit(disk,FALSE,FALSE,FALSE,&invalid,&expected)==E_INVALIDARG);
    OK(asb_display_resolution_edit(disk,FALSE,FALSE,FALSE,&value,&expected));
    AsbDisplaySettings s=read_settings();CHECK(s.resolution.windowed_width==2048 && s.resolution.fullscreen_width==2560 && s.resolution.fullscreen_override);
    CHECK(!s.share_clipboard && s.transmit_keyboard_hotkeys);
    free(before);before=contents();CHECK(strstr(before,"\"future\":{\"keep\":true}"));
    /* Stale expected values never replace the latest resolution. */
    CHECK(asb_display_resolution_edit(disk,FALSE,FALSE,FALSE,&expected,&expected)==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    after=contents();CHECK(!strcmp(before,after));free(before);free(after);
    /* Independent pin/clipboard edits do not cause resolution conflicts or get lost. */
    s.fullscreen_pinned=TRUE;s.share_clipboard=TRUE;s.suppress_fullscreen_reminder=TRUE;
    OK(asb_display_settings_update(disk,ASB_DISPLAY_PIN|ASB_DISPLAY_CLIPBOARD|ASB_DISPLAY_REMINDER,&s));
    OK(asb_display_resolution_edit(disk,FALSE,FALSE,FALSE,&expected,&value));
    s=read_settings();CHECK(s.fullscreen_pinned && s.share_clipboard && s.suppress_fullscreen_reminder && s.transmit_keyboard_hotkeys);
    /* Two editors with the same original snapshot: exactly one may commit. */
    HANDLE start=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(start);
    Writer writers[2]={{start,{1280,720,FALSE,1920,1080},expected,E_FAIL},{start,{2560,1440,TRUE,3840,2160},expected,E_FAIL}};
    HANDLE threads[2];for(int i=0;i<2;++i){threads[i]=CreateThread(NULL,0,writer,&writers[i],0,NULL);CHECK(threads[i]);}
    CHECK(SetEvent(start));CHECK(WaitForMultipleObjects(2,threads,TRUE,10000)==WAIT_OBJECT_0);
    int success=0,conflict=0;
    for(int i=0;i<2;++i){success+=SUCCEEDED(writers[i].result);conflict+=writers[i].result==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);CloseHandle(threads[i]);}
    CloseHandle(start);CHECK(success==1 && conflict==1);
    s=read_settings();CHECK(s.fullscreen_pinned && s.share_clipboard && s.suppress_fullscreen_reminder);
    CHECK(s.resolution.windowed_width==(SUCCEEDED(writers[0].result)?1280:2560));
    CHECK(DeleteFileW(path));CHECK(RemoveDirectoryW(directory));
    puts("PASS native resolution edits: strict command types/bounds, lifecycle guards, legacy retention, stale saves, atomic competing editors");return 0;
}
