/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_files_test
 *
 * @file      remote_windows_files_test.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows file-session receive rules (AppSandbox-eh6.9): asb_rw_files.c model tests.
   Every hostile or broken stream must fail before the staging layer would touch the disk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../transport/asb_rw_files.h"

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static AsbRwFileRx rx;
static uint32_t g_index;

static uint32_t begin(uint64_t session, uint32_t top)
{
    AsbRwFileBegin b;
    memset(&b, 0, sizeof(b));
    b.session_id = session;
    b.direction = ASB_RW_DIR_TO_GUEST;
    b.target_id = 1;
    b.entry_count = top;
    g_index = 0;
    return asb_rw_file_rx_begin(&rx, &b);
}

static uint32_t entry_at(uint64_t session, uint32_t index, const wchar_t *rel, uint32_t kind, uint64_t size)
{
    AsbRwFileEntry e;
    memset(&e, 0, sizeof(e));
    e.session_id = session;
    e.index = index;
    e.kind = kind;
    e.size = size;
    e.rel_len = (uint16_t)wcslen(rel);
    for (uint16_t i = 0; i < e.rel_len; ++i) e.rel[i] = (uint16_t)rel[i];
    return asb_rw_file_rx_entry(&rx, &e);
}

static uint32_t entry(const wchar_t *rel, uint32_t kind, uint64_t size)
{
    uint32_t r = entry_at(1, g_index, rel, kind, size);
    if (!r) ++g_index;
    return r;
}

static uint32_t stream(uint32_t count, uint64_t total)
{
    AsbRwFileStream s = { 1, count, total };
    return asb_rw_file_rx_stream(&rx, &s);
}

static uint32_t data(uint32_t index, uint64_t offset, uint32_t len)
{
    static const uint8_t bytes[64] = { 0 };
    AsbRwFileData d;
    memset(&d, 0, sizeof(d));
    d.session_id = 1;
    d.index = index;
    d.offset = offset;
    d.len = len;
    d.data = bytes;
    return asb_rw_file_rx_data(&rx, &d);
}

static uint32_t end(uint64_t done, uint64_t total, uint32_t status)
{
    AsbRwFileStatus e = { 1, status, done, total };
    return asb_rw_file_rx_end(&rx, &e);
}

static void reset(void)
{
    asb_rw_file_rx_free(&rx);
}

static void test_valid_stream(void)
{
    /* Two top-level items: a file and a nested Unicode folder tree. */
    reset();
    CHECK_EQ(begin(1, 2), ASB_RW_OK);
    CHECK_EQ(rx.phase, ASB_RW_RX_TOP);
    CHECK_EQ(entry(L"r\u00e9sum\u00e9.txt", ASB_RW_ENTRY_FILE, 10), ASB_RW_OK);
    CHECK_EQ(entry(L"Fold\u00e9r \u2713", ASB_RW_ENTRY_DIR, 0), ASB_RW_OK);
    CHECK_EQ(rx.phase, ASB_RW_RX_READY);
    CHECK_EQ(stream(6, 10 + 20 + 0 + 5), ASB_RW_OK);
    CHECK_EQ(data(0, 0, 4), ASB_RW_OK);          /* top-level file's data arrives in the stream */
    CHECK_EQ(data(0, 4, 6), ASB_RW_OK);
    CHECK_EQ(entry(L"Fold\u00e9r \u2713\\sub", ASB_RW_ENTRY_DIR, 0), ASB_RW_OK);
    CHECK_EQ(entry(L"Fold\u00e9r \u2713\\sub\\a.bin", ASB_RW_ENTRY_FILE, 20), ASB_RW_OK);
    CHECK_EQ(data(3, 0, 20), ASB_RW_OK);
    CHECK_EQ(entry(L"Fold\u00e9r \u2713\\empty.txt", ASB_RW_ENTRY_FILE, 0), ASB_RW_OK);
    CHECK_EQ(entry(L"Fold\u00e9r \u2713/sub/\u65e5\u672c.txt", ASB_RW_ENTRY_FILE, 5), ASB_RW_OK);   /* '/' separators */
    CHECK_EQ(data(5, 0, 5), ASB_RW_OK);
    CHECK_EQ(end(35, 35, ASB_RW_OK), ASB_RW_OK);
    CHECK_EQ(rx.phase, ASB_RW_RX_COMPLETE);
    CHECK_EQ(rx.done_bytes, 35);
}

