/* ddi.c -- see ddi.h. A direct C port of host/ddi_mount.py, which worked end to end on a rebooted
 * iPhone 13 (2026-08-23). Plists via libplist (migrated off CoreFoundation 2026-08-24); the mounter
 * is a classic shim service (u32-be length + plist, after RSDCheckin); the TSS request goes to
 * gs.apple.com over OpenSSL HTTPS. The TSS request build stays byte-identical to the Python (and to
 * the pre-migration CoreFoundation version) -- verified by diff. */
#include "ddi.h"

#include "compat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <plist/plist.h>
#include "usernet.h"
#include "rp_remotexpc.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

/* ------------------------------------------------------------------ plist helpers */

static void dset_str(plist_t d, const char *k, const char *v) { plist_dict_set_item(d, k, plist_new_string(v)); }
static void dset_bool(plist_t d, const char *k, int v) { plist_dict_set_item(d, k, plist_new_bool(v)); }
static void dset_int(plist_t d, const char *k, long long v) { plist_dict_set_item(d, k, plist_new_uint((uint64_t)v)); }
static void dset_data(plist_t d, const char *k, const uint8_t *b, size_t n) { plist_dict_set_item(d, k, plist_new_data((const char *)b, n)); }
/* Put a COPY of a borrowed node (owned by another plist) into d. */
static void dset_copy(plist_t d, const char *k, plist_t v) { plist_dict_set_item(d, k, plist_copy(v)); }

static plist_t dget(plist_t d, const char *k) { return d ? plist_dict_get_item(d, k) : NULL; }

static int dget_str(plist_t d, const char *k, char *out, size_t cap)
{
    plist_t v = dget(d, k);
    if (!v || plist_get_node_type(v) != PLIST_STRING) return -1;
    char *s = NULL;
    plist_get_string_val(v, &s);
    if (!s) return -1;
    snprintf(out, cap, "%s", s);
    free(s);
    return 0;
}

/* ------------------------------------------------------------------ framed plist over the tunnel */

