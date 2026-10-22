/*
 * test_rp_rtp — replay a captured RTP stream through the depacketizer.
 *
 * Reads length-prefixed packets (uint16 big-endian length, then the datagram) and writes Annex-B
 * to stdout. scripts/check-rtp.sh feeds it Apple's own Device Hub capture and compares the result
 * against the reference stream, so this is verified against real device output rather than
 * against my own idea of what the format is.
 */
#include "rp_rtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void on_nal(void *ctx, const uint8_t *nal, size_t len)
{
    FILE *out = ctx;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    fwrite(sc, 1, 4, out);
    fwrite(nal, 1, len, out);
}

int main(void)
{
#ifdef _WIN32
    /* Both streams are binary. In the CRT's default text mode a 0x1A byte reads as end-of-file
     * and every 0x0A written becomes 0D 0A, so the byte-compare would fail on any real capture. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    static uint8_t storage[(size_t)RP_RTP_REORDER_WINDOW * 1500 + RP_RTP_MAX_NAL];
    rp_rtp_session s;
    if (rp_rtp_init(&s, RP_RTP_CODEC_HEVC, storage, sizeof storage, on_nal, stdout) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    uint8_t pkt[65536];
    for (;;) {
        uint8_t hdr[2];
        if (fread(hdr, 1, 2, stdin) != 2) break;
        size_t n = ((size_t)hdr[0] << 8) | hdr[1];
        if (n > sizeof pkt) { fprintf(stderr, "packet too large\n"); return 1; }
        if (fread(pkt, 1, n, stdin) != n) break;
        rp_rtp_feed(&s, pkt, n);
    }
    rp_rtp_flush(&s);
    fprintf(stderr, "  received=%llu lost=%llu late=%llu dup=%llu\n",
            (unsigned long long)s.received, (unsigned long long)s.lost,
            (unsigned long long)s.late, (unsigned long long)s.duplicates);
    return 0;
}
