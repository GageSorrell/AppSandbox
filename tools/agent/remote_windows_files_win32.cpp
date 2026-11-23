/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_files_win32
 *
 * @file      remote_windows_files_win32.cpp
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows drag-and-drop file layer (AppSandbox-eh6.9). See remote_windows_files_win32.h. */
#include "remote_windows_files_win32.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include "../transport/asb_clipboard_temp.h"
}

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif

namespace {

const DWORD kVirtualAttrs = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN;
const size_t kListBuffer = 64 * 1024;

uint32_t source_fail(AsbRwSource *s, uint32_t error, const wchar_t *what, const wchar_t *path)
{
    if (!s->error) {
        s->error = error;
        swprintf_s(s->detail, ARRAYSIZE(s->detail), L"%s: %s", what, path ? path : L"");
    }
    return error;
}

/* Absolute \\?\ form of a dragged path; only drive and UNC filesystem paths are accepted. */
bool long_path(const wchar_t *in, std::wstring &out)
{
    if (!in || !*in) return false;
    std::wstring p(in);
    for (wchar_t &c : p) if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) { out = p; }
    else if (p.rfind(L"\\\\?\\", 0) == 0) {
        if (p.size() < 7 || !iswalpha(p[4]) || p[5] != L':' || p[6] != L'\\') return false;   /* no GLOBALROOT / Volume{} */
        out = p;
    } else if (p.rfind(L"\\\\.\\", 0) == 0 || p.rfind(L"\\??\\", 0) == 0) {
        return false;                                                                      /* devices */
    } else {
        DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
        if (!n) return false;
        std::wstring full(n, L'\0');
        n = GetFullPathNameW(p.c_str(), n, &full[0], nullptr);
        if (!n) return false;
        full.resize(n);
        if (full.size() >= 3 && iswalpha(full[0]) && full[1] == L':' && full[2] == L'\\') out = L"\\\\?\\" + full;
        else if (full.rfind(L"\\\\", 0) == 0 && full.size() > 2 && full[2] != L'?' && full[2] != L'.') out = L"\\\\?\\UNC\\" + full.substr(2);
        else return false;
    }
    while (out.size() > 7 && out.back() == L'\\') out.pop_back();
    return true;
}

struct ItemInfo {
    uint32_t kind = 0;
    uint64_t size = 0, file_id = 0;
    uint32_t volume = 0;
};

/* Opens path without following a final reparse point and validates it. */
uint32_t inspect(const wchar_t *path, ItemInfo *info, const wchar_t **why)
{
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        *why = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? L"missing" : L"unreadable";
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ASB_RW_ERR_FILE_REJECTED : ASB_RW_ERR_FILE_IO;
    }
    uint32_t result = ASB_RW_OK;
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    BY_HANDLE_FILE_INFORMATION bi = {};
    if (GetFileType(h) != FILE_TYPE_DISK) { *why = L"not a disk file"; result = ASB_RW_ERR_FILE_REJECTED; }
    else if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) || !GetFileInformationByHandle(h, &bi)) {
        *why = L"unreadable"; result = ASB_RW_ERR_FILE_IO;
    } else if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag)) {
        *why = L"link or junction rejected"; result = ASB_RW_ERR_FILE_REJECTED;
    } else if (tag.FileAttributes & kVirtualAttrs) {
        *why = L"virtual or offline item rejected"; result = ASB_RW_ERR_FILE_REJECTED;
    } else {
        info->kind = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? ASB_RW_ENTRY_DIR : ASB_RW_ENTRY_FILE;
        info->size = info->kind == ASB_RW_ENTRY_FILE ? ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow : 0;
        info->file_id = ((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
        info->volume = bi.dwVolumeSerialNumber;
    }
    CloseHandle(h);
    return result;
}

bool push_item(AsbRwSource *s, const std::wstring &path, const std::wstring &rel, const ItemInfo &info)
{
    if (s->count >= ASB_RW_MAX_FILE_ENTRIES) return false;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 64;
        if (cap > ASB_RW_MAX_FILE_ENTRIES) cap = ASB_RW_MAX_FILE_ENTRIES;
        AsbRwSourceItem *items = (AsbRwSourceItem *)realloc(s->items, cap * sizeof(*items));
        if (!items) return false;
        s->items = items;
        s->capacity = cap;
    }
    AsbRwSourceItem *it = &s->items[s->count];
    memset(it, 0, sizeof(*it));
    it->path = _wcsdup(path.c_str());
    it->rel = (uint16_t *)malloc((rel.size() + 1) * sizeof(uint16_t));
    if (!it->path || !it->rel) { free(it->path); free(it->rel); return false; }
    for (size_t i = 0; i < rel.size(); ++i) it->rel[i] = (uint16_t)rel[i];
    it->rel[rel.size()] = 0;
    it->rel_len = (uint16_t)rel.size();
    it->kind = info.kind;
    it->size = info.size;
    it->file_id = info.file_id;
    it->volume = info.volume;
    s->count++;
    return true;
}