static int recvn(int fd, uint8_t *buf, size_t n)
{ size_t g = 0; while (g < n) { long r = tun_read(fd, buf + g, n - g); if (r <= 0) return -1; g += (size_t)r; } return 0; }

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        long w = tun_write(fd, p + sent, n - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static int mounter_connect(const char *addr, long port)
{
    return tun_connect(addr, (int)port, 15);   /* tunnel: kernel socket or lwIP per userspace mode */
}

static int send_plist(int fd, plist_t d)
{
    char *xml = NULL;
    uint32_t xlen = 0;
    plist_to_xml(d, &xml, &xlen);
    if (!xml) return -1;
    uint8_t hdr[4] = { (uint8_t)(xlen >> 24), (uint8_t)(xlen >> 16), (uint8_t)(xlen >> 8), (uint8_t)xlen };
    int rc = (write_all(fd, hdr, 4) == 0 && write_all(fd, xml, xlen) == 0) ? 0 : -1;
    plist_mem_free(xml);
    return rc;
}

static plist_t recv_plist(int fd)
{
    uint8_t len[4];
    if (recvn(fd, len, 4) != 0) return NULL;
    size_t n = ((size_t)len[0] << 24) | ((size_t)len[1] << 16) | ((size_t)len[2] << 8) | len[3];
    if (n == 0 || n > (64u << 20)) return NULL;
    uint8_t *buf = malloc(n);
    if (!buf || recvn(fd, buf, n) != 0) { free(buf); return NULL; }
    plist_t pl = NULL;
    plist_from_memory((const char *)buf, (uint32_t)n, &pl, NULL);
    free(buf);
    if (pl && plist_get_node_type(pl) != PLIST_DICT) { plist_free(pl); return NULL; }
    return pl;
}

static int rsd_checkin(int fd)
{
    plist_t req = plist_new_dict();
    dset_str(req, "Label", "rplay-hub");
    dset_str(req, "ProtocolVersion", "2");
    dset_str(req, "Request", "RSDCheckin");
    int ok = send_plist(fd, req) == 0;
    plist_free(req);
    if (!ok) { fprintf(stderr, "  rsd_checkin: send_plist failed\n"); return -1; }
    plist_t r = recv_plist(fd);
    if (!r) { fprintf(stderr, "  rsd_checkin: recv_plist 1 failed\n"); return -1; }
    char v[32] = ""; int good = dget_str(r, "Request", v, sizeof v) == 0 && !strcmp(v, "RSDCheckin");
    if (!good) { fprintf(stderr, "  rsd_checkin: expected RSDCheckin, got '%s'\n", v); }
    plist_free(r);
    if (!good) return -1;
    r = recv_plist(fd);
    if (!r) { fprintf(stderr, "  rsd_checkin: recv_plist 2 failed\n"); return -1; }
    v[0] = 0;
    good = dget_str(r, "Request", v, sizeof v) == 0 && !strcmp(v, "StartService");
    if (!good) { fprintf(stderr, "  rsd_checkin: expected StartService, got '%s'\n", v); }
    plist_free(r);
    return good ? 0 : -1;
}

static plist_t mounter_cmd(int fd, plist_t req)
{
    if (send_plist(fd, req) != 0) return NULL;
    return recv_plist(fd);
}

static plist_t simple_cmd(int fd, const char *command, const char *k2, const char *v2)
{
    plist_t req = plist_new_dict();
    dset_str(req, "Command", command);
    if (k2) dset_str(req, k2, v2);
    plist_t r = mounter_cmd(fd, req);
    plist_free(req);
    return r;
}

/* ------------------------------------------------------------------ mount status */

static int copy_devices_has_personalized(int fd)
{
    plist_t r = simple_cmd(fd, "CopyDevices", NULL, NULL);
    if (!r) return 0;
    int found = 0;
    plist_t list = dget(r, "EntryList");
    for (uint32_t i = 0; list && plist_get_node_type(list) == PLIST_ARRAY && i < plist_array_get_size(list); i++) {
        plist_t e = plist_array_get_item(list, i);
        char t[32];
        if (dget_str(e, "DiskImageType", t, sizeof t) == 0 && !strcmp(t, "Personalized")) { found = 1; break; }
    }
    plist_free(r);
    return found;
}

int cdhost_ddi_is_mounted(const char *addr, long port)
{
    int fd = mounter_connect(addr, port);
    if (fd < 0) return 0;
    int m = rsd_checkin(fd) == 0 && copy_devices_has_personalized(fd);
    tun_close(fd);
    return m;
}

/* ------------------------------------------------------------------ DDI files on disk */

static int read_file(const char *path, uint8_t **out, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
    fclose(f);
    b[sz] = 0;
    *out = b; *n = (size_t)sz;
    return 0;
}

static plist_t load_manifest(const char *ddi_dir)
{
    char path[1300];   /* ddi_dir is up to 1200 bytes, plus the suffix */
    snprintf(path, sizeof path, "%s/Restore/BuildManifest.plist", ddi_dir);
    uint8_t *b; size_t n;
    if (read_file(path, &b, &n) != 0) return NULL;
    plist_t pl = NULL;
    plist_from_memory((const char *)b, (uint32_t)n, &pl, NULL);
    free(b);
    if (pl && plist_get_node_type(pl) != PLIST_DICT) { plist_free(pl); return NULL; }
    return pl;
}

/* The BuildIdentity whose ApChipID/ApBoardID (hex strings) match the phone. Borrowed reference.
 *
 * A phone newer than the DDI's manifest has no identity of its own (an iPhone19,7, chip 0x8160,
 * against the 27A5228h manifest, whose newest chip is 0x8150). The image and trust cache are the
 * same files for every phone -- across all 141 identities of that manifest there are only two
 * distinct PersonalizedDMG digests -- so the identity of the nearest chip at or below the phone's
 * (same chip any board first, then the highest older chip) carries the right digests. *exact says
 * which happened; build_tss_request then sends the phone's own chip and board, not the borrowed
 * ones, and Apple's TSS decides. RPLAY_DDI_STRICT=1 turns the fallback off. */
static plist_t pick_identity(plist_t manifest, long chip, long board, int *exact)
{
    plist_t ids = dget(manifest, "BuildIdentities");
    plist_t best = NULL;
    long best_chip = -1, best_board = -1;
    *exact = 0;
    for (uint32_t i = 0; ids && plist_get_node_type(ids) == PLIST_ARRAY && i < plist_array_get_size(ids); i++) {
        plist_t bi = plist_array_get_item(ids, i);
        char cs[16], bs[16];
        if (dget_str(bi, "ApChipID", cs, sizeof cs) != 0 || dget_str(bi, "ApBoardID", bs, sizeof bs) != 0) continue;
        long c = strtol(cs, NULL, 16), b = strtol(bs, NULL, 16);
        if (c == chip && b == board) { *exact = 1; return bi; }
        /* Same family only (0x81xx with 0x81xx): an older SoC line has different security
         * properties and is no stand-in. */
        if (c > chip || (c >> 8) != (chip >> 8)) continue;
        if (c > best_chip || (c == best_chip && labs(b - board) < labs(best_board - board))) {
            best = bi; best_chip = c; best_board = b;
        }
    }
    const char *strict = getenv("RPLAY_DDI_STRICT");
    if (strict && strict[0] == '1') return NULL;
    if (best)
        fprintf(stderr, "  no BuildIdentity for chip %#lx board %#lx; borrowing chip %#lx board %#lx's "
                "(same image), with this phone's own ids in the ticket request\n",
                chip, board, best_chip, best_board);
    return best;
}

/* ------------------------------------------------------------------ TSS request (see host/ddi_mount.py) */

static int node_is_true(plist_t v) { uint8_t b = 0; if (v && plist_get_node_type(v) == PLIST_BOOLEAN) plist_get_bool_val(v, &b); return b; }

static void apply_restore_rules(plist_t e, plist_t rules, int production, int security, int img4)
{
    for (uint32_t i = 0; i < plist_array_get_size(rules); i++) {
        plist_t rule = plist_array_get_item(rules, i);
        plist_t cond = dget(rule, "Conditions");
        plist_t acts = dget(rule, "Actions");
        if (!cond || !acts) continue;
        int ok = 1;
        plist_dict_iter it = NULL;
        plist_dict_new_iter(cond, &it);
        char *key = NULL;
        plist_t cval = NULL;
        for (;;) {
            plist_dict_next_item(cond, it, &key, &cval);
            if (!key) break;
            int actual;
            if (!strcmp(key, "ApRawProductionMode") || !strcmp(key, "ApCurrentProductionMode")) actual = production;
            else if (!strcmp(key, "ApRawSecurityMode")) actual = security;
            else if (!strcmp(key, "ApRequiresImage4")) actual = img4;
            else if (!strcmp(key, "ApInRomDFU")) actual = 0;
            else { ok = 0; free(key); key = NULL; break; }
            int want = node_is_true(cval);
            free(key); key = NULL;
            if (actual != want) { ok = 0; break; }
        }
        free(it);
        if (!ok) continue;
        plist_dict_iter ait = NULL;
        plist_dict_new_iter(acts, &ait);
        char *akey = NULL;
        plist_t aval = NULL;
        for (;;) {
            plist_dict_next_item(acts, ait, &akey, &aval);
            if (!akey) break;
            if (aval && plist_get_node_type(aval) == PLIST_BOOLEAN)
                plist_dict_set_item(e, akey, plist_copy(aval));
            free(akey); akey = NULL;
        }
        free(ait);
    }
}

static plist_t build_tss_request(plist_t bi, plist_t ids, long chip, long board,
                                 const uint8_t *nonce, size_t nlen)
{
    const int production = 1, security = 1, img4 = 1;
    plist_t req = plist_new_dict();
    dset_str(req, "@HostPlatformInfo", "mac");
    dset_str(req, "@VersionInfo", "libauthinstall-1049.100.23");
    dset_str(req, "@UUID", "00000000-0000-4000-8000-000000000000");

    /* Ap,* identifiers the device reports go in verbatim. */
    plist_dict_iter iit = NULL;
    plist_dict_new_iter(ids, &iit);
    char *ik = NULL;
    plist_t iv = NULL;
    for (;;) {
        plist_dict_next_item(ids, iit, &ik, &iv);
        if (!ik) break;
        if (!strncmp(ik, "Ap,", 3)) plist_dict_set_item(req, ik, plist_copy(iv));
        free(ik); ik = NULL;
    }
    free(iit);

    /* One entry per manifest component (tss_request_add_ap_tags). */
    plist_t manifest = dget(bi, "Manifest");
    plist_dict_iter mit = NULL;
    if (manifest) plist_dict_new_iter(manifest, &mit);
    char *mk = NULL;
    plist_t entry = NULL;
    while (manifest) {
        plist_dict_next_item(manifest, mit, &mk, &entry);
        if (!mk) break;
        plist_t info = dget(entry, "Info");
        if (!info) { free(mk); mk = NULL; continue; }
        if (node_is_true(dget(info, "IsFTAB"))) { free(mk); mk = NULL; continue; }
        int trusted = node_is_true(dget(entry, "Trusted"));
        plist_t rules = dget(info, "RestoreRequestRules");
        if (img4 && !rules && !trusted) { free(mk); mk = NULL; continue; }
        /* Copy the entry minus Info. */
        plist_t e = plist_new_dict();
        plist_dict_iter eit = NULL;
        plist_dict_new_iter(entry, &eit);
        char *ek = NULL;
        plist_t ev = NULL;
        for (;;) {
            plist_dict_next_item(entry, eit, &ek, &ev);
            if (!ek) break;
            if (strcmp(ek, "Info")) plist_dict_set_item(e, ek, plist_copy(ev));
            free(ek); ek = NULL;
        }
        free(eit);
        if (rules) apply_restore_rules(e, rules, production, security, img4);
        else if (img4) { dset_bool(e, "EPRO", production); dset_bool(e, "ESEC", security); }
        if (trusted && !dget(entry, "Digest")) dset_data(e, "Digest", NULL, 0);
        if (plist_dict_get_size(e) > 0) plist_dict_set_item(req, mk, e);
        else plist_free(e);
        free(mk); mk = NULL;
    }
    free(mit);

    /* common + img4 tags */
    plist_t ecid = dget(ids, "UniqueChipID"); if (ecid) dset_copy(req, "ApECID", ecid);
    plist_t ubid = dget(bi, "UniqueBuildID"); if (ubid) dset_copy(req, "UniqueBuildID", ubid);
    /* The phone's own chip and board: they equal the identity's when it matched exactly, and are
     * what must be signed for when the identity was borrowed (pick_identity). */
    if (chip >= 0 && board >= 0) {
        dset_int(req, "ApChipID", chip);
        dset_int(req, "ApBoardID", board);
    } else {
        char cs[16], bs[16];
        if (dget_str(bi, "ApChipID", cs, sizeof cs) == 0) dset_int(req, "ApChipID", strtol(cs, NULL, 16));
        if (dget_str(bi, "ApBoardID", bs, sizeof bs) == 0) dset_int(req, "ApBoardID", strtol(bs, NULL, 16));
    }
    char ds[16]; if (dget_str(bi, "ApSecurityDomain", ds, sizeof ds) == 0) dset_int(req, "ApSecurityDomain", strtol(ds, NULL, 16));
    const char *img4_keys[] = { "Ap,OSLongVersion", "Ap,OSReleaseType", "Ap,ProductMarketingVersion", "Ap,ProductType", "Ap,SDKPlatform", "Ap,Target", "Ap,TargetType", "Ap,Timestamp", NULL };
    for (int i = 0; img4_keys[i]; i++) { plist_t v = dget(bi, img4_keys[i]); if (v) dset_copy(req, img4_keys[i], v); }
    dset_data(req, "ApNonce", nonce, nlen);
    dset_bool(req, "@ApImg4Ticket", 1);
    dset_bool(req, "ApSecurityMode", security);
    dset_bool(req, "ApProductionMode", production);
    static const uint8_t zero20[20] = {0};
    dset_data(req, "SepNonce", zero20, 20);
    plist_t pearl = dget(bi, "PearlCertificationRootPub"); if (pearl) dset_copy(req, "PearlCertificationRootPub", pearl);
    dset_bool(req, "UID_MODE", 0);
    return req;
}

/* ------------------------------------------------------------------ HTTPS POST to gs.apple.com */

static char *tss_post(const uint8_t *body, size_t blen, size_t *rlen)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("gs.apple.com", "443", &hints, &res) != 0) return NULL;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
#ifdef _WIN32
        DWORD ms = 30000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        sock_close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx ? SSL_new(ctx) : NULL;
    if (!ssl) { if (ctx) SSL_CTX_free(ctx); sock_close(fd); return NULL; }
    SSL_set_tlsext_host_name(ssl, "gs.apple.com");
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); sock_close(fd); return NULL; }

    char hdr[512];
    int hn = snprintf(hdr, sizeof hdr,
        "POST /TSS/controller?action=2 HTTP/1.1\r\nHost: gs.apple.com\r\n"
        "User-Agent: InetURL/1.0\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "Cache-Control: no-cache\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    char *resp = NULL; size_t cap = 0, got = 0;
    int ok = SSL_write(ssl, hdr, hn) == hn && SSL_write(ssl, body, (int)blen) == (int)blen;
    while (ok) {
        if (got + 65536 + 1 > cap) { cap = cap ? cap * 2 : 1 << 17; char *np = realloc(resp, cap); if (!np) { ok = 0; break; } resp = np; }
        int r = SSL_read(ssl, resp + got, 65536);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (resp) resp[got] = 0;
    SSL_free(ssl); SSL_CTX_free(ctx); sock_close(fd);
    if (!ok) { free(resp); return NULL; }
    *rlen = got;
    return resp;
}

/* The first of `keys` (NULL-terminated) present in Apple's answer: ApImg4Ticket for the AP ticket,
 * Cryptex1,Ticket for a cryptex. A refusal is printed verbatim -- "STATUS=94&MESSAGE=This device
 * isn't eligible for the requested build." says more than any code we could map it to. */
static uint8_t *tss_extract_ticket(const char *resp, size_t rlen, size_t *tlen, const char *const *keys)
{
    (void)rlen;
    const char *body = strstr(resp, "\r\n\r\n");
    body = body ? body + 4 : resp;
    if (!strstr(body, "STATUS=0&")) {
        fprintf(stderr, "  Apple's signing server (TSS) refused: %.300s\n", body);
        return NULL;
    }
    const char *rs = strstr(body, "REQUEST_STRING=");
    if (!rs) { fprintf(stderr, "  TSS answer has no REQUEST_STRING\n"); return NULL; }
    rs += strlen("REQUEST_STRING=");
    plist_t pl = NULL;
    plist_from_memory(rs, (uint32_t)strlen(rs), &pl, NULL);
    if (!pl || plist_get_node_type(pl) != PLIST_DICT) { if (pl) plist_free(pl); return NULL; }
    plist_t ticket = NULL;
    for (int i = 0; keys[i] && !ticket; i++) ticket = dget(pl, keys[i]);
    if (!ticket) fprintf(stderr, "  TSS answer has no %s\n", keys[0]);
    uint8_t *out = NULL;
    if (ticket && plist_get_node_type(ticket) == PLIST_DATA) {
        uint64_t dl = 0;
        const char *bytes = plist_get_data_ptr(ticket, &dl);
        if (bytes) { *tlen = (size_t)dl; out = malloc(*tlen); if (out) memcpy(out, bytes, *tlen); }
    }
    plist_free(pl);
    return out;
}

/* ------------------------------------------------------------------ the whole flow */

/* Every iOS_DDI directory the engine considers, in priority order. Shared by the personalized
 * image (<dir>/Restore) and the Cryptex one (<dir>/Cryptex, or a Restore whose manifest carries the
 * cryptex identity, as Xcode's does). */
#define DDI_MAX_CANDIDATES 8
#define DDI_PATH_MAX 1100
static int ddi_candidates(const char *given, char cand[DDI_MAX_CANDIDATES][DDI_PATH_MAX])
{
    int n = 0;
    /* An explicit RPLAY_DDI always wins -- it is how a user overrides whatever else is found. */
    const char *env = getenv("RPLAY_DDI"); if (env && *env) snprintf(cand[n++], DDI_PATH_MAX, "%s", env);
    if (given && *given) snprintf(cand[n++], DDI_PATH_MAX, "%s", given);
#ifdef _WIN32
    /* Where the Windows build keeps it: per-user app data (what an in-app or PowerShell fetch
     * writes), then beside the executable -- iOS_DDI next to cdhost.exe in an installed or
     * portable layout, ..\deps\iOS_DDI in a source tree (host-c\cdhost.exe). Windows accepts
     * '/' in the "<dir>/Restore/..." probe below, so mixed separators are fine. */
    const char *lad = getenv("LOCALAPPDATA");
    if (lad && *lad) snprintf(cand[n++], DDI_PATH_MAX, "%s\\rplayhub\\iOS_DDI", lad);
    {
        char exe[1024];
        DWORD len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
        if (len > 0 && len < sizeof exe) {
            char *slash = NULL;
            for (char *q = exe; *q; q++) if (*q == '\\' || *q == '/') slash = q;
            if (slash) {
                *slash = 0;     /* the directory holding cdhost.exe */
                snprintf(cand[n++], DDI_PATH_MAX, "%s\\iOS_DDI", exe);
                snprintf(cand[n++], DDI_PATH_MAX, "%s\\..\\deps\\iOS_DDI", exe);
            }
        }
    }
#endif
    /* Where scripts/fetch-ddi.sh puts it -- the way to get one without Xcode, e.g. on Linux (and
     * under MSYS2, where HOME is set too). */
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cand[n++], DDI_PATH_MAX, "%s/rplayhub/iOS_DDI", xdg);
    else if (home && *home) snprintf(cand[n++], DDI_PATH_MAX, "%s/.local/share/rplayhub/iOS_DDI", home);
#ifndef _WIN32
    snprintf(cand[n++], DDI_PATH_MAX, "%s", "/Library/Developer/DeveloperDiskImages/iOS_DDI");
#endif
    return n;
}

