#include "../../src/backend_win/display_modes.h"
#include "../../src/app_win/display_capabilities_json.h"
#include "../../src/core/api_json.h"
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK(SUCCEEDED(x))
typedef struct MockMonitor { const wchar_t *name; const AsbDisplayMode *modes; size_t count; } MockMonitor;
typedef struct Mock {
    const MockMonitor *monitors;
    size_t count;
    int fail_device, fail_monitor, fail_mode;
    unsigned starts;
} Mock;
static HRESULT device(void *context, size_t i, wchar_t out[CCHDEVICENAME])
{
    Mock *m = context;
    if (!i) ++m->starts;
    if (m->fail_device >= 0 && i == (size_t)m->fail_device) return E_FAIL;
    if (i >= m->count) return S_FALSE;
    wcscpy_s(out, CCHDEVICENAME, m->monitors[i].name);
    return S_OK;
}
static HRESULT mode(void *context, const wchar_t *name, size_t i, AsbDisplayMode *out)
{
    Mock *m = context;
    for (size_t n = 0; n < m->count; ++n) if (!wcscmp(m->monitors[n].name, name)) {
        if ((int)n == m->fail_monitor && (int)i == m->fail_mode) return E_FAIL;
        if (i >= m->monitors[n].count) return S_FALSE;
        *out = m->monitors[n].modes[i];
        return S_OK;
    }
    return E_FAIL;
}
static BOOL has(const AsbDisplayCapabilities *caps, int w, int h)
{
    for (size_t i = 0; i < caps->preset_count; ++i)
        if (caps->presets[i].width == w && caps->presets[i].height == h) return TRUE;
    return FALSE;
}
static void sorted(const AsbDisplayMode *items, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        LONGLONG a = (LONGLONG)items[i-1].width * items[i-1].height;
        LONGLONG b = (LONGLONG)items[i].width * items[i].height;
        CHECK(a < b || (a == b && (items[i-1].width < items[i].width ||
            (items[i-1].width == items[i].width && items[i-1].height < items[i].height))));
    }
}
static void verify_json(const AsbDisplayCapabilities *caps)
{
    wchar_t *text;
    OK(display_capabilities_json(caps, &text));
    size_t capacity = wcslen(text) + 1;
    ApiJsonToken *tokens = calloc(capacity, sizeof(*tokens));
    CHECK(tokens);
    ApiJson json;
    CHECK(api_json_parse(&json, text, tokens, (int)capacity));
    int success;
    CHECK(api_json_bool(&json, api_json_field(&json,0,L"ok"), &success));
    CHECK(success == SUCCEEDED(caps->status));
    int error;
    CHECK(api_json_int(&json,api_json_field(&json,0,L"errorCode"),&error));
    CHECK(error == caps->status);
    int monitors = api_json_field(&json,0,L"monitors"), presets = api_json_field(&json,0,L"presets");
    CHECK(monitors > 0 && presets > 0);
    size_t count = 0;
    for (int i = monitors+1; i < tokens[monitors].next; i = tokens[i].next) {
        wchar_t name[CCHDEVICENAME];
        CHECK(count < caps->monitor_count);
        CHECK(api_json_string(&json,api_json_field(&json,i,L"device"),name,ARRAYSIZE(name)));
        CHECK(!wcscmp(name,caps->monitors[count].device));
        int maximum = api_json_field(&json,i,L"maximum"), value;
        if (!caps->monitors[count].has_maximum) CHECK(tokens[maximum].type == API_JSON_NULL);
        else {
            CHECK(api_json_int(&json,api_json_field(&json,maximum,L"width"),&value));
            CHECK(value == caps->monitors[count].maximum.width);
            CHECK(api_json_int(&json,api_json_field(&json,maximum,L"height"),&value));
            CHECK(value == caps->monitors[count].maximum.height);
        }
        int modes = api_json_field(&json,i,L"modes");
        size_t mc = 0;
        for (int t = modes+1; t < tokens[modes].next; t = tokens[t].next) ++mc;
        CHECK(mc == caps->monitors[count].mode_count);
        ++count;
    }
    CHECK(count == (SUCCEEDED(caps->status) ? caps->monitor_count : 0));
    count = 0;
    for (int i = presets+1; i < tokens[presets].next; i = tokens[i].next) {
        int w,h;
        CHECK(count < caps->preset_count);
        CHECK(api_json_int(&json,api_json_field(&json,i,L"width"),&w));
        CHECK(api_json_int(&json,api_json_field(&json,i,L"height"),&h));
        CHECK(w == caps->presets[count].width && h == caps->presets[count].height);
        ++count;
    }
    CHECK(count == (SUCCEEDED(caps->status) ? caps->preset_count : 0));
    free(tokens); free(text);
}
int main(void)
{
    const AsbDisplayMode hd[] = {{1920,1080},{1280,720},{1920,1080},{1920,1080}};
    const AsbDisplayMode uhd[] = {{3840,2160},{1920,1080}};
    const MockMonitor pair[] = {{L"\\\\.\\DISPLAY1",hd,ARRAYSIZE(hd)}, {L"\\\\.\\DISPLAY2",uhd,ARRAYSIZE(uhd)}};
    Mock mock = {pair,ARRAYSIZE(pair),-1,-1,-1,0};
    AsbDisplayModeSource source = {&mock,device,mode};
    AsbDisplayCapabilities caps;
    OK(asb_display_capabilities_build(&source,&caps));
    CHECK(caps.monitor_count == 2 && caps.monitors[0].mode_count == 2);
    CHECK(has(&caps,3840,2160) && has(&caps,1920,1080) && has(&caps,640,480) && has(&caps,800,600));
    CHECK(!has(&caps,5120,2880));
    sorted(caps.presets,caps.preset_count);
    AsbDisplayMode best;
    OK(asb_display_capabilities_maximum(&caps,pair[0].name,&best)); CHECK(best.width == 1920 && best.height == 1080);
    OK(asb_display_capabilities_maximum(&caps,pair[1].name,&best)); CHECK(best.width == 3840 && best.height == 2160);
    CHECK(FAILED(asb_display_capabilities_maximum(&caps,L"gone",&best)) && !best.width && !best.height);
    verify_json(&caps);
    asb_display_capabilities_free(&caps);
    /* Same-area tie, paired dimensions on one monitor and across monitors. */
    const AsbDisplayMode wide[] = {{3840,1080},{1920,2160},{3840,1080}};
    const AsbDisplayMode tall[] = {{1080,3840}};
    const MockMonitor crossed[] = {{L"wide",wide,ARRAYSIZE(wide)}, {L"tall",tall,1}};
    mock.monitors = crossed; mock.count = 2;
    OK(asb_display_capabilities_build(&source,&caps));
    CHECK(!asb_display_capabilities_accommodates(&caps,(AsbDisplayMode){3840,2160}));
    CHECK(!has(&caps,3840,2160) && has(&caps,3840,1080) && has(&caps,1080,3840));
    OK(asb_display_capabilities_maximum(&caps,L"wide",&best)); CHECK(best.width == 3840 && best.height == 1080);
    sorted(caps.monitors[0].modes,caps.monitors[0].mode_count);
    verify_json(&caps);
    asb_display_capabilities_free(&caps);
    const AsbDisplayMode portrait[] = {{1440,2560},{1080,1920}};
    const AsbDisplayMode ultra[] = {{3840,1600},{2560,1080},{3440,1440}};
    const AsbDisplayMode cinema[] = {{4096,2160}};
    const MockMonitor varied[] = {{L"portrait",portrait,2},{L"ultra",ultra,3},{L"cinema",cinema,1}};
    mock.monitors = varied; mock.count = 3;
    OK(asb_display_capabilities_build(&source,&caps));
    CHECK(has(&caps,1440,2560) && has(&caps,3840,1600) && has(&caps,2560,1080) && has(&caps,4096,2160));
    sorted(caps.presets,caps.preset_count);
    verify_json(&caps); asb_display_capabilities_free(&caps);
    /* Maximum means largest supported *within* both app bounds, never a clamp. */
    const AsbDisplayMode capped[] = {{8000,4500},{7680,4320},{3840,5000},{8000,1080},{640,480}};
    const MockMonitor large[] = {{L"large",capped,5}};
    mock.monitors = large; mock.count = 1;
    OK(asb_display_capabilities_build(&source,&caps));
    OK(asb_display_capabilities_maximum(&caps,L"large",&best)); CHECK(best.width == 7680 && best.height == 4320);
    verify_json(&caps); asb_display_capabilities_free(&caps);
    const MockMonitor outside[] = {{L"outside",capped,1}};
    mock.monitors = outside;
    OK(asb_display_capabilities_build(&source,&caps));
    CHECK(!caps.monitors[0].has_maximum && FAILED(asb_display_capabilities_maximum(&caps,L"outside",&best)));
    /* An over-cap host mode can still accommodate an in-cap custom selection. */
    CHECK(has(&caps,7680,4320)); verify_json(&caps); asb_display_capabilities_free(&caps);
    /* Hotplug: refresh starts again at zero, replaces the old snapshot, no stale 4K. */
    mock.monitors = pair; mock.count = 2;
    OK(asb_display_capabilities_build(&source,&caps)); CHECK(has(&caps,3840,2160)); asb_display_capabilities_free(&caps);
    unsigned starts = mock.starts;
    mock.count = 1;
    OK(asb_display_capabilities_build(&source,&caps)); CHECK(!has(&caps,3840,2160) && mock.starts == starts+1); asb_display_capabilities_free(&caps);
    mock.count = 2; mock.fail_monitor = 1; mock.fail_mode = 1;
    CHECK(FAILED(asb_display_capabilities_build(&source,&caps)));
    CHECK(FAILED(caps.status) && !caps.monitors && !caps.presets && !caps.monitor_count && !caps.preset_count);
    verify_json(&caps); CHECK(FAILED(asb_display_capabilities_maximum(&caps,pair[0].name,&best))); asb_display_capabilities_free(&caps);
    mock.fail_monitor = -1; mock.fail_device = 1;
    CHECK(FAILED(asb_display_capabilities_build(&source,&caps))); verify_json(&caps); asb_display_capabilities_free(&caps);
    mock.fail_device = -1; mock.count = 0;
    CHECK(FAILED(asb_display_capabilities_build(&source,&caps))); verify_json(&caps); asb_display_capabilities_free(&caps);
    const MockMonitor empty[] = {{L"empty",NULL,0}};
    mock.monitors = empty; mock.count = 1;
    CHECK(FAILED(asb_display_capabilities_build(&source,&caps))); asb_display_capabilities_free(&caps);
    CHECK(asb_display_capabilities_build(NULL,&caps) == E_INVALIDARG && caps.status == E_INVALIDARG);
    CHECK(asb_display_capabilities_build(&source,NULL) == E_INVALIDARG);
    /* Read-only live smoke check; availability depends on the current desktop. */
    HRESULT hr = asb_display_capabilities_enumerate(&caps);
    CHECK(hr == caps.status);
    if (SUCCEEDED(hr)) {
        CHECK(caps.monitor_count);
        for (size_t i = 0; i < caps.monitor_count; ++i) sorted(caps.monitors[i].modes,caps.monitors[i].mode_count);
        sorted(caps.presets,caps.preset_count); verify_json(&caps);
        printf("Native desktop enumeration: %zu monitors, %zu presets\n",caps.monitor_count,caps.preset_count);
    } else printf("Native desktop enumeration unavailable: 0x%08lx\n",(unsigned long)hr);
    asb_display_capabilities_free(&caps);
    puts("PASS display modes: union, pairs, portrait/ultrawide, duplicates, ranking, bounds, hotplug, failures, JSON");
    return 0;
}