bool rel_ok(const std::wstring &rel)
{
    if (rel.size() > ASB_RW_MAX_REL_PATH) return false;
    std::vector<uint16_t> u(rel.begin(), rel.end());
    return asb_rw_rel_path_safe(u.data(), (uint32_t)u.size()) != 0;
}

/* Lists one folder through a handle that was opened without following reparse points; every
   child is judged by the attributes and reparse tag the file system reports for it. */
uint32_t walk(AsbRwSource *s, const std::wstring &dir_path, const std::wstring &dir_rel, uint32_t volume,
              volatile LONG *cancel, int depth)
{
    if (depth > 256) return source_fail(s, ASB_RW_ERR_TOO_LARGE, L"folders nested too deeply", dir_path.c_str());
    HANDLE dir = CreateFileW(dir_path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return source_fail(s, ASB_RW_ERR_FILE_IO, L"folder unreadable", dir_path.c_str());
    std::vector<uint8_t> buf(kListBuffer);
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    std::vector<std::wstring> subdirs;
    uint32_t result = ASB_RW_OK;
    while (!result && GetFileInformationByHandleEx(dir, cls, buf.data(), (DWORD)buf.size())) {
        cls = FileIdBothDirectoryInfo;
        FILE_ID_BOTH_DIR_INFO *e = (FILE_ID_BOTH_DIR_INFO *)buf.data();
        for (;;) {
            std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                std::wstring path = dir_path + L"\\" + name, rel = dir_rel + L"\\" + name;
                DWORD a = e->FileAttributes;
                if (cancel && *cancel) { result = source_fail(s, ASB_RW_ERR_CANCELLED, L"cancelled", path.c_str()); break; }
                /* For reparse points EaSize holds the reparse tag. */
                if ((a & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(e->EaSize)) {
                    result = source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"link or junction rejected", path.c_str());
                    break;
                }
                if (a & kVirtualAttrs) { result = source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"virtual or offline item rejected", path.c_str()); break; }
                if (!rel_ok(rel)) { result = source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"unsupported name or path too long", path.c_str()); break; }
                ItemInfo info;
                info.kind = (a & FILE_ATTRIBUTE_DIRECTORY) ? ASB_RW_ENTRY_DIR : ASB_RW_ENTRY_FILE;
                info.size = info.kind == ASB_RW_ENTRY_FILE ? (uint64_t)e->EndOfFile.QuadPart : 0;
                info.file_id = (uint64_t)e->FileId.QuadPart;
                info.volume = volume;
                if (info.size > UINT64_MAX - s->total_bytes) { result = source_fail(s, ASB_RW_ERR_TOO_LARGE, L"too large", path.c_str()); break; }
                if (!push_item(s, path, rel, info)) { result = source_fail(s, ASB_RW_ERR_TOO_LARGE, L"too many items", path.c_str()); break; }
                s->total_bytes += info.size;
                if (info.kind == ASB_RW_ENTRY_DIR) {
                    /* Children follow their folder: walk it now (pre-order). */
                    result = walk(s, path, rel, volume, cancel, depth + 1);
                    if (result) break;
                }
            }
            if (!e->NextEntryOffset) break;
            e = (FILE_ID_BOTH_DIR_INFO *)((uint8_t *)e + e->NextEntryOffset);
        }
    }
    if (!result && GetLastError() != ERROR_NO_MORE_FILES && GetLastError() != ERROR_SUCCESS)
        result = source_fail(s, ASB_RW_ERR_FILE_IO, L"folder unreadable", dir_path.c_str());
    CloseHandle(dir);
    return result;
}

