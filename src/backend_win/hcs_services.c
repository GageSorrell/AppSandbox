#include "hcs_services.h"

#include <stdio.h>
#include <wchar.h>
#include "../../tools/transport/asb_automation.h"
#include "../../tools/transport/asb_remote_windows.h"

void hcs_service_guid(const wchar_t *os_type, unsigned port, GUID *out)
{
    /* Default to Windows-style GUIDs whenever os_type isn't explicitly
       set to a non-Windows value. Protects existing Windows VMs from any
       regression if their os_type field happens to be missing/empty —
       e.g. a vms.cfg entry from an older build that pre-dates the field.
       Only the explicit string "Linux" (or any other non-Windows guest
       string in the future) flips to the vsock-template GUIDs. */
    BOOL is_windows = !os_type
                   || os_type[0] == L'\0'
                   || _wcsicmp(os_type, L"Windows") == 0;
    if (is_windows) {
        /* a5b0cafe-<port>-4000-8000-000000000001 — the original AppSandbox
           service GUIDs, reached via AF_HYPERV from a Windows guest. */
        out->Data1    = 0xa5b0cafeu;
        out->Data2    = (unsigned short)port;
        out->Data3    = 0x4000;
        out->Data4[0] = 0x80; out->Data4[1] = 0x00;
        out->Data4[2] = 0x00; out->Data4[3] = 0x00;
        out->Data4[4] = 0x00; out->Data4[5] = 0x00;
        out->Data4[6] = 0x00; out->Data4[7] = 0x01;
    } else {
        /* <port>-FACB-11E6-BD58-64006A7986D3 — the Linux vsock template.
           A Linux guest calling bind/connect on AF_VSOCK port N is mapped
           by hv_sock onto exactly this GUID on the Hyper-V side. */
        out->Data1    = port;
        out->Data2    = 0xFACB;
        out->Data3    = 0x11E6;
        out->Data4[0] = 0xBD; out->Data4[1] = 0x58;
        out->Data4[2] = 0x64; out->Data4[3] = 0x00;
        out->Data4[4] = 0x6A; out->Data4[5] = 0x79;
        out->Data4[6] = 0x86; out->Data4[7] = 0xD3;
    }
}

void hcs_service_guid_str(const wchar_t *os_type, unsigned port,
                          wchar_t *out, size_t out_chars)
{
    GUID g;
    hcs_service_guid(os_type, port, &g);
    swprintf_s(out, out_chars,
        L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/* Matches the ServiceTable's historical Windows test (an explicit "Windows" os_type). */
static BOOL windows_guest(const wchar_t *os_type)
{
    return os_type && _wcsicmp(os_type, L"Windows") == 0;
}

BOOL hcs_guest_has_remote_windows(const wchar_t *os_type)
{
    return windows_guest(os_type);
}

int hcs_service_table_json(const wchar_t *os_type, wchar_t *out, size_t out_chars)
{
    static const unsigned remote_windows[] = { ASB_CH_RW_CONTROL, ASB_CH_RW_FRAMES, ASB_CH_RW_FILES };
    unsigned ports[ASB_CH_SCREENSHOT + 3];
    int count = 0;
    size_t used = 0;
    BOOL windows = windows_guest(os_type);

    for (unsigned port = 1; port <= ASB_CH_SCREENSHOT; port++) {
        if (!windows && port == 7) continue;   /* SSH channel: Windows guests only */
        ports[count++] = port;
    }
    if (windows)
        for (size_t i = 0; i < sizeof(remote_windows) / sizeof(remote_windows[0]); i++)
            ports[count++] = remote_windows[i];

    if (!out || !out_chars) return -1;
    out[0] = L'\0';
    for (int i = 0; i < count; i++) {
        wchar_t guid[64];
        hcs_service_guid_str(os_type, ports[i], guid, 64);
        int n = swprintf(out + used, out_chars - used,
            L"%s\"%s\":{"
                L"\"BindSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                L"\"AllowWildcardBinds\":true"
            L"}",
            i ? L"," : L"", guid);
        if (n < 0 || (size_t)n >= out_chars - used) {
            out[0] = L'\0';
            return -1;
        }
        used += (size_t)n;
    }
    return count;
}
