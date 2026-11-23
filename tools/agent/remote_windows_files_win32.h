/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_files_win32
 *
 * @file      remote_windows_files_win32.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows drag-and-drop file layer (AppSandbox-eh6.9), shared by the guest helper and the
 * host drag broker (both run as the signed-in user at medium integrity).
 *
 * Sources (sending side)
 *   asb_rw_source_top validates the dragged CF_HDROP paths: each must be an existing, local or
 *   UNC filesystem file or folder on a disk device, not a link (name-surrogate reparse point:
 *   symbolic link, junction, mount point), not offline / recall-on-access (cloud placeholders,
 *   generated or virtual items) and with a name the peer accepts (asb_rw_rel_path_safe).
 *   asb_rw_source_expand walks folders with the same rules: a link anywhere inside rejects the
 *   whole drag rather than being followed. Files are re-opened by handle when sent and must still
 *   be the same file (volume + file ID, no reparse point) with at least the collected size.
 *
 * Staging (receiving side)
 *   asb_rw_stage_* validates every message with the shared model (asb_rw_files.h) before any disk
 *   access and writes only inside a fresh private asb_clipboard_temp folder (owner-only ACL,
 *   crash-safe collection). Paths are built from validated relative names only; files are created
 *   with CREATE_NEW and FILE_FLAG_OPEN_REPARSE_POINT. Top-level placeholders exist from BEGIN so a
 *   drag can start before data moves. asb_rw_stage_close(keep = 0) deletes incomplete staging at
 *   once; keep = 1 (a delivered drop) retires it with the clipboard retention so destinations that
 *   copy asynchronously still find it. The clipboard itself is never touched.
 *
 * OLE (C++ only): shell data objects for staged items, CF_HDROP extraction, and minimal
 * callback-driven IDropSource / IDropTarget objects.
 */
#ifndef REMOTE_WINDOWS_FILES_WIN32_H
#define REMOTE_WINDOWS_FILES_WIN32_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include "../transport/asb_remote_windows.h"
#include "../transport/asb_rw_files.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_FILE_CHUNK_DEFAULT (512u * 1024u)

/* ---------------------------------- sources ---------------------------------- */

typedef struct AsbRwSourceItem {
    wchar_t *path;               /* absolute, \\?\ form */
    uint16_t *rel;
    uint16_t rel_len;
    uint32_t kind;               /* ASB_RW_ENTRY_* */
    uint64_t size;
    uint64_t file_id;
    uint32_t volume;
    uint32_t begin, end;         /* top-level folders: their descendants are items [begin, end) */
} AsbRwSourceItem;

typedef struct AsbRwSource {
    AsbRwSourceItem *items;      /* top-level 0..top_count-1, then descendants (pre-order per top folder) */
    uint32_t count, capacity, top_count;
    uint64_t total_bytes, top_bytes;
    int expanded;
    uint32_t error;              /* first failure (AsbRwError) */
    wchar_t detail[320];         /* the offending path or reason */
} AsbRwSource;

void asb_rw_source_init(AsbRwSource *s);
void asb_rw_source_free(AsbRwSource *s);
uint32_t asb_rw_source_top(AsbRwSource *s, const wchar_t *const *paths, uint32_t n);
/* Recursive walk; cancel (optional) is polled. */
uint32_t asb_rw_source_expand(AsbRwSource *s, volatile LONG *cancel);

typedef struct AsbRwFileSendOps {
    void *ctx;
    /* Sends a then b as one message (under the connection's send lock). 0 = failed. */
    int (*send)(void *ctx, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);
    int (*cancelled)(void *ctx);                              /* optional */
    void (*progress)(void *ctx, uint64_t done, uint64_t total);  /* optional */
} AsbRwFileSendOps;

/* BEGIN + the top-level ENTRY records. */
uint32_t asb_rw_source_send_top(const AsbRwSource *s, uint64_t session, uint32_t direction, uint64_t target,
                                int32_t x, int32_t y, const AsbRwFileSendOps *ops);
