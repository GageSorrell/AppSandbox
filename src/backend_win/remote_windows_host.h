/**
 *
 *
 * @module AppSandbox/src/backend_win/remote_windows_host
 *
 * @file      remote_windows_host.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows per-VM host (AppSandbox-eh6.7): binds one sharing manager (remote_windows_proxy)
 * to the VM's shared services, independent of the desktop viewer:
 *   - input: the per-VM input session (headless_vm.h) as the ASB_INPUT_SOURCE_WINDOWS user source
 *     (one arbitration with the viewer and the automation agent; its changes are forwarded to
 *     the manager);
 *   - clipboard and audio: a reference on the VM's integration services (vm_integration.h), with
 *     the proxies as one clipboard focus client, so both keep working after the viewer closes.
 * Started by the app for each running Windows guest; stopping it ends sharing (the guest restores
 * its windows) and drops the references.
 */
#ifndef REMOTE_WINDOWS_HOST_H
#define REMOTE_WINDOWS_HOST_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsbRwHost AsbRwHost;

typedef struct AsbRwHostConfig {
    UINT64 vm_id;
    GUID runtime;
    const wchar_t *name, *os_type;
    void (*log)(void *ctx, const char *message);
    void (*changed)(void *ctx);     /* eh6.8: inventory/selection/state changed (any thread; post only) */
    void *ctx;
} AsbRwHostConfig;

AsbRwHost *asb_rw_host_start(const AsbRwHostConfig *config);
void asb_rw_host_stop(AsbRwHost *host);
UINT64 asb_rw_host_vm_id(const AsbRwHost *host);
/* The sharing manager (selectors read its inventory and select through it). */
struct AsbRwManager *asb_rw_host_manager(const AsbRwHost *host);

#ifdef __cplusplus
}
#endif
#endif