static const char *find_ddi_dir(const char *given, char *buf, size_t cap)
{
    char cand[DDI_MAX_CANDIDATES][DDI_PATH_MAX];
    int n = ddi_candidates(given, cand);
    for (int i = 0; i < n; i++) {
        char p[DDI_PATH_MAX + 32]; snprintf(p, sizeof p, "%s/Restore/BuildManifest.plist", cand[i]);
        struct stat st;
        if (stat(p, &st) == 0) { snprintf(buf, cap, "%.1099s", cand[i]); return buf; }
    }
    return NULL;
}

int cdhost_ddi_activate(const char *addr, long port, const char *ddi_dir_in)
{
    if (!port) return RP_DDI_NO_SERVICE;

    char ddi_dir[1200];
    if (!find_ddi_dir(ddi_dir_in, ddi_dir, sizeof ddi_dir)) {
#ifdef _WIN32
        fprintf(stderr, "  DDI files not found: put them in %%LOCALAPPDATA%%\\rplayhub\\iOS_DDI or beside cdhost.exe "
                        "(iOS_DDI\\Restore\\BuildManifest.plist ...), or set RPLAY_DDI to an iOS_DDI directory\n");
#else
        fprintf(stderr, "  DDI files not found: run scripts/fetch-ddi.sh, or set RPLAY_DDI to an iOS_DDI directory\n");
#endif
        return RP_DDI_NO_DDI;
    }

    int fd = mounter_connect(addr, port);
    if (fd < 0) return RP_DDI_NO_SERVICE;
    if (rsd_checkin(fd) != 0) { tun_close(fd); return RP_DDI_ERR; }

    if (copy_devices_has_personalized(fd)) { tun_close(fd); return RP_DDI_ALREADY; }

    /* identity + nonce */
    plist_t ir = simple_cmd(fd, "QueryPersonalizationIdentifiers", "PersonalizedImageType", "DeveloperDiskImage");
    plist_t ids = ir ? dget(ir, "PersonalizationIdentifiers") : NULL;
    if (!ids) { if (ir) plist_free(ir); tun_close(fd); fprintf(stderr, "  QueryPersonalizationIdentifiers failed\n"); return RP_DDI_ERR; }
    long chip = 0, board = 0;
    { plist_t v = dget(ids, "ChipID"); if (v) { uint64_t u = 0; plist_get_uint_val(v, &u); chip = (long)u; }
      v = dget(ids, "BoardId"); if (v) { uint64_t u = 0; plist_get_uint_val(v, &u); board = (long)u; } }

    plist_t nr = simple_cmd(fd, "QueryNonce", "PersonalizedImageType", "DeveloperDiskImage");
    plist_t nonce = nr ? dget(nr, "PersonalizationNonce") : NULL;
    if (!nonce || plist_get_node_type(nonce) != PLIST_DATA) { if (nr) plist_free(nr); plist_free(ir); tun_close(fd); fprintf(stderr, "  QueryNonce failed\n"); return RP_DDI_ERR; }
    uint64_t nonce_len = 0;
    const char *nonce_bytes = plist_get_data_ptr(nonce, &nonce_len);

    plist_t manifest = load_manifest(ddi_dir);
    int exact = 0;
    plist_t bi = manifest ? pick_identity(manifest, chip, board, &exact) : NULL;
    if (bi && !exact) fprintf(stderr, "  (borrowed identity: Apple decides whether to sign it)\n");
    if (!bi) { if (manifest) plist_free(manifest); plist_free(nr); plist_free(ir); tun_close(fd);
               fprintf(stderr, "  no BuildIdentity for chip %#lx board %#lx\n", chip, board); return RP_DDI_ERR; }

    /* ticket from Apple */
    fprintf(stderr, "  requesting a DDI ticket from Apple...\n");
    plist_t tss = build_tss_request(bi, ids, chip, board, (const uint8_t *)nonce_bytes, (size_t)nonce_len);
    char *tss_xml = NULL; uint32_t tss_len = 0;
    plist_to_xml(tss, &tss_xml, &tss_len);
    size_t rlen = 0;
    char *resp = tss_xml ? tss_post((const uint8_t *)tss_xml, (size_t)tss_len, &rlen) : NULL;
    size_t tlen = 0;
    static const char *const ap_keys[] = { "ApImg4Ticket", NULL };
    if (!resp) fprintf(stderr, "  could not reach Apple's signing server (gs.apple.com)\n");
    uint8_t *ticket = resp ? tss_extract_ticket(resp, rlen, &tlen, ap_keys) : NULL;
    if (tss_xml) plist_mem_free(tss_xml);
    plist_free(tss);
    free(resp);
    if (!ticket) { plist_free(manifest); plist_free(nr); plist_free(ir); tun_close(fd); return RP_DDI_TSS_FAILED; }

    /* the .dmg and trust cache paths from the manifest */
    char dmg_rel[256] = "", tc_rel[256] = "";
    { plist_t man = dget(bi, "Manifest");
      plist_t pd = dget(man, "PersonalizedDMG"), lt = dget(man, "LoadableTrustCache");
      dget_str(dget(pd, "Info"), "Path", dmg_rel, sizeof dmg_rel);
      dget_str(dget(lt, "Info"), "Path", tc_rel, sizeof tc_rel); }
    char dmg_path[1400], tc_path[1400];
    snprintf(dmg_path, sizeof dmg_path, "%s/Restore/%s", ddi_dir, dmg_rel);
    snprintf(tc_path, sizeof tc_path, "%s/Restore/%s", ddi_dir, tc_rel);
    uint8_t *dmg = NULL, *tc = NULL; size_t dmg_n = 0, tc_n = 0;
    if (read_file(dmg_path, &dmg, &dmg_n) != 0 || read_file(tc_path, &tc, &tc_n) != 0) {
        free(ticket); free(dmg); free(tc); plist_free(manifest); plist_free(nr); plist_free(ir); tun_close(fd);
        fprintf(stderr, "  could not read the DDI image files\n"); return RP_DDI_NO_DDI;
    }

    /* ReceiveBytes -> upload -> MountImage */
    int rc = RP_DDI_ERR;
    plist_t rb = plist_new_dict();
    dset_str(rb, "Command", "ReceiveBytes");
    dset_str(rb, "ImageType", "Personalized");
    dset_int(rb, "ImageSize", (long long)dmg_n);
    dset_data(rb, "ImageSignature", ticket, tlen);
    plist_t ack = mounter_cmd(fd, rb);
    plist_free(rb);
    char st[32] = "";
    if (!ack || dget_str(ack, "Status", st, sizeof st) != 0 || strcmp(st, "ReceiveBytesAck") != 0) {
        if (ack) plist_free(ack);
        fprintf(stderr, "  ReceiveBytes refused -- is the phone unlocked?\n");
        rc = RP_DDI_LOCKED;
        goto done;
    }
    plist_free(ack);
    if (write_all(fd, dmg, dmg_n) != 0) { fprintf(stderr, "  upload failed\n"); goto done; }
    { plist_t up = recv_plist(fd); char s2[32] = "";
      int good = up && dget_str(up, "Status", s2, sizeof s2) == 0 && !strcmp(s2, "Complete");
      if (up) plist_free(up);
      if (!good) { fprintf(stderr, "  upload not acknowledged\n"); goto done; } }

    { plist_t mi = plist_new_dict();
      dset_str(mi, "Command", "MountImage");
      dset_str(mi, "ImagePath", "/private/var/mobile/Media/PublicStaging/staging.dimage");
      dset_data(mi, "ImageSignature", ticket, tlen);
      dset_str(mi, "ImageType", "Personalized");
      dset_data(mi, "ImageTrustCache", tc, tc_n);
      plist_t mr = mounter_cmd(fd, mi);
      plist_free(mi);
      char s3[32] = "";
      int good = mr && dget_str(mr, "Status", s3, sizeof s3) == 0 && !strcmp(s3, "Complete");
      if (mr && !good) { char e[64] = ""; dget_str(mr, "Error", e, sizeof e); fprintf(stderr, "  MountImage failed: %s\n", e[0] ? e : "?"); }
      if (mr) plist_free(mr);
      rc = good ? RP_DDI_OK : RP_DDI_ERR; }

done:
    free(ticket); free(dmg); free(tc);
    plist_free(manifest); plist_free(nr); plist_free(ir);
    tun_close(fd);
    if (rc == RP_DDI_OK) fprintf(stderr, "  DDI mounted -- developer services are live.\n");
    return rc;
}

