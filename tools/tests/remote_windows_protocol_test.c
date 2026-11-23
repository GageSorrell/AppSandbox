/* Remote Windows protocol and session-contract tests (AppSandbox-eh6.2).
   Pure codec/negotiation/identity/inventory tests plus native Win32 window filtering. */
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../transport/asb_remote_windows.h"
#include "../transport/asb_automation.h"
#include "../transport/asb_transport.h"
#include "../agent/remote_windows_inventory.h"
#include "../../src/backend_win/hcs_services.h"
#include "../../src/core/protocol.h"

static int g_failures, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); ++g_checks; \
    if (_a != _b) { ++g_failures; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static uint8_t g_buf[ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD + 4096];
static AsbRwPayload g_msg, g_out;

static void set_text(uint16_t *dst, uint16_t *len, const wchar_t *s)
{
    *len = (uint16_t)wcslen(s);
    memcpy(dst, s, *len * sizeof(uint16_t));
}

static int text_is(const uint16_t *s, uint32_t len, const wchar_t *expect)
{
    return wcslen(expect) == len && !memcmp(s, expect, len * sizeof(uint16_t));
}

/* =================================================================== channels */

static void test_channels_and_services(void)
{
    /* Existing channel numbers stay put; Remote Windows uses 9/10/11. */
    CHECK_EQ(ASB_CH_AGENT, 1); CHECK_EQ(ASB_CH_DISPLAY, 2); CHECK_EQ(ASB_CH_INPUT, 3); CHECK_EQ(ASB_CH_AUDIO, 4);
    CHECK_EQ(ASB_CH_CLIPBOARD, 5); CHECK_EQ(ASB_CH_CLIPBOARD_READER, 6); CHECK_EQ(ASB_CH_SSH, 7);
    CHECK_EQ(ASB_CH_SCREENSHOT, 8);
    CHECK_EQ(ASB_CH_RW_CONTROL, 9); CHECK_EQ(ASB_CH_RW_FRAMES, 10); CHECK_EQ(ASB_CH_RW_FILES, 11);
    /* Existing input/display packet contracts are untouched. */
    CHECK_EQ(sizeof(InputPacket), 20); CHECK_EQ(INPUT_MAGIC, 0x4E495341u); CHECK_EQ(DISPLAY_GPU_VERSION, 3);
    CHECK_EQ(sizeof(AsbScreenshotRequest), 24); CHECK_EQ(ASB_SCREENSHOT_VERSION, 1);

    CHECK_EQ(asb_rw_type_channel(ASB_RW_HELLO), ASB_CH_RW_CONTROL);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_SURFACES), ASB_CH_RW_CONTROL);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_FRAME), ASB_CH_RW_FRAMES);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_FILE_DATA), ASB_CH_RW_FILES);
    CHECK_EQ(asb_rw_type_channel(999), 0);

    static wchar_t table[8192];
    /* Windows guests: 1..11 (7 = SSH, 8 = screenshot, 9-11 = Remote Windows). */
    CHECK_EQ(hcs_service_table_json(L"Windows", table, ARRAYSIZE(table)), 11);
    for (unsigned port = 1; port <= 11; ++port) {
        wchar_t guid[64]; hcs_service_guid_str(L"Windows", port, guid, 64);
        CHECK(wcsstr(table, guid) != NULL);
    }
    CHECK(wcsstr(table, L"a5b0cafe-0009-4000-8000-000000000001") && wcsstr(table, L"a5b0cafe-000a-4000-8000-000000000001") &&
          wcsstr(table, L"a5b0cafe-000b-4000-8000-000000000001"));
    CHECK(hcs_guest_has_remote_windows(L"windows") && !hcs_guest_has_remote_windows(L"Linux") &&
          !hcs_guest_has_remote_windows(L"") && !hcs_guest_has_remote_windows(NULL));
    /* Linux and legacy empty os_type: exactly the previous table (1-6, 8), no Remote Windows. */
    CHECK_EQ(hcs_service_table_json(L"Linux", table, ARRAYSIZE(table)), 7);
    CHECK(wcsstr(table, L"00000008-facb-11e6-bd58-64006a7986d3") && !wcsstr(table, L"00000007-facb") &&
          !wcsstr(table, L"00000009-facb") && !wcsstr(table, L"0000000b-facb"));
    CHECK_EQ(hcs_service_table_json(L"", table, ARRAYSIZE(table)), 7);
    CHECK(!wcsstr(table, L"a5b0cafe-0009"));
    /* Every entry keeps the historical descriptors. */
    int count = 0; const wchar_t *p = table;
    while ((p = wcsstr(p, L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\",\"AllowWildcardBinds\":true")) != NULL) { ++count; ++p; }
    CHECK_EQ(count, 7);
    /* Fits the hcs_vm.c buffer; too-small buffers fail cleanly. */
    CHECK(hcs_service_table_json(L"Windows", table, 4096) == 11 && wcslen(table) < 4096);
    CHECK_EQ(hcs_service_table_json(L"Windows", table, 300), -1);
    CHECK_EQ(table[0], 0);
}

/* =================================================================== codec */

static void fill_entry(AsbRwInventoryEntry *e, uint64_t id, const wchar_t *title, const wchar_t *app)
{
    memset(e, 0, sizeof(*e));
    e->id = id; e->pid = 4242; e->flags = ASB_RW_ENTRY_MINIMIZED; e->dpi = 120;
    e->rect.left = -10; e->rect.top = 5; e->rect.right = 800; e->rect.bottom = 600;
    set_text(e->title, &e->title_len, title);
    set_text(e->app, &e->app_len, app);
}

static size_t encode(uint16_t type, const AsbRwPayload *m, uint32_t req)
{
    size_t n = 0;
    uint32_t err = asb_rw_encode(type, req, m, g_buf, sizeof(g_buf), &n);
    CHECK_EQ(err, ASB_RW_OK);
    return n;
}

/* Every strict prefix of a complete message is "need more", never a success. */
static void check_prefixes(int channel, size_t total)
{
    AsbRwHeader h;
    for (size_t len = 0; len < total; len += (total > 4096 ? 97 : 1)) {
        uint32_t err = asb_rw_decode(channel, g_buf, len, &h, &g_out, NULL, NULL);
        if (err != ASB_RW_ERR_NEED_MORE) { CHECK_EQ(err, ASB_RW_ERR_NEED_MORE); break; }
    }
}

