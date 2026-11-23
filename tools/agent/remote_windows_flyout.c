/**
 *
 *
 * @module AppSandbox/tools/agent/remote_windows_flyout
 *
 * @file      remote_windows_flyout.c
 * @author    Gage Sorrell <gage@sorrell.sh>
 * @copyright (c) 2026 Gage Sorrell
 * @license   MIT
 */

/* Remote Windows selection flyout shared by host and guest (AppSandbox-eh6.8). */
#ifndef UNICODE
#define UNICODE   /* ListView/common-control macros must take the wide structures */
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "remote_windows_flyout.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

/* ================================ pure helpers ============================ */

static void wcopy_utf16(wchar_t *dst, size_t cap, const uint16_t *src, uint32_t len)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = (wchar_t)src[i];
    dst[n] = 0;
}

void asb_rw_flyout_fill(AsbRwFlyoutModel *m, const AsbRwInventoryEntry *entries, const uint32_t *states,
                        const uint32_t *errors, uint32_t count)
{
    uint16_t label[ASB_RW_FLYOUT_LABEL];
    if (count > ASB_RW_FLYOUT_MAX_ITEMS) count = ASB_RW_FLYOUT_MAX_ITEMS;
    m->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        AsbRwFlyoutItem *it = &m->items[i];
        memset(it, 0, sizeof(*it));
        it->id = entries[i].id;
        it->state = states ? states[i] : ((entries[i].flags & ASB_RW_ENTRY_SELECTED) ? ASB_RW_STATE_SHARED : ASB_RW_STATE_NONE);
        it->error = errors ? errors[i] : 0;
        it->minimized = (entries[i].flags & ASB_RW_ENTRY_MINIMIZED) != 0;
        uint32_t n = asb_rw_entry_label(entries, count, i, label, ASB_RW_FLYOUT_LABEL);
        wcopy_utf16(it->label, ASB_RW_FLYOUT_LABEL, label, n);
        if (!it->label[0]) wcscpy_s(it->label, ASB_RW_FLYOUT_LABEL, L"(untitled)");
    }
}

int asb_rw_flyout_row_on(uint32_t state)
{
    return state == ASB_RW_STATE_SHARED || state == ASB_RW_STATE_PENDING;
}

void asb_rw_flyout_row_status(uint32_t state, uint32_t error, int requested, wchar_t *out, size_t cap)
{
    if (requested) { wcscpy_s(out, cap, L"Requesting…"); return; }
    switch (state) {
    case ASB_RW_STATE_PENDING: wcscpy_s(out, cap, L"Sharing…"); return;
    case ASB_RW_STATE_SHARED: wcscpy_s(out, cap, L"Shared"); return;
    case ASB_RW_STATE_RESTORING: wcscpy_s(out, cap, L"Restoring…"); return;
    default: break;
    }
    if (error && error != ASB_RW_ERR_STALE_ID) {
        const char *text = asb_rw_error_text(error);
        swprintf_s(out, cap, L"Failed: %hs", text ? text : "error");
        return;
    }
    out[0] = 0;
}

int asb_rw_flyout_interactive(uint32_t status)
{
    return status == ASB_RW_FLYOUT_READY;
}

void asb_rw_flyout_status_text(const AsbRwFlyoutModel *m, wchar_t *out, size_t cap)
{
    if (m->status != ASB_RW_FLYOUT_READY && m->message[0]) { wcscpy_s(out, cap, m->message); return; }
    switch (m->status) {
    case ASB_RW_FLYOUT_READY:
        if (!m->count) wcscpy_s(out, cap, L"No application windows are open.");
        else wcscpy_s(out, cap, m->message[0] ? m->message : L"Turn a window on to share it.");
        return;
    case ASB_RW_FLYOUT_CONNECTING: wcscpy_s(out, cap, L"Connecting to the guest…"); return;
    case ASB_RW_FLYOUT_DISCONNECTED: wcscpy_s(out, cap, L"Not connected."); return;
    case ASB_RW_FLYOUT_UPGRADE:
        wcscpy_s(out, cap, L"Update the App Sandbox guest components to share windows."); return;
    case ASB_RW_FLYOUT_NO_GUEST: wcscpy_s(out, cap, L"Start a Windows guest to share its windows."); return;
    case ASB_RW_FLYOUT_MULTIPLE:
        wcscpy_s(out, cap, L"Sharing windows needs exactly one running Windows guest."); return;
    default: wcscpy_s(out, cap, L"Window sharing is unavailable."); return;
    }
}