/* ================================================================== the Cryptex DDI, over cryptexd
 *
 * From iOS 27 the DDI goes on as a CRYPTEX, installed through cryptexd
 * (com.apple.security.cryptexd.remote, a RemoteXPC service in the RSD map), not mounted through
 * mobile_image_mounter. What matters here: the cryptex's BuildIdentity ("... Developer Disk Image
 * Cryptex") is not tied to a board -- it is personalized with Cryptex1,UseProductClass -- so it
 * works on phones newer than the DDI's manifest, which the personalized image cannot (an
 * iPhone19,7 against 27A5228h: no identity, and TSS answers "STATUS=94 ... isn't eligible" when one
 * is borrowed).
 *
 * The flow, ported from pymobiledevice3 11.42 (services/cryptexd.py auto_install_ddi, restore/tss.py
 * add_cryptex1_tags, remote/remotexpc.py send_file_transfer), every request on a fresh connection:
 *
 *   copy-installed            {}                              -> argv.remote-cryptex-array
 *   read-personalization-id   {}                              -> argv.img4_chip_{chip,ecid,cpro,...}
 *   get-nonce                 {nonce-domain-handle: u64}      -> argv.nonce (2-byte lead, nonce,
 *                                                                 u32le length at the end)
 *   TSS (gs.apple.com)        @Cryptex1,Ticket + Cryptex1,* + the personalized components'
 *                             digests                         -> Cryptex1,Ticket
 *   install                   {auth, client-version 3, cryptex1-properties, image-type-index 10,
 *                              nonce-persistence 1, persistence 2, image/trustcache/im4m/info/
 *                              volumehash as XPC FILE_TRANSFERs 1..5} then the five payloads on
 *                             their own HTTP/2 streams     -> {error: 0} or {cferr: {...}}
 *
 * Requests are {"routine": name, "argv": {...}}; failures come back as {"cferr": {cferr_domain,
 * cferr_code, cferr_userinfo}} or a non-zero Darwin errno in "error". */

