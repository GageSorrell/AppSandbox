#pragma once
#include "../backend_win/display_settings.h"
/* Strict numbers/boolean, both full resolution units required. No coercion. */
HRESULT parse_display_resolution_command(const wchar_t *json, AsbDisplayResolution *value,
                                         AsbDisplayResolution *expected);