int send_message(const AsbRwFileSendOps *ops, uint16_t type, const AsbRwPayload *msg, const uint8_t *bulk, size_t bulk_len)
{
    uint8_t buf[ASB_RW_HEADER_BYTES + 2048];
    size_t n = 0;
    if (asb_rw_encode(type, 0, msg, buf, sizeof(buf), &n)) return 0;
    return ops->send(ops->ctx, buf, n, bulk, bulk_len);
}

void entry_payload(const AsbRwSourceItem *it, uint64_t session, uint32_t index, AsbRwPayload *m)
{
    memset(&m->file_entry, 0, sizeof(m->file_entry));
    m->file_entry.session_id = session;
    m->file_entry.size = it->size;
    m->file_entry.index = index;
    m->file_entry.kind = it->kind;
    m->file_entry.rel_len = it->rel_len;
    memcpy(m->file_entry.rel, it->rel, it->rel_len * sizeof(uint16_t));
}

uint32_t send_file(AsbRwSource *s, uint32_t index, uint64_t session, std::vector<uint8_t> &buf, uint64_t *done,
                   const AsbRwFileSendOps *ops)
{
    const AsbRwSourceItem *it = &s->items[index];
    if (!it->size) return ASB_RW_OK;
    HANDLE h = CreateFileW(it->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return source_fail(s, ASB_RW_ERR_FILE_IO, L"cannot open", it->path);
    BY_HANDLE_FILE_INFORMATION bi = {};
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    bool same = GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &bi) &&
                GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                !(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag)) &&
                bi.dwVolumeSerialNumber == it->volume && (((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow) == it->file_id;
    if (!same) { CloseHandle(h); return source_fail(s, ASB_RW_ERR_FILE_IO, L"changed during the copy", it->path); }
    uint64_t offset = 0;
    uint32_t result = ASB_RW_OK;
    AsbRwPayload m;
    while (offset < it->size) {
        if (ops->cancelled && ops->cancelled(ops->ctx)) { result = source_fail(s, ASB_RW_ERR_CANCELLED, L"cancelled", it->path); break; }
        DWORD want = (DWORD)((it->size - offset) < buf.size() ? (it->size - offset) : buf.size()), got = 0;
        if (!ReadFile(h, buf.data(), want, &got, nullptr) || got != want) {
            result = source_fail(s, ASB_RW_ERR_FILE_IO, L"changed or unreadable during the copy", it->path);
            break;
        }
        memset(&m.file_data, 0, sizeof(m.file_data));
        m.file_data.session_id = session;
        m.file_data.offset = offset;
        m.file_data.index = index;
        m.file_data.len = got;
        if (!send_message(ops, ASB_RW_FILE_DATA, &m, buf.data(), got)) { result = source_fail(s, ASB_RW_ERR_DISCONNECTED, L"send failed", it->path); break; }
        offset += got;
        *done += got;
        if (ops->progress) ops->progress(ops->ctx, *done, s->total_bytes);
    }
    CloseHandle(h);
    return result;
}

std::wstring plain_path(const std::wstring &p)
{
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
    return p;
}

} // namespace

/* ================================ sources ================================= */

extern "C" void asb_rw_source_init(AsbRwSource *s) { memset(s, 0, sizeof(*s)); }

extern "C" void asb_rw_source_free(AsbRwSource *s)
{
    for (uint32_t i = 0; i < s->count; ++i) { free(s->items[i].path); free(s->items[i].rel); }
    free(s->items);
    memset(s, 0, sizeof(*s));
}