#define CX_DDI_IDENTIFIER   "com.apple.MobileAsset.DDI"
#define CX_VARIANT_SUFFIX   "Developer Disk Image Cryptex"
#define CX_CLIENT_VERSION   3     /* what Xcode sends; = the service's advertised ServiceVersion */
#define CX_IMAGE_TYPE_GDMG  10    /* cryptex_asset_types index of Cryptex1,GenericDmg (iOS 26.4+) */
#define CX_PERSISTENCE      2     /* survives a reboot, as Xcode installs the DDI */
#define CX_NONCE_PERSISTENCE 1

/* The four payloads, by the manifest key that names each file, in install order (im4m, the ticket,
 * goes between the trust cache and the info plist on the wire). */
static const char *const cx_payload_keys[4] = {
    "Cryptex1,GenericDmg", "Cryptex1,GenericTrustCache", "Cryptex1,CryptexInfoPlist", "Cryptex1,GenericVolume",
};

/* Numbers in the manifest are integers, or hex strings ("0xFF10") for chip and product class. */
static int pnum(plist_t v, uint64_t *out)
{
    if (!v) return -1;
    plist_type t = plist_get_node_type(v);
    if (t == PLIST_STRING) {
        char *s = NULL; plist_get_string_val(v, &s);
        if (!s) return -1;
        char *end = NULL; *out = strtoull(s, &end, 0);
        int ok = end && end != s && !*end;
        free(s);
        return ok ? 0 : -1;
    }
    if (t == PLIST_BOOLEAN) { uint8_t b = 0; plist_get_bool_val(v, &b); *out = b; return 0; }
    if (t == PLIST_UINT) { plist_get_uint_val(v, out); return 0; }
    return -1;
}

static plist_t load_plist_file(const char *path)
{
    uint8_t *b; size_t n;
    if (read_file(path, &b, &n) != 0) return NULL;
    plist_t pl = NULL;
    plist_from_memory((const char *)b, (uint32_t)n, &pl, NULL);
    free(b);
    if (pl && plist_get_node_type(pl) != PLIST_DICT) { plist_free(pl); return NULL; }
    return pl;
}

/* The one identity describing the cryptex, out of the ~141 in the manifest. Borrowed. */
static plist_t cryptex_identity(plist_t manifest)
{
    plist_t ids = dget(manifest, "BuildIdentities");
    for (uint32_t i = 0; ids && plist_get_node_type(ids) == PLIST_ARRAY && i < plist_array_get_size(ids); i++) {
        plist_t bi = plist_array_get_item(ids, i);
        char v[160];
        if (dget_str(dget(bi, "Info"), "Variant", v, sizeof v) != 0) continue;
        size_t lv = strlen(v), ls = strlen(CX_VARIANT_SUFFIX);
        if (lv >= ls && !strcmp(v + lv - ls, CX_VARIANT_SUFFIX)) return bi;
    }
    return NULL;
}

/* <mdir>/<Info.Path of `key`>. The manifest is downloaded data, so a path that is absolute, names a
 * drive, or climbs out with ".." is refused rather than followed. */
static int payload_path(const char *mdir, plist_t bi, const char *key, char *out, size_t cap)
{
    char rel[512];
    if (dget_str(dget(dget(dget(bi, "Manifest"), key), "Info"), "Path", rel, sizeof rel) != 0) return -1;
    if (!rel[0] || rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':')) return -1;
    for (const char *p = rel; *p;) {
        const char *q = p;
        while (*q && *q != '/' && *q != '\\') q++;
        if (q - p == 2 && p[0] == '.' && p[1] == '.') return -1;
        p = *q ? q + 1 : q;
    }
    snprintf(out, cap, "%s/%s", mdir, rel);
    return 0;
}

/* Find a manifest with the cryptex identity and all four payloads beside it: <dir>/Cryptex (what
 * fetch-ddi writes -- the mirror's Cryptex files share names with the personalized ones, so they
 * get their own folder), then <dir>/Restore (Xcode's own iOS_DDI holds both variants in one
 * manifest). Returns the manifest (owned); *bi is borrowed from it. */
