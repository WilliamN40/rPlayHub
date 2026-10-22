#include "rp_remotexpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rp_http2.h"

/* Set RPLAY_XPC_DEBUG=1 to trace every frame and message. A receive loop that silently skips
 * things is impossible to diagnose from the outside -- which is how "the service never answered"
 * ended up meaning four different things over four hardware runs. */
static int xpc_debug(void)
{
    static int cached = -1;
    if (cached < 0) { const char *e = getenv("RPLAY_XPC_DEBUG"); cached = (e && *e == '1'); }
    return cached;
}

#define H2_DATA          0x0
#define H2_RST_STREAM    0x3
#define H2_SETTINGS      0x4
#define H2_GOAWAY        0x7
#define H2_WINDOW_UPDATE 0x8
#define H2_FLAG_ACK      0x1
#define H2_FLAG_END_STREAM 0x1

/* HTTP/2's defaults for what the peer accepts, until its SETTINGS say otherwise. 16 KB is the
 * smallest maximum frame size a peer may advertise, so it is always safe to send. */
#define H2_DEFAULT_WINDOW     65535
#define H2_MAX_OUT_FRAME      16384

void rp_rxpc_init(rp_rxpc_session *s, rp_rxpc_io io,
                  uint8_t *reassembly, size_t reassembly_cap,
                  uint8_t *raw, size_t raw_cap)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->buf = reassembly;
    s->buf_cap = reassembly_cap;
    s->raw = raw;
    s->raw_cap = raw_cap;
    s->next_message_id = 1;
    s->out_conn_window = H2_DEFAULT_WINDOW;
    s->peer_initial_window = H2_DEFAULT_WINDOW;
    s->next_file_stream = 5;
}

static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* The peer-granted window of one stream, created at the peer's initial size on first use. NULL
 * only when the small table is full, in which case only the connection window gates sending. */
static int64_t *out_window(rp_rxpc_session *s, uint32_t stream)
{
    for (int i = 0; i < s->out_stream_count; i++)
        if (s->out_streams[i].stream == stream) return &s->out_streams[i].window;
    int n = (int)(sizeof s->out_streams / sizeof s->out_streams[0]);
    if (s->out_stream_count >= n) return NULL;
    s->out_streams[s->out_stream_count].stream = stream;
    s->out_streams[s->out_stream_count].window = s->peer_initial_window;
    return &s->out_streams[s->out_stream_count++].window;
}

/* Every DATA frame we send spends the peer's window, whatever it carries. */
static void charge(rp_rxpc_session *s, uint32_t stream, size_t len)
{
    s->out_conn_window -= (int64_t)len;
    int64_t *w = out_window(s, stream);
    if (w) *w -= (int64_t)len;
}

/* Record what a peer frame grants us. Applied to every frame read, so no path can drop a grant
 * (pymobiledevice3 learned that the hard way: the connection grant arrives BEFORE the SETTINGS
 * ack, and discarding it stalled every transfer at 65535 bytes). */
static void apply_flow_control(rp_rxpc_session *s, const rp_h2_frame *f)
{
    if (f->type == H2_WINDOW_UPDATE && f->length >= 4 && f->payload) {
        uint32_t inc = be32(f->payload) & 0x7FFFFFFFu;
        if (f->stream == 0) s->out_conn_window += inc;
        else { int64_t *w = out_window(s, f->stream); if (w) *w += inc; }
    } else if (f->type == H2_SETTINGS && !(f->flags & H2_FLAG_ACK) && f->payload) {
        for (size_t off = 0; off + 6 <= f->length; off += 6) {
            uint16_t id = (uint16_t)((f->payload[off] << 8) | f->payload[off + 1]);
            uint32_t val = be32(f->payload + off + 2);
            if (id == 0x4) {
                /* A new INITIAL_WINDOW_SIZE shifts every open stream's window retroactively. */
                int64_t delta = (int64_t)val - s->peer_initial_window;
                s->peer_initial_window = val;
                for (int i = 0; i < s->out_stream_count; i++) s->out_streams[i].window += delta;
            }
        }
    }
}

