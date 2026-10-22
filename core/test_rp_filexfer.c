/*
 * test_rp_filexfer -- XPC file transfers (cryptexd's install) over memory, with no device.
 *
 * What is checked, each against pymobiledevice3's RemoteXPCConnection (the implementation proven
 * against devices, from which this is ported):
 *   - the FILE_TRANSFER object's bytes, and that a dictionary holding one can still be walked;
 *   - the stream: HEADERS on 5, a 24-byte preamble with FILE_TX_STREAM_REQUEST and the transfer
 *     id as message id, the payload in DATA frames of at most 16 KB, an empty END_STREAM DATA;
 *   - flow control: nothing is sent beyond the window the peer granted, including the window the
 *     handshake's own DATA frames already spent, and a send blocked on window resumes when a
 *     WINDOW_UPDATE arrives;
 *   - the reply: a RST_STREAM on the finished transfer stream is not an error, and a reply that
 *     completes while the sender waits for window is handed out by the next recv.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rp_http2.h"
#include "rp_remotexpc.h"

#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); return 1; } while (0)

static uint8_t written[1 << 20];
static size_t written_len;
static uint8_t inbound[1 << 16];
static size_t inbound_len, inbound_pos;

static long mem_write(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (written_len + len > sizeof written) return -1;
    memcpy(written + written_len, buf, len);
    written_len += len;
    return (long)len;
}

/* Hands out the scripted device frames one read at a time. */
static long mem_read(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    size_t left = inbound_len - inbound_pos;
    if (!left) return 0;
    if (len > left) len = left;
    memcpy(buf, inbound + inbound_pos, len);
    inbound_pos += len;
    return (long)len;
}

static void add_inbound(const uint8_t *p, size_t n)
{
    memcpy(inbound + inbound_len, p, n);
    inbound_len += n;
}

static void add_window_update(uint32_t stream, uint32_t inc)
{
    uint8_t f[16];
    size_t n = rp_h2_write_window_update(stream, inc, f, sizeof f);
    add_inbound(f, n);
}

static void add_settings_window(uint32_t window)
{
    uint8_t body[6] = { 0, 4, (uint8_t)(window >> 24), (uint8_t)(window >> 16),
                        (uint8_t)(window >> 8), (uint8_t)window };
    uint8_t f[32];
    size_t n = rp_h2_write_frame(RP_H2_SETTINGS, 0, 0, body, sizeof body, f, sizeof f);
    add_inbound(f, n);
}

/* A reply {"error": 0} on stream 1, as cryptexd sends after an install. */
static void add_reply(uint32_t stream)
{
    uint8_t body[64], msg[128], f[256];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_uint64(&w, "error", 0);
    rp_xpc_dict_end(&w);
    size_t m = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET | RP_XPC_F_DATA_PRESENT | RP_XPC_F_REPLY,
                           1, msg, sizeof msg);
    size_t n = rp_h2_write_data(stream, msg, m, f, sizeof f);
    add_inbound(f, n);
}