static LONG clampl(LONG v, LONG lo, LONG hi) { return hi < lo ? lo : v < lo ? lo : v > hi ? hi : v; }

void asb_rw_flyout_place(const RECT *anchor, const RECT *work, const RECT *monitor, int w, int h, int gap, RECT *out)
{
    RECT wa = *work;
    if (wa.right <= wa.left || wa.bottom <= wa.top) wa = *monitor;
    LONG cx = (anchor->left + anchor->right) / 2, cy = (anchor->top + anchor->bottom) / 2, x, y;
    if (anchor->top >= wa.bottom) {               /* taskbar at the bottom */
        x = cx - w / 2;
        y = wa.bottom - gap - h;
    } else if (anchor->bottom <= wa.top) {        /* top */
        x = cx - w / 2;
        y = wa.top + gap;
    } else if (anchor->right <= wa.left) {        /* left */
        x = wa.left + gap;
        y = cy - h / 2;
    } else if (anchor->left >= wa.right) {        /* right */
        x = wa.right - gap - w;
        y = cy - h / 2;
    } else {                                      /* inside the work area (overflow area, cursor) */
        x = cx - w / 2;
        y = anchor->top - gap - h;
        if (y < wa.top + gap) y = anchor->bottom + gap;
    }
    x = clampl(x, wa.left + gap, wa.right - gap - w);
    y = clampl(y, wa.top + gap, wa.bottom - gap - h);
    out->left = x;
    out->top = y;
    out->right = x + w;
    out->bottom = y + h;
}

#define CLOSE_GRACE_MS 400u

uint32_t asb_rw_tray_event(AsbRwTrayClicks *c, UINT msg, uint64_t now, uint32_t dbl_ms)
{
    switch (msg) {
    case WM_LBUTTONUP:
        if (c->double_at && now - c->double_at <= dbl_ms) { c->double_at = 0; return ASB_RW_TRAY_NONE; }
        if (c->closed_at && now - c->closed_at <= CLOSE_GRACE_MS) { c->closed_at = 0; return ASB_RW_TRAY_NONE; }
        c->armed = 1;
        c->armed_at = now;
        return ASB_RW_TRAY_ARM;
    case WM_LBUTTONDBLCLK:
        c->armed = 0;
        c->double_at = now;
        return ASB_RW_TRAY_DOUBLE;
    case WM_RBUTTONUP:
        c->armed = 0;
        return ASB_RW_TRAY_MENU;
    default:
        return ASB_RW_TRAY_NONE;
    }
}

uint32_t asb_rw_tray_timer(AsbRwTrayClicks *c, uint64_t now)
{
    (void)now;
    if (!c->armed) return ASB_RW_TRAY_NONE;
    c->armed = 0;
    return ASB_RW_TRAY_OPEN;
}

void asb_rw_tray_closed(AsbRwTrayClicks *c, uint64_t now)
{
    c->closed_at = now ? now : 1;
}

HICON asb_rw_icon_to_hicon(const AsbRwWindowIcon *icon)
{
    if (!icon || !icon->width || !icon->height || icon->width > ASB_RW_MAX_ICON || icon->height > ASB_RW_MAX_ICON) return NULL;
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = (LONG)icon->width;
    bi.bV5Height = -(LONG)icon->height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color) return NULL;
    const uint8_t *src = icon->pixels;
    uint8_t *dst = (uint8_t *)bits;
    for (uint32_t i = 0; i < icon->width * icon->height; ++i) {
        uint8_t a = src[i * 4 + 3];
        for (int c = 0; c < 3; ++c) {   /* icons take straight alpha */
            unsigned v = a ? (unsigned)src[i * 4 + c] * 255u / a : 0u;
            dst[i * 4 + c] = (uint8_t)(v > 255 ? 255 : v);
        }
        dst[i * 4 + 3] = a;
    }
    HBITMAP mask = CreateBitmap((int)icon->width, (int)icon->height, 1, 1, NULL);
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return h;
}

/* ================================ tray icon =============================== */