extern "C" uint32_t asb_rw_source_top(AsbRwSource *s, const wchar_t *const *paths, uint32_t n)
{
    if (!n) return source_fail(s, ASB_RW_ERR_UNSUPPORTED_DATA, L"no files", L"");
    if (n > ASB_RW_MAX_TOP_ENTRIES) return source_fail(s, ASB_RW_ERR_TOO_LARGE, L"too many items", L"");
    std::wstring parent0;
    for (uint32_t i = 0; i < n; ++i) {
        std::wstring full;
        if (!long_path(paths[i], full)) return source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"not a filesystem path", paths[i]);
        size_t slash = full.find_last_of(L'\\');
        std::wstring name = slash == std::wstring::npos ? full : full.substr(slash + 1);
        if (name.empty() || (name.size() == 2 && name[1] == L':'))
            return source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"drive roots cannot be copied", paths[i]);
        if (!rel_ok(name)) return source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"unsupported name", paths[i]);
        ItemInfo info;
        const wchar_t *why = L"";
        uint32_t r = inspect(full.c_str(), &info, &why);
        if (r) return source_fail(s, r, why, paths[i]);
        for (uint32_t k = 0; k < s->count; ++k) {
            std::wstring other((const wchar_t *)s->items[k].rel, s->items[k].rel_len);
            if (!_wcsicmp(other.c_str(), name.c_str())) return source_fail(s, ASB_RW_ERR_FILE_REJECTED, L"duplicate name", paths[i]);
        }
        if (!push_item(s, full, name, info)) return source_fail(s, ASB_RW_ERR_LIMIT, L"out of memory", paths[i]);
        s->top_bytes += info.size;
        s->total_bytes += info.size;
    }
    s->top_count = s->count;
    return ASB_RW_OK;
}

extern "C" uint32_t asb_rw_source_expand(AsbRwSource *s, volatile LONG *cancel)
{
    if (s->error) return s->error;
    if (s->expanded) return ASB_RW_OK;
    for (uint32_t i = 0; i < s->top_count; ++i) {
        AsbRwSourceItem *top = &s->items[i];
        top->begin = top->end = s->count;
        if (top->kind != ASB_RW_ENTRY_DIR) continue;
        std::wstring path(top->path), rel((const wchar_t *)top->rel, top->rel_len);
        uint32_t volume = top->volume;
        uint32_t r = walk(s, path, rel, volume, cancel, 0);
        top = &s->items[i];   /* walk may reallocate */
        top->end = s->count;
        if (r) return r;
    }
    s->expanded = 1;
    return ASB_RW_OK;
}

extern "C" uint32_t asb_rw_source_send_top(const AsbRwSource *s, uint64_t session, uint32_t direction, uint64_t target,
                                           int32_t x, int32_t y, const AsbRwFileSendOps *ops)
{
    AsbRwPayload m;
    memset(&m.file_begin, 0, sizeof(m.file_begin));
    m.file_begin.session_id = session;
    m.file_begin.target_id = target;
    m.file_begin.direction = direction;
    m.file_begin.entry_count = s->top_count;
    m.file_begin.x = x;
    m.file_begin.y = y;
    m.file_begin.total_bytes = s->top_bytes;
    if (!send_message(ops, ASB_RW_FILE_BEGIN, &m, nullptr, 0)) return ASB_RW_ERR_DISCONNECTED;
    for (uint32_t i = 0; i < s->top_count; ++i) {
        entry_payload(&s->items[i], session, i, &m);
        if (!send_message(ops, ASB_RW_FILE_ENTRY, &m, nullptr, 0)) return ASB_RW_ERR_DISCONNECTED;
    }
    return ASB_RW_OK;
}