static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p)
{ uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

int main(void)
{
    /* ------------------------------------------------------------ the object itself */
    {
        uint8_t body[256];
        rp_xpc_writer w;
        rp_xpc_writer_init(&w, body, sizeof body);
        rp_xpc_dict_begin(&w);
        rp_xpc_set_uint64(&w, "auth", 0);
        rp_xpc_key(&w, "image");
        rp_xpc_file_transfer(&w, 1, 15895040);
        rp_xpc_set_string(&w, "after", "x");
        rp_xpc_dict_end(&w);
        if (w.overflow) FAIL("writer overflow");
        rp_xpc_obj d = { body, w.len };
        if (rp_xpc_dict_count(&d) != 3) FAIL("dict count %d, expected 3", rp_xpc_dict_count(&d));

        rp_xpc_obj ft, after, sz;
        if (rp_xpc_dict_get(&d, "image", &ft) != 0) FAIL("file transfer entry not found");
        if (rp_xpc_obj_type(&ft) != RP_XPC_FILE_TRANSFER) FAIL("wrong type");
        /* type | u64 id | dict {"s": uint64} -- the layout pymobiledevice3's XpcFileTransfer has. */
        if (le64(ft.data + 4) != 1) FAIL("transfer id");
        if (le32(ft.data + 12) != RP_XPC_DICT) FAIL("inner object is not a dictionary");
        rp_xpc_obj inner = { ft.data + 12, ft.size - 12 };
        uint64_t s = 0;
        if (rp_xpc_dict_get(&inner, "s", &sz) != 0 || rp_xpc_get_uint64(&sz, &s) != 0 || s != 15895040)
            FAIL("size");
        if (rp_xpc_dict_get(&d, "after", &after) != 0) FAIL("entry after the file transfer unreachable");
        /* Exact bytes: 1a000000 0100000000000000 00f00000 <len> 01000000 's\0\0\0' 00400000 <u64>. */
        static const uint8_t want[] = {
            0x00, 0xa0, 0x01, 0x00,  1, 0, 0, 0, 0, 0, 0, 0,
            0x00, 0xf0, 0x00, 0x00,  0x14, 0, 0, 0,  1, 0, 0, 0,  's', 0, 0, 0,
            0x00, 0x40, 0x00, 0x00,  0x00, 0x8a, 0xf2, 0x00, 0, 0, 0, 0 };
        if (ft.size != sizeof want || memcmp(ft.data, want, sizeof want) != 0) {
            printf("  got ");
            for (size_t i = 0; i < ft.size; i++) printf("%02x", ft.data[i]);
            printf("\n");
            FAIL("file transfer bytes differ");
        }
        printf("  object: FILE_TRANSFER encodes as type|id|{s:size} and dictionaries walk past it\n");
    }

    /* ------------------------------------------------------------ the stream + flow control */
    static uint8_t reassembly[1 << 18], raw[1 << 16];
    rp_rxpc_session s;
    rp_rxpc_io io = { mem_read, mem_write, NULL };
    rp_rxpc_init(&s, io, reassembly, sizeof reassembly, raw, sizeof raw);

    /* Handshake against a device that leaves the defaults (65535) in place. */
    uint8_t settings[9] = { 0, 0, 0, RP_H2_SETTINGS, 0, 0, 0, 0, 0 };
    add_inbound(settings, sizeof settings);
    if (rp_rxpc_handshake(&s) != 0) FAIL("handshake");
    /* DATA the handshake sent: {} on 1 (32+8+... bytes), 0x0201 on 1 and the init on 3. */
    size_t handshake_data = 0;
    for (size_t o = 0; o < written_len;) {
        rp_h2_frame f; size_t c = 0;
        if (o == 0) { o = 24; continue; }   /* preface */
        if (rp_h2_parse_frame(written + o, written_len - o, &f, &c) != 1) FAIL("handshake bytes");
        if (f.type == RP_H2_DATA) handshake_data += f.length;
        o += c;
    }

    /* 100000 bytes against a 65535 window that the handshake already dented: the sender must
     * stop, read the WINDOW_UPDATEs, and carry on. A reply sneaking in during the wait must be
     * kept. Then the device resets the finished stream (normal) and the reply is collected. */
    size_t len = 100000;
    uint8_t *payload = malloc(len);
    for (size_t i = 0; i < len; i++) payload[i] = (uint8_t)(i * 7);
    add_window_update(0, 1 << 20);
    add_reply(1);
    add_window_update(5, 1 << 20);
    add_settings_window(1 << 20);

    written_len = 0;
    if (rp_rxpc_send_file(&s, 3, payload, len) != 0) FAIL("send_file");

    /* Walk what was written. */
    size_t off = 0, data_total = 0, frames = 0;
    int saw_headers = 0, saw_pre = 0, saw_end = 0;
    while (off < written_len) {
        rp_h2_frame f;
        size_t c = 0;
        if (rp_h2_parse_frame(written + off, written_len - off, &f, &c) != 1) FAIL("bad frame written");
        off += c;
        if (f.stream != 5) FAIL("frame on stream %u, expected 5", f.stream);
        if (f.type == RP_H2_HEADERS) { if (frames) FAIL("HEADERS not first"); saw_headers = 1; }
        else if (f.type == RP_H2_DATA) {
            if (!saw_headers) FAIL("DATA before HEADERS");
            if (f.length > 16384) FAIL("DATA frame of %zu bytes exceeds 16 KB", f.length);
            if (!saw_pre) {
                uint32_t flags;
                uint64_t mid;
                rp_xpc_obj o;
                if (f.length != 24 || rp_xpc_unwrap(f.payload, f.length, &flags, &mid, &o) != 0)
                    FAIL("preamble is not an empty wrapper");
                if (flags != (RP_XPC_F_FILE_TX_STREAM_REQUEST | RP_XPC_F_ALWAYS_SET)) FAIL("preamble flags 0x%x", flags);
                if (mid != 3) FAIL("preamble message id %llu, expected the transfer id", (unsigned long long)mid);
                saw_pre = 1;
            } else if (f.flags & RP_H2_FLAG_END_STREAM) {
                if (f.length) FAIL("END_STREAM frame carries data");
                saw_end = 1;
            } else {
                if (memcmp(f.payload, payload + data_total, f.length) != 0) FAIL("payload bytes differ");
                data_total += f.length;
            }
        }
        frames++;
    }
    if (!saw_pre || !saw_end) FAIL("preamble or END_STREAM missing");
    if (data_total != len) FAIL("sent %zu of %zu payload bytes", data_total, len);
    if (inbound_pos == 0) FAIL("sender never read the WINDOW_UPDATEs");
    printf("  stream: HEADERS, preamble (FILE_TX_STREAM_REQUEST, id 3), %zu bytes in <=16 KB frames, END_STREAM\n",
           data_total);

    /* The connection window was 65535 minus the handshake's own DATA; the sender had to stop there,
     * and again when stream 5's 65535 ran out, and the reply read in between must be kept. */
    if (!s.has_pending) FAIL("the reply read during the window wait was not kept");
    if (s.out_conn_window != 65535 - (int64_t)handshake_data + (1 << 20) - (int64_t)(len + 24))
        FAIL("connection window accounting: %lld", (long long)s.out_conn_window);
    if (s.out_conn_window < 0) FAIL("connection window overrun (%lld)", (long long)s.out_conn_window);
    printf("  flow control: blocked at the granted window, resumed on WINDOW_UPDATE (window left %lld)\n",
           (long long)s.out_conn_window);

    /* The device resets the finished stream, then nothing else: the reply came in during the
     * wait and must still be handed out. */
    uint8_t rst[16];
    uint8_t code[4] = { 0, 0, 0, 5 };
    size_t rn = rp_h2_write_frame(RP_H2_RST_STREAM, 0, 5, code, 4, rst, sizeof rst);
    add_inbound(rst, rn);
    rp_xpc_obj reply, ev;
    uint64_t e = 99;
    if (rp_rxpc_recv(&s, &reply) != 0) FAIL("reply lost");
    if (rp_xpc_dict_get(&reply, "error", &ev) != 0 || rp_xpc_get_uint64(&ev, &e) != 0 || e != 0)
        FAIL("reply is not {error: 0}");
    printf("  reply: kept across the window wait and returned by recv\n");

    /* A second exchange: a RST_STREAM on the transfer stream before the reply must be skipped. */
    add_reply(1);
    if (rp_rxpc_recv(&s, &reply) != 0) FAIL("recv after RST_STREAM on a file stream");
    printf("  RST_STREAM on a finished transfer stream is not an error\n");

    free(payload);
    printf("\nOK -- XPC file transfer verified with no device.\n");
    return 0;
}