static void test_roundtrips(void)
{
    AsbRwHeader h;
    size_t n;

    memset(&g_msg, 0, sizeof(g_msg));
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_GUEST, ASB_RW_CAP_KNOWN_MASK, 0x010D00, 0x010D01, 0xA1B2C3D4u);
    n = encode(ASB_RW_HELLO, &g_msg, 7);
    CHECK_EQ(n, ASB_RW_HEADER_BYTES + 36);
    CHECK_EQ(g_buf[0], 'A'); CHECK_EQ(g_buf[1], 'S'); CHECK_EQ(g_buf[2], 'R'); CHECK_EQ(g_buf[3], 'W');
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(h.type == ASB_RW_HELLO && h.request_id == 7 && h.payload_len == 36);
    CHECK(!memcmp(&g_out.hello, &g_msg.hello, sizeof(g_msg.hello)));
    check_prefixes(ASB_CH_RW_CONTROL, n);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.snapshot.generation = 3;
    g_msg.snapshot.count = ASB_RW_MAX_INVENTORY;
    for (uint32_t i = 0; i < ASB_RW_MAX_INVENTORY; ++i) {
        wchar_t title[ASB_RW_MAX_TITLE + 1];
        for (int k = 0; k < (int)ASB_RW_MAX_TITLE; ++k) title[k] = (wchar_t)(0x4E00 + (k + (int)i) % 200);   /* CJK, max length */
        title[ASB_RW_MAX_TITLE] = 0;
        fill_entry(&g_msg.snapshot.entries[i], 0x100000000ull + i + 1, title, L"\u00C9diteur");
    }
    n = encode(ASB_RW_INVENTORY_SNAPSHOT, &g_msg, 1);
    CHECK(n <= ASB_RW_HEADER_BYTES + ASB_RW_MAX_CONTROL_PAYLOAD);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK_EQ(g_out.snapshot.count, ASB_RW_MAX_INVENTORY);
    CHECK(!memcmp(&g_out.snapshot.entries[255], &g_msg.snapshot.entries[255], sizeof(AsbRwInventoryEntry)));
    check_prefixes(ASB_CH_RW_CONTROL, n);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.event.generation = 9; g_msg.event.kind = ASB_RW_EVENT_CHANGED;
    fill_entry(&g_msg.event.entry, 0x55, L"Untitled - Notepad", L"Notepad");
    n = encode(ASB_RW_INVENTORY_EVENT, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.event.kind == ASB_RW_EVENT_CHANGED && text_is(g_out.event.entry.title, g_out.event.entry.title_len, L"Untitled - Notepad"));

    struct { uint16_t type; } control[] = { { ASB_RW_SELECT_REQUEST }, { ASB_RW_SELECT_RESULT }, { ASB_RW_GEOMETRY_COMMAND },
        { ASB_RW_GEOMETRY_ACK }, { ASB_RW_ACTIVATE_REQUEST }, { ASB_RW_ACTIVATE_ACK }, { ASB_RW_CLOSE_REQUEST },
        { ASB_RW_CLOSE_RESULT }, { ASB_RW_ERROR }, { ASB_RW_INVENTORY_REQUEST }, { ASB_RW_RESTORE_ALL } };
    for (size_t i = 0; i < sizeof(control) / sizeof(control[0]); ++i) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.select.id = 0x1234; g_msg.select.select = 1;                       /* union: id at offset 0 */
        if (control[i].type == ASB_RW_SELECT_RESULT) { g_msg.select_result.state = ASB_RW_STATE_SHARED; g_msg.select_result.error = 0; }
        if (control[i].type == ASB_RW_GEOMETRY_COMMAND || control[i].type == ASB_RW_GEOMETRY_ACK) {
            g_msg.geometry.serial = 4; g_msg.geometry.show_state = ASB_RW_SHOW_MAXIMIZED; g_msg.geometry.dpi = 144;
            g_msg.geometry.rect.left = 1920; g_msg.geometry.rect.right = 3000; g_msg.geometry.rect.bottom = 700;
        }
        if (control[i].type == ASB_RW_ACTIVATE_REQUEST || control[i].type == ASB_RW_ACTIVATE_ACK) {
            g_msg.activate.input_epoch = 77; g_msg.activate.foreground = 1;
        }
        if (control[i].type == ASB_RW_ERROR) { g_msg.error.code = ASB_RW_ERR_STALE_ID; g_msg.error.detail = 5; }
        n = encode(control[i].type, &g_msg, 11);
        uint32_t err = asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL);
        CHECK_EQ(err, ASB_RW_OK);
        CHECK_EQ(h.type, control[i].type);
        if (control[i].type == ASB_RW_GEOMETRY_ACK) CHECK(g_out.geometry.dpi == 144 && g_out.geometry.rect.right == 3000);
        if (control[i].type == ASB_RW_ACTIVATE_ACK) CHECK(g_out.activate.input_epoch == 77 && g_out.activate.foreground == 1);
        check_prefixes(ASB_CH_RW_CONTROL, n);
    }
    /* eh6.7: id 0 is the foreground release (request and ack), not a malformed message. */
    for (int k = 0; k < 2; ++k) {
        uint16_t type = k ? ASB_RW_ACTIVATE_ACK : ASB_RW_ACTIVATE_REQUEST;
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.activate.input_epoch = 9;
        g_msg.activate.foreground = (uint32_t)k;
        n = encode(type, &g_msg, 12);
        CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
        CHECK(h.type == type && !g_out.activate.id && g_out.activate.input_epoch == 9);
        g_msg.activate.foreground = 2;   /* still validated */
        n = encode(type, &g_msg, 13);
        CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    }

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.surfaces.root_id = 0x99; g_msg.surfaces.count = ASB_RW_MAX_SURFACES;
    for (uint32_t i = 0; i < ASB_RW_MAX_SURFACES; ++i) {
        AsbRwSurface *s = &g_msg.surfaces.surfaces[i];
        s->surface_id = 0x1000 + i; s->kind = ASB_RW_SURFACE_MENU; s->z = i;
        s->rect.left = 2000; s->rect.top = 900; s->rect.right = 2300; s->rect.bottom = 1400;   /* outside the root */
    }
    n = encode(ASB_RW_SURFACES, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.surfaces.count == ASB_RW_MAX_SURFACES && g_out.surfaces.surfaces[63].surface_id == 0x1000 + 63);

    /* Frames: header encoded, pixels follow the header out-of-band. */
    memset(&g_msg, 0, sizeof(g_msg));
    AsbRwFrameHeader *f = &g_msg.frame;
    f->surface_id = 0x77; f->root_id = 0x77; f->kind = ASB_RW_SURFACE_ROOT; f->generation = 1;
    f->frame_seq = 12; f->width = 3; f->height = 2; f->stride = 12; f->format = ASB_RW_FORMAT_BGRA8;
    f->rect.left = 10; f->rect.top = 20; f->rect.right = 13; f->rect.bottom = 22;
    f->transport = ASB_RW_TRANSPORT_CPU; f->data_bytes = 24;
    size_t hn = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &hn), ASB_RW_OK);
    for (int i = 0; i < 24; ++i) g_buf[hn + i] = (uint8_t)i;
    const uint8_t *bulk = NULL; size_t bulk_len = 0;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + 24, &h, &g_out, &bulk, &bulk_len), ASB_RW_OK);
    CHECK(bulk == g_buf + hn && bulk_len == 24 && bulk[23] == 23 && g_out.frame.frame_seq == 12);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, hn + 23, &h, &g_out, NULL, NULL), ASB_RW_ERR_NEED_MORE);
    check_prefixes(ASB_CH_RW_FRAMES, hn + 24);
    /* GPU frames name a pool slot and carry no pixels. */
    f->transport = ASB_RW_TRANSPORT_GPU; f->data_bytes = 0; f->stride = 0; f->slot = 2;
    n = encode(ASB_RW_FRAME, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, n, &h, &g_out, NULL, &bulk_len), ASB_RW_OK);
    CHECK(bulk_len == 0 && g_out.frame.slot == 2 && g_out.frame.rect.top == 20);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.frame_release.surface_id = 0x77; g_msg.frame_release.frame_seq = 12; g_msg.frame_release.slot = 2;
    g_msg.frame_release.generation = 1;
    n = encode(ASB_RW_FRAME_RELEASE, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);

    /* Files. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_begin.session_id = 5; g_msg.file_begin.target_id = 0x99; g_msg.file_begin.direction = ASB_RW_DIR_TO_GUEST;
    g_msg.file_begin.entry_count = 3; g_msg.file_begin.x = 2100; g_msg.file_begin.y = 300; g_msg.file_begin.total_bytes = 1 << 20;
    n = encode(ASB_RW_FILE_BEGIN, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_entry.session_id = 5; g_msg.file_entry.kind = ASB_RW_ENTRY_FILE; g_msg.file_entry.size = 10;
    set_text(g_msg.file_entry.rel, &g_msg.file_entry.rel_len, L"H\u00f4st folder \u2713\\sub\\b \u00fcn\u00ef.txt");
    n = encode(ASB_RW_FILE_ENTRY, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(text_is(g_out.file_entry.rel, g_out.file_entry.rel_len, L"H\u00f4st folder \u2713\\sub\\b \u00fcn\u00ef.txt"));
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_data.session_id = 5; g_msg.file_data.offset = 4096; g_msg.file_data.len = 5;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_DATA, 0, &g_msg, g_buf, sizeof(g_buf), &hn), ASB_RW_OK);
    memcpy(g_buf + hn, "hello", 5);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, hn + 5, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.file_data.len == 5 && !memcmp(g_out.file_data.data, "hello", 5) && g_out.file_data.offset == 4096);
    uint16_t statuses[] = { ASB_RW_FILE_END, ASB_RW_FILE_PROGRESS, ASB_RW_FILE_CANCEL, ASB_RW_FILE_RESULT };
    for (int i = 0; i < 4; ++i) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.file_status.session_id = 5; g_msg.file_status.status = ASB_RW_ERR_CANCELLED;
        g_msg.file_status.done_bytes = 10; g_msg.file_status.total_bytes = 20;
        n = encode(statuses[i], &g_msg, 0);
        CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    }

    /* Small output buffers report "buffer", not "malformed". */
    memset(&g_msg, 0, sizeof(g_msg));
    asb_rw_hello_init(&g_msg.hello, ASB_RW_ROLE_HOST, 0, 0, 0, 0);
    CHECK_EQ(asb_rw_encode(ASB_RW_HELLO, 0, &g_msg, g_buf, 20, &n), ASB_RW_ERR_BUFFER);
}

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_malformed(void)
{
    AsbRwHeader h;
    size_t n, total = 0;
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select.id = 5; g_msg.select.select = 1;
    n = encode(ASB_RW_SELECT_REQUEST, &g_msg, 1);

    g_buf[0] ^= 0xFF;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    g_buf[0] ^= 0xFF;
    g_buf[4] = 2;   /* future wire format */
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_CONTROL, g_buf, n, &total), ASB_RW_ERR_WIRE_VERSION);
    g_buf[4] = 1;
    g_buf[6] = 200;   /* unknown type */
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_UNKNOWN_TYPE);
    g_buf[6] = ASB_RW_SELECT_REQUEST;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_WRONG_CHANNEL);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_CONTROL, g_buf, 15, &total), ASB_RW_ERR_NEED_MORE);

    /* Oversized lengths are rejected from the header alone (before any allocation). */
    put_u32(g_buf + 12, ASB_RW_MAX_CONTROL_PAYLOAD + 1);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_CONTROL, g_buf, 16, &total), ASB_RW_ERR_TOO_LARGE);
    put_u32(g_buf + 12, 0xFFFFFFFFu);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_CONTROL, g_buf, 16, &total), ASB_RW_ERR_TOO_LARGE);

    /* Trailing byte inside a declared payload. */
    n = encode(ASB_RW_SELECT_REQUEST, &g_msg, 1);
    put_u32(g_buf + 12, 13);
    g_buf[n] = 0;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n + 1, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    /* Extra bytes beyond the declared message. */
    n = encode(ASB_RW_SELECT_REQUEST, &g_msg, 1);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n + 3, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    /* Invalid field values. */
    g_buf[16 + 8] = 2;   /* select must be 0 or 1 */
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    memset(&g_msg, 0, sizeof(g_msg));
    CHECK_EQ(asb_rw_encode(ASB_RW_SELECT_REQUEST, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_OK);   /* id 0 encodes... */
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);   /* ...but never decodes */

    /* Snapshot count above the limit; string longer than its field bound. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.snapshot.count = ASB_RW_MAX_INVENTORY + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_INVENTORY_SNAPSHOT, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.snapshot.count = 1; g_msg.snapshot.generation = 1;
    fill_entry(&g_msg.snapshot.entries[0], 9, L"T", L"A");
    n = encode(ASB_RW_INVENTORY_SNAPSHOT, &g_msg, 0);
    put_u32(g_buf + 20, ASB_RW_MAX_INVENTORY + 1);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_TOO_LARGE);
    put_u32(g_buf + 20, 1);
    size_t title_len_at = 16 + 8 + 8 + 4 + 4 + 16 + 4;
    g_buf[title_len_at] = 0xFF; g_buf[title_len_at + 1] = 0x7F;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);

    /* Inverted rectangles, bad enums, bad frame shapes, unsafe file paths. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.geometry.id = 3; g_msg.geometry.rect.left = 100; g_msg.geometry.rect.right = 50;
    n = encode(ASB_RW_GEOMETRY_COMMAND, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.select_result.id = 1; g_msg.select_result.state = 99;
    n = encode(ASB_RW_SELECT_RESULT, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.frame.surface_id = 1; g_msg.frame.root_id = 1; g_msg.frame.kind = ASB_RW_SURFACE_ROOT; g_msg.frame.generation = 1;
    g_msg.frame.width = 4; g_msg.frame.height = 4; g_msg.frame.stride = 16;
    g_msg.frame.format = ASB_RW_FORMAT_BGRA8; g_msg.frame.transport = ASB_RW_TRANSPORT_CPU; g_msg.frame.data_bytes = 63;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame.data_bytes = 64; g_msg.frame.width = ASB_RW_MAX_FRAME_WIDTH + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FRAME, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.frame.width = 4;
    n = encode(ASB_RW_FRAME, &g_msg, 0);
    put_u32(g_buf + 16 + 8 + 8 + 8 + 4 + 4 + 4 + 16 + 4 + 4, 0x7FFFFFFF);   /* stride */
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FRAMES, g_buf, n + 64, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_entry.session_id = 1; g_msg.file_entry.kind = ASB_RW_ENTRY_FILE;
    set_text(g_msg.file_entry.rel, &g_msg.file_entry.rel_len, L"..\\evil.txt");
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_ENTRY, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    set_text(g_msg.file_entry.rel, &g_msg.file_entry.rel_len, L"ok\\fine.txt");
    n = encode(ASB_RW_FILE_ENTRY, &g_msg, 0);
    g_buf[n - 2] = '.'; g_buf[n - 4] = '.'; g_buf[n - 6] = '\\';   /* ok\fine.t -> "ok\fine\.."-style tail */
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_FILE_REJECTED);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_data.session_id = 1; g_msg.file_data.len = ASB_RW_MAX_FILE_CHUNK + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_DATA, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_status.session_id = 1; g_msg.file_status.done_bytes = 2; g_msg.file_status.total_bytes = 1;
    n = encode(ASB_RW_FILE_PROGRESS, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);

    /* Deterministic mutation fuzz: never crashes; every accepted message re-encodes identically. */
    static uint8_t seed_msg[4096], again[4096];
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.event.generation = 2; g_msg.event.kind = ASB_RW_EVENT_ADDED;
    fill_entry(&g_msg.event.entry, 0x42, L"Fuzz title", L"App");
    size_t seed_len = encode(ASB_RW_INVENTORY_EVENT, &g_msg, 3);
    memcpy(seed_msg, g_buf, seed_len);
    uint32_t state = 12345, accepted = 0, round_trips = 0;
    for (int iter = 0; iter < 200000; ++iter) {
        memcpy(g_buf, seed_msg, seed_len);
        int flips = 1 + iter % 4;
        for (int k = 0; k < flips; ++k) {
            state = state * 1103515245u + 12345u;
            g_buf[(state >> 8) % seed_len] = (uint8_t)(state >> 20);
        }
        state = state * 1103515245u + 12345u;
        size_t len = (iter % 7 == 0) ? (state >> 8) % (seed_len + 8) : seed_len;
        uint32_t err = asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, len, &h, &g_out, NULL, NULL);
        if (err != ASB_RW_OK) continue;
        ++accepted;
        size_t m = 0;
        if (asb_rw_encode(h.type, h.request_id, &g_out, again, sizeof(again), &m) == ASB_RW_OK && m == len &&
            !memcmp(again, g_buf, len))
            ++round_trips;
    }
    CHECK(accepted > 0);
    CHECK_EQ(round_trips, accepted);
}