static void test_ordering(void)
{
    /* Index gaps, wrong session, entries before BEGIN, STREAM before the top-level items. */
    reset();
    CHECK_EQ(entry(L"a", ASB_RW_ENTRY_FILE, 1), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2);
    CHECK_EQ(entry_at(1, 1, L"a", ASB_RW_ENTRY_FILE, 1), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1);
    CHECK_EQ(entry_at(2, 0, L"a", ASB_RW_ENTRY_FILE, 1), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2);
    entry(L"a", ASB_RW_ENTRY_FILE, 1);
    CHECK_EQ(stream(2, 1), ASB_RW_ERR_MALFORMED);
    CHECK_EQ(rx.phase, ASB_RW_RX_FAILED);
    CHECK_EQ(entry(L"b", ASB_RW_ENTRY_FILE, 1), ASB_RW_ERR_MALFORMED);   /* failed stays failed */
    /* A second BEGIN in the same session. */
    reset();
    begin(1, 1);
    CHECK_EQ(begin(1, 1), ASB_RW_ERR_MALFORMED);
    /* Too many top-level items. */
    reset();
    CHECK_EQ(begin(1, ASB_RW_MAX_TOP_ENTRIES + 1), ASB_RW_ERR_TOO_LARGE);
    /* STREAM bounds: fewer entries than the top level, over the limit, total below declared. */
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 5); entry(L"b", ASB_RW_ENTRY_DIR, 0);
    CHECK_EQ(stream(1, 5), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_DIR, 0);
    CHECK_EQ(stream(ASB_RW_MAX_FILE_ENTRIES + 1, 0), ASB_RW_ERR_TOO_LARGE);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 5);
    CHECK_EQ(stream(1, 4), ASB_RW_ERR_MALFORMED);
}

static void test_names(void)
{
    static const wchar_t *const hostile[] = { L"..", L".", L"..\\etc", L"a\\..\\b", L"\\root", L"C:\\Windows", L"C:x",
                                              L"file:stream", L"CON", L"lpt1.log", L"trail.", L"trail ", L"a\\\\b",
                                              L"\\\\server\\share", L"\\\\?\\C:\\x", L"tab\there", L"<>", L"wild*" };
    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); ++i) {
        reset();
        begin(1, 1);
        CHECK_EQ(entry(hostile[i], ASB_RW_ENTRY_FILE, 1), ASB_RW_ERR_FILE_REJECTED);
    }
    /* Top-level entries are single segments; nested ones need an existing directory parent. */
    reset();
    begin(1, 1);
    CHECK_EQ(entry(L"a\\b", ASB_RW_ENTRY_FILE, 1), ASB_RW_ERR_FILE_REJECTED);
    reset();
    begin(1, 2); entry(L"dir", ASB_RW_ENTRY_DIR, 0); entry(L"file", ASB_RW_ENTRY_FILE, 0);
    stream(4, 0);
    CHECK_EQ(entry(L"other\\x", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);          /* orphan */
    reset();
    begin(1, 2); entry(L"dir", ASB_RW_ENTRY_DIR, 0); entry(L"file", ASB_RW_ENTRY_FILE, 0);
    stream(4, 0);
    CHECK_EQ(entry(L"file\\x", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);           /* under a file */
    reset();
    begin(1, 2); entry(L"dir", ASB_RW_ENTRY_DIR, 0); entry(L"file", ASB_RW_ENTRY_FILE, 0);
    stream(4, 0);
    CHECK_EQ(entry(L"dir\\x", ASB_RW_ENTRY_FILE, 0), ASB_RW_OK);
    CHECK_EQ(entry(L"dir\\x\\y", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);         /* x is a file */
    /* A top-level item may only be named in the top-level phase. */
    reset();
    begin(1, 1); entry(L"dir", ASB_RW_ENTRY_DIR, 0);
    stream(2, 0);
    CHECK_EQ(entry(L"late", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);
    /* Duplicates and case aliases. */
    reset();
    begin(1, 2); entry(L"Readme.TXT", ASB_RW_ENTRY_FILE, 0);
    CHECK_EQ(entry(L"README.txt", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);
    reset();
    begin(1, 1); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 0);
    entry(L"d\\\u00c9t\u00e9", ASB_RW_ENTRY_DIR, 0);
    CHECK_EQ(entry(L"d/\u00e9T\u00c9", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);
    /* Directories carry no size; unknown kinds. */
    reset();
    begin(1, 1);
    CHECK_EQ(entry(L"d", ASB_RW_ENTRY_DIR, 1), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1);
    CHECK_EQ(entry(L"d", 9, 0), ASB_RW_ERR_MALFORMED);
}

static void test_data(void)
{
    /* Data before STREAM, for a directory, beyond the size, at a wrong offset, interleaved files,
       a file restarted, a new entry while a file is incomplete, zero-length chunks. */
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 8); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    CHECK_EQ(data(0, 0, 4), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 8); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 16);
    CHECK_EQ(data(1, 0, 4), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 8); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 16);
    CHECK_EQ(data(0, 0, 9), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 8); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 16);
    CHECK_EQ(data(0, 1, 4), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 8); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 16);
    data(0, 0, 4);
    CHECK_EQ(entry(L"d\\b", ASB_RW_ENTRY_FILE, 8), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2); entry(L"a", ASB_RW_ENTRY_FILE, 8); entry(L"b", ASB_RW_ENTRY_FILE, 8);
    stream(2, 16);
    data(0, 0, 4);
    CHECK_EQ(data(1, 0, 4), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 8);
    stream(1, 8);
    data(0, 0, 8);
    CHECK_EQ(data(0, 0, 1), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 8);
    stream(1, 8);
    CHECK_EQ(data(0, 0, 0), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 8);
    stream(1, 8);
    CHECK_EQ(data(7, 0, 1), ASB_RW_ERR_MALFORMED);
    /* A nested file's data must follow its entry: skipping it (next entry, END, or another file's
       data) fails, so a receiver never keeps a half-written file open. */
    reset();
    begin(1, 1); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 16);
    CHECK_EQ(entry(L"d\\a", ASB_RW_ENTRY_FILE, 8), ASB_RW_OK);
    CHECK_EQ(entry(L"d\\b", ASB_RW_ENTRY_FILE, 8), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 2); entry(L"top.bin", ASB_RW_ENTRY_FILE, 4); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(3, 12);
    CHECK_EQ(entry(L"d\\a", ASB_RW_ENTRY_FILE, 8), ASB_RW_OK);
    CHECK_EQ(data(0, 0, 4), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(2, 8);
    entry(L"d\\a", ASB_RW_ENTRY_FILE, 8);
    CHECK_EQ(end(0, 8, ASB_RW_OK), ASB_RW_ERR_MALFORMED);
    /* Nested sizes may not exceed the declared total. */
    reset();
    begin(1, 1); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(2, 4);
    CHECK_EQ(entry(L"d\\big", ASB_RW_ENTRY_FILE, 5), ASB_RW_ERR_MALFORMED);
}