static void tray_data(const AsbRwTrayIcon *t, NOTIFYICONDATAW *nid)
{
    memset(nid, 0, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = t->hwnd;
    nid->uID = t->id;
    nid->uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid->uCallbackMessage = t->callback;
    nid->hIcon = t->icon;
    wcscpy_s(nid->szTip, ARRAYSIZE(nid->szTip), t->tip);
}

BOOL asb_rw_tray_add(AsbRwTrayIcon *t)
{
    NOTIFYICONDATAW nid;
    if (!t->taskbar_created) {
        t->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        /* An elevated owner (the host app) must accept the message from Explorer. */
        if (t->taskbar_created) ChangeWindowMessageFilterEx(t->hwnd, t->taskbar_created, MSGFLT_ALLOW, NULL);
    }
    tray_data(t, &nid);
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (ok) { t->added = 1; t->adds++; }
    return ok;
}

void asb_rw_tray_remove(AsbRwTrayIcon *t)
{
    NOTIFYICONDATAW nid;
    if (!t->added) return;
    tray_data(t, &nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    t->added = 0;
}

BOOL asb_rw_tray_message(AsbRwTrayIcon *t, UINT msg)
{
    if (!t->taskbar_created || msg != t->taskbar_created) return FALSE;
    t->added = 0;   /* the new taskbar does not know the icon */
    asb_rw_tray_add(t);
    return TRUE;
}

BOOL asb_rw_tray_rect(const AsbRwTrayIcon *t, RECT *out)
{
    NOTIFYICONIDENTIFIER id;
    memset(&id, 0, sizeof(id));
    id.cbSize = sizeof(id);
    id.hWnd = t->hwnd;
    id.uID = t->id;
    return SUCCEEDED(Shell_NotifyIconGetRect(&id, out));
}

/* ================================ flyout window =========================== */

#define FLYOUT_CLASS   L"AppSandbox.RemoteWindows.Flyout"
#define ID_LIST        100
#define TIMER_REQUESTS 1
#define REQUEST_TTL_MS 10000u
#define MAX_VISIBLE_ROWS 8u

typedef struct FlyoutRow {
    uint64_t id;
    int requested;
    uint32_t requested_state;
    uint64_t requested_at;
} FlyoutRow;

struct AsbRwFlyout {
    AsbRwFlyoutConfig cfg;
    HWND hwnd, heading, list, status;
    HFONT font, bold;
    HIMAGELIST icons, toggles;
    HBRUSH bg_brush;
    uint32_t dpi;
    int dark;
    COLORREF bg, fg, sub, err, accent;
    AsbRwFlyoutModel *model;
    FlyoutRow *rows;
    uint32_t count;
    int updating, visible;
    RECT anchor;
    int have_anchor;
};

static int S(const AsbRwFlyout *f, int v) { return MulDiv(v, (int)f->dpi, 96); }

static int system_dark(void)
{
    DWORD light = 1, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) != ERROR_SUCCESS)
        return 0;
    return light == 0;
}

static COLORREF accent_color(void)
{
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)) && argb)
        return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    return RGB(0, 120, 212);
}

/* ---- toggle images (analytic anti-aliasing, premultiplied BGRA) ---- */

static void blend(uint8_t *px, COLORREF c, double a)
{
    if (a <= 0) return;
    if (a > 1) a = 1;
    double inv = 1.0 - a;
    px[0] = (uint8_t)(GetBValue(c) * a + px[0] * inv + 0.5);
    px[1] = (uint8_t)(GetGValue(c) * a + px[1] * inv + 0.5);
    px[2] = (uint8_t)(GetRValue(c) * a + px[2] * inv + 0.5);
    px[3] = (uint8_t)(255.0 * a + px[3] * inv + 0.5);
}

static double cover(double d) { return d <= -0.5 ? 1.0 : d >= 0.5 ? 0.0 : 0.5 - d; }