/* =================================================================== negotiation */

static void test_negotiation(void)
{
    AsbRwHello host, guest;
    asb_rw_hello_init(&host, ASB_RW_ROLE_HOST, ASB_RW_CAP_KNOWN_MASK, 0x000D01, 0, 0);
    asb_rw_hello_init(&guest, ASB_RW_ROLE_GUEST, ASB_RW_CAP_REQUIRED | ASB_RW_CAP_FILES_TO_GUEST, 0x000D01, 0x000D01, 0xBEEF);
    AsbRwNegotiated a = asb_rw_negotiate(&host, &guest), b = asb_rw_negotiate(&guest, &host);
    CHECK(!memcmp(&a, &b, sizeof(a)));   /* both endpoints agree */
    CHECK_EQ(a.error, ASB_RW_OK); CHECK_EQ(a.version, 1); CHECK_EQ(a.session_tag, 0xBEEF);
    CHECK_EQ(a.capabilities, ASB_RW_CAP_REQUIRED | ASB_RW_CAP_FILES_TO_GUEST);

    /* Older guest helper: no private display / popups => upgrade required, never reduced sharing. */
    guest.capabilities = ASB_RW_CAP_INVENTORY | ASB_RW_CAP_SELECTION | ASB_RW_CAP_FRAMES_CPU;
    a = asb_rw_negotiate(&host, &guest); b = asb_rw_negotiate(&guest, &host);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK_EQ(a.error, ASB_RW_ERR_UPGRADE_REQUIRED);
    CHECK_EQ(a.missing_required, ASB_RW_CAP_PRIVATE_DISPLAY | ASB_RW_CAP_POPUPS | ASB_RW_CAP_GEOMETRY | ASB_RW_CAP_ACTIVATION);
    /* Unknown future capability bits are ignored. */
    guest.capabilities = ASB_RW_CAP_REQUIRED | 0x80000000u;
    host.capabilities = ASB_RW_CAP_KNOWN_MASK | 0x80000000u;
    CHECK_EQ(asb_rw_negotiate(&host, &guest).capabilities, ASB_RW_CAP_REQUIRED);
    host.capabilities = ASB_RW_CAP_KNOWN_MASK;

    /* Versions: older, newer-only, overlapping, invalid ranges. */
    guest.min_version = 0; guest.max_version = 0;
    CHECK_EQ(asb_rw_negotiate(&host, &guest).error, ASB_RW_ERR_UNSUPPORTED_VERSION);
    guest.min_version = 2; guest.max_version = 3;
    a = asb_rw_negotiate(&host, &guest); b = asb_rw_negotiate(&guest, &host);
    CHECK(a.error == ASB_RW_ERR_UNSUPPORTED_VERSION && b.error == ASB_RW_ERR_UNSUPPORTED_VERSION);
    guest.min_version = 1; guest.max_version = 3;
    CHECK_EQ(asb_rw_negotiate(&host, &guest).version, 1);
    CHECK_EQ(asb_rw_negotiate(&host, &guest).error, ASB_RW_OK);
    guest.min_version = 3; guest.max_version = 1;
    CHECK_EQ(asb_rw_negotiate(&host, &guest).error, ASB_RW_ERR_UNSUPPORTED_VERSION);
    guest.min_version = 1; guest.max_version = 1;

    /* Role/session/limit sanity. */
    AsbRwHello other = host;
    CHECK_EQ(asb_rw_negotiate(&host, &other).error, ASB_RW_ERR_MALFORMED);
    guest.session_tag = 0;
    CHECK_EQ(asb_rw_negotiate(&host, &guest).error, ASB_RW_ERR_MALFORMED);
    guest.session_tag = 7; guest.max_file_chunk = 4096; guest.max_frame_bytes = 1 << 20;
    a = asb_rw_negotiate(&host, &guest);
    CHECK(a.error == ASB_RW_OK && a.max_file_chunk == 4096 && a.max_frame_bytes == (1u << 20) &&
          a.max_control_payload == ASB_RW_MAX_CONTROL_PAYLOAD);
    guest.max_control_payload = 100;
    CHECK_EQ(asb_rw_negotiate(&host, &guest).error, ASB_RW_ERR_MALFORMED);

    /* An old-format HELLO from a hypothetical v0 peer still decodes (so the error is precise). */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.hello = guest; g_msg.hello.min_version = 0; g_msg.hello.max_version = 0;
    size_t n = encode(ASB_RW_HELLO, &g_msg, 0);
    AsbRwHeader h;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK_EQ(asb_rw_negotiate(&host, &g_out.hello).error, ASB_RW_ERR_UNSUPPORTED_VERSION);
}

