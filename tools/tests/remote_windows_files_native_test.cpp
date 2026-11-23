/**
 *
 *
 * @module AppSandbox/tools/tests/remote_windows_files_native_test
 *
 * @file      remote_windows_files_native_test.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows drag and drop, native suite (AppSandbox-eh6.9).
 *
 *   sources   real trees: nested Unicode folders, empty files/folders, a large file; junctions,
 *             symbolic links, offline (virtual) items, missing paths, devices and drive roots
 *             are rejected; a file changed during the copy fails the stream.
 *   staging   streams into private staging and compares byte for byte; truncated and hostile
 *             streams fail and their staging folder is removed.
 *   bridge    the guest bridge (remote_windows_files.cpp) and the host broker
 *             (remote_windows_drag_host.cpp) in one process over loopback TCP with real shell
 *             data objects, real OLE drop targets/sources and real IFileOperation copies; only
 *             the two DoDragDrop loops and the pointer are simulated (a real drag here would
 *             follow the desktop's mouse). Host -> guest: enter/over feedback, drop, conflict
 *             renaming by the destination, guest target refusal, leave, unsupported data,
 *             links, missing sources. Guest -> host: probe, deferred drop until staged, Escape,
 *             unsupported (sink + release), no drag. Disconnect mid-transfer. Sources stay
 *             intact, the clipboard is never touched and the guest button is always released.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <atomic>

#include "../agent/remote_windows_files.h"
#include "../agent/remote_windows_files_win32.h"
#include "../../src/backend_win/remote_windows_drag_host.h"
#include "../../src/core/protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fflush(stdout); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); fflush(stdout); } } while (0)

static std::wstring g_root;          /* test folder */
static bool g_verbose;

static void tlog(void *ctx, const char *m)
{
    (void)ctx;
    if (g_verbose) { printf("  [%s] %s\n", ctx ? (const char *)ctx : "-", m); fflush(stdout); }
}

/* ================================ filesystem helpers ====================== */

static void write_file(const std::wstring &path, size_t size, unsigned seed)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::vector<uint8_t> buf(1 << 20);
    size_t left = size;
    uint32_t x = seed * 2654435761u + 1;
    while (left) {
        size_t n = left < buf.size() ? left : buf.size();
        for (size_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (uint8_t)x; }
        DWORD w = 0;
        WriteFile(h, buf.data(), (DWORD)n, &w, nullptr);
        left -= n;
    }
    CloseHandle(h);
}

static void mkdirs(const std::wstring &p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }

static void remove_tree(const std::wstring &p)
{
    std::wstring from = p + L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

/* FNV over relative names, kinds and contents (sorted listing): equal trees hash equal. */
static uint64_t tree_hash(const std::wstring &path, const std::wstring &rel = L"")
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    mix(rel.data(), rel.size() * 2);
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) names.push_back(fd.cFileName); } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        for (size_t i = 0; i < names.size(); ++i)
            for (size_t j = i + 1; j < names.size(); ++j) if (_wcsicmp(names[j].c_str(), names[i].c_str()) < 0) std::swap(names[i], names[j]);
        for (const std::wstring &n : names) { uint64_t c = tree_hash(path + L"\\" + n, n); mix(&c, sizeof(c)); }
        mix("D", 1);
    } else {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        std::vector<uint8_t> buf(1 << 20);
        DWORD got = 0;
        while (f != INVALID_HANDLE_VALUE && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) mix(buf.data(), got);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        mix("F", 1);
    }
    return h;
}

/* Mount-point (junction) reparse point; needs no privilege. */
static bool make_junction(const std::wstring &link, const std::wstring &target)
{
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring sub = L"\\??\\" + target;
    std::vector<uint8_t> buf(16 + (sub.size() + 1 + 1) * 2 + 8);
    REPARSE_GUID_DATA_BUFFER *unused = nullptr; (void)unused;
    struct Mp { DWORD tag; WORD len; WORD res; WORD sub_off, sub_len, print_off, print_len; WCHAR path[1]; };
    Mp *r = (Mp *)buf.data();
    r->tag = IO_REPARSE_TAG_MOUNT_POINT;
    r->sub_off = 0;
    r->sub_len = (WORD)(sub.size() * 2);
    r->print_off = (WORD)(r->sub_len + 2);
    r->print_len = 0;
    memcpy(r->path, sub.c_str(), sub.size() * 2);
    r->path[sub.size()] = 0;
    r->path[sub.size() + 1] = 0;
    r->len = (WORD)(8 + r->sub_len + 2 + 2);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, r, 8 + r->len, nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    return ok != 0;
}