/* One toggle (track w_t x h_t at the image's vertical centre, left aligned with a small pad). */
static HBITMAP toggle_bitmap(const AsbRwFlyout *f, int w, int h, int on, int dim)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return NULL;
    uint8_t *p = (uint8_t *)bits;
    memset(p, 0, (size_t)w * h * 4);
    double th = S(f, 20), tw = S(f, 40), pad = S(f, 4), r = th / 2.0;
    double cy = h / 2.0, x0 = pad + r, x1 = pad + tw - r, stroke = S(f, 1) < 1 ? 1 : S(f, 1);
    COLORREF knob_off = f->fg, knob_on = f->dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double sx = px < x0 ? x0 : px > x1 ? x1 : px;
            double d = sqrt((px - sx) * (px - sx) + (py - cy) * (py - cy)) - r;   /* capsule SDF */
            uint8_t *o = p + ((size_t)y * w + x) * 4;
            if (on) {
                blend(o, f->accent, cover(d));
                double kd = sqrt((px - x1) * (px - x1) + (py - cy) * (py - cy)) - r * 0.55;
                blend(o, knob_on, cover(kd));
            } else {
                double ring = fabs(d + stroke / 2.0) - stroke / 2.0;
                blend(o, f->sub, cover(ring));
                double kd = sqrt((px - x0) * (px - x0) + (py - cy) * (py - cy)) - r * 0.45;
                blend(o, knob_off, cover(kd));
            }
            if (dim) o[3] = (uint8_t)(o[3] * 0.45);
            /* premultiply */
            o[0] = (uint8_t)(o[0] * o[3] / 255);
            o[1] = (uint8_t)(o[1] * o[3] / 255);
            o[2] = (uint8_t)(o[2] * o[3] / 255);
        }
    }
    return bmp;
}

static int row_height(const AsbRwFlyout *f) { return S(f, 36); }

static void build_toggles(AsbRwFlyout *f)
{
    int w = S(f, 48), h = row_height(f);
    HIMAGELIST il = ImageList_Create(w, h, ILC_COLOR32, 4, 0);
    /* state image n uses image n - 1: 1 off, 2 on, 3 requested off, 4 requested on */
    for (int i = 0; i < 4; ++i) {
        HBITMAP b = toggle_bitmap(f, w, h, i == 1 || i == 3, i >= 2);
        if (b) { ImageList_Add(il, b, NULL); DeleteObject(b); }
    }
    HIMAGELIST old = ListView_SetImageList(f->list, il, LVSIL_STATE);
    if (old && old != f->toggles) ImageList_Destroy(old);
    if (f->toggles && f->toggles != il) ImageList_Destroy(f->toggles);
    f->toggles = il;
}

/* Icon centred in a row-high cell so rows get their height from the small image list. */
static int add_icon(AsbRwFlyout *f, HICON icon)
{
    int cell = row_height(f), size = S(f, 20);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cell;
    bi.bmiHeader.biHeight = -cell;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int index = -1;
    if (bmp && dc) {
        memset(bits, 0, (size_t)cell * cell * 4);
        HGDIOBJ old = SelectObject(dc, bmp);
        DrawIconEx(dc, (cell - size) / 2, (cell - size) / 2, icon ? icon : LoadIconW(NULL, IDI_APPLICATION), size, size, 0,
                   NULL, DI_NORMAL);
        SelectObject(dc, old);
        index = ImageList_Add(f->icons, bmp, NULL);
    }
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    return index;
}

static void apply_theme(AsbRwFlyout *f)
{
    f->dark = f->cfg.theme == ASB_RW_THEME_DARK || (f->cfg.theme == ASB_RW_THEME_SYSTEM && system_dark());
    f->bg = f->dark ? RGB(43, 43, 43) : RGB(249, 249, 249);
    f->fg = f->dark ? RGB(255, 255, 255) : RGB(26, 26, 26);
    f->sub = f->dark ? RGB(200, 200, 200) : RGB(96, 96, 96);
    f->err = f->dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
    f->accent = accent_color();
    if (f->bg_brush) DeleteObject(f->bg_brush);
    f->bg_brush = CreateSolidBrush(f->bg);
    BOOL dark = f->dark;
    DwmSetWindowAttribute(f->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SetWindowTheme(f->list, f->dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    ListView_SetBkColor(f->list, f->bg);
    ListView_SetTextBkColor(f->list, f->bg);
    ListView_SetTextColor(f->list, f->fg);
}

static void apply_fonts(AsbRwFlyout *f)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, f->dpi)) {
        ncm.lfMessageFont.lfHeight = -S(f, 12);
        wcscpy_s(ncm.lfMessageFont.lfFaceName, LF_FACESIZE, L"Segoe UI");
    }
    if (f->font) DeleteObject(f->font);
    if (f->bold) DeleteObject(f->bold);
    f->font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW b = ncm.lfMessageFont;
    b.lfWeight = FW_SEMIBOLD;
    b.lfHeight = b.lfHeight * 115 / 100;
    f->bold = CreateFontIndirectW(&b);
    SendMessageW(f->heading, WM_SETFONT, (WPARAM)f->bold, FALSE);
    SendMessageW(f->list, WM_SETFONT, (WPARAM)f->font, FALSE);
    SendMessageW(f->status, WM_SETFONT, (WPARAM)f->font, FALSE);
}