static void test_end(void)
{
    /* END with missing entries, missing bytes, wrong totals, an error status. */
    reset();
    begin(1, 1); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(2, 0);
    CHECK_EQ(end(0, 0, ASB_RW_OK), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 8);
    stream(1, 8);
    data(0, 0, 4);
    CHECK_EQ(end(4, 8, ASB_RW_OK), ASB_RW_ERR_MALFORMED);   /* truncated transfer */
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 8);
    stream(1, 8);
    data(0, 0, 8);
    CHECK_EQ(end(8, 9, ASB_RW_OK), ASB_RW_ERR_MALFORMED);
    reset();
    begin(1, 1); entry(L"a", ASB_RW_ENTRY_FILE, 8);
    stream(1, 8);
    data(0, 0, 8);
    CHECK_EQ(end(8, 8, ASB_RW_ERR_FILE_IO), ASB_RW_ERR_FILE_IO);
    CHECK_EQ(rx.error, ASB_RW_ERR_FILE_IO);
    /* STREAM total larger than what the entries declare is caught at END. */
    reset();
    begin(1, 1); entry(L"d", ASB_RW_ENTRY_DIR, 0);
    stream(2, 10);
    entry(L"d\\a", ASB_RW_ENTRY_FILE, 4);
    data(1, 0, 4);
    CHECK_EQ(end(4, 10, ASB_RW_OK), ASB_RW_ERR_MALFORMED);
}

static void test_scale(void)
{
    /* Many entries: the key set grows; duplicates are still found at scale. */
    wchar_t name[64];
    reset();
    begin(1, 1); entry(L"root", ASB_RW_ENTRY_DIR, 0);
    CHECK_EQ(stream(20001, 0), ASB_RW_OK);
    uint32_t bad = 0;
    for (int i = 0; i < 20000; ++i) {
        swprintf(name, 64, L"root\\f%05d.txt", i);
        bad += entry(name, ASB_RW_ENTRY_FILE, 0) != ASB_RW_OK;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(end(0, 0, ASB_RW_OK), ASB_RW_OK);
    reset();
    begin(1, 1); entry(L"root", ASB_RW_ENTRY_DIR, 0);
    stream(5001, 0);
    for (int i = 0; i < 4999; ++i) { swprintf(name, 64, L"root\\f%05d", i); entry(name, ASB_RW_ENTRY_FILE, 0); }
    CHECK_EQ(entry(L"ROOT\\F01234", ASB_RW_ENTRY_FILE, 0), ASB_RW_ERR_FILE_REJECTED);
    /* Keys fold case but distinguish different names. */
    uint16_t a[] = { 'A', '\\', 'b' }, b[] = { 'a', '/', 'B' }, c[] = { 'a', '\\', 'c' };
    CHECK(asb_rw_path_key(a, 3) == asb_rw_path_key(b, 3));
    CHECK(asb_rw_path_key(a, 3) != asb_rw_path_key(c, 3));
    reset();
}

int main(void)
{
    asb_rw_file_rx_init(&rx);
    test_valid_stream();
    test_ordering();
    test_names();
    test_data();
    test_end();
    test_scale();
    if (g_failures) {
        printf("FAIL remote windows files model: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS remote windows files model: stream rules, ordering, hostile names, orphans/duplicates/case aliases, "
           "data sequencing, truncation/totals, scale (%d checks)\n", g_checks);
    return 0;
}