static void test_errors(void)
{
    for (uint32_t i = 0; i < ASB_RW_ERR_COUNT; ++i) {
        CHECK(asb_rw_error_name(i)[0] && asb_rw_error_text(i)[0]);
        for (uint32_t j = i + 1; j < ASB_RW_ERR_COUNT; ++j) CHECK(strcmp(asb_rw_error_name(i), asb_rw_error_name(j)));
    }
    CHECK(!strcmp(asb_rw_error_name(ASB_RW_ERR_STALE_ID), "stale_id"));
    CHECK(!strcmp(asb_rw_error_name(ASB_RW_ERR_COUNT + 5), "unknown"));
    CHECK(strstr(asb_rw_error_text(ASB_RW_ERR_UPGRADE_REQUIRED), "Update the guest components") != NULL);
}

/* =================================================================== identity */

typedef struct { AsbRwIdentity live[4]; int count; } FakeWindows;
static int fake_probe(void *ctx, uint64_t hwnd, AsbRwIdentity *live)
{
    FakeWindows *w = (FakeWindows *)ctx;
    for (int i = 0; i < w->count; ++i) if (w->live[i].hwnd == hwnd) { *live = w->live[i]; return 1; }
    return 0;
}

static AsbRwIdTable g_ids;

static void test_identity(void)
{
    AsbRwIdentity a = { 0x1001, 500, 600, 111 }, b = { 0x1002, 500, 600, 111 };
    FakeWindows fw = { { { 0x1001, 500, 600, 111 }, { 0x1002, 500, 600, 111 } }, 2 };
    uint64_t ida, ida2, idb, out;
    AsbRwIdentity got;
    asb_rw_ids_init(&g_ids, 0x11111111u);
    CHECK_EQ(asb_rw_ids_assign(&g_ids, &a, &ida), ASB_RW_OK);
    CHECK_EQ(asb_rw_ids_assign(&g_ids, &a, &ida2), ASB_RW_OK);
    CHECK(ida == ida2 && ida != 0);                      /* stable per identity */
    CHECK_EQ(asb_rw_ids_assign(&g_ids, &b, &idb), ASB_RW_OK);
    CHECK(idb != ida);
    CHECK_EQ(asb_rw_id_session_tag(ida), 0x11111111u);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, ida, fake_probe, &fw, &got), ASB_RW_OK);
    CHECK(got.hwnd == 0x1001 && got.process_start == 111);

    /* HWND value reused by another process: stale, and the old binding is dropped. */
    fw.live[0].pid = 900; fw.live[0].process_start = 222;
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, ida, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, ida, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);
    AsbRwIdentity reused = fw.live[0];
    CHECK_EQ(asb_rw_ids_assign(&g_ids, &reused, &out), ASB_RW_OK);
    CHECK(out != ida);                                   /* new window gets a new ID, even in the same slot */
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, out, fake_probe, &fw, &got), ASB_RW_OK);
    /* Same PID but the process restarted (PID reuse): stale. */
    fw.live[1].process_start = 999;
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, idb, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);
    /* Window destroyed. */
    fw.count = 0;
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, out, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);
    /* Same HWND, different thread => different identity. */
    AsbRwIdentity t1 = { 0x2000, 1, 2, 3 }, t2 = { 0x2000, 1, 9, 3 };
    uint64_t i1, i2;
    asb_rw_ids_assign(&g_ids, &t1, &i1); asb_rw_ids_assign(&g_ids, &t2, &i2);
    CHECK(i1 != i2);
    /* Release, double release, forged IDs. */
    CHECK_EQ(asb_rw_ids_release(&g_ids, i1), ASB_RW_OK);
    CHECK_EQ(asb_rw_ids_release(&g_ids, i1), ASB_RW_ERR_STALE_ID);
    CHECK_EQ(asb_rw_ids_release(&g_ids, 0), ASB_RW_ERR_STALE_ID);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, i2 + 1, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, (i2 & 0xFFFFFFFFull) | (0xFFFull << 20), fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);

    /* Reconnect: a new generation invalidates every previous ID, even for the same window. */
    fw.count = 1; fw.live[0] = a;
    asb_rw_ids_init(&g_ids, 0x11111111u);
    asb_rw_ids_assign(&g_ids, &a, &ida);
    asb_rw_ids_init(&g_ids, 0x22222222u);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, ida, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);
    asb_rw_ids_assign(&g_ids, &a, &ida2);
    CHECK(ida2 != ida && asb_rw_id_session_tag(ida2) == 0x22222222u);
    /* Even if a session tag repeats, slot serials keep old IDs dead. */
    asb_rw_ids_init(&g_ids, 0x22222222u);
    asb_rw_ids_assign(&g_ids, &a, &out);
    CHECK(out != ida2);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, ida2, fake_probe, &fw, &got), ASB_RW_ERR_STALE_ID);

    /* Bounded table. */
    asb_rw_ids_init(&g_ids, 5);
    uint32_t err = ASB_RW_OK;
    for (uint32_t i = 0; i < ASB_RW_MAX_WINDOWS && !err; ++i) {
        AsbRwIdentity w = { 0x10000 + i, 1, 1, 1 };
        err = asb_rw_ids_assign(&g_ids, &w, &out);
    }
    CHECK_EQ(err, ASB_RW_OK);
    AsbRwIdentity extra = { 0x90000, 1, 1, 1 };
    CHECK_EQ(asb_rw_ids_assign(&g_ids, &extra, &out), ASB_RW_ERR_LIMIT);
    asb_rw_ids_init(&g_ids, 0);
    CHECK_EQ(asb_rw_ids_assign(&g_ids, &a, &out), ASB_RW_ERR_NOT_ELIGIBLE);   /* no session yet */
}

/* =================================================================== filtering */

static AsbRwWindowFacts make_window(uint64_t hwnd, uint32_t pid, const wchar_t *title, const wchar_t *cls, const wchar_t *app)
{
    AsbRwWindowFacts w;
    memset(&w, 0, sizeof(w));
    w.identity.hwnd = hwnd; w.identity.pid = pid; w.identity.thread_id = pid + 1; w.identity.process_start = 1000 + pid;
    w.session_id = w.active_session_id = 1;
    w.style = 0x14CF0000u;   /* WS_OVERLAPPEDWINDOW | WS_VISIBLE */
    w.visible = 1;
    w.dpi = 96;
    w.rect.right = 800; w.rect.bottom = 600;
    set_text(w.title, &w.title_len, title);
    set_text(w.class_name, &w.class_len, cls);
    set_text(w.app, &w.app_len, app);
    return w;
}