extern "C" uint32_t asb_rw_source_send_stream(AsbRwSource *s, uint64_t session, uint32_t chunk, const AsbRwFileSendOps *ops)
{
    if (!s->expanded) return ASB_RW_ERR_INTERNAL;
    if (!chunk || chunk > ASB_RW_MAX_FILE_CHUNK) chunk = ASB_RW_FILE_CHUNK_DEFAULT;
    std::vector<uint8_t> buf(chunk);
    AsbRwPayload m;
    memset(&m.file_stream, 0, sizeof(m.file_stream));
    m.file_stream.session_id = session;
    m.file_stream.entry_count = s->count;
    m.file_stream.total_bytes = s->total_bytes;
    if (!send_message(ops, ASB_RW_FILE_STREAM, &m, nullptr, 0)) return ASB_RW_ERR_DISCONNECTED;
    uint64_t done = 0;
    for (uint32_t i = 0; i < s->top_count; ++i) {
        uint32_t r;
        if (s->items[i].kind == ASB_RW_ENTRY_FILE) {
            if ((r = send_file(s, i, session, buf, &done, ops)) != 0) return r;
            continue;
        }
        for (uint32_t j = s->items[i].begin; j < s->items[i].end; ++j) {
            if (ops->cancelled && ops->cancelled(ops->ctx)) return source_fail(s, ASB_RW_ERR_CANCELLED, L"cancelled", L"");
            entry_payload(&s->items[j], session, j, &m);
            if (!send_message(ops, ASB_RW_FILE_ENTRY, &m, nullptr, 0)) return ASB_RW_ERR_DISCONNECTED;
            if (s->items[j].kind == ASB_RW_ENTRY_FILE && (r = send_file(s, j, session, buf, &done, ops)) != 0) return r;
        }
    }
    return asb_rw_files_send_status(ops, ASB_RW_FILE_END, session, ASB_RW_OK, done, s->total_bytes) ? ASB_RW_OK
                                                                                                     : ASB_RW_ERR_DISCONNECTED;
}

extern "C" int asb_rw_files_send_status(const AsbRwFileSendOps *ops, uint16_t type, uint64_t session, uint32_t status,
                                        uint64_t done, uint64_t total)
{
    AsbRwPayload m;
    memset(&m.file_status, 0, sizeof(m.file_status));
    m.file_status.session_id = session;
    m.file_status.status = status;
    m.file_status.done_bytes = done > total ? total : done;
    m.file_status.total_bytes = total;
    return send_message(ops, type, &m, nullptr, 0);
}

/* ================================ staging ================================= */

struct AsbRwStage {
    AsbClipboardTemp *temp = nullptr;
    std::wstring root;                  /* \\?\ form of the staging data folder */
    std::wstring plain_root;
    AsbRwFileRx rx;
    HANDLE file = INVALID_HANDLE_VALUE;
    int64_t file_index = -1;
    std::vector<std::wstring> top;      /* plain paths of the top-level items */
    std::vector<const wchar_t *> top_ptrs;
};

