/**
 *
 *
 * @module AppSandbox/tools/remote-windows-spike/rw_spike_proto
 *
 * @file      rw_spike_proto.h
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/*
 * Remote Windows feasibility spike (AppSandbox-eh6.1) - wire protocol.
 *
 * Throwaway prototype protocol shared by rw_spike_guest.exe and rw_spike_host.exe.
 * It is NOT the product protocol (eh6.2 defines that). One AF_HYPERV stream carries
 * control, frames and file payloads; the product reserves channels 9/10/11 so bulk
 * data never blocks control/input.
 *
 * Every message: RwHdr followed by `len` payload bytes. Little-endian.
 */
#pragma once
#include <stdint.h>

#define RW_MAGIC   0x31535752u   /* "RWS1" */
#define RW_VERSION 1u
#define RW_MAX_PAYLOAD (64u * 1024u * 1024u)
#define RW_FILE_CHUNK  (1u * 1024u * 1024u)

/* Spike service: AppSandbox channel 7 (a5b0cafe-0007-4000-8000-000000000001). It is already in the
   VM's HvSocket ServiceTable with an Everyone connect descriptor and is idle when SSH is disabled. */
#define RW_SPIKE_SERVICE_GUID L"a5b0cafe-0007-4000-8000-000000000001"

enum RwType : uint32_t {
    RW_HELLO = 1,          /* G->H RwHello */
    RW_LIST = 2,           /* H->G (empty)          G->H RwListReply + entries */
    RW_SELECT = 3,         /* H->G RwHwnd           G->H RwSelectReply */
    RW_UNSELECT = 4,       /* H->G RwHwnd           G->H RwStatus */
    RW_FRAME = 5,          /* G->H RwFrame + BGRA pixels */
    RW_SURFACES = 6,       /* G->H RwSurfaces + RwSurface[count] (current popup set for a root) */
    RW_MOUSE = 7,          /* H->G RwMouse */
    RW_KEY = 8,            /* H->G RwKey */
    RW_ACTIVATE = 9,       /* H->G RwHwnd */
    RW_CLOSE = 10,         /* H->G RwHwnd (SC_CLOSE request, app keeps its prompts) */
    RW_DROP_BEGIN = 11,    /* H->G RwDropBegin, then RW_FILE_ENTRY/RW_FILE_DATA..., then RW_DROP_END */
    RW_FILE_ENTRY = 12,    /* both directions: RwFileEntry */
    RW_FILE_DATA = 13,     /* both directions: raw bytes for the current file */
    RW_DROP_END = 14,      /* H->G (empty) */
    RW_DROP_RESULT = 15,   /* G->H RwStatus (status = DROPEFFECT, detail = HRESULT) */
    RW_DRAGOUT_PROBE = 16, /* H->G (empty): host pointer left a proxy with the button held */
    RW_DRAGOUT_BEGIN = 17, /* G->H RwStatus(detail=entry count) then entries/data, then RW_DRAGOUT_READY */
    RW_DRAGOUT_NONE = 18,  /* G->H RwStatus: no guest OLE drag (or unsupported data) */
    RW_DRAGOUT_READY = 19, /* G->H (empty) */
    RW_DRAGOUT_DONE = 20,  /* H->G RwStatus(status = host DROPEFFECT) -> guest ends its drag over the catcher */
    RW_RESTORE_ALL = 21,   /* H->G (empty)          G->H RwStatus */
    RW_LOG = 22,           /* G->H UTF-8 text (diagnostics/evidence) */
    RW_SHELL_STATE = 23,   /* H->G RwHwnd           G->H RwShellState */
};

#pragma pack(push, 1)
typedef struct { uint32_t magic, type, len; } RwHdr;
typedef struct { int32_t l, t, r, b; } RwRect;
typedef struct { uint32_t version; RwRect privateDisplay; uint32_t privateDpi; RwRect desktop; } RwHello;
typedef struct { uint64_t hwnd; } RwHwnd;
/* RW_SELECT payload: RwHwnd optionally followed by a uint32 suppression mode. */
#define RW_SUPPRESS_TOOLWINDOW 0u   /* WS_EX_TOOLWINDOW (+ remove WS_EX_APPWINDOW) + DeleteTab */
#define RW_SUPPRESS_OWNER      1u   /* hidden helper owner (GWLP_HWNDPARENT) + DeleteTab; frame unchanged */
typedef struct { uint64_t hwnd; uint32_t mode; } RwSelect;
typedef struct { uint32_t count; } RwListReply;
typedef struct { uint64_t hwnd; uint32_t pid; RwRect rect; wchar_t title[128]; wchar_t cls[64]; wchar_t exe[64]; } RwListEntry;
typedef struct { uint64_t hwnd; int32_t status; RwRect frame; uint32_t dpi; wchar_t title[128]; } RwSelectReply;
typedef struct { int32_t status; int32_t detail; } RwStatus;
/* A captured surface. `rect` is the surface's guest screen rectangle (physical pixels) and
   `root` the selected root it belongs to; host places popups relative to that root. */
typedef struct { uint64_t root; uint64_t hwnd; RwRect rect; uint32_t width, height, kind, seq; } RwFrame;
#define RW_SURFACE_ROOT  1u
#define RW_SURFACE_POPUP 2u
typedef struct { uint64_t root; RwRect rootRect; uint32_t count; } RwSurfaces;
typedef struct { uint64_t hwnd; RwRect rect; uint32_t kind; } RwSurface;
#define RW_MOUSE_MOVE  0u
#define RW_MOUSE_DOWN  1u
#define RW_MOUSE_UP    2u
#define RW_MOUSE_WHEEL 3u
typedef struct { uint32_t kind, button; int32_t x, y, wheel; } RwMouse;   /* guest screen physical px */
typedef struct { uint16_t scan, flags; } RwKey;                             /* flags: 1 extended, 2 up */
typedef struct { uint64_t root; int32_t x, y; uint32_t count; } RwDropBegin;
typedef struct { uint32_t isDir; uint64_t size; wchar_t rel[260]; } RwFileEntry;
typedef struct { uint64_t hwnd; uint32_t onTaskbarCandidate, toolWindow, appWindow, visible, cloaked, onPrivate; RwRect rect; } RwShellState;
#pragma pack(pop)