static FlyoutRow *row_of(AsbRwFlyout *f, uint64_t id)
{
    for (uint32_t i = 0; i < f->count; ++i) if (f->rows[i].id == id) return &f->rows[i];
    return NULL;
}

static int row_requested(const AsbRwFlyout *f, uint32_t i)
{
    return f->rows[i].requested || f->model->items[i].pending;
}

static UINT state_image(const AsbRwFlyout *f, uint32_t i)
{
    const AsbRwFlyoutItem *it = &f->model->items[i];
    int on = asb_rw_flyout_row_on(it->state);
    if (f->rows[i].requested) return INDEXTOSTATEIMAGEMASK(on ? 3 : 4);   /* heading to the other side */
    if (it->pending || it->state == ASB_RW_STATE_PENDING || it->state == ASB_RW_STATE_RESTORING)
        return INDEXTOSTATEIMAGEMASK(it->state == ASB_RW_STATE_RESTORING ? 3 : 4);
    return INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
}

static void set_row(AsbRwFlyout *f, uint32_t i, int insert, int image)
{
    const AsbRwFlyoutItem *it = &f->model->items[i];
    LVITEMW lv;
    wchar_t status[ASB_RW_FLYOUT_LABEL];
    memset(&lv, 0, sizeof(lv));
    lv.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    lv.iItem = (int)i;
    lv.pszText = (LPWSTR)it->label;
    lv.iImage = image;
    lv.lParam = (LPARAM)i;
    if (insert) ListView_InsertItem(f->list, &lv);
    else ListView_SetItem(f->list, &lv);
    ListView_SetItemState(f->list, (int)i, state_image(f, i), LVIS_STATEIMAGEMASK);
    asb_rw_flyout_row_status(it->state, it->error, row_requested(f, i), status, ARRAYSIZE(status));
    ListView_SetItemText(f->list, (int)i, 1, status);
}

static void layout(AsbRwFlyout *f, int reposition);

static uint64_t focused_id(const AsbRwFlyout *f)
{
    int focused = ListView_GetNextItem(f->list, -1, LVNI_FOCUSED);
    return focused >= 0 && (uint32_t)focused < f->count ? f->rows[focused].id : 0;
}

