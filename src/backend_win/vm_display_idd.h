#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

/* Create IDD display window for VM. Connects to VM's AF_HYPERV channels
   (:0002 frames, :0003 input, :0005 clipboard writer, :0006 clipboard reader)
   and renders received frames via D3D11.
   main_hwnd receives WM_VM_DISPLAY_CLOSED when closed. */
VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_active(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);
UINT_PTR vm_display_idd_token(VmDisplayIdd *display);
BOOL vm_display_idd_is_fullscreen(VmDisplayIdd *display);
BOOL vm_display_idd_set_fullscreen(VmDisplayIdd *display, BOOL enabled);
/* Optional manual-reset cancellation event. Returns the actual completed outcome;
   cancellation before commit prevents entry, including requests not yet dispatched. */
BOOL vm_display_idd_set_fullscreen_cancellable(VmDisplayIdd *display, BOOL enabled, HANDLE cancel);

/* Bring an already-open display window to the foreground/focus.
   Safe to call from any thread; the work is marshaled to the window thread. */
void vm_display_idd_focus(VmDisplayIdd *display);

/* Queue a Windows guest mode on the negotiated GPU or CPU control channel.
   S_OK means queued; actual completion requires frames at the requested mode.
   Unavailable/legacy guests and repeated failed targets return a failure HRESULT. */
HRESULT vm_display_idd_request_resolution(VmDisplayIdd *display, UINT width, UINT height);

#endif /* VM_DISPLAY_IDD_H */