/* Builds the standard host-side source tree; returns its top-level items. */
static std::vector<std::wstring> build_tree(const std::wstring &base, size_t big)
{
    std::wstring folder = base + L"\\H\u00f4st folder \u2713";
    mkdirs(folder + L"\\sub\\deeper \u65e5\u672c");
    mkdirs(folder + L"\\empty folder");
    write_file(folder + L"\\sub\\b \u00fcn\u00ef.txt", 12345, 1);
    write_file(folder + L"\\sub\\deeper \u65e5\u672c\\\u0434\u0430\u043d\u043d\u044b\u0435.bin", 3 * 1024 * 1024 + 17, 2);
    write_file(folder + L"\\zero.txt", 0, 3);
    write_file(folder + L"\\hidden.dat", 777, 4);
    SetFileAttributesW((folder + L"\\hidden.dat").c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::wstring file = base + L"\\big \u00e9l\u00e8ve.bin";
    write_file(file, big, 5);
    return { folder, file };
}

/* ================================ sources & staging ======================= */

struct MemSink {
    std::vector<uint8_t> bytes;
    int fail_after = -1;   /* messages */
    int sent = 0;
};

static int mem_send(void *ctx, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    MemSink *s = (MemSink *)ctx;
    if (s->fail_after >= 0 && s->sent >= s->fail_after) return 0;
    s->bytes.insert(s->bytes.end(), a, a + alen);
    if (blen) s->bytes.insert(s->bytes.end(), b, b + blen);
    s->sent++;
    return 1;
}

/* Feeds a recorded stream into staging; skip_type drops the first message of that type. */
static uint32_t replay(AsbRwStage *st, const std::vector<uint8_t> &bytes, uint16_t skip_type = 0)
{
    static AsbRwPayload m;
    size_t pos = 0;
    bool skipped = false;
    while (pos < bytes.size()) {
        size_t total = 0;
        uint32_t r = asb_rw_frame_check(ASB_CH_RW_FILES, bytes.data() + pos, bytes.size() - pos, &total);
        if (r) return r;
        AsbRwHeader h;
        r = asb_rw_decode(ASB_CH_RW_FILES, bytes.data() + pos, total, &h, &m, nullptr, nullptr);
        if (r) return r;
        pos += total;
        if (h.type == skip_type && !skipped) { skipped = true; continue; }
        r = asb_rw_stage_message(st, h.type, &m);
        if (r) return r;
    }
    return ASB_RW_OK;
}

static bool wait_gone(const std::wstring &path, DWORD ms)
{
    ULONGLONG until = GetTickCount64() + ms;
    while (exists(path) && GetTickCount64() < until) Sleep(50);
    return !exists(path);
}

static void test_sources_and_staging()
{
    std::wstring base = g_root + L"\\src";
    mkdirs(base);
    std::vector<std::wstring> top = build_tree(base, 9 * 1024 * 1024 + 3);
    const wchar_t *paths[2] = { top[0].c_str(), top[1].c_str() };
    AsbRwSource src;
    asb_rw_source_init(&src);
    CHECK_EQ(asb_rw_source_top(&src, paths, 2), ASB_RW_OK);
    CHECK_EQ(src.top_count, 2);
    CHECK_EQ(asb_rw_source_expand(&src, nullptr), ASB_RW_OK);
    CHECK_EQ(src.count, 2 + 7);   /* sub, b.txt, deeper, data.bin, empty folder, zero, hidden */
    CHECK_EQ(src.total_bytes, (uint64_t)(9 * 1024 * 1024 + 3 + 12345 + 3 * 1024 * 1024 + 17 + 777));
    /* Parents precede children. */
    bool ordered = true;
    for (uint32_t i = src.top_count; i < src.count; ++i) {
        std::wstring rel((const wchar_t *)src.items[i].rel, src.items[i].rel_len);
        size_t slash = rel.find_last_of(L'\\');
        std::wstring parent = rel.substr(0, slash);
        bool found = false;
        for (uint32_t j = 0; j < i; ++j)
            found |= std::wstring((const wchar_t *)src.items[j].rel, src.items[j].rel_len) == parent && src.items[j].kind == ASB_RW_ENTRY_DIR;
        ordered &= found;
    }
    CHECK(ordered);

    /* Stream into staging and compare. */
    MemSink sink;
    AsbRwFileSendOps ops = {};
    ops.ctx = &sink;
    ops.send = mem_send;
    CHECK_EQ(asb_rw_source_send_top(&src, 42, ASB_RW_DIR_TO_GUEST, 7, 1, 2, &ops), ASB_RW_OK);
    CHECK_EQ(asb_rw_source_send_stream(&src, 42, 256 * 1024, &ops), ASB_RW_OK);
    std::wstring parent = g_root + L"\\stage";
    AsbRwStage *st = asb_rw_stage_open(parent.c_str());
    CHECK(st != nullptr);
    CHECK_EQ(replay(st, sink.bytes), ASB_RW_OK);
    CHECK_EQ(asb_rw_stage_phase(st), ASB_RW_RX_COMPLETE);
    const wchar_t *const *staged = nullptr;
    CHECK_EQ(asb_rw_stage_top(st, &staged), 2);
    CHECK(tree_hash(staged[0], L"x") == tree_hash(top[0], L"x"));
    CHECK(tree_hash(staged[1], L"x") == tree_hash(top[1], L"x"));
    std::wstring kept = asb_rw_stage_root(st);
    asb_rw_stage_close(st, 1);   /* delivered: kept for the retention period */
    Sleep(300);
    CHECK(exists(kept));

    /* Truncated stream (one DATA message lost): END fails, staging is deleted. */
    st = asb_rw_stage_open(parent.c_str());
    CHECK(replay(st, sink.bytes, ASB_RW_FILE_DATA) != ASB_RW_OK);
    std::wstring dead = asb_rw_stage_root(st);
    std::wstring dead_root = dead.substr(0, dead.find_last_of(L'\\'));
    asb_rw_stage_close(st, 0);
    if (!wait_gone(dead_root, 10000)) {
        CHECK(!"truncated staging removed");
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dead_root + L"\\d\\*").c_str(), &fd);
        while (f != INVALID_HANDLE_VALUE) { printf("  left: %ls\n", fd.cFileName); if (!FindNextFileW(f, &fd)) { FindClose(f); break; } }
        std::vector<std::wstring> stack = { dead_root + L"\\d" };
        while (!stack.empty()) {
            std::wstring d = stack.back(); stack.pop_back();
            WIN32_FIND_DATAW e;
            HANDLE g = FindFirstFileW((d + L"\\*").c_str(), &e);
            if (g == INVALID_HANDLE_VALUE) continue;
            do {
                if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..")) continue;
                std::wstring q = d + L"\\" + e.cFileName;
                BOOL ok = (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(q.c_str()) : DeleteFileW(q.c_str());
                printf("  item attrs=%08lx delete=%d err=%lu len=%zu\n", e.dwFileAttributes, ok, ok ? 0 : GetLastError(), q.size());
                if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) stack.push_back(q);
            } while (FindNextFileW(g, &e));
            FindClose(g);
        }
    }

    /* Hostile receiver input: an orphan nested entry and a duplicate never reach the disk. */
    st = asb_rw_stage_open(parent.c_str());
    AsbRwPayload m = {};
    m.file_begin.session_id = 9; m.file_begin.direction = ASB_RW_DIR_TO_GUEST; m.file_begin.target_id = 1; m.file_begin.entry_count = 1;
    CHECK_EQ(asb_rw_stage_message(st, ASB_RW_FILE_BEGIN, &m), ASB_RW_OK);
    memset(&m, 0, sizeof(m));
    m.file_entry.session_id = 9; m.file_entry.kind = ASB_RW_ENTRY_DIR;
    m.file_entry.rel_len = 1; m.file_entry.rel[0] = 'd';
    CHECK_EQ(asb_rw_stage_message(st, ASB_RW_FILE_ENTRY, &m), ASB_RW_OK);
    memset(&m, 0, sizeof(m));
    m.file_stream.session_id = 9; m.file_stream.entry_count = 3;
    CHECK_EQ(asb_rw_stage_message(st, ASB_RW_FILE_STREAM, &m), ASB_RW_OK);
    memset(&m, 0, sizeof(m));
    m.file_entry.session_id = 9; m.file_entry.index = 1; m.file_entry.kind = ASB_RW_ENTRY_FILE;
    const wchar_t *orphan = L"x\\evil.txt";
    m.file_entry.rel_len = (uint16_t)wcslen(orphan);
    for (uint16_t i = 0; i < m.file_entry.rel_len; ++i) m.file_entry.rel[i] = orphan[i];
    CHECK_EQ(asb_rw_stage_message(st, ASB_RW_FILE_ENTRY, &m), ASB_RW_ERR_FILE_REJECTED);
    CHECK(!exists(std::wstring(asb_rw_stage_root(st)) + L"\\x"));
    dead = asb_rw_stage_root(st);
    dead_root = dead.substr(0, dead.find_last_of(L'\\'));
    asb_rw_stage_close(st, 0);
    CHECK(wait_gone(dead_root, 10000));

    /* A source that changes after it was walked fails the stream (never sends short data). */
    std::wstring changing = base + L"\\changing.bin";
    write_file(changing, 200000, 9);
    const wchar_t *cp[1] = { changing.c_str() };
    AsbRwSource s2;
    asb_rw_source_init(&s2);
    CHECK_EQ(asb_rw_source_top(&s2, cp, 1), ASB_RW_OK);
    CHECK_EQ(asb_rw_source_expand(&s2, nullptr), ASB_RW_OK);
    write_file(changing, 1000, 9);   /* truncated */
    MemSink sink2;
    ops.ctx = &sink2;
    CHECK_EQ(asb_rw_source_send_stream(&s2, 43, 0, &ops), ASB_RW_ERR_FILE_IO);
    asb_rw_source_free(&s2);
    /* Replaced by another file of the same name: identity mismatch. */
    write_file(changing, 200000, 9);
    asb_rw_source_init(&s2);
    asb_rw_source_top(&s2, cp, 1);
    asb_rw_source_expand(&s2, nullptr);
    DeleteFileW(changing.c_str());
    write_file(changing, 200000, 9);
    sink2.bytes.clear();
    CHECK_EQ(asb_rw_source_send_stream(&s2, 44, 0, &ops), ASB_RW_ERR_FILE_IO);
    asb_rw_source_free(&s2);

    /* Rejections: junction inside a folder and at the top, offline item, missing, devices, roots. */
    std::wstring jfolder = base + L"\\with link";
    mkdirs(jfolder);
    write_file(jfolder + L"\\ok.txt", 10, 1);
    bool junction = make_junction(jfolder + L"\\escape", g_root);
    CHECK(junction);
    const wchar_t *jp[1] = { jfolder.c_str() };
    asb_rw_source_init(&s2);
    CHECK_EQ(asb_rw_source_top(&s2, jp, 1), ASB_RW_OK);
    CHECK_EQ(asb_rw_source_expand(&s2, nullptr), ASB_RW_ERR_FILE_REJECTED);
    CHECK(wcsstr(s2.detail, L"link") != nullptr);
    asb_rw_source_free(&s2);
    std::wstring jtop = jfolder + L"\\escape";
    const wchar_t *jt[1] = { jtop.c_str() };
    asb_rw_source_init(&s2);
    CHECK_EQ(asb_rw_source_top(&s2, jt, 1), ASB_RW_ERR_FILE_REJECTED);
    asb_rw_source_free(&s2);
    std::wstring slink = base + L"\\file link.txt";
    if (CreateSymbolicLinkW(slink.c_str(), (jfolder + L"\\ok.txt").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const wchar_t *sl[1] = { slink.c_str() };
        asb_rw_source_init(&s2);
        CHECK_EQ(asb_rw_source_top(&s2, sl, 1), ASB_RW_ERR_FILE_REJECTED);
        asb_rw_source_free(&s2);
    }
    std::wstring vfolder = base + L"\\virtual";
    mkdirs(vfolder);
    write_file(vfolder + L"\\placeholder.txt", 10, 1);
    SetFileAttributesW((vfolder + L"\\placeholder.txt").c_str(), FILE_ATTRIBUTE_OFFLINE);
    const wchar_t *vp[1] = { vfolder.c_str() };
    asb_rw_source_init(&s2);
    asb_rw_source_top(&s2, vp, 1);
    CHECK_EQ(asb_rw_source_expand(&s2, nullptr), ASB_RW_ERR_FILE_REJECTED);
    asb_rw_source_free(&s2);
    std::wstring missing = base + L"\\does not exist.txt";
    const wchar_t *bad[] = { missing.c_str(), L"\\\\.\\PhysicalDrive0", L"\\\\?\\GLOBALROOT\\Device\\Null", L"C:\\", L"NUL", L"" };
    for (const wchar_t *b : bad) {
        const wchar_t *one[1] = { b };
        asb_rw_source_init(&s2);
        CHECK(asb_rw_source_top(&s2, one, 1) != ASB_RW_OK);
        asb_rw_source_free(&s2);
    }
    /* Duplicate top-level names (different folders, same name). */
    mkdirs(base + L"\\a");
    mkdirs(base + L"\\b");
    write_file(base + L"\\a\\same.txt", 1, 1);
    write_file(base + L"\\b\\SAME.txt", 1, 1);
    std::wstring d1 = base + L"\\a\\same.txt", d2 = base + L"\\b\\SAME.txt";
    const wchar_t *dup[2] = { d1.c_str(), d2.c_str() };
    asb_rw_source_init(&s2);
    CHECK_EQ(asb_rw_source_top(&s2, dup, 2), ASB_RW_ERR_FILE_REJECTED);
    asb_rw_source_free(&s2);
    asb_rw_source_free(&src);
}