static plist_t find_cryptex(const char *given, char *mdir, size_t cap, plist_t *bi)
{
    char cand[DDI_MAX_CANDIDATES][DDI_PATH_MAX];
    int n = ddi_candidates(given, cand);
    static const char *const subs[] = { "Cryptex", "Restore" };
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 2; j++) {
            char dir[1200], mp[1300];
            snprintf(dir, sizeof dir, "%.1099s/%s", cand[i], subs[j]);
            snprintf(mp, sizeof mp, "%s/BuildManifest.plist", dir);
            struct stat st;
            if (stat(mp, &st) != 0) continue;
            plist_t m = load_plist_file(mp);
            plist_t id = m ? cryptex_identity(m) : NULL;
            int ok = id != NULL;
            for (int k = 0; ok && k < 4; k++) {
                char pp[1500];
                if (payload_path(dir, id, cx_payload_keys[k], pp, sizeof pp) != 0 || stat(pp, &st) != 0) {
                    if (j == 0) fprintf(stderr, "  %s: no usable %s\n", mp, cx_payload_keys[k]);
                    ok = 0;
                }
            }
            if (ok) { snprintf(mdir, cap, "%s", dir); *bi = id; return m; }
            if (m) plist_free(m);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ cryptexd over RemoteXPC */

static long cx_io_read(void *ctx, void *buf, size_t len) { return tun_read(*(int *)ctx, buf, len); }
static long cx_io_write(void *ctx, const void *buf, size_t len) { return tun_write(*(int *)ctx, buf, len); }

typedef struct { int fd; rp_rxpc_session s; } cx_conn;
/* Replies are small; requests are a few hundred bytes. The payloads are sent from their own
 * buffers by rp_rxpc_send_file and never copied in here. */
static uint8_t cx_reassembly[1 << 19], cx_raw[1 << 17];
/* The last cryptexd failure, for the caller to classify (a locked phone). */
static char cx_last_error[512];

static int cx_open(cx_conn *c, const char *addr, long port, int timeout_ms)
{
    c->fd = tun_connect(addr, (int)port, 15);
    if (c->fd < 0) { fprintf(stderr, "  cannot connect to cryptexd at [%s]:%ld\n", addr, port); return -1; }
    /* Never block forever: a daemon that dies mid-request would otherwise hang Layer 3c, and with
     * it the whole engine. */
    if (usernet_owns(c->fd)) usernet_set_recv_timeout_ms(c->fd, timeout_ms);
    else {
#ifdef _WIN32
        DWORD ms = (DWORD)timeout_ms;
        setsockopt((SOCKET)c->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    }
    rp_rxpc_io io = { cx_io_read, cx_io_write, &c->fd };
    rp_rxpc_init(&c->s, io, cx_reassembly, sizeof cx_reassembly, cx_raw, sizeof cx_raw);
    if (rp_rxpc_handshake(&c->s) != 0) {
        fprintf(stderr, "  cryptexd: RemoteXPC handshake failed\n");
        tun_close(c->fd);
        return -1;
    }
    return 0;
}

static const char *darwin_errno_name(uint64_t e)
{
    switch (e) {
    case 1: return "EPERM"; case 2: return "ENOENT"; case 5: return "EIO"; case 13: return "EACCES";
    case 16: return "EBUSY"; case 17: return "EEXIST"; case 22: return "EINVAL"; case 28: return "ENOSPC";
    case 45: return "ENOTSUP"; case 70: return "ESTALE"; case 79: return "EFTYPE"; case 80: return "EAUTH";
    case 89: return "ECANCELED"; default: return "?";
    }
}

/* Check a reply; on success *argv is its "argv" (size 0 when absent). Prints the daemon's own words
 * on failure -- its NSLocalizedDescription and every other string in cferr_userinfo. */
static int cx_check(const char *routine, const rp_xpc_obj *reply, rp_xpc_obj *argv)
{
    rp_xpc_obj cferr, v, ui;
    if (rp_xpc_dict_get(reply, "cferr", &cferr) == 0) {
        const char *desc = NULL, *domain = NULL;
        uint64_t code = 0;
        int have_ui = rp_xpc_dict_get(&cferr, "cferr_userinfo", &ui) == 0;
        if (have_ui && rp_xpc_dict_get(&ui, "NSLocalizedDescription", &v) == 0) rp_xpc_get_string(&v, &desc);
        if (rp_xpc_dict_get(&cferr, "cferr_domain", &v) == 0) rp_xpc_get_string(&v, &domain);
        if (rp_xpc_dict_get(&cferr, "cferr_code", &v) == 0) rp_xpc_get_uint64(&v, &code);
        snprintf(cx_last_error, sizeof cx_last_error, "%s (%s: %lld)", desc ? desc : "(no description)",
                 domain ? domain : "?", (long long)code);
        fprintf(stderr, "  cryptexd %s failed: %s\n", routine, cx_last_error);
        if (have_ui) {
            size_t cur = 0; const char *k; rp_xpc_obj e;
            while (rp_xpc_dict_next(&ui, &cur, &k, &e) == 0) {
                const char *sv = NULL;
                if (strcmp(k, "NSLocalizedDescription") && rp_xpc_get_string(&e, &sv) == 0)
                    fprintf(stderr, "    %s: %s\n", k, sv);
            }
        }
        return -1;
    }
    uint64_t e = 0;
    if (rp_xpc_dict_get(reply, "error", &v) == 0 && rp_xpc_get_uint64(&v, &e) == 0 && e) {
        snprintf(cx_last_error, sizeof cx_last_error, "Darwin errno %llu (%s)", (unsigned long long)e, darwin_errno_name(e));
        fprintf(stderr, "  cryptexd %s failed: %s\n", routine, cx_last_error);
        return -1;
    }
    if (argv && rp_xpc_dict_get(reply, "argv", argv) != 0) { argv->data = NULL; argv->size = 0; }
    return 0;
}

/* One routine on a fresh connection, as pymobiledevice3's invoke does. The argv it returns points
 * into cx_reassembly: read it before the next call. `nonce_handle` >= 0 adds the get-nonce
 * selector; otherwise argv is empty. */
static int cx_invoke(const char *addr, long port, const char *routine, long long nonce_handle, rp_xpc_obj *argv)
{
    cx_conn c;
    if (cx_open(&c, addr, port, 30000) != 0) return -1;
    uint8_t body[512];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_string(&w, "routine", routine);
    rp_xpc_key(&w, "argv");
    rp_xpc_dict_begin(&w);
    if (nonce_handle >= 0) rp_xpc_set_uint64(&w, "nonce-domain-handle", (uint64_t)nonce_handle);
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);
    rp_xpc_obj reply;
    int rc = -1;
    if (w.overflow || rp_rxpc_send(&c.s, body, w.len, 1) != 0)
        fprintf(stderr, "  cryptexd %s: could not send the request\n", routine);
    else if (rp_rxpc_recv(&c.s, &reply) != 0)
        fprintf(stderr, "  cryptexd %s: no reply\n", routine);
    else
        rc = cx_check(routine, &reply, argv);
    tun_close(c.fd);
    return rc;
}

/* 1 when the DDI cryptex is installed (its version in `ver`), 0 when not, -1 on error. */
static int cx_ddi_installed(const char *addr, long port, char *ver, size_t cap)
{
    rp_xpc_obj argv, arr, e, v;
    if (cx_invoke(addr, port, "copy-installed", -1, &argv) != 0) return -1;
    if (!argv.size || rp_xpc_dict_get(&argv, "remote-cryptex-array", &arr) != 0) return 0;
    size_t cur = 0;
    while (rp_xpc_array_next(&arr, &cur, &e) == 0) {
        const char *id = NULL, *vs = NULL;
        if (rp_xpc_dict_get(&e, "remote-cryptex-identifier", &v) != 0 || rp_xpc_get_string(&v, &id) != 0) continue;
        if (strcmp(id, CX_DDI_IDENTIFIER)) continue;
        if (rp_xpc_dict_get(&e, "remote-cryptex-version", &v) == 0) rp_xpc_get_string(&v, &vs);
        snprintf(ver, cap, "%s", vs ? vs : "?");
        return 1;
    }
    return 0;
}

static int xget_num(const rp_xpc_obj *d, const char *k, uint64_t *out)
{
    rp_xpc_obj v;
    bool b;
    if (rp_xpc_dict_get(d, k, &v) != 0) return -1;
    if (rp_xpc_get_uint64(&v, out) == 0) return 0;
    if (rp_xpc_get_bool(&v, &b) == 0) { *out = b; return 0; }
    return -1;
}

/* ------------------------------------------------------------------ the Cryptex1 TSS request */

static void dset_uint(plist_t d, const char *k, uint64_t v) { plist_dict_set_item(d, k, plist_new_uint(v)); }

static plist_t build_cryptex_tss(plist_t bi, uint64_t chip, uint64_t ecid, int production,
                                 const uint8_t *nonce, size_t nlen)
{
    plist_t req = plist_new_dict();
    dset_str(req, "@HostPlatformInfo", "mac");
    dset_str(req, "@VersionInfo", "libauthinstall-1104.0.9");
    {
        uint8_t u[16];
        if (RAND_bytes(u, sizeof u) != 1) for (int i = 0; i < 16; i++) u[i] = (uint8_t)rand();
        u[6] = (uint8_t)((u[6] & 0x0F) | 0x40); u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
        char us[40];
        snprintf(us, sizeof us, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        dset_str(req, "@UUID", us);
    }
    dset_bool(req, "@Cryptex1,Ticket", 1);
    uint64_t n = 0;
    if (pnum(dget(bi, "Cryptex1,ChipID"), &n) == 0) dset_uint(req, "Cryptex1,ChipID", n);
    if (pnum(dget(bi, "Cryptex1,Type"), &n) == 0) dset_uint(req, "Cryptex1,Type", n);
    if (pnum(dget(bi, "Cryptex1,SubType"), &n) == 0) dset_uint(req, "Cryptex1,SubType", n);
    if (pnum(dget(bi, "Cryptex1,ProductClass"), &n) == 0) dset_uint(req, "Cryptex1,ProductClass", n);
    { plist_t v = dget(bi, "Cryptex1,UseProductClass"); if (v) dset_copy(req, "Cryptex1,UseProductClass", v); }
    if (pnum(dget(bi, "Cryptex1,NonceDomain"), &n) == 0) dset_uint(req, "Cryptex1,NonceDomain", n);
    { plist_t v = dget(bi, "Cryptex1,Version"); if (v) dset_copy(req, "Cryptex1,Version", v); }
    { plist_t v = dget(bi, "Cryptex1,PreauthorizationVersion"); if (v) dset_copy(req, "Cryptex1,PreauthorizationVersion", v); }
    dset_data(req, "Cryptex1,Nonce", nonce, nlen);
    dset_bool(req, "Cryptex1,ProductionMode", production);
    /* Cryptex1,UDID: chip id then ECID, each 8 bytes big-endian. The device is identified by this
     * alone -- none of the Ap* tags of an AP request appear. */
    uint8_t udid[16];
    for (int i = 0; i < 8; i++) { udid[i] = (uint8_t)(chip >> (56 - 8 * i)); udid[8 + i] = (uint8_t)(ecid >> (56 - 8 * i)); }
    dset_data(req, "Cryptex1,UDID", udid, sizeof udid);
    dset_data(req, "Cryptex1,UniqueTagList", (const uint8_t *)"", 0);

    /* Each personalized Cryptex1 component contributes only its Digest. The generic DMG is
     * Personalize: false and stays out. */
    plist_t man = dget(bi, "Manifest");
    plist_dict_iter it = NULL;
    if (man) plist_dict_new_iter(man, &it);
    char *k = NULL;
    plist_t e = NULL;
    while (man) {
        plist_dict_next_item(man, it, &k, &e);
        if (!k) break;
        if (!strncmp(k, "Cryptex1,", 9) && node_is_true(dget(dget(e, "Info"), "Personalize")) && dget(e, "Digest")) {
            plist_t d = plist_new_dict();
            dset_copy(d, "Digest", dget(e, "Digest"));
            plist_dict_set_item(req, k, d);
        }
        free(k); k = NULL;
    }
    free(it);
    return req;
}

/* ------------------------------------------------------------------ install */

static int cx_install(const char *addr, long port, plist_t bi,
                      const uint8_t *const data[5], const size_t len[5])
{
    static const char *const names[5] = { "image", "trustcache", "im4m", "info", "volumehash" };
    cx_conn c;
    /* The reply comes once the daemon has checked a 16 MB image; give it time. */
    if (cx_open(&c, addr, port, 180000) != 0) return -1;

    uint8_t body[2048];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_string(&w, "routine", "install");
    rp_xpc_key(&w, "argv");
    rp_xpc_dict_begin(&w);
    rp_xpc_set_uint64(&w, "auth", 0);
    rp_xpc_set_uint64(&w, "client-version", CX_CLIENT_VERSION);
    /* The Cryptex1,* parameters, as Xcode sends them. The integers must be uint64: int64 gets
     * "Cryptex1,NonceDomain [79: Inappropriate file type or format]". */
    rp_xpc_key(&w, "cryptex1-properties");
    rp_xpc_dict_begin(&w);
    rp_xpc_set_bool(&w, "Cryptex1,UseProductClass", node_is_true(dget(bi, "Cryptex1,UseProductClass")));
    rp_xpc_set_bool(&w, "MountedCryptex", false);
    uint64_t sub = 0, dom = 0;
    pnum(dget(bi, "Cryptex1,SubType"), &sub);
    pnum(dget(bi, "Cryptex1,NonceDomain"), &dom);
    rp_xpc_set_uint64(&w, "Cryptex1,SubType", sub);
    rp_xpc_set_uint64(&w, "Cryptex1,NonceDomain", dom);
    char ver[128] = "", pre[128] = "";
    dget_str(bi, "Cryptex1,Version", ver, sizeof ver);
    dget_str(bi, "Cryptex1,PreauthorizationVersion", pre, sizeof pre);
    rp_xpc_set_string(&w, "Cryptex1,Version", ver);
    rp_xpc_set_string(&w, "Cryptex1,PreauthVersion", pre);
    rp_xpc_dict_end(&w);
    rp_xpc_set_int64(&w, "image-type-index", CX_IMAGE_TYPE_GDMG);
    rp_xpc_set_uint64(&w, "nonce-persistence", CX_NONCE_PERSISTENCE);
    rp_xpc_set_uint64(&w, "persistence", CX_PERSISTENCE);
    for (int i = 0; i < 5; i++) { rp_xpc_key(&w, names[i]); rp_xpc_file_transfer(&w, (uint64_t)(i + 1), len[i]); }
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);

    int rc = -1;
    rp_xpc_obj reply;
    if (w.overflow || rp_rxpc_send(&c.s, body, w.len, 1) != 0) {
        fprintf(stderr, "  cryptexd install: could not send the request\n");
        goto out;
    }
    for (int i = 0; i < 5; i++) {
        if (rp_rxpc_send_file(&c.s, (uint64_t)(i + 1), data[i], len[i]) != 0) {
            fprintf(stderr, "  cryptexd install: sending %s (%zu bytes) failed\n", names[i], len[i]);
            goto out;
        }
    }
    fprintf(stderr, "  sent image %zu, trustcache %zu, ticket %zu, info %zu, volume hash %zu bytes; waiting for cryptexd\n",
            len[0], len[1], len[2], len[3], len[4]);
    if (rp_rxpc_recv(&c.s, &reply) != 0) {
        snprintf(cx_last_error, sizeof cx_last_error, "closed the connection without replying");
        fprintf(stderr, "  cryptexd install: the daemon closed the connection without replying -- it rejected "
                        "or crashed on the request (the phone's syslog, process cryptexd, says why)\n");
        goto out;
    }
    rc = cx_check("install", &reply, NULL);
out:
    tun_close(c.fd);
    return rc;
}

/* ------------------------------------------------------------------ the whole Cryptex flow */

static int contains_ci(const char *h, const char *n)
{
    for (; *h; h++) {
        size_t i = 0;
        while (n[i] && h[i] && ((h[i] | 0x20) == (n[i] | 0x20))) i++;
        if (!n[i]) return 1;
    }
    return 0;
}

int cdhost_ddi_install_cryptex(const char *addr, long cryptexd_port, long mounter_port, const char *ddi_dir_in)
{
    if (!cryptexd_port) return RP_DDI_NO_SERVICE;
    cx_last_error[0] = 0;

    /* Already there? A personalized image from the image mounter holds /System/Developer too, and
     * installing over it fails with "mkdir custom mount path: /System/Developer [17: File exists]". */
    if (mounter_port && cdhost_ddi_is_mounted(addr, mounter_port)) {
        fprintf(stderr, "  a personalized DDI is mounted (image mounter)\n");
        return RP_DDI_ALREADY;
    }
    char have[128] = "";
    int inst = cx_ddi_installed(addr, cryptexd_port, have, sizeof have);
    if (inst == 1) { fprintf(stderr, "  the %s cryptex is installed (version %s)\n", CX_DDI_IDENTIFIER, have); return RP_DDI_ALREADY; }
    if (inst < 0) { fprintf(stderr, "  could not ask cryptexd what is installed\n"); return RP_DDI_ERR; }

    char mdir[1200];
    plist_t bi = NULL;
    plist_t manifest = find_cryptex(ddi_dir_in, mdir, sizeof mdir, &bi);
    if (!manifest) {
#ifdef _WIN32
        fprintf(stderr, "  Cryptex DDI files not found: run scripts\\fetch-ddi.ps1 (they go in "
                        "%%LOCALAPPDATA%%\\rplayhub\\iOS_DDI\\Cryptex), or set RPLAY_DDI\n");
#else
        fprintf(stderr, "  Cryptex DDI files not found: run scripts/fetch-ddi.sh (they go in "
                        "~/.local/share/rplayhub/iOS_DDI/Cryptex), or set RPLAY_DDI\n");
#endif
        return RP_DDI_NO_DDI;
    }
    { char build[64] = ""; dget_str(manifest, "ProductBuildVersion", build, sizeof build);
      fprintf(stderr, "  Cryptex DDI %s from %s\n", build[0] ? build : "(unknown build)", mdir); }

    int rc = RP_DDI_ERR;
    uint8_t *files[4] = { NULL, NULL, NULL, NULL };
    size_t flen[4] = { 0, 0, 0, 0 };
    uint8_t *ticket = NULL;
    size_t tlen = 0;
    for (int k = 0; k < 4; k++) {
        char pp[1500];
        if (payload_path(mdir, bi, cx_payload_keys[k], pp, sizeof pp) != 0 || read_file(pp, &files[k], &flen[k]) != 0) {
            fprintf(stderr, "  could not read %s (%s)\n", cx_payload_keys[k], pp);
            rc = RP_DDI_NO_DDI;
            goto done;
        }
    }

    /* The device's chip instance and the nonce of the identity's own domain. The selector is a
     * HANDLE (Cryptex1,NonceDomain, 4 for the DDI), not an index: index 4 is a different domain,
     * and a ticket for its nonce is refused at import with "Manifest no longer valid". */
    uint64_t chip = 0, ecid = 0, cpro = 0, board = 0, handle = 0;
    {
        rp_xpc_obj ids;
        if (cx_invoke(addr, cryptexd_port, "read-personalization-id", -1, &ids) != 0) goto done;
        if (xget_num(&ids, "img4_chip_chip", &chip) != 0 || xget_num(&ids, "img4_chip_ecid", &ecid) != 0) {
            fprintf(stderr, "  read-personalization-id: no img4_chip_chip/img4_chip_ecid in the answer\n");
            goto done;
        }
        xget_num(&ids, "img4_chip_cpro", &cpro);
        xget_num(&ids, "img4_chip_bord", &board);
        fprintf(stderr, "  chip %#llx board %#llx ecid %#llx production %llu\n", (unsigned long long)chip,
                (unsigned long long)board, (unsigned long long)ecid, (unsigned long long)cpro);
    }
    if (pnum(dget(bi, "Cryptex1,NonceDomain"), &handle) != 0) { fprintf(stderr, "  manifest has no Cryptex1,NonceDomain\n"); goto done; }
    uint8_t nonce[128];
    size_t nlen = 0;
    {
        rp_xpc_obj argv, nv;
        const uint8_t *blob = NULL;
        size_t blen = 0;
        if (cx_invoke(addr, cryptexd_port, "get-nonce", (long long)handle, &argv) != 0) goto done;
        if (rp_xpc_dict_get(&argv, "nonce", &nv) != 0 || rp_xpc_get_data(&nv, &blob, &blen) != 0 || blen < 6) {
            fprintf(stderr, "  get-nonce: no nonce in the answer\n");
            goto done;
        }
        /* A 2-byte lead, the nonce, then its length as a little-endian u32 at the very end. */
        size_t inner = (size_t)blob[blen - 4] | ((size_t)blob[blen - 3] << 8) | ((size_t)blob[blen - 2] << 16) | ((size_t)blob[blen - 1] << 24);
        if (inner == 0 || inner > sizeof nonce || 2 + inner > blen - 4) {
            fprintf(stderr, "  get-nonce: unexpected nonce structure (%zu bytes, inner length %zu)\n", blen, inner);
            goto done;
        }
        memcpy(nonce, blob + 2, inner);
        nlen = inner;
        fprintf(stderr, "  cryptex nonce (domain handle %llu): %zu bytes\n", (unsigned long long)handle, nlen);
    }

    /* The ticket. */
    fprintf(stderr, "  requesting a Cryptex1 ticket from Apple...\n");
    {
        plist_t tss = build_cryptex_tss(bi, chip, ecid, cpro != 0, nonce, nlen);
        char *xml = NULL; uint32_t xlen = 0;
        plist_to_xml(tss, &xml, &xlen);
        plist_free(tss);
        const char *dump = getenv("RPLAY_TSS_DUMP");
        if (dump && *dump && xml) {
            FILE *f = fopen(dump, "wb");
            if (f) { fwrite(xml, 1, xlen, f); fclose(f); fprintf(stderr, "  (TSS request written to %s)\n", dump); }
        }
        const char *dry = getenv("RPLAY_TSS_DRYRUN");
        if (dry && *dry == '1') {
            fprintf(stderr, "  RPLAY_TSS_DRYRUN=1: not contacting Apple\n");
            if (xml) plist_mem_free(xml);
            rc = RP_DDI_TSS_FAILED;
            goto done;
        }
        size_t rlen = 0;
        char *resp = xml ? tss_post((const uint8_t *)xml, xlen, &rlen) : NULL;
        if (xml) plist_mem_free(xml);
        if (!resp) fprintf(stderr, "  could not reach Apple's signing server (gs.apple.com)\n");
        static const char *const cx_keys[] = { "Cryptex1,Ticket", "ApImg4Ticket", NULL };
        ticket = resp ? tss_extract_ticket(resp, rlen, &tlen, cx_keys) : NULL;
        free(resp);
        if (!ticket) { rc = RP_DDI_TSS_FAILED; goto done; }
        fprintf(stderr, "  got a %zu-byte Cryptex1 ticket\n", tlen);
    }

    /* Install: image, trust cache, ticket, info plist, volume hash -- transfer ids 1..5. */
    {
        const uint8_t *data[5] = { files[0], files[1], ticket, files[2], files[3] };
        const size_t len[5] = { flen[0], flen[1], tlen, flen[2], flen[3] };
        if (cx_install(addr, cryptexd_port, bi, data, len) != 0) {
            rc = contains_ci(cx_last_error, "lock") ? RP_DDI_LOCKED : RP_DDI_ERR;
            goto done;
        }
    }

    {
        char now[128] = "";
        int after = cx_ddi_installed(addr, cryptexd_port, now, sizeof now);
        if (after == 1) {
            fprintf(stderr, "  installed %s %s -- developer services are coming up\n", CX_DDI_IDENTIFIER, now);
            rc = RP_DDI_OK;
        } else {
            fprintf(stderr, "  install reported success but %s is not listed as installed\n", CX_DDI_IDENTIFIER);
        }
    }

done:
    for (int k = 0; k < 4; k++) free(files[k]);
    free(ticket);
    plist_free(manifest);
    return rc;
}

/* What pymobiledevice3's auto_mount does: from iOS 27 the Cryptex DDI over cryptexd (it is the only
 * variant newer phones can get signed); below it, or when the cryptex files are missing, the
 * personalized image over mobile_image_mounter. */
int cdhost_ddi_auto(const char *addr, long mounter_port, long cryptexd_port, const char *product_version,
                    const char *ddi_dir)
{
    int major = product_version ? atoi(product_version) : 0;
    if (cryptexd_port && major >= 27) {
        fprintf(stderr, "  iOS %s: installing the Cryptex DDI through cryptexd\n", product_version);
        int rc = cdhost_ddi_install_cryptex(addr, cryptexd_port, mounter_port, ddi_dir);
        if (rc != RP_DDI_NO_DDI || !mounter_port) return rc;
        fprintf(stderr, "  falling back to the personalized DDI (works only for phones in its manifest)\n");
    }
    if (!mounter_port) return RP_DDI_NO_SERVICE;
    return cdhost_ddi_activate(addr, mounter_port, ddi_dir);
}