static void test_filtering(void)
{
    AsbRwWindowFacts w = make_window(1, 10, L"Doc - Notepad", L"Notepad", L"Notepad");
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_ELIGIBLE);
    w.minimized = 1;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_ELIGIBLE);              /* minimized is listed */
    w.visible = 0;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_HIDDEN);
    w = make_window(1, 10, L"T", L"C", L"A"); w.cloaked = 1;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_CLOAKED);
    w = make_window(1, 10, L"Save As", L"#32770", L"A"); w.owner = 7;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_OWNED);      /* dialogs are not independent roots */
    w = make_window(1, 10, L"Tool", L"C", L"A"); w.ex_style = ASB_RW_WS_EX_TOOLWINDOW;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_TOOL_WINDOW);
    w.ex_style |= ASB_RW_WS_EX_APPWINDOW;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_ELIGIBLE);
    w = make_window(1, 10, L"Popup", L"C", L"A"); w.style = 0x90000000u;   /* WS_POPUP|WS_VISIBLE, no caption */
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_NOT_APP_WINDOW);
    w.ex_style = ASB_RW_WS_EX_APPWINDOW;                                    /* custom-chrome apps */
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_ELIGIBLE);
    w = make_window(1, 10, L"NoActivate", L"C", L"A"); w.ex_style = ASB_RW_WS_EX_NOACTIVATE;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_NOT_APP_WINDOW);
    w = make_window(1, 10, L"", L"C", L"A");
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_UNTITLED);
    static const wchar_t *shell[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow" };
    for (size_t i = 0; i < 6; ++i) {
        w = make_window(1, 10, L"Shell", shell[i], L"Windows Explorer");
        CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_SHELL);
    }
    w = make_window(1, 10, L"Folder - File Explorer", L"CabinetWClass", L"Windows Explorer");
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_ELIGIBLE);              /* Explorer windows are apps */
    w.elevation = ASB_RW_ELEVATION_YES;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_ELEVATED);
    w.elevation = ASB_RW_ELEVATION_UNKNOWN;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_ELEVATION_UNKNOWN);
    w.elevation = ASB_RW_ELEVATION_NO; w.is_helper = 1;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_HELPER);
    w.is_helper = 0; w.session_id = 2;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_OTHER_SESSION);
    /* A selected root stays listed although sharing re-owned it and hid its shell entry... */
    w = make_window(1, 10, L"Shared", L"Notepad", L"Notepad");
    w.selected = 1; w.owner = 0x5555; w.ex_style = ASB_RW_WS_EX_TOOLWINDOW;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_ELIGIBLE);
    /* ...but never if it became elevated-unknown or left the session. */
    w.elevation = ASB_RW_ELEVATION_YES;
    CHECK_EQ(asb_rw_root_eligibility(&w), ASB_RW_INELIGIBLE_ELEVATED);
}

static void test_association(void)
{
    const uint64_t root = 0x100;
    AsbRwWindowFacts d = make_window(0x200, 10, L"Save As", L"#32770", L"Notepad");
    d.owner = root; d.owner_chain[0] = root; d.owner_depth = 1;
    CHECK_EQ(asb_rw_association(&d, root), ASB_RW_SURFACE_OWNED);
    AsbRwWindowFacts nested = make_window(0x300, 10, L"Confirm", L"#32770", L"Notepad");
    nested.owner = 0x200; nested.owner_chain[0] = 0x200; nested.owner_chain[1] = root; nested.owner_depth = 2;
    CHECK_EQ(asb_rw_association(&nested, root), ASB_RW_SURFACE_OWNED);
    AsbRwWindowFacts tip = make_window(0x400, 10, L"", L"tooltips_class32", L"Notepad");
    tip.style = 0x84000000u; tip.owner = 0x200; tip.owner_chain[0] = 0x200; tip.owner_chain[1] = root; tip.owner_depth = 2;
    CHECK_EQ(asb_rw_association(&tip, root), ASB_RW_SURFACE_TOOLTIP);
    AsbRwWindowFacts xaml = make_window(0x500, 10, L"", L"Microsoft.UI.Content.PopupWindowSiteBridge", L"Notepad");
    xaml.style = 0x94000000u; xaml.owner = root; xaml.owner_chain[0] = root; xaml.owner_depth = 1;
    CHECK_EQ(asb_rw_association(&xaml, root), ASB_RW_SURFACE_POPUP);
    AsbRwWindowFacts menu = make_window(0x600, 10, L"", L"#32768", L"Notepad");
    menu.style = 0x94000000u; menu.menu_owner = root; menu.owner_chain[0] = root; menu.owner_depth = 1;
    CHECK_EQ(asb_rw_association(&menu, root), ASB_RW_SURFACE_MENU);
    /* Same process, no ownership link: NOT associated (another top-level window of a multi-window app). */
    AsbRwWindowFacts sibling = make_window(0x700, 10, L"Other - Notepad", L"Notepad", L"Notepad");
    CHECK_EQ(asb_rw_association(&sibling, root), 0);
    AsbRwWindowFacts shadow = make_window(0x800, 10, L"", L"SysShadow", L"Notepad");
    CHECK_EQ(asb_rw_association(&shadow, root), 0);
    AsbRwWindowFacts foreign = make_window(0x900, 99, L"", L"#32768", L"Other");
    foreign.menu_owner = 0x999; foreign.owner_chain[0] = 0x999; foreign.owner_depth = 1;
    CHECK_EQ(asb_rw_association(&foreign, root), 0);
    CHECK_EQ(asb_rw_association(&d, 0), 0);
    AsbRwWindowFacts self = make_window(root, 10, L"Root", L"Notepad", L"Notepad");
    CHECK_EQ(asb_rw_association(&self, root), 0);
    /* eh6.9: a shared File Explorer window shares explorer.exe with the shell. The private
       display's taskbar and the desktop window spanning both displays are never its popups;
       an ordinary unowned popup of the same process inside the private display still is. */
    AsbRwRect priv = { 2560, 0, 5120, 1080 };
    AsbRwWindowFacts xroot = make_window(0xB00, 42, L"eh69-src - File Explorer", L"CabinetWClass", L"File Explorer");
    xroot.rect.left = 3000; xroot.rect.top = 100; xroot.rect.right = 3800; xroot.rect.bottom = 700;
    AsbRwWindowFacts tray2 = make_window(0xB01, 42, L"", L"Shell_SecondaryTrayWnd", L"File Explorer");
    tray2.style = 0x96000000u; tray2.ex_style = ASB_RW_WS_EX_TOOLWINDOW;
    tray2.rect.left = 2560; tray2.rect.top = 1020; tray2.rect.right = 5120; tray2.rect.bottom = 1080;
    CHECK_EQ(asb_rw_surface_association(&tray2, &xroot, &priv), 0);
    AsbRwWindowFacts desk = make_window(0xB02, 42, L"", L"WorkerW", L"File Explorer");
    desk.style = 0x96000000u;
    desk.rect.left = -2560; desk.rect.top = -80; desk.rect.right = 5120; desk.rect.bottom = 1360;
    CHECK_EQ(asb_rw_surface_association(&desk, &xroot, &priv), 0);
    AsbRwWindowFacts spanning = make_window(0xB03, 42, L"", L"SomeOverlay", L"File Explorer");
    spanning.style = 0x94000000u;
    spanning.rect.left = 0; spanning.rect.top = 0; spanning.rect.right = 5120; spanning.rect.bottom = 1080;
    CHECK_EQ(asb_rw_surface_association(&spanning, &xroot, &priv), 0);
    AsbRwWindowFacts xpop = make_window(0xB04, 42, L"", L"Microsoft.UI.Content.PopupWindowSiteBridge", L"File Explorer");
    xpop.style = 0x94000000u;
    xpop.rect.left = 3100; xpop.rect.top = 200; xpop.rect.right = 3400; xpop.rect.bottom = 500;
    CHECK_EQ(asb_rw_surface_association(&xpop, &xroot, &priv), ASB_RW_SURFACE_POPUP);
    xpop.rect.left = 2500;   /* reaches off the private display */
    CHECK_EQ(asb_rw_surface_association(&xpop, &xroot, &priv), 0);
    /* Deep chains are bounded. */
    AsbRwWindowFacts deep = make_window(0xA00, 10, L"Deep", L"#32770", L"Notepad");
    deep.owner = 1; deep.owner_depth = 1000;
    for (uint32_t i = 0; i < ASB_RW_MAX_OWNER_DEPTH; ++i) deep.owner_chain[i] = 1 + i;
    CHECK_EQ(asb_rw_association(&deep, root), 0);
}

/* =================================================================== inventory */

static AsbRwInventory g_inv;
static AsbRwInventoryEvent g_events[64];
static AsbRwWindowFacts g_windows[300];