namespace {

std::wstring stage_path(const AsbRwStage *st, const uint16_t *rel, uint16_t len)
{
    std::wstring p = st->root + L"\\";
    for (uint16_t i = 0; i < len; ++i) p += rel[i] == '/' ? L'\\' : (wchar_t)rel[i];
    return p;
}

void close_file(AsbRwStage *st)
{
    if (st->file != INVALID_HANDLE_VALUE) CloseHandle(st->file);
    st->file = INVALID_HANDLE_VALUE;
    st->file_index = -1;
}

/* A file handle we created inside staging must be an ordinary file (never a reparse point). */
bool plain_file(HANDLE h)
{
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    return GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
           !(tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}

uint32_t stage_entry(AsbRwStage *st, const AsbRwFileEntry *e)
{
    uint32_t phase_before = st->rx.phase;
    uint32_t r = asb_rw_file_rx_entry(&st->rx, e);
    if (r) return r;
    std::wstring path = stage_path(st, e->rel, e->rel_len);
    if (phase_before == ASB_RW_RX_TOP) {
        std::wstring plain = st->plain_root + L"\\";
        for (uint16_t i = 0; i < e->rel_len; ++i) plain += (wchar_t)e->rel[i];
        st->top.push_back(plain);
    }
    if (e->kind == ASB_RW_ENTRY_DIR) {
        if (!CreateDirectoryW(path.c_str(), nullptr)) return ASB_RW_ERR_FILE_IO;
        return ASB_RW_OK;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ASB_RW_ERR_FILE_IO;
    if (!plain_file(h)) { CloseHandle(h); return ASB_RW_ERR_FILE_REJECTED; }
    if (phase_before == ASB_RW_RX_STREAM && e->size) {
        /* Its data follows immediately. */
        if (e->size >= 4096) {
            LARGE_INTEGER size;
            size.QuadPart = (LONGLONG)e->size;
            FILE_END_OF_FILE_INFO eof = { size };
            SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof));   /* reserve; best effort */
            LARGE_INTEGER zero = {};
            SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
        }
        close_file(st);   /* the model already refuses this; never leak a handle regardless */
        st->file = h;
        st->file_index = e->index;
    } else {
        CloseHandle(h);   /* empty file, or a top-level placeholder filled during the stream */
    }
    return ASB_RW_OK;
}

uint32_t stage_data(AsbRwStage *st, const AsbRwFileData *d)
{
    uint32_t r = asb_rw_file_rx_data(&st->rx, d);
    if (r) return r;
    if (st->file_index != (int64_t)d->index) {
        close_file(st);
        if (d->index >= st->top.size()) return ASB_RW_ERR_INTERNAL;   /* nested files are opened by their entry */
        std::wstring path = st->root + st->top[d->index].substr(st->plain_root.size());
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return ASB_RW_ERR_FILE_IO;
        if (!plain_file(h)) { CloseHandle(h); return ASB_RW_ERR_FILE_REJECTED; }
        st->file = h;
        st->file_index = d->index;
    }
    DWORD written = 0;
    if (!WriteFile(st->file, d->data, d->len, &written, nullptr) || written != d->len) return ASB_RW_ERR_FILE_IO;
    const AsbRwRxEntry *e = asb_rw_file_rx_at(&st->rx, d->index);
    if (e && e->done == e->size) {
        BOOL ok = SetEndOfFile(st->file);   /* drops the reservation beyond the real size, if any */
        close_file(st);
        if (!ok) return ASB_RW_ERR_FILE_IO;
    }
    return ASB_RW_OK;
}

} // namespace

extern "C" AsbRwStage *asb_rw_stage_open(const wchar_t *parent)
{
    if (!parent || !*parent) return nullptr;
    SHCreateDirectoryExW(nullptr, parent, nullptr);
    AsbRwStage *st = new (std::nothrow) AsbRwStage();
    if (!st) return nullptr;
    asb_rw_file_rx_init(&st->rx);
    st->temp = asb_clipboard_temp_create(parent);
    const wchar_t *p = st->temp ? asb_clipboard_temp_path(st->temp) : nullptr;
    if (!p) {
        if (st->temp) asb_clipboard_temp_discard(st->temp);
        delete st;
        return nullptr;
    }
    st->plain_root = p;
    if (!long_path(p, st->root)) {
        asb_clipboard_temp_discard(st->temp);
        delete st;
        return nullptr;
    }
    return st;
}

extern "C" uint32_t asb_rw_stage_message(AsbRwStage *st, uint16_t type, const AsbRwPayload *msg)
{
    uint32_t r;
    switch (type) {
    case ASB_RW_FILE_BEGIN: return asb_rw_file_rx_begin(&st->rx, &msg->file_begin);
    case ASB_RW_FILE_ENTRY: r = stage_entry(st, &msg->file_entry); break;
    case ASB_RW_FILE_STREAM: return asb_rw_file_rx_stream(&st->rx, &msg->file_stream);
    case ASB_RW_FILE_DATA: r = stage_data(st, &msg->file_data); break;
    case ASB_RW_FILE_END:
        r = asb_rw_file_rx_end(&st->rx, &msg->file_status);
        close_file(st);
        break;
    default: r = ASB_RW_ERR_UNKNOWN_TYPE; break;
    }
    if (r && st->rx.phase != ASB_RW_RX_FAILED) { st->rx.phase = ASB_RW_RX_FAILED; st->rx.error = r; }
    return r;
}

extern "C" uint32_t asb_rw_stage_phase(const AsbRwStage *st) { return st ? st->rx.phase : ASB_RW_RX_IDLE; }
extern "C" uint64_t asb_rw_stage_session(const AsbRwStage *st) { return st ? st->rx.session_id : 0; }

