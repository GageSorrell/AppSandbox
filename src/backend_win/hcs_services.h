/**
 *
 *
 * @module AppSandbox/backend_win/hcs_services
 *
 * @file      hcs_services.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

#ifndef HCS_SERVICES_H
#define HCS_SERVICES_H

/* AppSandbox Hyper-V socket services: GUID derivation and the HCS HvSocket ServiceTable.
   Kept free of HCS runtime dependencies so the registration contract is unit-testable. */

#include <windows.h>

#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/* Compute the AppSandbox service GUID for a guest OS and port.
     Windows guests get a5b0cafe-<port>-4000-8000-000000000001.
     Non-Windows guests (Linux) get <port>-FACB-11E6-BD58-64006A7986D3
       (the vsock-template GUID — Linux AF_VSOCK clients on port N reach
        the host service registered with that GUID).
   Both forms are listed in the HCS ServiceTable for the appropriate
   guest; the host's socket code picks the right one via this helper. */
ASB_API void hcs_service_guid(const wchar_t *os_type, unsigned port, GUID *out);

/* Same as hcs_service_guid, formatted as the GUID string the HCS JSON
   expects (lowercase hex, hyphenated, no braces). out must be at least
   40 wide chars. */
ASB_API void hcs_service_guid_str(const wchar_t *os_type, unsigned port,
                                  wchar_t *out, size_t out_chars);

/* TRUE when the guest OS is offered the Remote Windows services (channels 9-11). */
ASB_API BOOL hcs_guest_has_remote_windows(const wchar_t *os_type);

/* Writes the ServiceTable object members (without surrounding braces) for a guest:
     all guests: channels 1-6 and 8 (screenshot); Windows guests also 7 (SSH) and
     9-11 (Remote Windows control/frames/files). Every service uses the Everyone
     bind/connect descriptor and allows wildcard binds, as before.
   Returns the number of services written, or -1 if out is too small. */
int hcs_service_table_json(const wchar_t *os_type, wchar_t *out, size_t out_chars);

#endif /* HCS_SERVICES_H */
