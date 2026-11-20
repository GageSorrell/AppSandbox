#pragma once
#include "../backend_win/display_modes.h"
/* UI-bridge message, allocated in the caller's module; free with free(). */
HRESULT display_capabilities_json(const AsbDisplayCapabilities *caps, wchar_t **out);
