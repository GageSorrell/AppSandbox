#pragma once
#include <stdint.h>
#include <stddef.h>
#include "protocol.h"
#include "../../tools/transport/asb_automation.h"

/*
 * Per-VM input arbitration (one router per guest input channel).
 *
 * Ownership: the automation agent and the user take turns (owner/transition/generation, with
 * a MOUSE_QUERY barrier on every transfer). Held keys/buttons are released on each transfer.
 *
 * User sources (AppSandbox-eh6.7): several host surfaces deliver user input to one guest
 * session - the desktop viewer and Remote Windows proxies. Exactly one user source is current.
 * asb_input_router_focus makes a source current: the previous source's held keys/buttons are
 * released in the guest and the user epoch advances. Sends name their source and (optionally)
 * the epoch they were authorized for; anything else is refused (5), so there is never competing
 * injection. The epoch also advances on every ownership transfer and disconnect, which makes
 * proxies re-verify their guest activation before injecting again.
 *
 * Foreground guard: a source whose guest window holds the single guest foreground (Remote
 * Windows) sets the guard. When another user source takes over, its KEYBOARD input is held
 * (bounded queue) until the guarding source has moved the guest foreground away
 * (asb_input_router_unhold ok=1: flushed) or failed/timed out (ok=0: discarded), so keys never
 * reach a hidden window. Mouse input is not held: a click selects its own guest target.
 */
#define ASB_INPUT_SOURCE_VIEWER  1u
#define ASB_INPUT_SOURCE_WINDOWS 2u
#define ASB_INPUT_HOLD_MAX       64u
#define ASB_INPUT_HOLD_MS        1500u

typedef int (*AsbInputEmit)(void *context,const InputPacket *packet);
typedef struct {
    uint32_t generation,work_id,cancelled_work,owner,working,paused,cancelled;
    uint32_t cursor_x,cursor_y,buttons,transition,transition_id,sequence;
    uint8_t scan[2][256],vk[2][256],legacy[256];
    AsbInputEmit emit;
    void *context;
    /* eh6.7 user sources */
    uint32_t user_source,user_epoch,guard_source,hold,held_count;
    uint64_t hold_since;
    InputPacket held[ASB_INPUT_HOLD_MAX];
} AsbInputRouter;
void asb_input_router_init(AsbInputRouter *router,AsbInputEmit emit,void *context);
uint32_t asb_input_router_request(AsbInputRouter *router,uint32_t work);
uint32_t asb_input_router_control(AsbInputRouter *router,uint32_t command,uint32_t work);
uint32_t asb_input_router_send(AsbInputRouter *router,int agent,uint32_t generation,const InputPacket *packet);
void asb_input_router_observe(AsbInputRouter *router,int agent,const InputPacket *packet);
void asb_input_router_reply(AsbInputRouter *router,const InputPacket *packet);
void asb_input_router_disconnect(AsbInputRouter *router);
uint32_t asb_input_router_flags(const AsbInputRouter *router);

/* Makes `source` the current user source (renew: even if it already is, e.g. a different
   proxy window). Releases held state and advances the epoch on change. Returns the epoch. */
uint32_t asb_input_router_focus(AsbInputRouter *router,uint32_t source,int renew,uint64_t now_ms);
/* User input from `source`. epoch 0 = the current epoch (viewer). The viewer becomes current
   on a press/move when it is not (it only sends while it is the host foreground window);
   releases from a non-current source are dropped (already released). 0 ok, 5 refused,
   29 write failure, 14 hold queue full. */
uint32_t asb_input_router_send_user(AsbInputRouter *router,uint32_t source,uint32_t epoch,const InputPacket *packet,uint64_t now_ms);
/* The guest foreground is (on) / is not (off) a window of `source`. */
void asb_input_router_guard(AsbInputRouter *router,uint32_t source,int on);
/* Holds keyboard input of the current epoch now (the guarding source saw its guest window take
   the foreground on its own while another source is current). Returns the hold id (0: none). */
uint32_t asb_input_router_hold(AsbInputRouter *router,uint64_t now_ms);
/* Ends hold `hold`: flushes (ok) or discards the held keyboard input. */
void asb_input_router_unhold(AsbInputRouter *router,uint32_t hold,int ok);
/* Discards a hold older than ASB_INPUT_HOLD_MS. Returns 1 when one expired. */
int asb_input_router_expire(AsbInputRouter *router,uint64_t now_ms);
