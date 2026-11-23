/**
 *
 *
 * @module AppSandbox/src/backend_win/vm_integration
 *
 * @file      vm_integration.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Per-VM integration services shared by every host surface of one guest (AppSandbox-eh6.7):
 * the clipboard coordinator and guest audio playback.
 *
 * One instance per running VM, reference counted. The desktop viewer and the Remote Windows
 * host each hold a reference, so closing the viewer leaves clipboard sharing and audio running
 * for shared windows, and opening it again reuses the same connections (never a second
 * clipboard reader/writer or audio stream for one guest).
 *
 * Clipboard: exactly one vm_clipboard (channels 5/6) per VM, owned by a hidden message window
 * on the instance's own thread (clipboard listener, guest-to-host apply). Host surfaces register
 * as focus clients; sync is on while the persisted "Share clipboard" setting is on AND one of
 * the clients has host focus - the same focus-gated policy the viewer always had, now covering
 * the viewer and every shared window. Moving focus between the viewer and a proxy keeps sync on.
 *
 * Audio: one guest-to-host playback stream (channel 4, WASAPI shared mode) per VM for as long as
 * anything holds the instance. Mute is per VM and shared by every surface.
 */
#ifndef VM_INTEGRATION_H
#define VM_INTEGRATION_H

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmIntegration VmIntegration;

#define VM_INTEGRATION_MAX_CLIENTS 32u

/* Replaceable services (tests). NULL members keep the defaults. */
typedef struct VmIntegrationBackend {
    void *(*clipboard_create)(const GUID *runtime, const wchar_t *os_type, HWND hwnd, VmIntegration *vi);
    void (*clipboard_destroy)(void *clipboard);
    void (*clipboard_sync)(void *clipboard, BOOL enabled);
    void (*clipboard_sharing)(void *clipboard, BOOL enabled);
    void (*clipboard_update)(void *clipboard);     /* WM_CLIPBOARDUPDATE */
    void (*clipboard_apply)(void *clipboard);      /* guest data ready (WM_APP + 11) */
    BOOL (*audio_start)(VmIntegration *vi);        /* runs until vm_integration_audio_stopping */
    void (*audio_stop)(VmIntegration *vi);         /* joins */
    BOOL (*load_sharing)(UINT64 vm_id);            /* persisted "Share clipboard" */
    void (*log)(const wchar_t *message);
} VmIntegrationBackend;

/* Takes a reference to the VM's instance (created on first use). NULL on failure. */
VmIntegration *vm_integration_acquire(UINT64 vm_id, const GUID *runtime, const wchar_t *os_type, const wchar_t *name);
/* Drops a reference; the last one stops audio and closes the clipboard connections. */
void vm_integration_release(VmIntegration *vi);

/* Clipboard focus clients (the viewer, the Remote Windows proxies of this VM). */
uint32_t vm_integration_client_add(VmIntegration *vi);
void vm_integration_client_focus(VmIntegration *vi, uint32_t client, BOOL focused);
void vm_integration_client_remove(VmIntegration *vi, uint32_t client);
/* The "Share clipboard" setting changed (the caller persists it). */
void vm_integration_set_clipboard_sharing(VmIntegration *vi, BOOL enabled);
BOOL vm_integration_clipboard_sharing(VmIntegration *vi);

void vm_integration_set_audio_muted(VmIntegration *vi, BOOL muted);
BOOL vm_integration_audio_muted(VmIntegration *vi);

/* Optional log sink (the viewer's log window); NULL restores the default. */
typedef void (*VmIntegrationLog)(void *ctx, const wchar_t *message);
void vm_integration_set_log(VmIntegration *vi, VmIntegrationLog log, void *ctx);
/* Logs through the sink. Safe from any thread, including from backend calls the coordinator
   makes while holding its own lock (vm_clipboard logs synchronously from set_sync_enabled). */
void vm_integration_log(VmIntegration *vi, const wchar_t *message);

typedef struct VmIntegrationInfo {
    uint32_t refs, clients, focused;
    BOOL sharing, sync, muted, audio_running;
    void *clipboard;
    uint32_t clipboard_creates, audio_starts;
} VmIntegrationInfo;
void vm_integration_info(VmIntegration *vi, VmIntegrationInfo *out);
/* Number of live instances (all VMs). */
uint32_t vm_integration_count(void);

void vm_integration_set_backend(const VmIntegrationBackend *backend);

/* For audio implementations: TRUE once the instance is being torn down. */
BOOL vm_integration_audio_stopping(VmIntegration *vi);

#ifdef __cplusplus
}
#endif
#endif