static void test_inventory(void)
{
    uint32_t n_events = 0, dropped = 0;
    asb_rw_ids_init(&g_ids, 0xABCD);
    asb_rw_inventory_init(&g_inv);
    g_windows[0] = make_window(1, 10, L"Untitled - Notepad", L"Notepad", L"Notepad");
    g_windows[1] = make_window(2, 11, L"Untitled - Notepad", L"Notepad", L"Notepad");
    g_windows[2] = make_window(3, 12, L"Untitled - Notepad", L"Chrome_WidgetWin_1", L"Visual Studio Code");
    g_windows[3] = make_window(4, 13, L"Admin", L"ConsoleWindowClass", L"Windows PowerShell");
    g_windows[3].elevation = ASB_RW_ELEVATION_YES;
    g_windows[4] = make_window(5, 10, L"Save As", L"#32770", L"Notepad");
    g_windows[4].owner = 1;
    g_windows[5] = make_window(6, 14, L"Task Switching", L"XamlExplorerHostIslandWindow", L"Windows Explorer");
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_windows, 6, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    CHECK_EQ(g_inv.count, 3);
    CHECK_EQ(n_events, 3);
    CHECK_EQ(g_inv.generation, 1);
    for (uint32_t i = 0; i < n_events; ++i) CHECK(g_events[i].kind == ASB_RW_EVENT_ADDED && g_events[i].generation == 1);
    for (uint32_t i = 0; i < 3; ++i) CHECK(g_inv.entries[i].flags & ASB_RW_ENTRY_DUPLICATE_TITLE);

    /* Duplicate titles are distinguished by application name, then by ordinal. */
    uint16_t label[400];
    uint32_t len = asb_rw_entry_label(g_inv.entries, g_inv.count, 0, label, 400);
    CHECK(text_is(label, len, L"Untitled - Notepad \u2014 Notepad (1)"));
    len = asb_rw_entry_label(g_inv.entries, g_inv.count, 1, label, 400);
    CHECK(text_is(label, len, L"Untitled - Notepad \u2014 Notepad (2)"));
    len = asb_rw_entry_label(g_inv.entries, g_inv.count, 2, label, 400);
    CHECK(text_is(label, len, L"Untitled - Notepad \u2014 Visual Studio Code"));
    len = asb_rw_entry_label(g_inv.entries, g_inv.count, 2, label, 10);
    CHECK(len == 9 && label[9] == 0);                                     /* truncation is bounded */
    uint64_t first_id = g_inv.entries[0].id;

    /* No change => no events, no generation bump. */
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_windows, 6, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    CHECK(n_events == 0 && g_inv.generation == 1 && g_inv.entries[0].id == first_id);
    /* Moving a window alone is not an inventory change (geometry acks carry it). */
    g_windows[0].rect.left = 50;
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_windows, 6, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    CHECK_EQ(n_events, 0);

    /* Title change and minimize => CHANGED; the duplicate flag clears for the renamed window. */
    set_text(g_windows[1].title, &g_windows[1].title_len, L"notes.txt - Notepad");
    g_windows[2].minimized = 1;
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_windows, 6, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    /* Entry 0 stays a duplicate of entry 2, so only the renamed and the minimized windows change. */
    CHECK_EQ(n_events, 2);
    CHECK_EQ(g_inv.generation, 2);
    int changed = 0;
    for (uint32_t i = 0; i < n_events; ++i) changed += g_events[i].kind == ASB_RW_EVENT_CHANGED;
    CHECK_EQ(changed, 2);
    CHECK(!(g_inv.entries[1].flags & ASB_RW_ENTRY_DUPLICATE_TITLE));
    CHECK(g_inv.entries[2].flags & ASB_RW_ENTRY_MINIMIZED);

    /* Selecting re-owns the root and hides its shell presence; it stays listed and flags SELECTED. */
    g_windows[0].selected = 1; g_windows[0].owner = 0x7777; g_windows[0].ex_style = ASB_RW_WS_EX_TOOLWINDOW;
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_windows, 6, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    CHECK(n_events == 1 && g_events[0].kind == ASB_RW_EVENT_CHANGED && g_events[0].entry.id == first_id &&
          (g_events[0].entry.flags & ASB_RW_ENTRY_SELECTED));

    /* Window closes => REMOVED, ID released (stale from now on). */
    AsbRwWindowFacts survivors[2] = { g_windows[1], g_windows[2] };
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, survivors, 2, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    /* The closed window is REMOVED; its former twin loses the duplicate flag (CHANGED). */
    CHECK_EQ(n_events, 2);
    int removed_first = 0, twin_changed = 0;
    for (uint32_t i = 0; i < n_events; ++i) {
        removed_first += g_events[i].kind == ASB_RW_EVENT_REMOVED && g_events[i].entry.id == first_id;
        twin_changed += g_events[i].kind == ASB_RW_EVENT_CHANGED && !(g_events[i].entry.flags & ASB_RW_ENTRY_DUPLICATE_TITLE);
    }
    CHECK(removed_first == 1 && twin_changed == 1);
    CHECK_EQ(asb_rw_ids_release(&g_ids, first_id), ASB_RW_ERR_STALE_ID);
    /* The same HWND value reappearing for a new process is a different window with a new ID. */
    AsbRwWindowFacts back[3] = { g_windows[0], g_windows[1], g_windows[2] };
    back[0].selected = 0; back[0].owner = 0; back[0].ex_style = 0;
    back[0].identity.pid = 77; back[0].identity.process_start = 4242;
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, back, 3, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    int added_new = 0;
    for (uint32_t i = 0; i < n_events; ++i)
        added_new += g_events[i].kind == ASB_RW_EVENT_ADDED && g_events[i].entry.id != first_id && g_events[i].entry.pid == 77;
    CHECK_EQ(added_new, 1);

    /* Bounded events and inventory size. */
    for (uint32_t i = 0; i < 300; ++i) {
        wchar_t t[32]; swprintf_s(t, 32, L"Window %u", i);
        g_windows[i] = make_window(0x5000 + i, 2000 + i, t, L"App", L"App");
    }
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_windows, 300, g_events, 16, &n_events, &dropped), ASB_RW_ERR_LIMIT);
    CHECK_EQ(g_inv.count, ASB_RW_MAX_INVENTORY);
    CHECK(n_events == 16 && dropped > 0);   /* dropped events => caller sends a snapshot */
    static AsbRwInventorySnapshot snap;
    asb_rw_inventory_snapshot(&g_inv, &snap);
    CHECK(snap.count == ASB_RW_MAX_INVENTORY && snap.generation == g_inv.generation);
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.snapshot = snap;
    size_t n = encode(ASB_RW_INVENTORY_SNAPSHOT, &g_msg, 0);
    CHECK(n > ASB_RW_HEADER_BYTES);
}

/* =================================================================== paths */

static void test_paths(void)
{
    static const wchar_t *good[] = { L"a.txt", L"folder\\sub\\file.bin", L"H\u00f4st t\u00ebst \u2713\\x", L"a/b",
        L".hidden", L"console\\x", L"COM10", L"name.with.dots.txt" };
    static const wchar_t *bad[] = { L"", L"\\abs", L"/abs", L"C:\\x", L"C:x", L"a:stream", L"..", L".", L"a\\..\\b",
        L"a\\.\\b", L"a\\\\b", L"a\\", L"trail.", L"trail ", L"x\\CON", L"con.d\\x", L"nul.txt", L"COM1", L"lpt9.log",
        L"a<b", L"a|b", L"a?b", L"a*b", L"q\"q", L"ctl\x01", L"CONIN$" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
        if (!asb_rw_rel_path_safe((const uint16_t *)good[i], (uint32_t)wcslen(good[i]))) {
            ++g_failures; printf("FAIL safe path rejected: %ls\n", good[i]);
        }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        if (asb_rw_rel_path_safe((const uint16_t *)bad[i], (uint32_t)wcslen(bad[i]))) {
            ++g_failures; printf("FAIL unsafe path accepted: %ls\n", bad[i]);
        }
    static uint16_t long_path[ASB_RW_MAX_REL_PATH + 1];
    for (int i = 0; i <= (int)ASB_RW_MAX_REL_PATH; ++i) long_path[i] = 'a';
    CHECK(asb_rw_rel_path_safe(long_path, ASB_RW_MAX_REL_PATH));
    CHECK(!asb_rw_rel_path_safe(long_path, ASB_RW_MAX_REL_PATH + 1));
    CHECK(!asb_rw_rel_path_safe(NULL, 3));
}

/* =================================================================== native Win32 */

static LRESULT CALLBACK test_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }

static HWND make_native(const wchar_t *title, DWORD style, DWORD ex, HWND owner)
{
    /* Far off-screen so the test never covers the user's desktop or takes focus. */
    return CreateWindowExW(ex, L"RwProtocolTest", title, style, -30000, -30000, 300, 200, owner, NULL,
                           GetModuleHandleW(NULL), NULL);
}