/* STREAM + remaining entries + data + END (needs asb_rw_source_expand). chunk <= ASB_RW_MAX_FILE_CHUNK. */
uint32_t asb_rw_source_send_stream(AsbRwSource *s, uint64_t session, uint32_t chunk, const AsbRwFileSendOps *ops);

/* Encodes one status-type message (FILE_CANCEL / RESULT / PROGRESS / END) and sends it. */
int asb_rw_files_send_status(const AsbRwFileSendOps *ops, uint16_t type, uint64_t session, uint32_t status,
                             uint64_t done, uint64_t total);

/* ---------------------------------- staging ---------------------------------- */

typedef struct AsbRwStage AsbRwStage;

/* parent: a folder the caller owns (created if missing). NULL on failure. */
AsbRwStage *asb_rw_stage_open(const wchar_t *parent);
/* One decoded channel-11 message of the session being received (BEGIN, ENTRY, STREAM, DATA, END). */
uint32_t asb_rw_stage_message(AsbRwStage *st, uint16_t type, const AsbRwPayload *msg);
uint32_t asb_rw_stage_phase(const AsbRwStage *st);         /* ASB_RW_RX_* */
uint64_t asb_rw_stage_session(const AsbRwStage *st);
void asb_rw_stage_progress(const AsbRwStage *st, uint64_t *done, uint64_t *total);
/* Absolute paths of the staged top-level items (valid until close). */
uint32_t asb_rw_stage_top(const AsbRwStage *st, const wchar_t *const **paths);
const wchar_t *asb_rw_stage_root(const AsbRwStage *st);
void asb_rw_stage_close(AsbRwStage *st, int keep);

/* Collects stale staging left by a crash under parent (call once at start). */
void asb_rw_stage_collect(const wchar_t *parent);

/* ---------------------------------- misc ---------------------------------- */

uint64_t asb_rw_files_random64(void);
/* "%LOCALAPPDATA%\AppSandbox\RemoteWindows\<leaf>" (created). FALSE on failure. */
BOOL asb_rw_files_user_dir(const wchar_t *leaf, wchar_t *out, size_t cap);

#ifdef __cplusplus
}

#include <ole2.h>

/* Real shell data object (CF_HDROP, Shell IDList Array, ...) for filesystem items sharing one
   parent folder, with CFSTR_PREFERREDDROPEFFECT = COPY. */
HRESULT asb_rw_shell_data_object(const wchar_t *const *paths, uint32_t n, IDataObject **out);
/* CF_HDROP paths of a data object. ASB_RW_ERR_UNSUPPORTED_DATA when it has none (text, virtual
   files only, ...). *paths is an array of n heap strings: free with asb_rw_free_paths. */
uint32_t asb_rw_hdrop_paths(IDataObject *data, wchar_t ***paths, uint32_t *n);
void asb_rw_free_paths(wchar_t **paths, uint32_t n);
BOOL asb_rw_data_has_files(IDataObject *data);

/* Drag source driven by callbacks. decide returns S_OK, DRAGDROP_S_DROP or DRAGDROP_S_CANCEL;
   feedback (optional) sees each effect and may return S_OK after setting its own cursor. */
struct AsbRwDropSourceOps {
    void *ctx;
    HRESULT (*decide)(void *ctx, BOOL escape, DWORD keys);
    HRESULT (*feedback)(void *ctx, DWORD effect);
};
IDropSource *asb_rw_drop_source_create(const AsbRwDropSourceOps *ops);

struct AsbRwDropTargetOps {
    void *ctx;
    DWORD (*enter)(void *ctx, IDataObject *data, POINTL pt, DWORD keys);
    DWORD (*over)(void *ctx, POINTL pt, DWORD keys);
    void (*leave)(void *ctx);
    DWORD (*drop)(void *ctx, IDataObject *data, POINTL pt, DWORD keys);
};
IDropTarget *asb_rw_drop_target_create(const AsbRwDropTargetOps *ops);

/* Logical mouse button state from the physical keys (swapped buttons honored). */
BOOL asb_rw_primary_down(void);
BOOL asb_rw_any_button_down(void);
#endif

#endif
