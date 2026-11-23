/**
 *
 *
 * @module AppSandbox/tools/transport/asb_rw_files
 *
 * @file      asb_rw_files.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows file sessions (AppSandbox-eh6.9): the platform-neutral receive rules of
 * channel 11, shared by the guest helper and the host drag broker (see "Drag and drop" in
 * asb_remote_windows.h). Pure bookkeeping: the Win32 staging layer asks this model first and
 * only touches the disk for messages it accepts, so a hostile or broken sender can never write
 * outside the staging folder, overrun a declared size, skip a parent, alias an entry or finish
 * with incomplete files. Every limit is bounded before allocation.
 *
 * Accepted stream (indices are session-wide and strictly sequential):
 *   BEGIN(n top-level) ENTRY[0..n-1] (single segments)
 *   STREAM(N entries, T bytes) { ENTRY[n..N-1] | DATA }* END(T)
 * - nested entries name an existing directory entry as their parent;
 * - names are unique (case-insensitive, Windows folding);
 * - FILE_DATA continues the current file at exactly its received offset; a new entry or another
 *   file's data may only start once the current file is complete (top-level files declared
 *   before STREAM receive their data in the stream);
 * - END requires every entry, every byte and total_bytes == T.
 */
#ifndef ASB_RW_FILES_H
#define ASB_RW_FILES_H

#include <stdint.h>
#include "asb_remote_windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_RW_RX_IDLE      0u
#define ASB_RW_RX_TOP       1u   /* BEGIN accepted, top-level entries arriving */
#define ASB_RW_RX_READY     2u   /* all top-level entries known (hover / waiting for STREAM) */
#define ASB_RW_RX_STREAM    3u   /* data phase */
#define ASB_RW_RX_COMPLETE  4u   /* END verified */
#define ASB_RW_RX_FAILED    5u

typedef struct AsbRwRxEntry {
    uint64_t size, done;
    uint64_t key;                /* folded-name hash of the full relative path */
    uint32_t kind;               /* ASB_RW_ENTRY_* */
    uint32_t top;                /* index of its top-level item */
} AsbRwRxEntry;

typedef struct AsbRwFileRx {
    uint32_t phase;
    uint32_t error;              /* first AsbRwError after FAILED */
    uint64_t session_id;
    uint32_t direction;
    uint32_t top_count;          /* BEGIN.entry_count */
    uint32_t entry_count;        /* STREAM.entry_count (0 before STREAM) */
    uint32_t seen;               /* entries accepted */
    uint64_t total_bytes;        /* STREAM.total_bytes */
    uint64_t declared_bytes;     /* sum of accepted file sizes */
    uint64_t done_bytes;
    int64_t current;             /* file receiving data (-1 none) */
    AsbRwRxEntry *entries;       /* capacity grows to at most ASB_RW_MAX_FILE_ENTRIES */
    uint32_t capacity;
    uint64_t *keys;              /* open-addressing set: entry key -> */
    uint32_t *key_vals;          /* entry index + 1 */
    uint32_t key_slots;
} AsbRwFileRx;

void asb_rw_file_rx_init(AsbRwFileRx *rx);
void asb_rw_file_rx_free(AsbRwFileRx *rx);   /* back to IDLE */

/* Each returns ASB_RW_OK or the AsbRwError that failed the session (the model is then FAILED and
   rejects everything until free/init). ASB_RW_ERR_FILE_REJECTED: unsafe/duplicate/orphan name;
   ASB_RW_ERR_TOO_LARGE: limits; ASB_RW_ERR_MALFORMED: ordering/size/offset violations. */
uint32_t asb_rw_file_rx_begin(AsbRwFileRx *rx, const AsbRwFileBegin *b);
uint32_t asb_rw_file_rx_entry(AsbRwFileRx *rx, const AsbRwFileEntry *e);
uint32_t asb_rw_file_rx_stream(AsbRwFileRx *rx, const AsbRwFileStream *s);
uint32_t asb_rw_file_rx_data(AsbRwFileRx *rx, const AsbRwFileData *d);
uint32_t asb_rw_file_rx_end(AsbRwFileRx *rx, const AsbRwFileStatus *end);

/* The entry FILE_DATA d belongs to (valid after asb_rw_file_rx_data accepted it). */
const AsbRwRxEntry *asb_rw_file_rx_at(const AsbRwFileRx *rx, uint32_t index);

/* Case-insensitive (ASCII + Latin-1/Greek/Cyrillic simple folding) hash of a relative path,
   '/' treated as '\\'. Equal for names Windows treats as the same file in practice. */
uint64_t asb_rw_path_key(const uint16_t *rel, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif
