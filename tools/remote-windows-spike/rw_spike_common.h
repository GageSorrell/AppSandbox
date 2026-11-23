/**
 *
 *
 * @module AppSandbox/tools/remote-windows-spike/rw_spike_common
 *
 * @file      rw_spike_common.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows feasibility spike (AppSandbox-eh6.1) - socket and file-staging helpers. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <mutex>
#include "rw_spike_proto.h"

/* AF_HYPERV definitions (hvsocket.h is not in every SDK layout). */
#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#define HV_PROTOCOL_RAW 1
typedef struct { ADDRESS_FAMILY Family; USHORT Reserved; GUID VmId; GUID ServiceId; } RW_SOCKADDR_HV;

static FILE* g_rwLog;
static void RwLog(const char* fmt, ...)
{
    char buf[2048];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = sprintf_s(buf, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap); va_end(ap);
    fputs(buf, stdout); fputs("\n", stdout); fflush(stdout);
    if (g_rwLog) { fputs(buf, g_rwLog); fputs("\n", g_rwLog); fflush(g_rwLog); }
}

struct RwConn {
    SOCKET s = INVALID_SOCKET;
    std::mutex sendLock;
    bool SendAll(const void* p, size_t n) {
        const char* c = (const char*)p;
        while (n) {
            int k = send(s, c, (int)std::min<size_t>(n, 1u << 20), 0);
            if (k <= 0) return false;
            c += k; n -= (size_t)k;
        }
        return true;
    }
    bool RecvAll(void* p, size_t n) {
        char* c = (char*)p;
        while (n) {
            int k = recv(s, c, (int)std::min<size_t>(n, 1u << 20), 0);
            if (k <= 0) return false;
            c += k; n -= (size_t)k;
        }
        return true;
    }
    /* Sends header + up to two payload parts atomically with respect to other senders. */
    bool Send(uint32_t type, const void* a = nullptr, uint32_t an = 0, const void* b = nullptr, uint32_t bn = 0) {
        RwHdr h = { RW_MAGIC, type, an + bn };
        std::lock_guard<std::mutex> g(sendLock);
        return SendAll(&h, sizeof(h)) && (!an || SendAll(a, an)) && (!bn || SendAll(b, bn));
    }
    bool Recv(RwHdr& h, std::vector<uint8_t>& payload) {
        if (!RecvAll(&h, sizeof(h)) || h.magic != RW_MAGIC || h.len > RW_MAX_PAYLOAD) return false;
        payload.resize(h.len);
        return !h.len || RecvAll(payload.data(), h.len);
    }
};

static bool RwParseGuid(const wchar_t* s, GUID* g)
{
    std::wstring w = std::wstring(L"{") + s + L"}";
    return SUCCEEDED(CLSIDFromString(w.c_str(), g));
}

/* ---------------- file transfer (copy only) ---------------- */

struct RwEntry { std::wstring src, rel; bool dir; uint64_t size; };

/* Validates a relative path from the peer: no absolute/drive/stream/UNC forms, no traversal. */
static bool RwSafeRel(const std::wstring& rel)
{
    if (rel.empty() || rel.size() >= 250 || rel[0] == L'\\' || rel[0] == L'/') return false;
    if (rel.find(L':') != std::wstring::npos) return false;
    size_t start = 0;
    for (;;) {
        size_t sep = rel.find_first_of(L"\\/", start);
        std::wstring part = rel.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        if (part.empty() || part == L"." || part == L"..") return false;
        if (part.find_first_of(L"<>\"|?*") != std::wstring::npos) return false;
        for (wchar_t c : part) if (c < 32) return false;
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return true;
}

static bool RwCollect(const std::wstring& path, const std::wstring& rel, std::vector<RwEntry>& out, std::wstring& err)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) { err = L"unreadable: " + path; return false; }
    if (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { err = L"reparse point rejected: " + path; return false; }
    if (!RwSafeRel(rel)) { err = L"unsafe name: " + rel; return false; }
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        out.push_back({ path, rel, true, 0 });
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) return true;
        bool ok = true;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            ok = RwCollect(path + L"\\" + fd.cFileName, rel + L"\\" + fd.cFileName, out, err);
        } while (ok && FindNextFileW(f, &fd));
        FindClose(f);
        return ok;
    }
    out.push_back({ path, rel, false, ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow });
    return true;
}

static bool RwSendEntries(RwConn& c, const std::vector<RwEntry>& entries)
{
    std::vector<uint8_t> buf(RW_FILE_CHUNK);
    for (const RwEntry& e : entries) {
        RwFileEntry fe = {};
        fe.isDir = e.dir; fe.size = e.size;
        wcsncpy_s(fe.rel, e.rel.c_str(), _TRUNCATE);
        if (!c.Send(RW_FILE_ENTRY, &fe, sizeof(fe))) return false;
        if (e.dir) continue;
        HANDLE h = CreateFileW(e.src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        uint64_t left = e.size;
        while (left) {
            DWORD want = (DWORD)std::min<uint64_t>(left, RW_FILE_CHUNK), got = 0;
            if (h == INVALID_HANDLE_VALUE || !ReadFile(h, buf.data(), want, &got, nullptr) || !got) {
                memset(buf.data(), 0, want); got = want;   /* keep the stream framed; receiver sees size */
            }
            if (!c.Send(RW_FILE_DATA, buf.data(), got)) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); return false; }
            left -= got;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return true;
}

static void RwDeleteTree(const std::wstring& dir)
{
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE; op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

/* Receives entries into a fresh staging directory; on any failure the partial tree is removed. */
struct RwStager {
    std::wstring root;
    std::vector<std::wstring> topLevel;
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t left = 0;
    bool failed = false;
    std::wstring error;

    bool Begin(const wchar_t* base) {
        GUID g; CoCreateGuid(&g);
        wchar_t id[64]; StringFromGUID2(g, id, 64);
        root = std::wstring(base) + L"\\" + std::wstring(id + 1, 36);
        SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
        return GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    void Fail(const std::wstring& why) { if (!failed) { failed = true; error = why; } }
    void Entry(const RwFileEntry& fe) {
        CloseFile();
        std::wstring rel(fe.rel, wcsnlen(fe.rel, 260));
        if (!RwSafeRel(rel)) { Fail(L"rejected unsafe path " + rel); left = fe.isDir ? 0 : fe.size; return; }
        std::wstring full = root + L"\\" + rel;
        if (rel.find_first_of(L"\\/") == std::wstring::npos) topLevel.push_back(full);
        if (fe.isDir) { if (!CreateDirectoryW(full.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) Fail(L"mkdir " + rel); left = 0; return; }
        file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) Fail(L"create " + rel);
        left = fe.size;
    }
    void Data(const uint8_t* p, size_t n) {
        if (n > left) { Fail(L"overlong file data"); n = (size_t)left; }
        left -= n;
        DWORD w;
        if (file != INVALID_HANDLE_VALUE && !WriteFile(file, p, (DWORD)n, &w, nullptr)) Fail(L"write");
        if (!left) CloseFile();
    }
    void CloseFile() { if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; } }
    bool End() {
        CloseFile();
        if (left) Fail(L"truncated transfer");
        if (failed) { RwDeleteTree(root); topLevel.clear(); }
        return !failed;
    }
};

/* A real shell IDataObject (CF_HDROP, Shell IDList Array, ...) for filesystem paths that share one
   parent folder - the same object Explorer itself produces for a selection. */
static HRESULT RwShellDataObject(const std::vector<std::wstring>& paths, IDataObject** out)
{
    *out = nullptr;
    std::vector<PIDLIST_ABSOLUTE> ids;
    HRESULT hr = S_OK;
    for (const std::wstring& p : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        hr = SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr);
        if (FAILED(hr)) break;
        ids.push_back(id);
    }
    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(hr) && !ids.empty())
        hr = SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr);
    if (SUCCEEDED(hr) && arr) hr = arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(out));
    if (arr) arr->Release();
    for (auto id : ids) CoTaskMemFree(id);
    return ids.empty() && SUCCEEDED(hr) ? E_INVALIDARG : hr;
}

static void RwSetPreferredCopy(IDataObject* d)
{
    static CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    FORMATETC fe = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m = {}; m.tymed = TYMED_HGLOBAL;
    m.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!m.hGlobal) return;
    *(DWORD*)GlobalLock(m.hGlobal) = DROPEFFECT_COPY; GlobalUnlock(m.hGlobal);
    if (FAILED(d->SetData(&fe, &m, TRUE))) GlobalFree(m.hGlobal);
}

static std::vector<std::wstring> RwHdropPaths(IDataObject* d)
{
    std::vector<std::wstring> paths;
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m = {};
    if (FAILED(d->GetData(&fe, &m))) return paths;
    HDROP drop = (HDROP)GlobalLock(m.hGlobal);
    if (drop) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t p[MAX_PATH * 2];
            if (DragQueryFileW(drop, i, p, ARRAYSIZE(p))) paths.push_back(p);
        }
        GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    return paths;
}

/* Minimal IDropSource for a copy-only drag driven by the real (or injected) mouse state. */
struct RwDropSource : IDropSource {
    LONG ref = 1;
    STDMETHODIMP QueryInterface(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_IDropSource) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    DWORD lastEffect = 0, feedbackCount = 0;
    STDMETHODIMP GiveFeedback(DWORD effect) override { lastEffect = effect; ++feedbackCount; return DRAGDROP_S_USEDEFAULTCURSORS; }
};