extern "C" void asb_rw_stage_progress(const AsbRwStage *st, uint64_t *done, uint64_t *total)
{
    *done = st ? st->rx.done_bytes : 0;
    *total = st ? st->rx.total_bytes : 0;
}

extern "C" uint32_t asb_rw_stage_top(const AsbRwStage *st, const wchar_t *const **paths)
{
    AsbRwStage *m = const_cast<AsbRwStage *>(st);
    m->top_ptrs.clear();
    for (const std::wstring &p : m->top) m->top_ptrs.push_back(p.c_str());
    *paths = m->top_ptrs.empty() ? nullptr : m->top_ptrs.data();
    return (uint32_t)m->top_ptrs.size();
}

extern "C" const wchar_t *asb_rw_stage_root(const AsbRwStage *st) { return st ? st->plain_root.c_str() : L""; }

/* Deletes what we staged (no reparse point is ever followed; we never create one). */
static bool delete_staged(const std::wstring &dir)
{
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ok = delete_staged(p) && ok;
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ok = RemoveDirectoryW(p.c_str()) && ok;
            else
                ok = DeleteFileW(p.c_str()) && ok;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(dir.c_str()) && ok;
}

extern "C" void asb_rw_stage_close(AsbRwStage *st, int keep)
{
    if (!st) return;
    close_file(st);
    if (!keep) {
        /* Incomplete staging goes now (a scanner may hold a fresh file briefly); whatever is
           left is collected with the folder by asb_clipboard_temp. */
        for (int i = 0; i < 20 && !delete_staged(st->root); ++i) Sleep(100);
        CreateDirectoryW(st->root.c_str(), nullptr);   /* the collector expects its data folder */
    }
    if (keep) {
        /* Published, then retired at once: deleted after the clipboard retention period. */
        asb_clipboard_temp_publish(st->temp, st, 0);
        asb_clipboard_temp_retire_owner(st);
    } else {
        asb_clipboard_temp_discard(st->temp);
    }
    asb_rw_file_rx_free(&st->rx);
    delete st;
}

extern "C" void asb_rw_stage_collect(const wchar_t *parent)
{
    if (!parent || !*parent) return;
    SHCreateDirectoryExW(nullptr, parent, nullptr);
    asb_clipboard_temp_collect(parent);
}

/* ================================ misc ==================================== */

extern "C" uint64_t asb_rw_files_random64(void)
{
    uint64_t v = 0;
    while (!v) BCryptGenRandom(nullptr, (PUCHAR)&v, sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v;
}

extern "C" BOOL asb_rw_files_user_dir(const wchar_t *leaf, wchar_t *out, size_t cap)
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) return FALSE;
    int n = swprintf_s(out, cap, L"%s\\AppSandbox\\RemoteWindows\\%s", base, leaf);
    CoTaskMemFree(base);
    if (n < 0) return FALSE;
    int r = SHCreateDirectoryExW(nullptr, out, nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

/* ================================ OLE ===================================== */

HRESULT asb_rw_shell_data_object(const wchar_t *const *paths, uint32_t n, IDataObject **out)
{
    *out = nullptr;
    if (!n) return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> ids;
    HRESULT hr = S_OK;
    for (uint32_t i = 0; i < n && SUCCEEDED(hr); ++i) {
        PIDLIST_ABSOLUTE id = nullptr;
        hr = SHParseDisplayName(plain_path(paths[i]).c_str(), nullptr, &id, 0, nullptr);
        if (SUCCEEDED(hr)) ids.push_back(id);
    }
    IShellItemArray *arr = nullptr;
    if (SUCCEEDED(hr)) hr = SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr);
    if (SUCCEEDED(hr)) hr = arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(out));
    if (arr) arr->Release();
    for (PIDLIST_ABSOLUTE id : ids) CoTaskMemFree(id);
    if (SUCCEEDED(hr) && *out) {
        static CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        FORMATETC fe = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM md = {};
        md.tymed = TYMED_HGLOBAL;
        md.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (md.hGlobal) {
            *(DWORD *)GlobalLock(md.hGlobal) = DROPEFFECT_COPY;
            GlobalUnlock(md.hGlobal);
            if (FAILED((*out)->SetData(&fe, &md, TRUE))) GlobalFree(md.hGlobal);
        }
    }
    return hr;
}