/* ================================ bridge ================================== */

static SOCKET g_listen = INVALID_SOCKET;
static int g_port;
static AsbRwFiles *g_files;
static AsbRwDragHost *g_host;
static std::atomic<int> g_guest_effect{ DROPEFFECT_COPY };   /* what the simulated guest drop target accepts */
static std::atomic<bool> g_guest_escape{ false }, g_host_escape{ false };
static std::wstring g_guest_dest, g_host_dest, g_guest_src_folder;
static std::atomic<int> g_pressed{ 0 }, g_ups{ 0 };
static std::atomic<int32_t> g_last_x{ 0 }, g_last_y{ 0 };
static CRITICAL_SECTION g_state_lock;
static std::vector<AsbRwDrag> g_states;
static std::atomic<int> g_probe_mode{ 0 };   /* 0 none, 1 files, 2 text */
static IDataObject *g_probe_data;

static SOCKET guest_accept(void *ctx, DWORD timeout_ms)
{
    (void)ctx;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(g_listen, &rd);
    timeval tv = { 0, (long)timeout_ms * 1000 };
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return INVALID_SOCKET;
    return accept(g_listen, nullptr, nullptr);
}

static UINT_PTR host_connect(void *ctx, int channel, DWORD timeout_ms)
{
    (void)ctx; (void)channel; (void)timeout_ms;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr *)&a, sizeof(a))) { closesocket(s); return (UINT_PTR)INVALID_SOCKET; }
    return (UINT_PTR)s;
}