/* Rebuilds the list from f->model/f->rows; the focus stays on the window `focus_id`. */
static void refresh_rows_keep(AsbRwFlyout *f, uint64_t focus_id)
{
    f->updating = 1;
    SendMessageW(f->list, WM_SETREDRAW, FALSE, 0);
    ImageList_RemoveAll(f->icons);
    int generic = add_icon(f, NULL);
    int have = ListView_GetItemCount(f->list);
    for (uint32_t i = 0; i < f->model->count; ++i) {
        int image = f->model->items[i].icon ? add_icon(f, f->model->items[i].icon) : generic;
        set_row(f, i, (int)i >= have, image);
    }
    while (ListView_GetItemCount(f->list) > (int)f->model->count) ListView_DeleteItem(f->list, ListView_GetItemCount(f->list) - 1);
    int target = 0;
    for (uint32_t i = 0; i < f->count; ++i) if (focus_id && f->rows[i].id == focus_id) target = (int)i;
    if (f->count) {
        ListView_SetItemState(f->list, target, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
        ListView_EnsureVisible(f->list, target, FALSE);
    }
    SendMessageW(f->list, WM_SETREDRAW, TRUE, 0);
    f->updating = 0;
    InvalidateRect(f->list, NULL, TRUE);
}

static void refresh_rows(AsbRwFlyout *f) { refresh_rows_keep(f, focused_id(f)); }

void asb_rw_flyout_update(AsbRwFlyout *f, const AsbRwFlyoutModel *m)
{
    static FlyoutRow next[ASB_RW_FLYOUT_MAX_ITEMS];
    uint64_t now = GetTickCount64(), focus = focused_id(f);   /* before the rows change */
    uint32_t n = m->count > ASB_RW_FLYOUT_MAX_ITEMS ? ASB_RW_FLYOUT_MAX_ITEMS : m->count;
    for (uint32_t i = 0; i < n; ++i) {
        const AsbRwFlyoutItem *it = &m->items[i];
        FlyoutRow *old = row_of(f, it->id);
        memset(&next[i], 0, sizeof(next[i]));
        next[i].id = it->id;
        /* A request stays shown until the acknowledged state moves, an error arrives, the
           owner reports its own pending request, or it is too old. */
        if (old && old->requested && it->state == old->requested_state && !it->error && !it->pending &&
            now - old->requested_at < REQUEST_TTL_MS)
            next[i] = *old;
    }
    *f->model = *m;
    f->model->count = n;
    memcpy(f->rows, next, n * sizeof(next[0]));
    f->count = n;
    SetWindowTextW(f->heading, m->heading);
    SetWindowTextW(f->list, m->heading[0] ? m->heading : L"Windows");
    wchar_t status[400];
    asb_rw_flyout_status_text(m, status, ARRAYSIZE(status));
    SetWindowTextW(f->status, status);
    EnableWindow(f->list, asb_rw_flyout_interactive(m->status) && n);
    refresh_rows_keep(f, focus);
    layout(f, f->visible);
}

static void request(AsbRwFlyout *f, int index)
{
    if (index < 0 || (uint32_t)index >= f->count || !asb_rw_flyout_interactive(f->model->status)) return;
    const AsbRwFlyoutItem *it = &f->model->items[index];
    FlyoutRow *row = &f->rows[index];
    if (row_requested(f, (uint32_t)index) || it->state == ASB_RW_STATE_PENDING || it->state == ASB_RW_STATE_RESTORING) {
        MessageBeep(MB_OK);   /* a transition is already in flight */
        return;
    }
    int select = !asb_rw_flyout_row_on(it->state);
    row->requested = 1;
    row->requested_state = it->state;
    row->requested_at = GetTickCount64();
    refresh_rows(f);   /* "Requesting..." and the dimmed target toggle */
    if (f->cfg.toggle) f->cfg.toggle(f->cfg.ctx, it->id, select);
}

static int list_rows_height(AsbRwFlyout *f, uint32_t rows)
{
    RECT r;
    int h = row_height(f);
    if (ListView_GetItemCount(f->list) && ListView_GetItemRect(f->list, 0, &r, LVIR_BOUNDS) && r.bottom > r.top) h = r.bottom - r.top;
    return (int)rows * h;
}

static void layout(AsbRwFlyout *f, int reposition)
{
    uint32_t dpi = f->cfg.dpi ? f->cfg.dpi : GetDpiForWindow(f->hwnd);
    if (!dpi) dpi = 96;
    if (dpi != f->dpi) {
        f->dpi = dpi;
        apply_fonts(f);
        ImageList_SetIconSize(f->icons, row_height(f), row_height(f));
        build_toggles(f);
        refresh_rows(f);
    }
    int width = S(f, 380), margin = S(f, 12), heading = S(f, 32), status_h = S(f, 40);
    int show_list = f->count && (f->model->status == ASB_RW_FLYOUT_READY || f->model->status == ASB_RW_FLYOUT_UPGRADE);
    uint32_t rows = f->count < MAX_VISIBLE_ROWS ? f->count : MAX_VISIBLE_ROWS;
    int list_h = show_list ? list_rows_height(f, rows) + S(f, 2) : 0;
    int inner = width - 2 * margin, status_w = S(f, 120);
    int scroll = f->count > MAX_VISIBLE_ROWS ? GetSystemMetricsForDpi(SM_CXVSCROLL, f->dpi) : 0;
    ListView_SetColumnWidth(f->list, 0, inner - status_w - scroll - S(f, 2));
    ListView_SetColumnWidth(f->list, 1, status_w);
    int y = margin;
    MoveWindow(f->heading, margin, y, inner, heading - S(f, 4), FALSE);
    y += heading;
    ShowWindow(f->list, show_list ? SW_SHOWNA : SW_HIDE);
    MoveWindow(f->list, margin, y, inner, list_h, FALSE);
    if (show_list) y += list_h + S(f, 6);
    MoveWindow(f->status, margin, y, inner, status_h, FALSE);
    y += status_h + margin;
    int height = y;
    if (reposition) {
        RECT anchor = f->anchor, out;
        if (!f->have_anchor) {
            POINT pt;
            GetCursorPos(&pt);
            SetRect(&anchor, pt.x, pt.y, pt.x + 1, pt.y + 1);
        }
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
        asb_rw_flyout_place(&anchor, &mi.rcWork, &mi.rcMonitor, width, height, S(f, 12), &out);
        SetWindowPos(f->hwnd, HWND_TOPMOST, out.left, out.top, width, height, SWP_NOACTIVATE);
    } else {
        SetWindowPos(f->hwnd, NULL, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(f->hwnd, NULL, TRUE);
}

void asb_rw_flyout_hide(AsbRwFlyout *f)
{
    if (!f->visible) return;
    f->visible = 0;
    KillTimer(f->hwnd, TIMER_REQUESTS);
    ShowWindow(f->hwnd, SW_HIDE);
    if (f->cfg.closed) f->cfg.closed(f->cfg.ctx, 0);
}

static void hide_deactivated(AsbRwFlyout *f)
{
    if (!f->visible) return;
    f->visible = 0;
    KillTimer(f->hwnd, TIMER_REQUESTS);
    ShowWindow(f->hwnd, SW_HIDE);
    if (f->cfg.closed) f->cfg.closed(f->cfg.ctx, 1);
}

void asb_rw_flyout_show(AsbRwFlyout *f, const RECT *anchor)
{
    f->have_anchor = anchor != NULL;
    if (anchor) f->anchor = *anchor;
    /* Move onto the anchor's monitor first so the window takes that monitor's DPI. */
    POINT pt;
    if (anchor) { pt.x = anchor->left; pt.y = anchor->top; } else GetCursorPos(&pt);
    SetWindowPos(f->hwnd, HWND_TOPMOST, pt.x, pt.y, 10, 10, SWP_NOACTIVATE);
    layout(f, 1);
    f->visible = 1;
    ShowWindow(f->hwnd, SW_SHOW);
    SetForegroundWindow(f->hwnd);
    SetFocus(IsWindowEnabled(f->list) && IsWindowVisible(f->list) ? f->list : f->hwnd);
    SetTimer(f->hwnd, TIMER_REQUESTS, 1000, NULL);
}

int asb_rw_flyout_visible(const AsbRwFlyout *f) { return f && f->visible; }

void asb_rw_flyout_info(const AsbRwFlyout *f, AsbRwFlyoutInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->hwnd = f->hwnd;
    out->list = f->list;
    out->heading = f->heading;
    out->status = f->status;
    out->dpi = f->dpi;
    out->dark = (uint32_t)f->dark;
    out->rows = f->count;
    out->row_height = (uint32_t)row_height(f);
    out->visible_rows = f->count < MAX_VISIBLE_ROWS ? f->count : MAX_VISIBLE_ROWS;
    for (uint32_t i = 0; i < f->count; ++i) out->requested += (uint32_t)row_requested(f, i);
}

static LRESULT on_notify(AsbRwFlyout *f, NMHDR *nm)
{
    if (nm->hwndFrom != f->list) return 0;
    switch (nm->code) {
    case LVN_ITEMCHANGING: {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if (f->updating || !(lv->uChanged & LVIF_STATE)) return FALSE;
        if (((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) == 0) return FALSE;
        /* The toggle only asks; the acknowledged model decides what it shows. */
        PostMessageW(f->hwnd, WM_APP + 1, (WPARAM)lv->iItem, 0);
        return TRUE;
    }
    case NM_RETURN:
        request(f, ListView_GetNextItem(f->list, -1, LVNI_FOCUSED));
        return 0;
    case LVN_KEYDOWN:
        if (((NMLVKEYDOWN *)nm)->wVKey == VK_ESCAPE) asb_rw_flyout_hide(f);
        return 0;
    case NM_CUSTOMDRAW: {
        NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)nm;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            uint32_t i = (uint32_t)cd->nmcd.dwItemSpec;
            if (cd->iSubItem == 1 && i < f->count) {
                const AsbRwFlyoutItem *it = &f->model->items[i];
                int failed = !row_requested(f, i) && it->error && it->error != ASB_RW_ERR_STALE_ID &&
                             it->state != ASB_RW_STATE_SHARED && it->state != ASB_RW_STATE_PENDING;
                cd->clrText = failed ? f->err : f->sub;
            } else {
                cd->clrText = f->fg;
            }
            cd->clrTextBk = f->bg;
            return CDRF_NEWFONT;
        }
        default: return CDRF_DODEFAULT;
        }
    }
    default:
        return 0;
    }
}

static LRESULT CALLBACK flyout_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsbRwFlyout *f = (AsbRwFlyout *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!f) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) hide_deactivated(f);
        return 0;
    case WM_NOTIFY:
        return on_notify(f, (NMHDR *)lp);
    case WM_APP + 1:
        request(f, (int)wp);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { asb_rw_flyout_hide(f); return 0; }
        break;
    case WM_TIMER:
        if (wp == TIMER_REQUESTS) {
            uint64_t now = GetTickCount64();
            int changed = 0;
            for (uint32_t i = 0; i < f->count; ++i)
                if (f->rows[i].requested && now - f->rows[i].requested_at >= REQUEST_TTL_MS) { f->rows[i].requested = 0; changed = 1; }
            if (changed) refresh_rows(f);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, (HWND)lp == f->status ? f->sub : f->fg);
        SetBkColor((HDC)wp, f->bg);
        return (LRESULT)f->bg_brush;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, f->bg_brush);
        return 1;
    }
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
            apply_theme(f);
            build_toggles(f);
            refresh_rows(f);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        apply_theme(f);
        build_toggles(f);
        refresh_rows(f);
        return 0;
    case WM_DPICHANGED:
        layout(f, f->visible);
        return 0;
    case WM_CLOSE:
        asb_rw_flyout_hide(f);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

