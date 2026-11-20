#include "display_resolution_command.h"
#include "../core/api_json.h"
static BOOL resolution(const ApiJson *json, int object, AsbDisplayResolution *out)
{
    return api_json_int(json, api_json_field(json, object, L"windowedWidth"), &out->windowed_width) &&
        api_json_int(json, api_json_field(json, object, L"windowedHeight"), &out->windowed_height) &&
        api_json_bool(json, api_json_field(json, object, L"fullscreenOverride"), &out->fullscreen_override) &&
        api_json_int(json, api_json_field(json, object, L"fullscreenWidth"), &out->fullscreen_width) &&
        api_json_int(json, api_json_field(json, object, L"fullscreenHeight"), &out->fullscreen_height) &&
        out->windowed_width >= 640 && out->windowed_width <= 7680 &&
        out->windowed_height >= 480 && out->windowed_height <= 4320 &&
        out->fullscreen_width >= 640 && out->fullscreen_width <= 7680 &&
        out->fullscreen_height >= 480 && out->fullscreen_height <= 4320;
}
HRESULT parse_display_resolution_command(const wchar_t *text, AsbDisplayResolution *value,
                                         AsbDisplayResolution *expected)
{
    if (!text || !value || !expected) return E_INVALIDARG;
    ApiJson json;
    ApiJsonToken tokens[128];
    if (!api_json_parse(&json, text, tokens, ARRAYSIZE(tokens)) || tokens[0].type != API_JSON_OBJECT ||
        !resolution(&json, api_json_field(&json, 0, L"resolution"), value) ||
        !resolution(&json, api_json_field(&json, 0, L"expected"), expected)) return E_INVALIDARG;
    return S_OK;
}