/* The fake host manager: allows host -> guest drags (as the session does after claiming input). */
static void guest_drag_state(void *ctx, const AsbRwDrag *d)
{
    (void)ctx;
    EnterCriticalSection(&g_state_lock);
    g_states.push_back(*d);
    LeaveCriticalSection(&g_state_lock);
    if (d->state == ASB_RW_DRAG_ACTIVE && d->direction == ASB_RW_DIR_TO_GUEST) {
        AsbRwDrag ack = {};
        ack.session_id = d->session_id;
        ack.state = ASB_RW_DRAG_ALLOW;
        asb_rw_files_control(g_files, ASB_RW_DRAG_ACK, &ack);
    }
}

static int count_states(uint32_t state)
{
    EnterCriticalSection(&g_state_lock);
    int n = 0;
    for (const AsbRwDrag &d : g_states) n += d.state == state;
    LeaveCriticalSection(&g_state_lock);
    return n;
}

static void guest_move(void *ctx, int32_t x, int32_t y) { (void)ctx; g_last_x = x; g_last_y = y; }
static void guest_button(void *ctx, uint32_t button, int down)
{
    (void)ctx; (void)button;
    if (down) g_pressed++;
    else { g_ups++; if (g_pressed > 0) g_pressed--; }
}

/* The destination application: copies the dropped CF_HDROP items with the shell, which applies
   its own conflict policy (here: rename on collision). */
static HRESULT shell_copy(IDataObject *data, const std::wstring &dest)
{
    wchar_t **paths = nullptr;
    uint32_t n = 0;
    if (asb_rw_hdrop_paths(data, &paths, &n)) return E_FAIL;
    IFileOperation *op = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    IShellItem *to = nullptr;
    if (SUCCEEDED(hr)) hr = op->SetOperationFlags(FOF_NO_UI | FOF_RENAMEONCOLLISION);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&to));
    for (uint32_t i = 0; SUCCEEDED(hr) && i < n; ++i) {
        IShellItem *item = nullptr;
        hr = SHCreateItemFromParsingName(paths[i], nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) { hr = op->CopyItem(item, to, nullptr, nullptr); item->Release(); }
    }
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    if (to) to->Release();
    if (op) op->Release();
    asb_rw_free_paths(paths, n);
    return hr;
}

