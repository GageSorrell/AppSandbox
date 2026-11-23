#pragma once
#include <winsock2.h>
#include "asb_core.h"
#include "../core/input_router.h"

typedef struct {
    AsbVmId id;
    GUID runtime;
    wchar_t name[256],os_type[32],user[128];
    DWORD ssh_port;
    UINT64 ssh_session;
} AsbHeadlessTarget;
typedef struct AsbInputSession AsbInputSession;
typedef struct {
    UINT32 flags,generation,work_id,x,y,keyboard_version,mouse_version;
    SOCKET connection;
    wchar_t identity[161],vendor[161],model[161],purpose[601];
} AsbInputStatus;
typedef struct {
    AsbScreenshotReply header;
    BYTE *pixels;
} AsbCapture;

ASB_API AsbInputSession *asb_input_session(const AsbHeadlessTarget *target);
ASB_API void asb_input_release(AsbInputSession *session);
ASB_API void asb_input_viewer(AsbInputSession *session,BOOL attached);
ASB_API void asb_input_status(AsbInputSession *session,AsbInputStatus *status);
ASB_API DWORD asb_input_claim(AsbInputSession *session,const AsbInputStatus *context);
ASB_API DWORD asb_input_control(AsbInputSession *session,UINT32 command,UINT32 work_id);
ASB_API DWORD asb_input_send(AsbInputSession *session,BOOL agent,UINT32 generation,const InputPacket *packet);
ASB_API BOOL asb_input_poll(AsbInputSession *session,InputPacket *packet);
ASB_API DWORD asb_input_barrier(AsbInputSession *session,UINT32 generation,DWORD timeout);
ASB_API DWORD asb_input_begin_batch(AsbInputSession *session);
ASB_API void asb_input_end_batch(AsbInputSession *session);
ASB_API void asb_input_context(AsbVmId id,AsbInputStatus *context);
/* User sources (AppSandbox-eh6.7; see input_router.h). */
typedef void (*AsbInputWatch)(void *context);
typedef struct {
    UINT32 source,epoch,hold,user_owns,connected,mouse_version,keyboard_version;
} AsbInputUserState;
ASB_API UINT32 asb_input_focus(AsbInputSession *session,UINT32 source,BOOL renew);
ASB_API DWORD asb_input_send_user(AsbInputSession *session,UINT32 source,UINT32 epoch,const InputPacket *packet);
ASB_API void asb_input_guard(AsbInputSession *session,UINT32 source,BOOL on);
ASB_API UINT32 asb_input_hold(AsbInputSession *session);
ASB_API void asb_input_unhold(AsbInputSession *session,UINT32 hold,BOOL ok);
/* One watcher per session (NULL removes); called on any thread, must only post a message. */
ASB_API void asb_input_watch(AsbInputSession *session,AsbInputWatch watch,void *context);
ASB_API void asb_input_user_state(AsbInputSession *session,AsbInputUserState *state);
ASB_API void asb_headless_forget(AsbVmId id);
ASB_API void asb_headless_shutdown(void);
ASB_API DWORD asb_capture(const AsbHeadlessTarget *target,DWORD timeout,AsbCapture *capture);
ASB_API void asb_capture_free(AsbCapture *capture);

SOCKET asb_channel_connect(const AsbHeadlessTarget *target,unsigned channel,DWORD timeout);
DWORD asb_channel_read(SOCKET connection,void *data,size_t size);
DWORD asb_channel_write(SOCKET connection,const void *data,size_t size);
