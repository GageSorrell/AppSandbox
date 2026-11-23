/**
 *
 *
 * @module AppSandbox/tools/vdd/private_control
 *
 * @file      private_control.inl
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Included by the real driver and the focused lifecycle harness. Callers hold
   privateControlLock (the control work item); swap callbacks use privateLock. */
static NTSTATUS VddPrivateCommand(VDD_DEVICE_CONTEXT* ctx,
    const AsbPrivateDisplayRequest* command, AsbPrivateDisplayReply* reply)
{
    if (!ctx || ctx->stopping || !ctx->hAdapter) return STATUS_DEVICE_NOT_READY;
    if (command->size != sizeof(*command) || command->version != ASB_PRIVATE_DISPLAY_VERSION ||
        command->reserved || command->operation > ASB_PRIVATE_REMOVE) return STATUS_INVALID_PARAMETER;
    if (command->operation != ASB_PRIVATE_QUERY) {
        if (!command->token) return STATUS_INVALID_PARAMETER;
        if (ctx->privateToken && command->token != ctx->privateToken) return STATUS_ACCESS_DENIED;
    }
    NTSTATUS st = STATUS_SUCCESS;
    switch (command->operation) {
    case ASB_PRIVATE_ACTIVATE: {
        if (!asb_private_mode_supported(command->width, command->height)) return STATUS_NOT_SUPPORTED;
        if (asb_transport_is_ivshmem()) return STATUS_NOT_SUPPORTED;
        if (ctx->privateState == ASB_PRIVATE_RESTORING) return STATUS_DEVICE_BUSY;
        if (ctx->privateState == ASB_PRIVATE_READY) {
            if (ctx->privateWidth != command->width || ctx->privateHeight != command->height)
                return STATUS_DEVICE_BUSY; /* restore before changing private modes */
            break;
        }
        AcquireSRWLockShared(&ctx->swapLock);
        BOOL desktopReady;
        desktopReady = ctx->hMonitor != NULL;
        ReleaseSRWLockShared(&ctx->swapLock);
        if (!desktopReady) return STATUS_DEVICE_NOT_READY;
        st = VddCreatePrivateMonitor(ctx, command->width, command->height);
        if (!NT_SUCCESS(st)) return st;
        ctx->privateToken = command->token;
        ctx->privateWidth = command->width;
        ctx->privateHeight = command->height;
        ctx->privateState = ASB_PRIVATE_READY;
        break;
    }
    case ASB_PRIVATE_RESTORE:
        if (ctx->privateState != ASB_PRIVATE_ABSENT) ctx->privateState = ASB_PRIVATE_RESTORING;
        break;
    case ASB_PRIVATE_REMOVE:
        if (ctx->privateState == ASB_PRIVATE_ABSENT) break;
        if (ctx->privateState != ASB_PRIVATE_RESTORING) return STATUS_DEVICE_BUSY;
        st = VddRemovePrivateMonitor(ctx);
        if (!NT_SUCCESS(st)) return st;
        ctx->privateState = ASB_PRIVATE_ABSENT;
        ctx->privateToken = 0;
        ctx->privateWidth = ctx->privateHeight = 0;
        break;
    }
    ZeroMemory(reply, sizeof(*reply));
    reply->size = sizeof(*reply);
    reply->version = ASB_PRIVATE_DISPLAY_VERSION;
    reply->state = ctx->privateState;
    reply->width = ctx->privateWidth;
    reply->height = ctx->privateHeight;
    reply->desktop_id = ASB_DISPLAY_DESKTOP;
    reply->private_id = ASB_DISPLAY_PRIVATE;
    return STATUS_SUCCESS;
}