/* Guest DoDragDrop stand-in: polls the real drag source, reports the simulated target's effect
   and copies on drop. */
static long guest_run_drag(void *ctx, void *pdata, void *psource, unsigned long *effect)
{
    (void)ctx;
    IDataObject *data = (IDataObject *)pdata;
    IDropSource *src = (IDropSource *)psource;
    for (int i = 0; i < 6000; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        HRESULT q = src->QueryContinueDrag(g_guest_escape ? TRUE : FALSE, MK_LBUTTON);
        if (q == DRAGDROP_S_CANCEL) { *effect = DROPEFFECT_NONE; return DRAGDROP_S_CANCEL; }
        if (q == DRAGDROP_S_DROP) {
            if (g_guest_effect == DROPEFFECT_COPY && SUCCEEDED(shell_copy(data, g_guest_dest))) { *effect = DROPEFFECT_COPY; }
            else *effect = DROPEFFECT_NONE;
            return DRAGDROP_S_DROP;
        }
        src->GiveFeedback((DWORD)g_guest_effect.load());
        Sleep(5);
    }
    *effect = DROPEFFECT_NONE;
    return DRAGDROP_S_CANCEL;
}

/* Host DoDragDrop stand-in for guest -> host: the button is held briefly, then released (the
   drop must wait for staging), or Escape. */
static long host_run_drag(void *ctx, void *pdata, void *psource, unsigned long *effect)
{
    (void)ctx;
    IDataObject *data = (IDataObject *)pdata;
    IDropSource *src = (IDropSource *)psource;
    for (int i = 0; i < 6000; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        BOOL esc = g_host_escape ? TRUE : FALSE;
        HRESULT q = src->QueryContinueDrag(esc, i < 5 ? MK_LBUTTON : 0);
        if (q == DRAGDROP_S_CANCEL) { *effect = DROPEFFECT_NONE; return DRAGDROP_S_CANCEL; }
        if (q == DRAGDROP_S_DROP) {
            *effect = SUCCEEDED(shell_copy(data, g_host_dest)) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return DRAGDROP_S_DROP;
        }
        src->GiveFeedback(DROPEFFECT_COPY);
        Sleep(5);
    }
    *effect = DROPEFFECT_NONE;
    return DRAGDROP_S_CANCEL;
}

/* The catcher receives the guest application's drag (guest -> host probe). */
static void guest_probe(void *ctx, void *pcatcher)
{
    (void)ctx;
    IDropTarget *catcher = (IDropTarget *)pcatcher;
    int mode = g_probe_mode;
    if (!mode) return;
    IDataObject *data = nullptr;
    if (mode == 1) {
        const wchar_t *p[1] = { g_guest_src_folder.c_str() };
        asb_rw_shell_data_object(p, 1, &data);
    } else if (SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data)))) {
        FORMATETC fe = { CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM md = {};
        md.tymed = TYMED_HGLOBAL;
        md.hGlobal = GlobalAlloc(GMEM_MOVEABLE, 32);
        memcpy(GlobalLock(md.hGlobal), L"just text", 20);
        GlobalUnlock(md.hGlobal);
        data->SetData(&fe, &md, TRUE);
    }
    if (!data) return;
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    POINTL pt = { 0, 0 };
    catcher->DragEnter(data, MK_LBUTTON, pt, &eff);
    DWORD over = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    catcher->DragOver(MK_LBUTTON, pt, &over);
    if (g_probe_data) g_probe_data->Release();
    g_probe_data = data;   /* the guest application's drag continues on the catcher */
}

static uint64_t host_surface_at(void *ctx, POINT screen, POINT *origin)
{
    (void)ctx;
    if (screen.x < 100 || screen.y < 100 || screen.x >= 740 || screen.y >= 520) return 0;
    origin->x = 100;
    origin->y = 100;
    return 7;
}

template <typename F>
static bool wait_for(DWORD ms, F f)
{
    ULONGLONG until = GetTickCount64() + ms;
    while (!f()) {
        if (GetTickCount64() > until) return false;
        Sleep(20);
    }
    return true;
}

static AsbRwFilesStats gstats() { AsbRwFilesStats s; asb_rw_files_stats(g_files, &s); return s; }
static AsbRwDragHostStats hstats() { AsbRwDragHostStats s; asb_rw_drag_host_stats(g_host, &s); return s; }

/* ---- host drop target driven on the broker's UI thread ---- */

struct TargetCall {
    int op;               /* 1 enter, 2 over, 3 leave, 4 drop */
    std::vector<std::wstring> paths;
    int text;             /* enter with a text-only data object; 2: delete the files after creating it */
    POINT pt;
    DWORD effect;
};
static IDataObject *g_host_data;

static void target_call(void *arg)
{
    TargetCall *c = (TargetCall *)arg;
    IDropTarget *t = asb_rw_drag_host_target(g_host);
    POINTL pt = { c->pt.x, c->pt.y };
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    switch (c->op) {
    case 1:
        if (g_host_data) { g_host_data->Release(); g_host_data = nullptr; }
        if (c->text == 1) {
            SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&g_host_data));
        } else {
            std::vector<const wchar_t *> p;
            for (const std::wstring &s : c->paths) p.push_back(s.c_str());
            asb_rw_shell_data_object(p.data(), (uint32_t)p.size(), &g_host_data);
            if (c->text == 2) for (const std::wstring &s : c->paths) DeleteFileW(s.c_str());   /* gone since the drag began */
        }
        if (g_host_data) t->DragEnter(g_host_data, MK_LBUTTON, pt, &eff);
        else eff = 0xFFFF;
        break;
    case 2: t->DragOver(MK_LBUTTON, pt, &eff); break;
    case 3: t->DragLeave(); eff = 0; break;
    case 4: t->Drop(g_host_data, 0, pt, &eff); break;
    }
    c->effect = eff;
}

