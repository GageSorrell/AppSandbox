#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "fullscreen_request.h"
#include "protocol.h"
#include "asb_automation.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ExitProcess(1); } } while(0)
#define WM_TEST_FULLSCREEN (WM_APP+1)
#define WM_TEST_BLOCK (WM_APP+2)
typedef UINT_PTR AsbVm;
typedef struct { BOOL suppress_fullscreen_reminder; } AsbDisplaySettings;
typedef struct VmDisplayIdd {
 HWND hwnd,fullscreen_dialog;
 HINSTANCE hInstance;
 UINT_PTR vm_id;
 BOOL fullscreen,fullscreen_pinned,fullscreen_entry_active,fullscreen_close_pending,stop;
 void *agent_overlay,*input;
} VmDisplayIdd;
static VmDisplayIdd display;
static AsbDisplaySettings saved[2];
static int prompts,transitions,saves,key_flushes,mouse_flushes,overlay_cancels,hook_removes;
static int answer=IDOK,behavior;
static BOOL check_box,fail_transition,fail_dialog,read_failure,fail_save;
static HANDLE ready,blocked,unblock,signal_cancel;
static LONG live_keys,live_buttons;
static int input_packets;
static DWORD asb_input_control(void *input,UINT32 type,UINT32 p1) { (void)input;(void)type;(void)p1;++input_packets;return 0; }
static DWORD asb_input_send(void *input,BOOL agent,UINT32 id,const InputPacket *packet) {
 (void)input;(void)agent;(void)id;(void)packet;++input_packets;return 0;
}
/* ACTUAL INPUT GUARD */
static AsbVm asb_find_vm_by_id(UINT_PTR id) { return id; }
static HRESULT asb_vm_get_display_settings(AsbVm id,AsbDisplaySettings *out) { *out=saved[id];return read_failure?E_FAIL:S_OK; }
static HRESULT asb_vm_set_fullscreen_reminder_suppressed(AsbVm id,BOOL value) { ++saves;if (fail_save) return E_FAIL;saved[id].suppress_fullscreen_reminder=value;return S_OK; }
static BOOL agent_overlay_is_ready(void *overlay) { (void)overlay;return TRUE; }
static void idd_remove_kbd_hook(VmDisplayIdd *d) { CHECK(d->fullscreen_entry_active);++hook_removes; }
static void agent_overlay_cancel_input(void *overlay) { (void)overlay;++overlay_cancels; }
static void idd_flush_held_keys(VmDisplayIdd *d) { (void)d;++key_flushes;live_keys=0; }
static void idd_flush_mouse_buttons(VmDisplayIdd *d) { (void)d;++mouse_flushes;live_buttons=0;ReleaseCapture(); }
static void idd_update_relative_mouse(VmDisplayIdd *d) { (void)d; }
static void idd_update_kbd_hook(VmDisplayIdd *d) { CHECK(!d->fullscreen_entry_active); }
static void idd_log(VmDisplayIdd *d,const wchar_t *format,...) { (void)d;(void)format; }
static BOOL idd_set_fullscreen(VmDisplayIdd *d,BOOL enabled) {
 ++transitions;
 if (signal_cancel && behavior==4) SetEvent(signal_cancel); /* cancellation loses after commit */
 if (fail_transition) return FALSE;
 d->fullscreen=enabled;return TRUE;
}
static HRESULT test_dialog(const TASKDIALOGCONFIG *config,int *button,int *radio,BOOL *checked);
#define TaskDialogIndirect test_dialog
/* ACTUAL FULLSCREEN GATE */
#undef TaskDialogIndirect