AsbRwFlyout *asb_rw_flyout_create(const AsbRwFlyoutConfig *config)
{
    static LONG registered;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!InterlockedExchange(&registered, 1)) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = flyout_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = FLYOUT_CLASS;
        RegisterClassExW(&wc);
    }
    AsbRwFlyout *f = (AsbRwFlyout *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->model = (AsbRwFlyoutModel *)calloc(1, sizeof(*f->model));
    f->rows = (FlyoutRow *)calloc(ASB_RW_FLYOUT_MAX_ITEMS, sizeof(*f->rows));
    if (config) f->cfg = *config;
    f->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, FLYOUT_CLASS, L"App Sandbox windows",
                              WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    if (!f->model || !f->rows || !f->hwnd) { asb_rw_flyout_destroy(f); return NULL; }
    f->heading = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_ENDELLIPSIS |
                                 SS_CENTERIMAGE, 0, 0, 10, 10, f->hwnd, NULL, inst, NULL);
    f->list = CreateWindowExW(0, WC_LISTVIEWW, L"Windows", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER |
                              LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, 0, 0, 10, 10, f->hwnd,
                              (HMENU)(INT_PTR)ID_LIST, inst, NULL);
    f->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, f->hwnd, NULL, inst,
                                NULL);
    if (!f->heading || !f->list || !f->status) { asb_rw_flyout_destroy(f); return NULL; }
    ListView_SetExtendedListViewStyle(f->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP);
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 200;
    ListView_InsertColumn(f->list, 0, &col);
    col.iSubItem = 1;
    col.cx = 100;
    ListView_InsertColumn(f->list, 1, &col);
    f->dpi = f->cfg.dpi ? f->cfg.dpi : GetDpiForWindow(f->hwnd);
    if (!f->dpi) f->dpi = 96;
    f->icons = ImageList_Create(row_height(f), row_height(f), ILC_COLOR32, 16, 16);
    ListView_SetImageList(f->list, f->icons, LVSIL_SMALL);
    SetWindowLongPtrW(f->hwnd, GWLP_USERDATA, (LONG_PTR)f);
    DWORD corner = 2;   /* DWMWCP_ROUND */
    DwmSetWindowAttribute(f->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    apply_theme(f);
    apply_fonts(f);
    build_toggles(f);
    asb_rw_flyout_update(f, f->model);
    return f;
}

void asb_rw_flyout_destroy(AsbRwFlyout *f)
{
    if (!f) return;
    if (f->hwnd) {
        SetWindowLongPtrW(f->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(f->hwnd);
    }
    if (f->icons) ImageList_Destroy(f->icons);
    if (f->toggles) ImageList_Destroy(f->toggles);
    if (f->font) DeleteObject(f->font);
    if (f->bold) DeleteObject(f->bold);
    if (f->bg_brush) DeleteObject(f->bg_brush);
    free(f->model);
    free(f->rows);
    free(f);
}