BOOL asb_rw_data_has_files(IDataObject *data)
{
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return data && data->QueryGetData(&fe) == S_OK;
}

uint32_t asb_rw_hdrop_paths(IDataObject *data, wchar_t ***paths, uint32_t *n)
{
    *paths = nullptr;
    *n = 0;
    if (!asb_rw_data_has_files(data)) return ASB_RW_ERR_UNSUPPORTED_DATA;
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM md = {};
    if (FAILED(data->GetData(&fe, &md))) return ASB_RW_ERR_UNSUPPORTED_DATA;
    uint32_t result = ASB_RW_OK;
    HDROP drop = (HDROP)GlobalLock(md.hGlobal);
    if (!drop) result = ASB_RW_ERR_UNSUPPORTED_DATA;
    else {
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (!count) result = ASB_RW_ERR_UNSUPPORTED_DATA;
        else if (count > ASB_RW_MAX_TOP_ENTRIES) result = ASB_RW_ERR_TOO_LARGE;
        else {
            wchar_t **list = (wchar_t **)calloc(count, sizeof(wchar_t *));
            for (UINT i = 0; list && i < count && !result; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                list[i] = (wchar_t *)calloc(len + 1, sizeof(wchar_t));
                if (!list[i] || DragQueryFileW(drop, i, list[i], len + 1) != len) result = ASB_RW_ERR_UNSUPPORTED_DATA;
            }
            if (!list) result = ASB_RW_ERR_LIMIT;
            if (result) asb_rw_free_paths(list, count);
            else { *paths = list; *n = count; }
        }
        GlobalUnlock(md.hGlobal);
    }
    ReleaseStgMedium(&md);
    return result;
}

void asb_rw_free_paths(wchar_t **paths, uint32_t n)
{
    if (!paths) return;
    for (uint32_t i = 0; i < n; ++i) free(paths[i]);
    free(paths);
}

namespace {

struct DropSource final : IDropSource {
    LONG ref = 1;
    AsbRwDropSourceOps ops;
    explicit DropSource(const AsbRwDropSourceOps *o) : ops(*o) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **pp) override
    {
        if (iid == IID_IUnknown || iid == IID_IDropSource) { *pp = static_cast<IDropSource *>(this); AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override { return ops.decide(ops.ctx, escape, keys); }
    STDMETHODIMP GiveFeedback(DWORD effect) override
    {
        return ops.feedback ? ops.feedback(ops.ctx, effect) : DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

struct DropTarget final : IDropTarget {
    LONG ref = 1;
    AsbRwDropTargetOps ops;
    explicit DropTarget(const AsbRwDropTargetOps *o) : ops(*o) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **pp) override
    {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *pp = static_cast<IDropTarget *>(this); AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP DragEnter(IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) override
    {
        *effect = ops.enter(ops.ctx, data, pt, keys) & *effect & DROPEFFECT_COPY;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD *effect) override
    {
        *effect = ops.over(ops.ctx, pt, keys) & *effect & DROPEFFECT_COPY;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override
    {
        ops.leave(ops.ctx);
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) override
    {
        *effect = ops.drop(ops.ctx, data, pt, keys) & *effect & DROPEFFECT_COPY;
        return S_OK;
    }
};

} // namespace

IDropSource *asb_rw_drop_source_create(const AsbRwDropSourceOps *ops) { return new (std::nothrow) DropSource(ops); }
IDropTarget *asb_rw_drop_target_create(const AsbRwDropTargetOps *ops) { return new (std::nothrow) DropTarget(ops); }

BOOL asb_rw_primary_down(void)
{
    int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;   /* GetAsyncKeyState reads physical buttons */
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

BOOL asb_rw_any_button_down(void)
{
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
}