static HRESULT test_dialog(const TASKDIALOGCONFIG *config,int *button,int *radio,BOOL *checked)
{
 (void)radio;++prompts;
 CHECK(config->hwndParent==display.hwnd);
 CHECK(!wcscmp(config->pszWindowTitle,L"Before entering fullscreen"));
 CHECK(!wcscmp(config->pszVerificationText,L"Don't show this again for this VM"));
 CHECK(!(config->dwFlags&TDF_VERIFICATION_FLAG_CHECKED));
 CHECK(config->dwFlags&TDF_ALLOW_DIALOG_CANCELLATION);
 CHECK(config->dwCommonButtons==TDCBF_CANCEL_BUTTON);
 CHECK(config->cButtons==1 && config->pButtons[0].nButtonID==IDOK);
 CHECK(!wcscmp(config->pButtons[0].pszButtonText,L"Enter fullscreen"));
 CHECK(wcsstr(config->pszContent,L"top-center") && wcsstr(config->pszContent,L"half a second"));
 CHECK(wcsstr(config->pszContent,L"Viewer options") && wcsstr(config->pszContent,L"Exit fullscreen"));
 CHECK(wcsstr(config->pszContent,L"Pin fullscreen controls"));
 CHECK(!!config->pszFooter==!!display.fullscreen_pinned);
 CHECK(display.fullscreen_entry_active && !live_keys && !live_buttons && !GetCapture());
 CHECK(key_flushes && mouse_flushes && overlay_cancels && hook_removes);
 if (behavior==1) {
  int previous=prompts;
  CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
  CHECK(prompts==previous); /* same-thread reentrant request */
 }
 if (behavior==2) {
  IddFullscreenPrompt *prompt=(IddFullscreenPrompt*)config->lpCallbackData;
  while (!asb_fullscreen_request_cancelled(prompt->request)) Sleep(1);
 }
 if (behavior==3) display.fullscreen_close_pending=TRUE;
 if (behavior==5) Sleep(1100);
 *button=answer;*checked=check_box;
 return fail_dialog?E_FAIL:S_OK;
}
static LRESULT CALLBACK window_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
 if (message==WM_TEST_FULLSCREEN) {
  AsbFullscreenRequest *r=(AsbFullscreenRequest*)lp;BOOL result=FALSE;
  if (asb_fullscreen_request_begin(r)) result=idd_request_fullscreen(&display,!!wp,r);
  asb_fullscreen_request_finish(r,result);return result;
 }
 if (message==WM_TEST_BLOCK) { SetEvent(blocked);WaitForSingleObject(unblock,10000);return 0; }
 if (message==WM_CLOSE) return 0; /* assert deferred close, keep fixture reusable */
 if (message==WM_DESTROY) { PostQuitMessage(0);return 0; }
 return DefWindowProcW(hwnd,message,wp,lp);
}
static DWORD WINAPI viewer_thread(void *unused)
{
 (void)unused;
 WNDCLASSW wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"FullscreenGateTest";
 CHECK(RegisterClassW(&wc));
 display.hwnd=CreateWindowW(wc.lpszClassName,L"Test",0,0,0,100,100,HWND_MESSAGE,NULL,wc.hInstance,NULL);
 CHECK(display.hwnd);SetEvent(ready);
 MSG msg;
 while (GetMessageW(&msg,NULL,0,0)>0) { TranslateMessage(&msg);DispatchMessageW(&msg); }
 DestroyWindow(display.hwnd);
 asb_fullscreen_discard_requests(WM_TEST_FULLSCREEN);
 return 0;
}
static DWORD WINAPI cancel_thread(void *event) { Sleep(30);SetEvent((HANDLE)event);return 0; }
static void reset(void) {
 display.fullscreen=FALSE;display.fullscreen_pinned=FALSE;display.stop=FALSE;
 display.fullscreen_close_pending=FALSE;display.vm_id=0;
 saved[0].suppress_fullscreen_reminder=FALSE;saved[1].suppress_fullscreen_reminder=FALSE;
 check_box=FALSE;answer=IDOK;behavior=0;fail_transition=fail_dialog=read_failure=fail_save=FALSE;signal_cancel=NULL;
 live_keys=2;live_buttons=1;
}
int main(void)
{
 ready=CreateEventW(NULL,TRUE,FALSE,NULL);blocked=CreateEventW(NULL,TRUE,FALSE,NULL);unblock=CreateEventW(NULL,TRUE,FALSE,NULL);
 HANDLE thread=CreateThread(NULL,0,viewer_thread,NULL,0,NULL);CHECK(thread);
 CHECK(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0);
 reset();display.input=&display;
 display.fullscreen_entry_active=TRUE;
 CHECK(!send_input_locked(&display,INPUT_MOUSE_MOVE,1,1,0));
 CHECK(!send_input_locked(&display,INPUT_MOUSE_RELATIVE,1,1,0));
 CHECK(!send_input_locked(&display,INPUT_MOUSE_BUTTON,0,1,0));
 CHECK(!send_input_locked(&display,INPUT_MOUSE_WHEEL,120,0,0));
 CHECK(!send_input_locked(&display,INPUT_KEY,65,30,0));
 CHECK(!send_input_locked(&display,INPUT_KEY_PHYSICAL,65,30,0));
 CHECK(!send_input_locked(&display,ASB_INPUT_USER_GRANT,0,0,0));CHECK(!input_packets);
 CHECK(send_input_locked(&display,INPUT_KEY_PHYSICAL,65,30,INPUT_KEY_UP));
 CHECK(send_input_locked(&display,INPUT_MOUSE_BUTTON,0,0,0));CHECK(input_packets==2);
 display.fullscreen_entry_active=FALSE;
 CHECK(send_input_locked(&display,INPUT_MOUSE_MOVE,1,1,0));CHECK(input_packets==3);
 int before=prompts;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(prompts==before+1);
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,FALSE,NULL));CHECK(prompts==before+1);
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(prompts==before+2);
 // Native TaskDialog maps Cancel, Escape, Alt-F4 and title-bar close to IDCANCEL.
 for (int cancel_source=0;cancel_source<4;cancel_source++) {
  reset();answer=IDCANCEL;check_box=TRUE;int old_saves=saves;
  CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
  CHECK(!display.fullscreen && saves==old_saves && !display.fullscreen_entry_active);
 }
 reset();check_box=TRUE;fail_transition=TRUE;int old_saves=saves;
 CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(saves==old_saves);
 reset();fail_dialog=TRUE;CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
 reset();behavior=5;CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
 reset();display.fullscreen_pinned=TRUE;behavior=1;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
 reset();read_failure=TRUE;saved[0].suppress_fullscreen_reminder=TRUE;before=prompts;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(prompts==before+1);
 reset();check_box=TRUE;fail_save=TRUE;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(!saved[0].suppress_fullscreen_reminder);
 reset();check_box=TRUE;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(saved[0].suppress_fullscreen_reminder);
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,FALSE,NULL));before=prompts;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(prompts==before);
 display.vm_id=1;display.fullscreen=FALSE;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(prompts==before+1);
 reset();behavior=3;CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));CHECK(!display.fullscreen);
 reset();HANDLE cancel=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(cancel);
 behavior=2;HANDLE canceller=CreateThread(NULL,0,cancel_thread,cancel,0,NULL);CHECK(canceller);
 CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,cancel));CHECK(!display.fullscreen);
 WaitForSingleObject(canceller,10000);CloseHandle(canceller);
 // Abandon a queued request before the viewer dispatches it.
 reset();CHECK(PostMessageW(display.hwnd,WM_TEST_BLOCK,0,0));CHECK(WaitForSingleObject(blocked,10000)==WAIT_OBJECT_0);
 int old_transitions=transitions;
 CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,cancel));
 SetEvent(unblock);SendMessageW(display.hwnd,WM_NULL,0,0);CHECK(transitions==old_transitions && !display.fullscreen);
 // Cancellation cannot report failure while committed work enters later.
 reset();ResetEvent(cancel);signal_cancel=cancel;behavior=4;
 CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,cancel));CHECK(display.fullscreen);
 CHECK(!asb_fullscreen_call(NULL,WM_TEST_FULLSCREEN,TRUE,NULL));
 // WM_QUIT abandons pending work and is preserved for the caller's outer loop.
 reset();ResetEvent(blocked);ResetEvent(unblock);
 CHECK(PostMessageW(display.hwnd,WM_TEST_BLOCK,0,0));CHECK(WaitForSingleObject(blocked,10000)==WAIT_OBJECT_0);
 PostQuitMessage(17);CHECK(!asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,TRUE,NULL));
 MSG quit;CHECK(PeekMessageW(&quit,NULL,WM_QUIT,WM_QUIT,PM_REMOVE) && quit.wParam==17);
 SetEvent(unblock);SendMessageW(display.hwnd,WM_NULL,0,0);CHECK(!display.fullscreen);
 DWORD handles_before,handles_after;
 CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
 for (int i=0;i<100;i++) CHECK(asb_fullscreen_call(display.hwnd,WM_TEST_FULLSCREEN,FALSE,NULL));
 SendMessageW(display.hwnd,WM_NULL,0,0);
 CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after) && handles_after==handles_before);
 CHECK(PostThreadMessageW(GetThreadId(thread),WM_QUIT,0,0));CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);
 CloseHandle(thread);CloseHandle(ready);CloseHandle(blocked);CloseHandle(unblock);CloseHandle(cancel);
 puts("PASS fullscreen reminder: outcomes, ownership, checkbox, failure, pin text, per-VM suppression, input release, reentrancy, completion, cancellation, abandonment");
 return 0;
}