static DWORD host_target(int op, POINT pt, const std::vector<std::wstring> &paths = {}, int text = 0)
{
    TargetCall c;
    c.op = op;
    c.paths = paths;
    c.text = text;
    c.pt = pt;
    c.effect = 0;
    asb_rw_drag_host_invoke(g_host, target_call, &c);
    return c.effect;
}

/* Enter, then hover until the guest target's feedback arrives (or the timeout). */
static DWORD hover(const std::vector<std::wstring> &paths, DWORD want, DWORD ms = 4000)
{
    POINT pt = { 300, 250 };
    DWORD e = host_target(1, pt, paths);
    ULONGLONG until = GetTickCount64() + ms;
    int k = 0;
    while (e != want && GetTickCount64() < until) {
        Sleep(20);
        pt.x = 300 + (k++ % 7);
        e = host_target(2, pt);
    }
    return e;
}

static void start_bridge()
{
    g_files = nullptr;
    AsbRwFilesConfig fc = {};
    fc.ctx = (void *)"guest";
    fc.log = tlog;
    fc.drag_state = guest_drag_state;
    fc.accept = guest_accept;
    fc.inject_move = guest_move;
    fc.inject_button = guest_button;
    std::wstring gstage = g_root + L"\\guest-stage";
    fc.stage_parent = gstage.c_str();
    fc.probe_ms = 600;
    fc.run_drag = guest_run_drag;
    fc.probe = guest_probe;
    g_files = asb_rw_files_start(&fc);
    CHECK(g_files != nullptr);
    asb_rw_files_session(g_files, 0x1234);
    AsbRwFilesSurface sf = { 7, { 3000, 100, 3640, 520 } };
    RECT area = { 2560, 0, 5120, 1440 };
    asb_rw_files_surfaces(g_files, &sf, 1, &area);
    AsbRwDragHostConfig hc = {};
    hc.connect = host_connect;
    hc.ctx = (void *)"host";
    hc.log = tlog;
    hc.cookie = 0x1111;
    std::wstring hstage = g_root + L"\\host-stage";
    hc.stage_parent = hstage.c_str();
    hc.guest_name = L"Test guest";
    hc.no_overlay = 1;
    hc.no_progress = 1;
    hc.surface_at = host_surface_at;
    hc.run_drag = host_run_drag;
    g_host = asb_rw_drag_host_start(&hc);
    CHECK(g_host != nullptr);
    CHECK(wait_for(8000, [] { return gstats().connected && hstats().connected; }));
}