static const AsbRwWindowFacts *find_facts(const AsbRwWindowFacts *list, uint32_t n, HWND h)
{
    for (uint32_t i = 0; i < n; ++i) if (list[i].identity.hwnd == (uint64_t)(ULONG_PTR)h) return &list[i];
    return NULL;
}

static AsbRwWindowFacts g_native[2048];

static void test_native_windows(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = test_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"RwProtocolTest";
    RegisterClassW(&wc);
    const DWORD app = WS_OVERLAPPEDWINDOW;
    HWND a = make_native(L"RW Native Test", app, 0, NULL);
    HWND dup = make_native(L"RW Native Test", app, 0, NULL);
    HWND tool = make_native(L"RW Tool", app, WS_EX_TOOLWINDOW, NULL);
    HWND owned = make_native(L"RW Owned Dialog", app, 0, a);
    HWND hidden = make_native(L"RW Hidden", app, 0, NULL);
    HWND minimized = make_native(L"RW Minimized", app, 0, NULL);
    HWND cloaked = make_native(L"RW Cloaked", app, 0, NULL);
    HWND popup = make_native(L"RW Popup", WS_POPUP, 0, NULL);
    HWND app_popup = make_native(L"RW Custom Chrome", WS_POPUP, WS_EX_APPWINDOW, NULL);
    HWND untitled = make_native(L"", app, 0, NULL);
    HWND unowned_sibling_popup = make_native(L"", WS_POPUP, WS_EX_TOOLWINDOW, NULL);
    HWND shown[] = { a, dup, tool, owned, cloaked, popup, app_popup, untitled, unowned_sibling_popup };
    for (size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); ++i) ShowWindow(shown[i], SW_SHOWNOACTIVATE);
    ShowWindow(minimized, SW_SHOWMINNOACTIVE);
    BOOL cloak = TRUE;
    HRESULT cloak_hr = DwmSetWindowAttribute(cloaked, DWMWA_CLOAK, &cloak, sizeof(cloak));

    uint32_t count = 0;
    CHECK_EQ(asb_rw_enumerate_windows(g_native, 2048, 0, &count), ASB_RW_OK);
    const AsbRwWindowFacts *f;
    CHECK(((f = find_facts(g_native, count, a)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_ELIGIBLE);
    if (f) {
        CHECK(f->identity.pid == GetCurrentProcessId() && f->identity.thread_id == GetCurrentThreadId() && f->identity.process_start);
        CHECK(f->app_len > 0 && f->elevation != ASB_RW_ELEVATION_UNKNOWN && f->dpi >= 96);
    }
    CHECK(((f = find_facts(g_native, count, dup)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_ELIGIBLE);
    CHECK(((f = find_facts(g_native, count, tool)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_TOOL_WINDOW);
    CHECK(((f = find_facts(g_native, count, owned)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_OWNED);
    if (f) CHECK_EQ(asb_rw_association(f, (uint64_t)(ULONG_PTR)a), ASB_RW_SURFACE_OWNED);
    CHECK(((f = find_facts(g_native, count, hidden)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_HIDDEN);
    CHECK(((f = find_facts(g_native, count, minimized)) != NULL) && f->minimized && asb_rw_root_eligibility(f) == ASB_RW_ELIGIBLE);
    if (SUCCEEDED(cloak_hr)) CHECK(((f = find_facts(g_native, count, cloaked)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_CLOAKED);
    CHECK(((f = find_facts(g_native, count, popup)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_NOT_APP_WINDOW);
    CHECK(((f = find_facts(g_native, count, app_popup)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_ELIGIBLE);
    CHECK(((f = find_facts(g_native, count, untitled)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_UNTITLED);
    CHECK(((f = find_facts(g_native, count, unowned_sibling_popup)) != NULL) && asb_rw_association(f, (uint64_t)(ULONG_PTR)a) == 0);
    /* The helper's own windows are never offered. */
    uint32_t helper_count = 0;
    asb_rw_enumerate_windows(g_native, 2048, GetCurrentProcessId(), &helper_count);
    CHECK(((f = find_facts(g_native, helper_count, a)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_HELPER);
    /* Shell windows of the real desktop are excluded. */
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (tray) CHECK(((f = find_facts(g_native, helper_count, tray)) != NULL) && asb_rw_root_eligibility(f) == ASB_RW_INELIGIBLE_SHELL);

    /* Inventory over the real desktop restricted to our windows: duplicates and live IDs. */
    asb_rw_enumerate_windows(g_native, 2048, 0, &count);
    uint32_t mine = 0;
    for (uint32_t i = 0; i < count; ++i) if (g_native[i].identity.pid == GetCurrentProcessId()) g_native[mine++] = g_native[i];
    uint32_t n_events = 0, dropped = 0;
    asb_rw_ids_init(&g_ids, 0x5151);
    asb_rw_inventory_init(&g_inv);
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_native, mine, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    CHECK_EQ(g_inv.count, 4);   /* a, dup, minimized, custom chrome */
    uint64_t id_a = 0;
    for (uint32_t i = 0; i < g_inv.count; ++i) {
        if (text_is(g_inv.entries[i].title, g_inv.entries[i].title_len, L"RW Native Test"))
            CHECK(g_inv.entries[i].flags & ASB_RW_ENTRY_DUPLICATE_TITLE);
        if (!id_a && text_is(g_inv.entries[i].title, g_inv.entries[i].title_len, L"RW Native Test")) id_a = g_inv.entries[i].id;
    }
    AsbRwIdentity got;
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, id_a, asb_rw_probe_identity, NULL, &got), ASB_RW_OK);
    HWND target = (HWND)(ULONG_PTR)got.hwnd;
    /* Destroying the window makes its ID stale, even before the next inventory pass. */
    DestroyWindow(target);
    CHECK_EQ(asb_rw_ids_resolve(&g_ids, id_a, asb_rw_probe_identity, NULL, &got), ASB_RW_ERR_STALE_ID);
    asb_rw_enumerate_windows(g_native, 2048, 0, &count);
    mine = 0;
    for (uint32_t i = 0; i < count; ++i) if (g_native[i].identity.pid == GetCurrentProcessId()) g_native[mine++] = g_native[i];
    CHECK_EQ(asb_rw_inventory_update(&g_inv, &g_ids, g_native, mine, g_events, 64, &n_events, &dropped), ASB_RW_OK);
    CHECK(g_inv.count == 3 && n_events >= 1);
    int removed = 0;
    for (uint32_t i = 0; i < n_events; ++i) removed += g_events[i].kind == ASB_RW_EVENT_REMOVED && g_events[i].entry.id == id_a;
    CHECK_EQ(removed, 1);
    /* The surviving twin no longer collides. */
    for (uint32_t i = 0; i < g_inv.count; ++i)
        if (text_is(g_inv.entries[i].title, g_inv.entries[i].title_len, L"RW Native Test"))
            CHECK(!(g_inv.entries[i].flags & ASB_RW_ENTRY_DUPLICATE_TITLE));

    HWND all[] = { a, dup, tool, owned, hidden, minimized, cloaked, popup, app_popup, untitled, unowned_sibling_popup };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) if (IsWindow(all[i])) DestroyWindow(all[i]);
}


/* ============================================================ drag and drop (eh6.9) */

static void test_drag_codec(void)
{
    AsbRwHeader h;
    size_t n, total = 0;
    /* Control-channel drag messages. */
    CHECK_EQ(asb_rw_type_channel(ASB_RW_DRAG_PROBE), ASB_CH_RW_CONTROL);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_DRAG_STATE), ASB_CH_RW_CONTROL);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_DRAG_ACK), ASB_CH_RW_CONTROL);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_FILES_HELLO), ASB_CH_RW_FILES);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_FILE_POINTER), ASB_CH_RW_FILES);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_FILE_FEEDBACK), ASB_CH_RW_FILES);
    CHECK_EQ(asb_rw_type_channel(ASB_RW_FILE_STREAM), ASB_CH_RW_FILES);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.surface_id = 0x1234; g_msg.drag.probe = 7; g_msg.drag.button = INPUT_BTN_RIGHT;
    n = encode(ASB_RW_DRAG_PROBE, &g_msg, 3);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.drag.surface_id == 0x1234 && g_out.drag.probe == 7 && g_out.drag.button == INPUT_BTN_RIGHT);
    CHECK_EQ(asb_rw_frame_check(ASB_CH_RW_FILES, g_buf, n, &total), ASB_RW_ERR_WRONG_CHANNEL);
    /* PROBE without a surface / probe serial, or with a state, is malformed. */
    g_msg.drag.surface_id = 0;
    CHECK_EQ(asb_rw_encode(ASB_RW_DRAG_PROBE, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.drag.surface_id = 5; g_msg.drag.state = ASB_RW_DRAG_ACTIVE;
    CHECK_EQ(asb_rw_encode(ASB_RW_DRAG_PROBE, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.session_id = 0xABCDEF; g_msg.drag.direction = ASB_RW_DIR_TO_GUEST; g_msg.drag.state = ASB_RW_DRAG_ACTIVE;
    g_msg.drag.surface_id = 9;
    n = encode(ASB_RW_DRAG_STATE, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.drag.session_id == 0xABCDEF && g_out.drag.direction == ASB_RW_DIR_TO_GUEST);
    g_msg.drag.session_id = 0;   /* ACTIVE needs a session */
    CHECK_EQ(asb_rw_encode(ASB_RW_DRAG_STATE, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.drag.session_id = 1; g_msg.drag.state = ASB_RW_DRAG_ALLOW;   /* an ACK state in STATE */
    CHECK_EQ(asb_rw_encode(ASB_RW_DRAG_STATE, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    for (uint32_t st = ASB_RW_DRAG_ENDED; st <= ASB_RW_DRAG_SINK; ++st) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.drag.state = st; g_msg.drag.probe = 4; g_msg.drag.error = ASB_RW_ERR_UNSUPPORTED_DATA;
        n = encode(ASB_RW_DRAG_STATE, &g_msg, 0);
        CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
        CHECK_EQ(g_out.drag.error, ASB_RW_ERR_UNSUPPORTED_DATA);
    }
    for (uint32_t st = ASB_RW_DRAG_ALLOW; st <= ASB_RW_DRAG_RELEASE; ++st) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.drag.state = st; g_msg.drag.session_id = 2;
        n = encode(ASB_RW_DRAG_ACK, &g_msg, 0);
        CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    }
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.state = ASB_RW_DRAG_ACTIVE;   /* STATE value in an ACK */
    CHECK_EQ(asb_rw_encode(ASB_RW_DRAG_ACK, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    /* Hand-corrupted error code past the table. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.drag.state = ASB_RW_DRAG_RELEASE;
    n = encode(ASB_RW_DRAG_ACK, &g_msg, 0);
    g_buf[n - 4] = 0xFF;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_CONTROL, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);

    /* File-channel additions. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.files_hello.role = ASB_RW_ROLE_GUEST; g_msg.files_hello.version = ASB_RW_FILES_VERSION;
    g_msg.files_hello.directions = ASB_RW_DIR_TO_GUEST | ASB_RW_DIR_FROM_GUEST; g_msg.files_hello.max_chunk = 512 * 1024;
    g_msg.files_hello.session_tag = 0x55;
    n = encode(ASB_RW_FILES_HELLO, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.files_hello.session_tag == 0x55 && g_out.files_hello.max_chunk == 512 * 1024);
    g_msg.files_hello.max_chunk = ASB_RW_MAX_FILE_CHUNK + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILES_HELLO, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.files_hello.max_chunk = 65536; g_msg.files_hello.directions = 8;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILES_HELLO, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_pointer.session_id = 3; g_msg.file_pointer.surface_id = 4; g_msg.file_pointer.x = -20;
    g_msg.file_pointer.y = 700; g_msg.file_pointer.action = ASB_RW_POINTER_DROP;
    n = encode(ASB_RW_FILE_POINTER, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.file_pointer.x == -20 && g_out.file_pointer.y == 700 && g_out.file_pointer.action == ASB_RW_POINTER_DROP);
    g_msg.file_pointer.action = 3;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_POINTER, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.file_pointer.action = ASB_RW_POINTER_OVER; g_msg.file_pointer.x = 70000;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_POINTER, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_feedback.session_id = 3; g_msg.file_feedback.effect = ASB_RW_EFFECT_COPY;
    n = encode(ASB_RW_FILE_FEEDBACK, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    g_msg.file_feedback.effect = 2;   /* only NONE or COPY: never MOVE */
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_FEEDBACK, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);

    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_stream.session_id = 3; g_msg.file_stream.entry_count = 12; g_msg.file_stream.total_bytes = 1ull << 40;
    n = encode(ASB_RW_FILE_STREAM, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);
    CHECK(g_out.file_stream.total_bytes == 1ull << 40);
    g_msg.file_stream.entry_count = ASB_RW_MAX_FILE_ENTRIES + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_STREAM, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    /* An oversize count on the wire is a limit violation, found before any allocation. */
    g_msg.file_stream.entry_count = 12;
    n = encode(ASB_RW_FILE_STREAM, &g_msg, 0);
    g_buf[ASB_RW_HEADER_BYTES + 8] = 0xFF; g_buf[ASB_RW_HEADER_BYTES + 9] = 0xFF; g_buf[ASB_RW_HEADER_BYTES + 10] = 0xFF;
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_TOO_LARGE);

    /* BEGIN: top-level count bounded; host -> guest needs a target surface. */
    memset(&g_msg, 0, sizeof(g_msg));
    g_msg.file_begin.session_id = 1; g_msg.file_begin.direction = ASB_RW_DIR_TO_GUEST; g_msg.file_begin.target_id = 2;
    g_msg.file_begin.entry_count = ASB_RW_MAX_TOP_ENTRIES + 1;
    CHECK_EQ(asb_rw_encode(ASB_RW_FILE_BEGIN, 0, &g_msg, g_buf, sizeof(g_buf), &n), ASB_RW_ERR_MALFORMED);
    g_msg.file_begin.entry_count = 1; g_msg.file_begin.target_id = 0;
    n = encode(ASB_RW_FILE_BEGIN, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_MALFORMED);
    g_msg.file_begin.direction = ASB_RW_DIR_FROM_GUEST;
    n = encode(ASB_RW_FILE_BEGIN, &g_msg, 0);
    CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_OK);

    /* Hostile entry names never decode: traversal, roots, drives, streams, devices, aliases. */
    static const wchar_t *const bad[] = { L"..\\x", L"a\\..\\b", L"\\abs", L"C:\\x", L"a:stream", L"NUL", L"com1.txt",
                                          L"x.", L"x ", L"a\\\\b", L"\\\\?\\C:\\x", L"a/../b" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&g_msg, 0, sizeof(g_msg));
        g_msg.file_entry.session_id = 1; g_msg.file_entry.kind = ASB_RW_ENTRY_FILE;
        set_text(g_msg.file_entry.rel, &g_msg.file_entry.rel_len, L"ok.txt");
        n = encode(ASB_RW_FILE_ENTRY, &g_msg, 0);
        /* patch the name bytes in place (the encoder refuses unsafe names) */
        uint16_t len = (uint16_t)wcslen(bad[i]);
        size_t name_at = ASB_RW_HEADER_BYTES + 8 + 8 + 4 + 4;
        g_buf[name_at] = (uint8_t)len; g_buf[name_at + 1] = (uint8_t)(len >> 8);
        for (uint16_t k = 0; k < len; ++k) { g_buf[name_at + 2 + 2 * k] = (uint8_t)bad[i][k]; g_buf[name_at + 3 + 2 * k] = (uint8_t)(bad[i][k] >> 8); }
        n = name_at + 2 + 2 * (size_t)len;
        g_buf[12] = (uint8_t)(n - ASB_RW_HEADER_BYTES); g_buf[13] = (uint8_t)((n - ASB_RW_HEADER_BYTES) >> 8);
        g_buf[14] = 0; g_buf[15] = 0;
        CHECK_EQ(asb_rw_decode(ASB_CH_RW_FILES, g_buf, n, &h, &g_out, NULL, NULL), ASB_RW_ERR_FILE_REJECTED);
    }
    CHECK(!strcmp(asb_rw_error_name(ASB_RW_ERR_FILE_IO), "file_io"));
    CHECK(!strcmp(asb_rw_error_name(ASB_RW_ERR_UNSUPPORTED_DATA), "unsupported_data"));
    CHECK(!strcmp(asb_rw_error_name(ASB_RW_ERR_DISCONNECTED), "disconnected"));
}

int main(void)
{
    test_channels_and_services();
    test_roundtrips();
    test_malformed();
    test_negotiation();
    test_errors();
    test_identity();
    test_filtering();
    test_association();
    test_inventory();
    test_paths();
    test_drag_codec();
    test_native_windows();
    if (g_failures) {
        printf("FAILED %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS remote windows protocol: channels/services, codec round trips, partial and malformed messages, fuzz, "
           "versions/capabilities, errors, IDs (reuse, reconnect), filtering, ownership, inventory, duplicates, paths, native windows (%d checks)\n",
           g_checks);
    return 0;
}