static int write_all(rp_rxpc_session *s, const uint8_t *p, size_t n)
{
    while (n) {
        long w = s->io.write(s->io.ctx, p, n);
        if (w <= 0) {
            fprintf(stderr, "  rxpc: write_all failed (w=%ld)\n", w);
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* Pull one frame, reading from the transport until a whole one is buffered.
 *
 * The returned payload points into the raw buffer and stays valid until the NEXT call. That is
 * why consuming the previous frame is deferred to the top of this function rather than done at
 * the bottom: compacting the buffer immediately would move the bytes out from under the pointer
 * we just handed back. */
static int read_frame(rp_rxpc_session *s, rp_h2_frame *frame)
{
    if (s->pending_consume) {
        memmove(s->raw, s->raw + s->pending_consume, s->raw_len - s->pending_consume);
        s->raw_len -= s->pending_consume;
        s->pending_consume = 0;
    }
    for (;;) {
        size_t consumed = 0;
        if (rp_h2_parse_frame(s->raw, s->raw_len, frame, &consumed) == 1) {
            s->pending_consume = consumed;
            apply_flow_control(s, frame);
            return 0;
        }
        if (s->raw_len == s->raw_cap) {
            fprintf(stderr, "  rxpc: read_frame overflowed raw_cap (%zu)\n", s->raw_cap);
            return -1;
        }
        long r = s->io.read(s->io.ctx, s->raw + s->raw_len, s->raw_cap - s->raw_len);
        if (r <= 0) {
            fprintf(stderr, "  rxpc: read_frame io.read failed (r=%ld)\n", r);
            return -1;
        }
        s->raw_len += (size_t)r;
    }
}

int rp_rxpc_handshake(rp_rxpc_session *s)
{
    uint8_t out[512];
    uint8_t msg[128];
    size_t n, mlen;

    /* Written out step by step rather than via rp_h2_write_connection_start, because the order
     * here is not the obvious one: the root stream carries a message BETWEEN the two HEADERS
     * frames. This mirrors the Python that is proven against real devices; a tidier ordering is
     * not worth the risk of the device disagreeing. */
    if (!(n = rp_h2_write_preface(out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;
    if (!(n = rp_h2_write_settings(100, RP_RXPC_INITIAL_WINDOW, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;
    if (!(n = rp_h2_write_window_update(0, RP_RXPC_INITIAL_WINDOW - 65535, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    if (!(n = rp_h2_write_headers(RP_RXPC_ROOT_STREAM, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    /* An empty dictionary, message id 0. Note the flags: DATA_PRESENT is NOT set even though a
     * payload follows, because the reference implementation keys that flag off the dictionary
     * being non-empty. Faithfulness beats tidiness here. */
    {
        rp_xpc_writer w;
        uint8_t body[32];
        rp_xpc_writer_init(&w, body, sizeof body);
        rp_xpc_dict_begin(&w);
        rp_xpc_dict_end(&w);
        if (!(mlen = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET, 0, msg, sizeof msg))) return -1;
        if (!(n = rp_h2_write_data(RP_RXPC_ROOT_STREAM, msg, mlen, out, sizeof out))) return -1;
        if (write_all(s, out, n)) return -1;
        charge(s, RP_RXPC_ROOT_STREAM, mlen);
    }

    if (!(n = rp_h2_write_headers(RP_RXPC_REPLY_STREAM, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    if (!(mlen = rp_xpc_wrap_empty(0x0201, 0, msg, sizeof msg))) return -1;
    if (!(n = rp_h2_write_data(RP_RXPC_ROOT_STREAM, msg, mlen, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;
    charge(s, RP_RXPC_ROOT_STREAM, mlen);

    if (!(mlen = rp_xpc_wrap_empty(RP_XPC_F_ALWAYS_SET | RP_XPC_F_INIT_HANDSHAKE, 0,
                                   msg, sizeof msg))) return -1;
    if (!(n = rp_h2_write_data(RP_RXPC_REPLY_STREAM, msg, mlen, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;
    charge(s, RP_RXPC_REPLY_STREAM, mlen);

    for (;;) {
        rp_h2_frame f;
        if (read_frame(s, &f) != 0) {
            fprintf(stderr, "  rp_rxpc_handshake: read_frame failed\n");
            return -1;
        }
        fprintf(stderr, "  rxpc handshake frame: type=%d flags=0x%x len=%u stream=%u\n",
                f.type, f.flags, (unsigned)f.length, (unsigned)f.stream);
        if (f.type == H2_GOAWAY || f.type == H2_RST_STREAM) {
            fprintf(stderr, "  rp_rxpc_handshake: got GOAWAY or RST_STREAM (type=%d)\n", f.type);
            return -1;
        }
        if (f.type == H2_SETTINGS && !(f.flags & H2_FLAG_ACK)) {
            if (!(n = rp_h2_write_settings_ack(out, sizeof out))) return -1;
            return write_all(s, out, n);
        }
    }
}

int rp_rxpc_send(rp_rxpc_session *s, const uint8_t *body, size_t body_len, int wanting_reply)
{
    /* One buffer for the whole frame. Requests are small -- the largest is a media-stream offer
     * of a few hundred bytes -- so this stays well inside the 16 KB default frame limit, and
     * building header and payload together avoids a partial write splitting them. */
    static uint8_t frame[16384];
    uint8_t msg[8192];

    uint32_t flags = RP_XPC_F_ALWAYS_SET;
    if (body_len) flags |= RP_XPC_F_DATA_PRESENT;
    if (wanting_reply) flags |= RP_XPC_F_WANTING_REPLY;

    size_t mlen = rp_xpc_wrap(body, body_len, flags, s->next_message_id++, msg, sizeof msg);
    if (!mlen) return -1;

    size_t n = rp_h2_write_data(RP_RXPC_ROOT_STREAM, msg, mlen, frame, sizeof frame);
    if (!n) return -1;
    if (write_all(s, frame, n)) return -1;
    charge(s, RP_RXPC_ROOT_STREAM, mlen);
    return 0;
}

/* Take one DATA frame into the per-stream reassembly. Returns 1 when it completed a real message
 * (left in *obj, pointing into the reassembly buffer), 0 when there is nothing to hand out yet
 * (incomplete, an ack, or a stream that carries no messages), -1 when the reply overflowed. */
static int absorb_data(rp_rxpc_session *s, const rp_h2_frame *f, rp_xpc_obj *obj)
{
    if (f->type != H2_DATA || !f->length || !f->payload) return 0;

    /* Replenish the flow-control window as data is consumed.
     *
     * HTTP/2 stops a sender once it has written INITIAL_WINDOW bytes without being told the
     * receiver has consumed them. We advertised 1 MB and never sent a WINDOW_UPDATE, so any
     * reply larger than that stalled exactly at the limit and looked like the device going
     * silent mid-transfer. The RSD service map is ~25 kB and never came close; a full-screen
     * screenshot is the first reply big enough to hit it.
     *
     * Both windows have to be replenished -- the stream's and the connection's. */
    s->window_used += f->length;
    if (s->window_used >= RP_RXPC_INITIAL_WINDOW / 4) {
        uint8_t wu[32];
        uint32_t inc = (uint32_t)s->window_used;
        size_t n = rp_h2_write_window_update(f->stream, inc, wu, sizeof wu);
        if (n && write_all(s, wu, n) != 0) return -1;
        n = rp_h2_write_window_update(0, inc, wu, sizeof wu);
        if (n && write_all(s, wu, n) != 0) return -1;
        if (xpc_debug())
            fprintf(stderr, "    [xpc] window update +%u on stream %u and connection\n",
                    inc, f->stream);
        s->window_used = 0;
    }

    /* Messages live on the root and reply streams. Anything else is a file-transfer stream, whose
     * only traffic toward us is the device's FILE_TX_STREAM_RESPONSE preamble -- not a reply, and
     * appending it to the root stream's buffer would splice it into one. */
    if (f->stream != RP_RXPC_ROOT_STREAM && f->stream != RP_RXPC_REPLY_STREAM) {
        if (xpc_debug())
            fprintf(stderr, "    [xpc] %zu bytes on file-transfer stream %u (ignored)\n",
                    f->length, f->stream);
        return 0;
    }

    /* One accumulator per stream. The device interleaves DATA on the root and reply streams,
     * and appending both to a single buffer produces a message that is two halves of
     * different replies -- which fails to parse, forever, while the bytes keep arriving. */
    int slot = (f->stream == RP_RXPC_REPLY_STREAM) ? 1 : 0;
    size_t half = s->buf_cap / 2;
    uint8_t *buf = s->buf + (size_t)slot * half;

    if (s->buf_len[slot] + f->length > half) {
        /* The reply is larger than the caller's buffer. Dropping it silently makes a service
         * look like it answered nothing, which is indistinguishable from a protocol fault --
         * so report it instead of discarding it quietly. */
        if (xpc_debug())
            fprintf(stderr, "    [xpc] OVERFLOW: %zu buffered + %zu incoming > %zu available\n",
                    s->buf_len[slot], f->length, half);
        s->overflow_needed = s->buf_len[slot] + f->length;
        s->buf_len[slot] = 0;
        s->overflowed = 1;
        return -1;
    }
    memcpy(buf + s->buf_len[slot], f->payload, f->length);
    s->buf_len[slot] += f->length;

    uint32_t flags;
    uint64_t mid;
    rp_xpc_obj parsed;
    if (rp_xpc_unwrap(buf, s->buf_len[slot], &flags, &mid, &parsed) != 0) {
        if (xpc_debug())
            fprintf(stderr, "    [xpc] incomplete: %zu bytes buffered on stream %u\n",
                    s->buf_len[slot], f->stream);
        return 0;      /* incomplete: keep the bytes and wait for the rest */
    }
    if (xpc_debug())
        fprintf(stderr, "    [xpc] message flags=0x%x id=%llu payload=%zu bytes%s\n",
                flags, (unsigned long long)mid, parsed.size,
                (parsed.size == 0 || rp_xpc_is_empty_dict(&parsed)) ? "  (SKIPPED as ack)" : "");
    s->buf_len[slot] = 0;

    /* Skip acknowledgements -- no payload, or an empty dictionary. The device sends one of
     * these before the real answer, and mistaking it for the answer is the classic way to
     * "receive" a reply that has not arrived. */
    if (parsed.size == 0 || rp_xpc_is_empty_dict(&parsed)) return 0;

    if (obj) *obj = parsed;
    return 1;
}

static void report_goaway(const rp_h2_frame *f)
{
    /* last-stream-id(4) + error code(4): say which, since "the device closed it" covers both a
     * flow-control mistake (3) and a protocol one (1). */
    uint32_t code = (f->length >= 8 && f->payload) ? be32(f->payload + 4) : 0;
    fprintf(stderr, "  rxpc: device sent GOAWAY (error %u)\n", code);
}

/* Streams from 5 up are our file-transfer streams. The device resets each one once it has the
 * payload -- normal completion, not a failure. */
static int is_file_stream(uint32_t stream) { return stream >= 5 && (stream & 1u); }

int rp_rxpc_recv(rp_rxpc_session *s, rp_xpc_obj *obj)
{
    uint8_t out[64];
    if (s->has_pending) {
        s->has_pending = 0;
        if (obj) *obj = s->pending;
        return 0;
    }
    for (;;) {
        rp_h2_frame f;
        if (read_frame(s, &f) != 0) return -1;

        if (f.type == H2_GOAWAY) { report_goaway(&f); return -1; }
        if (f.type == H2_RST_STREAM) {
            if (is_file_stream(f.stream)) continue;
            return -1;
        }
        if (f.type == H2_SETTINGS && !(f.flags & H2_FLAG_ACK)) {
            size_t n = rp_h2_write_settings_ack(out, sizeof out);
            if (!n || write_all(s, out, n)) return -1;
            continue;
        }
        if (xpc_debug())
            fprintf(stderr, "    [xpc] frame type=0x%x flags=0x%x stream=%u len=%zu\n",
                    f.type, f.flags, f.stream, f.length);
        int r = absorb_data(s, &f, obj);
        if (r < 0) return -1;
        if (r == 1) return 0;
    }
}

/* Read one frame while blocked on the peer's window, so its WINDOW_UPDATEs are seen (read_frame
 * applies them). A reply completed meanwhile is kept for the next rp_rxpc_recv. */
static int pump_one(rp_rxpc_session *s)
{
    rp_h2_frame f;
    uint8_t out[64];
    if (read_frame(s, &f) != 0) return -1;
    if (f.type == H2_GOAWAY) { report_goaway(&f); return -1; }
    if (f.type == H2_RST_STREAM) {
        if (is_file_stream(f.stream)) return 0;
        fprintf(stderr, "  rxpc: device reset stream %u during a file transfer\n", f.stream);
        return -1;
    }
    if (f.type == H2_SETTINGS && !(f.flags & H2_FLAG_ACK)) {
        size_t n = rp_h2_write_settings_ack(out, sizeof out);
        return (!n || write_all(s, out, n)) ? -1 : 0;
    }
    rp_xpc_obj got;
    int r = absorb_data(s, &f, &got);
    if (r < 0) return -1;
    if (r == 1) {
        if (s->has_pending)
            fprintf(stderr, "  rxpc: a second reply arrived during a file transfer; keeping the first\n");
        else { s->pending = got; s->has_pending = 1; }
    }
    return 0;
}

static void put_frame_header(uint8_t h[9], size_t len, uint8_t type, uint8_t flags, uint32_t stream)
{
    h[0] = (uint8_t)(len >> 16); h[1] = (uint8_t)(len >> 8); h[2] = (uint8_t)len;
    h[3] = type; h[4] = flags;
    h[5] = (uint8_t)((stream >> 24) & 0x7F); h[6] = (uint8_t)(stream >> 16);
    h[7] = (uint8_t)(stream >> 8); h[8] = (uint8_t)stream;
}

/* Send `len` bytes as DATA on `stream`, in frames no larger than the peer's window allows. */
static int send_flow_controlled(rp_rxpc_session *s, uint32_t stream, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int64_t budget;
        for (;;) {
            int64_t *sw = out_window(s, stream);
            budget = s->out_conn_window;
            if (sw && *sw < budget) budget = *sw;
            if (budget > H2_MAX_OUT_FRAME) budget = H2_MAX_OUT_FRAME;
            if (budget > 0) break;
            if (pump_one(s) != 0) {
                fprintf(stderr, "  rxpc: stream %u stalled after %zu/%zu bytes (no window from the device)\n",
                        stream, off, len);
                return -1;
            }
        }
        size_t chunk = len - off;
        if ((int64_t)chunk > budget) chunk = (size_t)budget;
        uint8_t h[9];
        put_frame_header(h, chunk, H2_DATA, 0, stream);
        if (write_all(s, h, sizeof h) || write_all(s, data + off, chunk)) return -1;
        charge(s, stream, chunk);
        off += chunk;
    }
    return 0;
}

int rp_rxpc_send_file(rp_rxpc_session *s, uint64_t transfer_id, const uint8_t *data, size_t len)
{
    uint32_t stream = s->next_file_stream;
    s->next_file_stream += 2;

    uint8_t out[64];
    size_t n = rp_h2_write_headers(stream, out, sizeof out);
    if (!n || write_all(s, out, n)) return -1;

    /* The preamble is flow controlled too: sending it unaccounted overruns the connection window
     * once an earlier transfer has drained it. The device's own response preamble arrives AFTER the
     * payload and is not a gate -- holding the stream open waiting for it gets the stream reset. */
    uint8_t pre[32];
    size_t plen = rp_xpc_wrap_empty(RP_XPC_F_FILE_TX_STREAM_REQUEST | RP_XPC_F_ALWAYS_SET,
                                    transfer_id, pre, sizeof pre);
    if (!plen || send_flow_controlled(s, stream, pre, plen)) return -1;
    if (len && send_flow_controlled(s, stream, data, len)) return -1;

    uint8_t h[9];
    put_frame_header(h, 0, H2_DATA, H2_FLAG_END_STREAM, stream);
    if (write_all(s, h, sizeof h)) return -1;
    if (xpc_debug())
        fprintf(stderr, "    [xpc] file transfer %llu: %zu bytes on stream %u\n",
                (unsigned long long)transfer_id, len, stream);
    return 0;
}