static int dir_count(const std::wstring &dir)
{
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do { n += wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."); } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* Staging folders still holding data (incomplete staging must not survive a failure). */
static int live_stages(const std::wstring &parent)
{
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((parent + L"\\asc*.tmp").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring d = parent + L"\\" + fd.cFileName + L"\\d";
        n += exists(d) && dir_count(d) > 0;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

static void test_bridge()
{
    std::wstring hsrc = g_root + L"\\host-src", gsrc = g_root + L"\\guest-src";
    g_guest_dest = g_root + L"\\guest-dest";
    g_host_dest = g_root + L"\\host-dest";
    mkdirs(hsrc); mkdirs(gsrc); mkdirs(g_guest_dest); mkdirs(g_host_dest);
    std::vector<std::wstring> host_items = build_tree(hsrc, 64 * 1024 * 1024 + 11);   /* large transfer */
    std::vector<std::wstring> guest_items = build_tree(gsrc, 24 * 1024 * 1024 + 5);
    g_guest_src_folder = guest_items[0];
    uint64_t hsrc_hash = tree_hash(hsrc), gsrc_hash = tree_hash(gsrc);
    InitializeCriticalSection(&g_state_lock);
    start_bridge();

    /* ---- host -> guest: genuine drop, copy only ---- */
    POINT pt = { 300, 250 };
    DWORD e = hover(host_items, DROPEFFECT_COPY);
    CHECK_EQ(e, DROPEFFECT_COPY);
    CHECK(count_states(ASB_RW_DRAG_ACTIVE) == 1 && g_pressed == 1);
    CHECK(g_last_x >= 3200 && g_last_x < 3210 && g_last_y == 250);   /* surface origin 3000,100 + (x-100, y-100) */
    pt.x = 400; pt.y = 300;
    host_target(2, pt);
    Sleep(60);
    e = host_target(4, pt);
    CHECK_EQ(e, DROPEFFECT_COPY);   /* never MOVE: the host source keeps its files */
    CHECK(wait_for(60000, [] { return gstats().to_guest_ok == 1 && hstats().to_guest_ok == 1; }));
    CHECK(g_last_x == 3300 && g_last_y == 300);   /* released at the drop point */
    CHECK_EQ(g_pressed.load(), 0);
    CHECK(wait_for(3000, [] { return count_states(ASB_RW_DRAG_ENDED) == 1; }));
    std::wstring folder_name = host_items[0].substr(host_items[0].find_last_of(L'\\') + 1);
    std::wstring file_name = host_items[1].substr(host_items[1].find_last_of(L'\\') + 1);
    CHECK(tree_hash(g_guest_dest + L"\\" + folder_name, L"x") == tree_hash(host_items[0], L"x"));
    CHECK(tree_hash(g_guest_dest + L"\\" + file_name, L"x") == tree_hash(host_items[1], L"x"));
    CHECK_EQ(gstats().last_effect, DROPEFFECT_COPY);

    /* Again into the same folder: the destination's own conflict handling decides (renamed
       copies here); the existing files stay untouched. */
    uint64_t before_conflict = tree_hash(g_guest_dest + L"\\" + file_name, L"x");
    e = hover(host_items, DROPEFFECT_COPY);
    CHECK_EQ(host_target(4, pt), DROPEFFECT_COPY);
    CHECK(wait_for(60000, [] { return gstats().to_guest_ok == 2; }));
    CHECK_EQ(dir_count(g_guest_dest), 4);
    CHECK(tree_hash(g_guest_dest + L"\\" + file_name, L"x") == before_conflict);

    /* The guest drop target refuses: NONE is reported and dropping transfers nothing. */
    int dest_before = dir_count(g_guest_dest);
    g_guest_effect = DROPEFFECT_NONE;
    uint64_t refused_before = gstats().cancelled;
    e = hover(host_items, DROPEFFECT_NONE, 600);
    CHECK(wait_for(5000, [] { return gstats().phase == ASB_RW_FILES_HOVER; }));   /* the guest drag runs */
    Sleep(200);
    CHECK_EQ(host_target(2, pt), DROPEFFECT_NONE);   /* the guest target's refusal is reported */
    CHECK_EQ(host_target(4, pt), DROPEFFECT_NONE);
    CHECK(wait_for(5000, [&] { return gstats().cancelled == refused_before + 1 && gstats().phase == ASB_RW_FILES_IDLE && g_pressed == 0; }));
    CHECK_EQ(dir_count(g_guest_dest), dest_before);
    g_guest_effect = DROPEFFECT_COPY;

    /* Leave (the host drag moved away or Escape in the host source): cancelled, released. */
    uint64_t cancelled = gstats().cancelled;
    e = hover(host_items, DROPEFFECT_COPY);
    CHECK_EQ(e, DROPEFFECT_COPY);
    host_target(3, pt);
    CHECK(wait_for(5000, [&] { return gstats().cancelled == cancelled + 1 && gstats().phase == ASB_RW_FILES_IDLE; }));
    CHECK_EQ(g_pressed.load(), 0);
    CHECK_EQ(dir_count(g_guest_dest), dest_before);

    /* Escape inside the guest drag loop: cancelled, nothing delivered, the host drop is refused. */
    g_guest_escape = true;
    e = hover(host_items, DROPEFFECT_COPY, 800);
    CHECK_EQ(e, DROPEFFECT_NONE);
    CHECK(wait_for(5000, [] { return gstats().phase == ASB_RW_FILES_IDLE; }));
    CHECK_EQ(host_target(4, pt), DROPEFFECT_NONE);
    CHECK(wait_for(20000, [] { return gstats().phase == ASB_RW_FILES_IDLE && hstats().phase == ASB_RW_DH_IDLE; }));
    g_guest_escape = false;
    CHECK_EQ(g_pressed.load(), 0);
    CHECK_EQ(dir_count(g_guest_dest), dest_before);

    /* Unsupported data (no CF_HDROP), a link inside the folder, a missing source. */
    CHECK_EQ(host_target(1, pt, {}, 1), DROPEFFECT_NONE);
    host_target(3, pt);
    std::wstring linked = g_root + L"\\host-src\\linked";
    mkdirs(linked);
    write_file(linked + L"\\ok.txt", 5, 1);
    CHECK(make_junction(linked + L"\\escape", g_root + L"\\host-src"));
    uint64_t rejected = hstats().rejected;
    e = hover({ linked }, DROPEFFECT_COPY, 1500);
    CHECK_EQ(e, DROPEFFECT_NONE);   /* the walk found the junction */
    CHECK_EQ(host_target(4, pt), DROPEFFECT_NONE);
    CHECK(hstats().rejected == rejected + 1);
    CHECK(wait_for(5000, [] { return gstats().phase == ASB_RW_FILES_IDLE && g_pressed == 0; }));
    CHECK_EQ(dir_count(g_guest_dest), dest_before);
    write_file(g_root + L"\\host-src\\gone.txt", 10, 1);
    CHECK_EQ(host_target(1, pt, { g_root + L"\\host-src\\gone.txt" }, 2), DROPEFFECT_NONE);
    host_target(3, pt);
    RemoveDirectoryW((linked + L"\\escape").c_str());
    remove_tree(linked);

    /* ---- guest -> host ---- */
    g_probe_mode = 1;
    AsbRwDrag probe = {};
    probe.surface_id = 7; probe.probe = 1; probe.button = INPUT_BTN_LEFT;
    int ups = g_ups;
    asb_rw_files_control(g_files, ASB_RW_DRAG_PROBE, &probe);
    CHECK(wait_for(60000, [] { return hstats().from_guest_ok == 1 && gstats().from_guest_ok == 1; }));
    CHECK(wait_for(5000, [&] { return gstats().phase == ASB_RW_FILES_IDLE && g_ups == ups + 1; }));   /* released on the catcher */
    std::wstring gfolder = guest_items[0].substr(guest_items[0].find_last_of(L'\\') + 1);
    CHECK(tree_hash(g_host_dest + L"\\" + gfolder, L"x") == tree_hash(guest_items[0], L"x"));
    CHECK_EQ(hstats().last_effect, DROPEFFECT_COPY);
    EnterCriticalSection(&g_state_lock);
    bool active_from = false;
    for (const AsbRwDrag &d : g_states) active_from |= d.state == ASB_RW_DRAG_ACTIVE && d.direction == ASB_RW_DIR_FROM_GUEST && d.probe == 1;
    LeaveCriticalSection(&g_state_lock);
    CHECK(active_from);

    /* Escape in the host drag: the guest is told, releases, and nothing lands on the host. */
    int host_before = dir_count(g_host_dest);
    g_host_escape = true;
    probe.probe = 2;
    ups = g_ups;
    asb_rw_files_control(g_files, ASB_RW_DRAG_PROBE, &probe);
    CHECK(wait_for(20000, [&] { return gstats().phase == ASB_RW_FILES_IDLE && g_ups == ups + 1 && hstats().phase == ASB_RW_DH_IDLE; }));
    g_host_escape = false;
    CHECK_EQ(dir_count(g_host_dest), host_before);

    /* Unsupported guest drag (text only): parked on the catcher until the host button goes up. */
    g_probe_mode = 2;
    probe.probe = 3;
    int sinks = count_states(ASB_RW_DRAG_SINK);
    asb_rw_files_control(g_files, ASB_RW_DRAG_PROBE, &probe);
    CHECK(wait_for(5000, [&] { return count_states(ASB_RW_DRAG_SINK) == sinks + 1; }));
    CHECK(wait_for(2000, [] { return gstats().phase == ASB_RW_FILES_SINK; }));
    ups = g_ups;
    AsbRwDrag release = {};
    release.probe = 3; release.state = ASB_RW_DRAG_RELEASE;
    asb_rw_files_control(g_files, ASB_RW_DRAG_ACK, &release);
    CHECK(wait_for(5000, [&] { return gstats().phase == ASB_RW_FILES_IDLE && g_ups == ups + 1; }));

    /* No drag at all: NONE after the catcher wait; the pointer is left alone. */
    g_probe_mode = 0;
    probe.probe = 4;
    int nones = count_states(ASB_RW_DRAG_NONE);
    ups = g_ups;
    asb_rw_files_control(g_files, ASB_RW_DRAG_PROBE, &probe);
    CHECK(wait_for(5000, [&] { return count_states(ASB_RW_DRAG_NONE) == nones + 1; }));
    CHECK_EQ(g_ups.load(), ups);

    /* ---- disconnect mid-transfer: the guest cleans up and releases the pointer ---- */
    uint64_t delivered = gstats().to_guest_ok;
    int delivered_dirs = dir_count(g_guest_dest);
    e = hover(host_items, DROPEFFECT_COPY);
    CHECK_EQ(host_target(4, pt), DROPEFFECT_COPY);
    asb_rw_drag_host_stop(g_host);   /* the broker goes away while 70 MB are in flight */
    g_host = nullptr;
    CHECK(wait_for(10000, [] { return gstats().phase == ASB_RW_FILES_IDLE && !gstats().connected; }));
    CHECK_EQ(gstats().to_guest_ok, delivered);
    CHECK_EQ(gstats().last_error, ASB_RW_ERR_DISCONNECTED);
    CHECK_EQ(g_pressed.load(), 0);
    CHECK_EQ(dir_count(g_guest_dest), delivered_dirs);
    CHECK(wait_for(10000, [&] { return live_stages(g_root + L"\\guest-stage") == 2; }));   /* only the 2 delivered drops remain */

    asb_rw_files_stop(g_files);
    g_files = nullptr;
    if (g_probe_data) { g_probe_data->Release(); g_probe_data = nullptr; }
    if (g_host_data) { g_host_data->Release(); g_host_data = nullptr; }
    CHECK(tree_hash(hsrc) != 0);
    CHECK(tree_hash(gsrc) == gsrc_hash);   /* guest sources intact (copy only) */
    (void)hsrc_hash;
    /* host-src was modified by the junction test above; its original items must be intact. */
    CHECK(exists(host_items[0]) && exists(host_items[1]));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--verbose")) g_verbose = true;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[64];
    swprintf_s(name, L"asb-eh69-%08lx", GetTickCount());
    g_root = std::wstring(tmp) + name;
    mkdirs(g_root);
    DWORD clip_before = GetClipboardSequenceNumber();

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(g_listen, (sockaddr *)&a, sizeof(a));
    int len = sizeof(a);
    getsockname(g_listen, (sockaddr *)&a, &len);
    g_port = ntohs(a.sin_port);
    listen(g_listen, 4);

    test_sources_and_staging();
    printf("sources/staging  %s\n", g_failures ? "FAILED" : "ok");
    fflush(stdout);
    int before = g_failures;
    test_bridge();
    printf("bridge           %s\n", g_failures == before ? "ok" : "FAILED");

    CHECK_EQ(GetClipboardSequenceNumber(), clip_before);   /* the clipboard is never touched */
    closesocket(g_listen);
    RemoveDirectoryW((g_root + L"\\src\\with link\\escape").c_str());
    remove_tree(g_root);
    WSACleanup();
    OleUninitialize();
    if (g_failures) {
        printf("FAIL remote windows files native: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS remote windows files native: sources, staging, cleanup, host -> guest and guest -> host OLE bridges, "
           "rejection, conflicts, cancel/Escape, disconnect, clipboard untouched (%d checks)\n", g_checks);
    return 0;
}
