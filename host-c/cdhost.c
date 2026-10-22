// coredevice-c — from-scratch CoreDevice host in C, on the CarPlay SDK.
//
// Layers 0-2, the C port of scripts/coredevice-host/*.py, verified live against the same iPhone.
//   L0 usbmux · L1 lockdown · L1.5 TLS session (OpenSSL) · L2 CoreDevice tunnel (CDTunnel)
// Uses the SDK's CF plist API (CFLite = CoreFoundation-compatible) + OpenSSL (as the SDK's
// MFiClientPlatformOpenSSL does). The network RemotePairing door (SRP/ChaCha20/Curve25519 +
// BonjourBrowser) is the next module — see remotepairing.c.
//
// Build:  make            (macOS)      Run:  ./cdhost

#include "compat.h"
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#endif
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#else
#include <mmsystem.h>     /* timeBeginPeriod (winmm) */
static inline int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    return _putenv_s(name, value);
}
#endif
#include <pthread.h>
#include <signal.h>

#include "../core/rp_remotexpc.h"
#include "api_server.h"
#include "../core/rp_xpc.h"
#include "ddi.h"
#include "usernet.h"

#define USBMUX_TYPE_PLIST 8
#define CDTUNNEL_MAGIC "CDTunnel"

// ============================ connection (raw fd or TLS) ============================
/* `idev` is set for the CoreDevice tunnel connection, which now rides on libimobiledevice
 * (Stage 3); `fd`/`tls` are the old hand-rolled path, still used by the usbmux/lockdown code that
 * has not been migrated. Exactly one of {idev} or {fd,tls} is active per conn_t. */
typedef struct { int fd; rp_tls_conn *tls; void *idev; } conn_t;

static int cwrite(conn_t *c, const void *buf, size_t n) {
    if (c->idev) return imd_conn_send(c->idev, buf, n);
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->tls ? rp_tls_write(c->tls, (const char *)buf + off, n - off)
                           : send(c->fd, (const char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}
static int cread_n(conn_t *c, void *buf, size_t n) {
    if (c->idev) return imd_conn_recv(c->idev, buf, n);
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->tls ? rp_tls_read(c->tls, (char *)buf + off, n - off)
                           : recv(c->fd, (char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

// ============================ Layer 2: CoreDevice tunnel (CDTunnel) ============================
// naive extractors for the small JSON handshake response
static int json_str(const char *j, const char *key, char *out, size_t outlen) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return -1;
    size_t n = (size_t)(e - p); if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n); out[n] = 0; return 0;
}
static long json_int(const char *j, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? strtol(p + strlen(pat), NULL, 10) : -1;
}
static int tunnel_handshake_full(conn_t *c, char *addr, size_t addrlen,
                                 char *ours, size_t ourslen, long *rsd_port, long *mtu) {
    const char *req = "{\"type\":\"clientHandshakeRequest\",\"mtu\":16000}";
    uint16_t blen = htons((uint16_t)strlen(req));
    if (cwrite(c, CDTUNNEL_MAGIC, 8) < 0 || cwrite(c, &blen, 2) < 0 || cwrite(c, req, strlen(req)) < 0)
        return -1;
    uint8_t hdr[10];
    if (cread_n(c, hdr, sizeof hdr) < 0) return -1;
    uint16_t plen = ntohs(*(uint16_t *)(hdr + 8));
    char *body = malloc(plen + 1);
    if (cread_n(c, body, plen) < 0) { free(body); return -1; }
    body[plen] = 0;
    int rc = json_str(body, "serverAddress", addr, addrlen);
    /* "address" appears inside clientParameters, which comes first — that is our end. */
    if (ours) json_str(body, "address", ours, ourslen);
    *rsd_port = json_int(body, "serverRSDPort");
    if (mtu) *mtu = json_int(body, "mtu");
    free(body);
    return rc;
}

/* ---------------------------------------------------------------- Layer 3a: utun + pump
 *
 * The tunnel socket carries raw IPv6 packets. Put those on a utun with a route and the device's
 * RSD port becomes reachable with an ordinary socket — which is the whole point, and the only
 * step in the stack that needs root.
 *
 * This is the C port of host/rplayhub/net/{tun,pump}.py. The failure signalling matters: if
 * either direction dies the tunnel is dead, and saying so beats looking alive.
 */
#ifdef __APPLE__
#define UTUN_CONTROL_NAME "com.apple.net.utun_control"
#define UTUN_OPT_IFNAME 2
#define IPV6_HDR_LEN 40

static int utun_open(char *ifname, size_t ifname_len) {
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) return -1;
    struct ctl_info ci;
    memset(&ci, 0, sizeof ci);
    strlcpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof ci.ctl_name);
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) { close(fd); return -1; }

    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof sc);
    sc.sc_len = sizeof sc;
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = ci.ctl_id;
    sc.sc_unit = 0;                     /* 0 => the kernel picks a free utun number */
    if (connect(fd, (struct sockaddr *)&sc, sizeof sc) < 0) { close(fd); return -1; }

    socklen_t len = (socklen_t)ifname_len;
    if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &len) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int run_cmd(const char *fmt, ...) {
    char cmd[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

static int utun_configure(const char *ifname, const char *ours, const char *device, long mtu) {
    if (run_cmd("ifconfig %s inet6 %s prefixlen 64 up >/dev/null 2>&1", ifname, ours) != 0)
        return -1;
    if (mtu > 0) run_cmd("ifconfig %s mtu %ld >/dev/null 2>&1", ifname, mtu);
    /* Host route for the device end; without it nothing knows to use this interface. */
    run_cmd("route add -inet6 %s -interface %s >/dev/null 2>&1", device, ifname);
    return 0;
}

typedef struct {
    int     utun;
    conn_t *tun;
    volatile int failed;
    const char *reason;
    unsigned long tx, rx;
} pump_t;

/* When either pump thread fails, the CoreDevice tunnel is dead and cannot be recovered without
 * re-establishing it -- which needs a full restart. Rather than leave the daemon serving a
 * live-looking-but-dead session (bug #7: it keeps answering tunnel_info while nothing flows),
 * exit the process. The kernel destroys the utun on exit; the app's control connection drops and
 * it reconnects; under launchd the daemon auto-restarts. A clean drop beats a silent black. */
/* Include errno and the traffic counters. Every past round of "why did it quit this time" was a
 * guessing game because the message named a stage and nothing else: five call sites, one string
 * each, no way to tell a dropped connection from a transient error, and no idea whether the
 * tunnel had ever carried anything. */
static void pump_die(const char *reason) {
    int e = errno;
    fprintf(stderr, "\n  tunnel died: %s (errno %d: %s) -- exiting so the session does not go "
                    "stale (bug #7)\n", reason ? reason : "unknown", e, e ? strerror(e) : "none");
    _exit(1);
}

static void *pump_host_to_device(void *arg) {
    pump_t *p = arg;
    uint8_t buf[70000];
    for (;;) {
        ssize_t n = read(p->utun, buf, sizeof buf);
        /* A signal interrupting the read is not the tunnel dying. macOS's signal() asks for
         * SA_RESTART so this should not happen, but "should not" is what made the previous
         * unexplained exits so expensive: retrying costs nothing and removes the whole class. */
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 4) { pump_die("utun read ended"); }
        if (cwrite(p->tun, buf + 4, (size_t)(n - 4)) < 0) {   /* strip the 4-byte AF prefix */
            pump_die("tunnel write failed");
        }
        p->tx++;
    }
}

static void *pump_device_to_host(void *arg) {
    pump_t *p = arg;
    uint8_t frame[70000];
    uint32_t af = htonl(AF_INET6);
    for (;;) {
        /* Reframe from a byte stream: Payload Length covers everything after the fixed header. */
        if (cread_n(p->tun, frame + 4, IPV6_HDR_LEN) < 0) {
            pump_die("tunnel read ended");
        }
        uint16_t plen = ntohs(*(uint16_t *)(frame + 4 + 4));
        if (plen && cread_n(p->tun, frame + 4 + IPV6_HDR_LEN, plen) < 0) {
            pump_die("tunnel read ended mid-packet");
        }
        memcpy(frame, &af, 4);
        ssize_t w;
        do { w = write(p->utun, frame, 4 + IPV6_HDR_LEN + plen); }
        while (w < 0 && (errno == EINTR || errno == EAGAIN));
        if (w < 0) {
            pump_die("utun write failed");
        }
        p->rx++;
    }
}
#endif /* __APPLE__ — the kernel-utun path; other platforms use RPLAY_USERSPACE_NET=1 (lwIP) */

/* Defined below, at Layer 3b; the attach path probes candidate ports with it. */
static int rsd_enumerate(const char *addr, long port, api_session *out);

/* ------------------------------------------------ attaching to an EXISTING CoreDevice tunnel
 *
 * Why this exists. Layers 0-2 start at usbmux, so no usbmux entry means no session -- and a phone
 * can be perfectly reachable with no usbmux entry at all. Device Hub reaches it over CoreDevice
 * RemotePairing, which never registers with usbmuxd: observed with usbmuxd reporting zero devices
 * while Device Hub had a live tunnel and the device answered ping over it in 6ms.
 *
 * The tunnel it builds is an ordinary routed utun, so anything on this machine can use it. Given
 * the device address and its RSD port, everything from Layer 3b onwards is unchanged -- it is all
 * plain sockets. So this finds those two facts and hands them over, skipping usbmux, the
 * CoreDeviceProxy handshake and the packet pump entirely.
 *
 * The catch is that the RSD port is chosen per session and only the app that built the tunnel is
 * told it, so it has to be found: scan for open ports, then attempt the RemoteXPC handshake on the
 * lowest few. Measured on two tunnels, RSD sits at the bottom of the open set (52722 with 52721
 * beside it; 51724 on one of ours) while the ~85 service ports cluster far above (56104+), so the
 * lowest handful is a short list, not a guess.
 */

/* Find a CoreDevice tunnel: a utun carrying a ULA (fd..) address with the 16000 MTU those use.
 * Writes our address; the device is the same prefix ending ::1. Returns 0 when one is found. */
static int find_coredevice_tunnel(char *ours, size_t ours_len, char *device, size_t device_len)
{
    FILE *f = popen("/sbin/ifconfig", "r");
    if (!f) return -1;
    char line[512], iface[64] = {0}, found_addr[128] = {0}, found_if[64] = {0};
    long mtu = 0;
    int have = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != '\t' && line[0] != ' ') {          /* new interface stanza */
            char *colon = strchr(line, ':');
            if (colon) {
                size_t n = (size_t)(colon - line);
                if (n >= sizeof iface) n = sizeof iface - 1;
                memcpy(iface, line, n); iface[n] = 0;
            }
            char *m = strstr(line, "mtu ");
            mtu = m ? strtol(m + 4, NULL, 10) : 0;
            continue;
        }
        if (strncmp(iface, "utun", 4) != 0) continue;
        char *inet6 = strstr(line, "inet6 ");
        if (!inet6) continue;
        char a[128] = {0};
        if (sscanf(inet6 + 6, "%127s", a) != 1) continue;
        if (strncmp(a, "fd", 2) != 0) continue;            /* ULA only, not link-local fe80 */
        if (mtu < 8000) continue;                          /* CoreDevice tunnels use 16000 */
        snprintf(found_addr, sizeof found_addr, "%s", a);
        snprintf(found_if, sizeof found_if, "%s", iface);
        have = 1;
        break;
    }
    pclose(f);
    if (!have) return -1;

    snprintf(ours, ours_len, "%s", found_addr);
    /* The device is ::1 on the same prefix; we are ::2. */
    char base[128];
    snprintf(base, sizeof base, "%s", found_addr);
    char *tail = strrchr(base, ':');
    if (!tail) return -1;
    *tail = 0;
    snprintf(device, device_len, "%s:1", base);
    printf("  found tunnel on %s: us=%s device=%s\n", found_if, ours, device);
    return 0;
}

/* Non-blocking connect sweep. Fills `out` with open ports, ascending, and returns how many. */
static int scan_open_ports(const char *host, int lo, int hi, int *out, int max)
{
#ifdef _WIN32
    (void)host; (void)lo; (void)hi; (void)out; (void)max;
    return 0;
#else
    int found = 0;
    const int BATCH = 256;
    for (int base = lo; base <= hi && found < max; base += BATCH) {
        int fds[BATCH], ports[BATCH], n = 0;
        for (int p = base; p < base + BATCH && p <= hi; p++) {
            int fd = socket(AF_INET6, SOCK_STREAM, 0);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, O_NONBLOCK);
            struct sockaddr_in6 sa;
            memset(&sa, 0, sizeof sa);
            sa.sin6_family = AF_INET6;
            sa.sin6_port = htons((uint16_t)p);
            if (inet_pton(AF_INET6, host, &sa.sin6_addr) != 1) { close(fd); continue; }
            connect(fd, (struct sockaddr *)&sa, sizeof sa);   /* EINPROGRESS expected */
            fds[n] = fd; ports[n] = p; n++;
        }
        struct pollfd pfd[BATCH];
        for (int i = 0; i < n; i++) { pfd[i].fd = fds[i]; pfd[i].events = POLLOUT; pfd[i].revents = 0; }
        poll(pfd, (nfds_t)n, 400);
        for (int i = 0; i < n; i++) {
            if (pfd[i].revents & POLLOUT) {
                int err = 0; socklen_t el = sizeof err;
                if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0
                    && found < max)
                    out[found++] = ports[i];
            }
            close(fds[i]);
        }
    }
    return found;
#endif
}

/* Bring up a session on a tunnel someone else built. Returns 0 and fills addr/rsd on success. */
static int attach_existing_tunnel(char *addr, size_t addr_len, char *ours, size_t ours_len,
                                  long *rsd_out)
{
    printf("\n== attaching to an existing CoreDevice tunnel ==\n");
    if (find_coredevice_tunnel(ours, ours_len, addr, addr_len) != 0) {
        fprintf(stderr, "  no CoreDevice tunnel found (open Device Hub or Xcode to create one)\n");
        return -1;
    }
    const char *rp = getenv("RPLAY_ATTACH_RSD");
    if (rp && *rp) {                                   /* skip the search when told the port */
        *rsd_out = strtol(rp, NULL, 10);
        printf("  RSD port %ld (from RPLAY_ATTACH_RSD)\n", *rsd_out);
        return 0;
    }
    int lo = 49152, hi = 57000;
    const char *r = getenv("RPLAY_ATTACH_SCAN");
    if (r && *r) sscanf(r, "%d-%d", &lo, &hi);
    printf("  scanning [%s]:%d-%d for the RSD port...\n", addr, lo, hi);
    int open_ports[64];
    int n = scan_open_ports(addr, lo, hi, open_ports, 64);
    printf("  %d open port(s)\n", n);
    /* RSD sits at the bottom of the open set; the service ports cluster well above it. */
    int tries = n < 8 ? n : 8;
    for (int i = 0; i < tries; i++) {
        api_session probe;
        memset(&probe, 0, sizeof probe);
        if (rsd_enumerate(addr, open_ports[i], &probe) > 0) {
            *rsd_out = open_ports[i];
            printf("  RSD found on port %ld\n", *rsd_out);
            return 0;
        }
    }
    fprintf(stderr, "  no RSD port answered; widen with RPLAY_ATTACH_SCAN=lo-hi "
                    "or set RPLAY_ATTACH_RSD\n");
    return -1;
}

/* Prove the routing works: an ordinary TCP connect to the device's RSD port. */
static int g_userspace;   /* fwd: set in main from RPLAY_USERSPACE_NET */
static int rsd_reachable(const char *device, long rsd_port) {
    if (g_userspace) {
        int s = usernet_connect(device, (int)rsd_port);
        if (s < 0) return -1;
        usernet_close(s);
        return 0;
    }
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)rsd_port);
    if (inet_pton(AF_INET6, device, &sa.sin6_addr) != 1) return -1;
    int fd = (int)socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef _WIN32
    DWORD ms = 5000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    sock_close(fd);
    return rc;
}


/* ---------------------------------------------------------------- Layer 3b: RSD over RemoteXPC
 *
 * The tunnel gives us an ordinary socket to the device; this is what makes it useful. One
 * RemoteXPC handshake to the RSD port returns the service map -- roughly 85 entries naming every
 * coredevice.* feature and the port it listens on. Nothing else can be reached without it.
 *
 * The session layer lives in ../core so the daemon and the ports share it; only the socket
 * plumbing is here.
 */
/* g_userspace (declared above): reach the device over lwIP (no root utun) instead of kernel
 * sockets routed through the utun. Read/write route by fd ownership (usernet_owns: the table of
 * live lwIP sockets usernet.c keeps), not by fd number. */
static long sock_read(void *ctx, void *buf, size_t len)
{
    int fd = *(int *)ctx;
    return usernet_owns(fd) ? usernet_read(fd, buf, len) : (long)recv(fd, buf, len, 0);
}

static long sock_write(void *ctx, const void *buf, size_t len)
{
    int fd = *(int *)ctx;
    return usernet_owns(fd) ? usernet_write(fd, buf, len) : (long)send(fd, buf, len, 0);
}

static int rsd_connect(const char *addr, long port)
{
    if (g_userspace) return usernet_connect(addr, (int)port);
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) return -1;
    int fd = (int)socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef _WIN32
    DWORD ms = 8000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { sock_close(fd); return -1; }
    return fd;
}

/* Ask RSD what the device offers, and print it. Returns the number of services, or -1. */
static int rsd_enumerate(const char *addr, long port, api_session *out)
{
    int fd = rsd_connect(addr, port);
    if (fd < 0) { fprintf(stderr, "  cannot connect to [%s]:%ld\n", addr, port); return -1; }

    static uint8_t reassembly[1 << 20];   /* the service map runs to tens of kilobytes */
    static uint8_t raw[1 << 16];
    rp_rxpc_session s;
    rp_rxpc_io io = { sock_read, sock_write, &fd };
    rp_rxpc_init(&s, io, reassembly, sizeof reassembly, raw, sizeof raw);

    if (rp_rxpc_handshake(&s) != 0) {
        fprintf(stderr, "  RemoteXPC handshake failed\n");
        tun_close(fd);
        return -1;
    }

    uint8_t body[512];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_string(&w, "MessageType", "Handshake");
    rp_xpc_set_uint64(&w, "MessagingProtocolVersion", 7);
    {
        /* A fixed UUID is fine: the device echoes it and does not key anything on the value. */
        static const uint8_t uuid[16] = {0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78,
                                         0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78};
        rp_xpc_set_uuid(&w, "UUID", uuid);
    }
    rp_xpc_key(&w, "Properties");
    rp_xpc_dict_begin(&w);
    rp_xpc_set_uint64(&w, "RemoteXPCVersionFlags", 0x0100000000000006ULL);
    rp_xpc_set_bool(&w, "SensitivePropertiesVisible", true);
    rp_xpc_dict_end(&w);
    rp_xpc_key(&w, "Services");
    rp_xpc_dict_begin(&w);
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);
    if (w.overflow) { fprintf(stderr, "  handshake message overflowed\n"); tun_close(fd); return -1; }

    if (rp_rxpc_send(&s, body, w.len, 0) != 0) {
        fprintf(stderr, "  could not send the RSD handshake\n");
        tun_close(fd);
        return -1;
    }

    /* The answer is whichever message carries Services or Properties; earlier ones are noise. */
    rp_xpc_obj peer, services;
    int replies = 0;
    for (;;) {
        if (rp_rxpc_recv(&s, &peer) != 0) {
            /* Say what came back before giving up. "No answer" and "an answer without the key we
             * wanted" are different faults and were previously indistinguishable. */
            fprintf(stderr, "  no RSD answer after %d message(s)\n", replies);
            tun_close(fd);
            return -1;
        }
        replies++;
        if (rp_xpc_dict_get(&peer, "Services", &services) == 0) break;
        if (replies <= 3) {
            const char *k = NULL;
            rp_xpc_obj v;
            size_t cursor = 0;
            fprintf(stderr, "  (message %d has keys:", replies);
            while (rp_xpc_dict_next(&peer, &cursor, &k, &v) == 0 && cursor <= 6)
                fprintf(stderr, " %s", k);
            fprintf(stderr, ")\n");
        }
        if (replies > 20) {
            fprintf(stderr, "  gave up after %d messages without a Services map\n", replies);
            tun_close(fd);
            return -1;
        }
    }

    int count = rp_xpc_dict_count(&services);
    printf("  %d services\n", count);

    /* RPLAY_DUMP_SERVICES=1 lists every advertised service and its port. The three below are
     * what mirroring needs, but the device offers far more -- pairing, restart, diagnostics,
     * app and process listing -- and there is no other way to see what this OS version
     * actually exposes. Names change between releases, so guessing them is not an option. */
    if (getenv("RPLAY_DUMP_SERVICES")) {
        size_t cursor = 0;
        const char *key = NULL;
        rp_xpc_obj entry;
        while (rp_xpc_dict_next(&services, &cursor, &key, &entry) == 0) {
            rp_xpc_obj portv;
            const char *ps = NULL;
            uint64_t p = 0;
            if (rp_xpc_dict_get(&entry, "Port", &portv) == 0) {
                if (rp_xpc_get_string(&portv, &ps) == 0) printf("    %-60s %s\n", key, ps);
                else if (rp_xpc_get_uint64(&portv, &p) == 0)
                    printf("    %-60s %llu\n", key, (unsigned long long)p);
                else printf("    %-60s (port unreadable)\n", key);
            } else {
                printf("    %-60s (no port)\n", key);
            }
        }
    }

    /* The three the mirroring product actually depends on, plus the Device Hub parity set:
     * diagnostics_relay (restart/shutdown/sleep) and syslog_relay are classic lockdown-style
     * services, appservice is CoreDevice like the first three. */
    static const char *want[] = {
        "com.apple.coredevice.screencaptureservice",
        "com.apple.coredevice.displayservice",
        "com.apple.coredevice.hid.universalhidservice",
        "com.apple.mobile.diagnostics_relay.shim.remote",
        "com.apple.coredevice.appservice",
        "com.apple.syslog_relay.shim.remote",
        "com.apple.mobile.installation_proxy.shim.remote",
        "com.apple.misagent.shim.remote",
        "com.apple.mobile.MCInstall.shim.remote",
        "com.apple.afc.shim.remote",
        "com.apple.crashreportcopymobile.shim.remote",
        "com.apple.crashreportmover.shim.remote",
        "com.apple.mobile.mobile_image_mounter.shim.remote",
        "com.apple.springboardservices.shim.remote",
        /* Settings-tab candidates: the appearance/accessibility actions live on one of these and
         * which one is not yet established (doc/COREDEVICE-ACTIONS.md). Resolved here so the
         * settings_probe API can try each without another RSD round trip. */
        "com.apple.coredevice.configuration",
        "com.apple.coredevice.devicecontrol",
        "com.apple.coredevice.deviceinfo",
        "com.apple.coredevice.locationservice",
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        rp_xpc_obj svc, portv;
        uint64_t p = 0;
        if (rp_xpc_dict_get(&services, want[i], &svc) == 0 &&
            rp_xpc_dict_get(&svc, "Port", &portv) == 0) {
            const char *ps = NULL;
            long resolved = 0;
            if (rp_xpc_get_string(&portv, &ps) == 0) { resolved = strtol(ps, NULL, 10);
                printf("    %-46s port %s\n", want[i], ps); }
            else if (rp_xpc_get_uint64(&portv, &p) == 0) { resolved = (long)p;
                printf("    %-46s port %llu\n", want[i], (unsigned long long)p); }
            if (out) {
                if (i == 0) out->screenshot_port = resolved;
                else if (i == 1) out->display_port = resolved;
                else if (i == 2) out->hid_port = resolved;
                else if (i == 3) out->diag_port = resolved;
                else if (i == 4) out->app_port = resolved;
                else if (i == 5) out->syslog_port = resolved;
                else if (i == 6) out->instproxy_port = resolved;
                else if (i == 7) out->misagent_port = resolved;
                else if (i == 8) out->mcinstall_port = resolved;
                else if (i == 9) out->afc_port = resolved;
                else if (i == 10) out->crashcopy_port = resolved;
                else if (i == 11) out->crashmover_port = resolved;
                else if (i == 12) out->mounter_port = resolved;
                else if (i == 13) out->sbservices_port = resolved;
                else if (i == 14) out->configuration_port = resolved;
                else if (i == 15) out->devicecontrol_port = resolved;
                else if (i == 16) out->deviceinfo_port = resolved;
                else out->location_port = resolved;
            }
        } else {
            printf("    %-46s MISSING\n", want[i]);
        }
    }

    tun_close(fd);
    return count;
}

/* Absolute path to this binary, captured at startup so a rebind can re-exec it. argv[0] is
 * relative when run as ./host-c/cdhost, and the working directory is not guaranteed later. */
static char g_self[PATH_MAX];

/* Every flag that takes a value, and the variable it sets.
 *
 * One table drives parsing, --help and the rebind re-exec together. Selecting a device in the app
 * re-executes this daemon, so a flag the rebind path does not know about is silently dropped at
 * that point and the session continues on the default -- which looks exactly like the device
 * ignoring the override. Adding a knob here carries it across a rebind by construction. */
static const struct { const char *flag, *env, *help; } g_flags[] = {
    { "--udid", "RPLAY_UDID",
      "which device to bind; a prefix is enough. Without it the USB device\n"
      "                    wins, which is wrong when the phone you want is on wifi." },
    { "--hevc-features", "RPLAY_HEVC_FEATURES",
      "override the offer's feature-list string. \"FLS;VRA:0;MVRA:0;RVRA1:0;SW:1;\"\n"
      "                    asks the encoder not to adapt resolution -- it does anyway, see\n"
      "                    doc/RVRA-AND-PORTABILITY.md." },
    { "--rctl", "RPLAY_RCTL",
      "target bitrate in bits/s to report in RTCP RCTL, 50 times a second.\n"
      "                    Default 6000000, the ceiling the device itself negotiates. \"0\"\n"
      "                    stops sending RCTL at all." },
    { "--max-bitrate", "RPLAY_MAX_BITRATE",
      "streamConfig TXMaxBitrate, in bits/s. Sent only when given, so the\n"
      "                    default offer stays byte-identical to Apple's." },
    { "--min-bitrate", "RPLAY_MIN_BITRATE", "streamConfig TXMinBitrate, in bits/s." },
};
#define N_FLAGS ((int)(sizeof g_flags / sizeof g_flags[0]))

/* The device the engine should bind while it is waiting for one. Seeded from --udid/RPLAY_UDID;
 * select_device replaces it while nothing is bound (see cdhost_rebind). Guarded by g_bind_mu,
 * which also covers the waiting loop's decision to start binding, so a selection either lands
 * before that decision or turns into a relaunch after it -- never lost in between. */
static pthread_mutex_t g_bind_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_want_udid[128];
static char g_binding_udid[64];    /* the device bring-up is working on (state BINDING) */
static char g_bound_udid[64];      /* the device that bring-up succeeded for */

/* Per-device backoff after a failed bring-up (also under g_bind_mu).
 *
 * A phone that cannot be bound -- locked, untrusted, too old, a pairing prompt nobody answers --
 * used to be retried every 5 s forever: it starved every other attached phone (it kept winning
 * the pick) and re-raised the Trust prompt each time. Now each failure doubles its wait (5, 10,
 * 20, 40, then 60 s), other devices are preferred meanwhile, and the slate is wiped by a
 * successful bind, by the device detaching from usbmux, or by the user selecting it. */
static struct { char udid[64]; int fails; time_t next; } g_backoff[API_MAX_DEVICES];

static int backoff_find(const char *udid)
{
    for (int i = 0; i < API_MAX_DEVICES; i++)
        if (g_backoff[i].udid[0] && !strcmp(g_backoff[i].udid, udid)) return i;
    return -1;
}

static void backoff_clear(const char *udid)
{
    int i = backoff_find(udid);
    if (i >= 0) memset(&g_backoff[i], 0, sizeof g_backoff[i]);
}

/* Record a failure; returns the seconds until the device is tried again. */
static int backoff_fail(const char *udid, int extra_fails)
{
    int i = backoff_find(udid);
    if (i < 0) {
        for (i = 0; i < API_MAX_DEVICES && g_backoff[i].udid[0]; i++) { }
        if (i == API_MAX_DEVICES) i = 0;           /* table full: reuse a slot; harmless */
        memset(&g_backoff[i], 0, sizeof g_backoff[i]);
        snprintf(g_backoff[i].udid, sizeof g_backoff[i].udid, "%s", udid);
    }
    g_backoff[i].fails += 1 + extra_fails;
    int f = g_backoff[i].fails;
    int delay = f >= 5 ? 60 : 5 << (f - 1);
    if (delay > 60) delay = 60;
    g_backoff[i].next = time(NULL) + delay;
    return delay;
}

static int backoff_blocked(const char *udid, time_t now)
{
    int i = backoff_find(udid);
    return i >= 0 && now < g_backoff[i].next;
}

/* A device that left usbmux and came back is a fresh start (replugged, rebooted, trusted now). */
static void backoff_forget_detached(const api_device *devs, int n)
{
    for (int i = 0; i < API_MAX_DEVICES; i++) {
        if (!g_backoff[i].udid[0]) continue;
        int present = 0;
        for (int k = 0; k < n; k++) if (!strcmp(devs[k].udid, g_backoff[i].udid)) { present = 1; break; }
        if (!present) memset(&g_backoff[i], 0, sizeof g_backoff[i]);
    }
}

int cdhost_binding_udid(char *out, size_t cap)
{
    int binding = 0;
    pthread_mutex_lock(&g_bind_mu);
    if (api_engine_state_get() == API_ENGINE_BINDING && g_binding_udid[0]) {
        snprintf(out, cap, "%s", g_binding_udid);
        binding = 1;
    }
    pthread_mutex_unlock(&g_bind_mu);
    return binding;
}

#ifdef _WIN32
/* Append one argument to a CreateProcess command line, quoted the way the MSVC/UCRT argv parser
 * (and CommandLineToArgvW) splits it back: backslashes are literal unless they precede a quote,
 * where they are doubled. A udid never needs it, but g_self can hold spaces ("Program Files"),
 * and an --hevc-features value carries ';' and could carry quotes. */
static int win_append_arg(char *cmd, size_t cap, size_t *o, const char *arg)
{
#define PUT(ch) do { if (*o + 1 >= cap) return -1; cmd[(*o)++] = (char)(ch); } while (0)
    if (*o) PUT(' ');
    if (*arg && !strpbrk(arg, " \t\n\v\"")) {
        for (const char *p = arg; *p; p++) PUT(*p);
        cmd[*o] = 0;
        return 0;
    }
    PUT('"');
    for (const char *p = arg; ; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < 2 * bs; i++) PUT('\\'); break; }
        if (*p == '"') { for (size_t i = 0; i < 2 * bs + 1; i++) PUT('\\'); PUT('"'); }
        else { for (size_t i = 0; i < bs; i++) PUT('\\'); PUT(*p); }
    }
    PUT('"');
    cmd[*o] = 0;
    return 0;
#undef PUT
}

/* Windows has no exec-in-place, so a rebind is: start the new engine SUSPENDED (so it cannot race
 * us for the ports), close every listening and streaming socket, let it run, and exit. Only the
 * standard handles are inherited -- an explicit handle list, so no socket (all inheritable by
 * default on Winsock) leaks into the child; the new image retries its binds for a while
 * (RPLAY_REBOUND=1) while this process finishes letting go of 9876/9877/9878. Returns only on
 * failure, with this engine still serving. */
static void relaunch_windows(char **args)
{
    static char cmd[32768];
    size_t o = 0;
    cmd[0] = 0;
    for (int i = 0; args[i]; i++)
        if (win_append_arg(cmd, sizeof cmd, &o, args[i]) != 0) { fprintf(stderr, "  rebind: command line too long\n"); return; }

    _putenv_s("RPLAY_REBOUND", "1");
    SetEnvironmentVariableA("RPLAY_REBOUND", "1");

    HANDLE std[3] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                      GetStdHandle(STD_ERROR_HANDLE) };
    HANDLE inherit[3];
    DWORD ninherit = 0;
    for (int i = 0; i < 3; i++) {
        HANDLE h = std[i];
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        int dup = 0;
        for (DWORD k = 0; k < ninherit; k++) if (inherit[k] == h) dup = 1;
        if (dup) continue;
        if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) continue;
        inherit[ninherit++] = h;
    }

    STARTUPINFOEXA si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    BOOL inherit_handles = FALSE;
    DWORD flags = CREATE_SUSPENDED;
    /* Launched by the GUI the engine has no console; without this its replacement would open a
     * visible console window on every rebind or tunnel restart. */
    if (!GetConsoleWindow()) flags |= CREATE_NO_WINDOW;
    if (ninherit) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, size);
        if (attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                      ninherit * sizeof(HANDLE), NULL, NULL)) {
            si.lpAttributeList = attrs;
            si.StartupInfo.cb = sizeof si;
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = std[0];
            si.StartupInfo.hStdOutput = std[1];
            si.StartupInfo.hStdError = std[2];
            inherit_handles = TRUE;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        } else if (attrs) {
            HeapFree(GetProcessHeap(), 0, attrs);
            attrs = NULL;
        }
    }
    if (!inherit_handles) si.StartupInfo.cb = sizeof si.StartupInfo;   /* plain STARTUPINFO, inherit nothing */

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(g_self, cmd, NULL, NULL, inherit_handles, flags, NULL, NULL,
                             &si.StartupInfo, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (attrs) { DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(), 0, attrs); }
    if (!ok) {
        fprintf(stderr, "  rebind: CreateProcess failed (error %lu); still serving this device\n",
                (unsigned long)err);
        return;
    }
    api_close_listeners();      /* free the ports before the new engine looks for them */
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
#endif

/* Start this engine afresh with the same flags, binding `udid` (NULL: whatever --udid /
 * RPLAY_UDID says, else the default choice). Does not return on success.
 *
 * Once only: the tunnel reader, the bring-up watchdog and a client's select_device can all ask
 * for a restart, possibly at the same moment, and two relaunches would share the command-line
 * buffer and start two engines racing for the ports. The first caller holds the lock for good
 * (success never returns); a second caller blocks until the process is replaced, or -- if the
 * first relaunch failed -- gets its own attempt. */
static pthread_mutex_t g_relaunch_mu = PTHREAD_MUTEX_INITIALIZER;

static void relaunch(const char *udid_in)
{
    if (!g_self[0]) return;
    pthread_mutex_lock(&g_relaunch_mu);

    char udid[128] = "";
    if (udid_in) snprintf(udid, sizeof udid, "%s", udid_in);

    char *args[2 * N_FLAGS + 6];
    int n = 0;
    args[n++] = g_self;
    if (udid[0]) {
        args[n++] = (char *)"--udid";
        args[n++] = udid;
    }
    for (int i = 0; i < N_FLAGS; i++) {
        if (udid[0] && !strcmp(g_flags[i].flag, "--udid")) continue;   /* the caller's choice wins */
        const char *v = getenv(g_flags[i].env);
        if (!v || !*v) continue;
        args[n++] = (char *)g_flags[i].flag;
        args[n++] = (char *)v;
    }
    if (getenv("RPLAY_DUMP_SERVICES")) args[n++] = (char *)"--dump-services";
    if (getenv("RPLAY_NO_WAIT") && getenv("RPLAY_NO_WAIT")[0] == '1') args[n++] = (char *)"--no-wait";
    args[n] = NULL;

    if (udid[0]) printf("\nrebinding to %s -- restarting\n", udid);
    else         printf("\nrestarting the engine\n");
    fflush(stdout);

#ifdef _WIN32
    relaunch_windows(args);
#else
    /* Close everything before exec, or the new image inherits it.
     *
     * File descriptors survive execv unless they carry FD_CLOEXEC, and none of ours do. The
     * listening sockets on 9876 and 9877 came through still bound, so the replacement process
     * failed to bind them ("Address already in use") and then ran on with no API server at all --
     * pumping packets, answering nothing. The utun and the tunnel socket came through too.
     *
     * Closing from 3 upward is blunt and exactly right here: the process is about to be replaced,
     * so there is nothing left to preserve, and stdin/stdout/stderr must stay for the new image
     * to report anything. */
    int maxfd = getdtablesize();
    if (maxfd < 3 || maxfd > 65536) maxfd = 4096;
    for (int fd = 3; fd < maxfd; fd++) close(fd);

    execv(g_self, args);
    /* Only reached if exec failed; the caller has already replied, so say why and carry on
     * serving the device we still have rather than dying silently. */
    perror("execv");
#endif
    pthread_mutex_unlock(&g_relaunch_mu);
}

/* Hand a value to the relaunched image through its environment (inherited by execv and by
 * CreateProcess with a NULL environment block alike). */
static void env_for_child(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
    SetEnvironmentVariableA(name, value);
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

void cdhost_rebind(const char *udid)
{
    if (!udid || !*udid) return;
    pthread_mutex_lock(&g_bind_mu);
    api_engine_state st = api_engine_state_get();
    /* Nothing bound yet: no tunnel or media session to tear down, so just tell the waiting loop
     * which device to bind -- no restart, and the API never blinks. An explicit choice also wipes
     * that device's failure backoff: the user is asking for it now. */
    if (st == API_ENGINE_WAITING) {
        snprintf(g_want_udid, sizeof g_want_udid, "%s", udid);
        backoff_clear(udid);
        pthread_mutex_unlock(&g_bind_mu);
        printf("  selected %s; binding it as soon as it is reachable\n", udid);
        return;
    }
    /* Already bringing exactly this device up: restarting would only throw that work away. */
    if (st == API_ENGINE_BINDING && !strcmp(g_binding_udid, udid)) {
        pthread_mutex_unlock(&g_bind_mu);
        return;
    }
    pthread_mutex_unlock(&g_bind_mu);
    relaunch(udid);
}

/* The userspace tunnel's reader saw the CoreDeviceProxy connection end: the phone was unplugged,
 * slept off wifi, or rebooted. The session cannot be revived in place (lwIP and the media state
 * are per tunnel), and leaving it up is bug #7 -- a daemon answering tunnel_info for a device that
 * is gone. So start over: the relaunched engine comes back up in the waiting state, answers the
 * GUI at once, and binds the phone again when it reappears.
 *
 * It is told WHICH phone: the one the user selected, else the one that was bound. Relaunching with
 * no preference would let the default pick (USB first) grab a different attached phone -- e.g. an
 * iOS 26 one on USB that cannot mirror -- in place of the one that just dropped. */
void cdhost_tunnel_died(const char *why)
{
    fprintf(stderr, "\n  tunnel died: %s -- restarting so the session does not go stale (bug #7)\n",
            why ? why : "connection ended");
    char pref[128];
    pthread_mutex_lock(&g_bind_mu);
    snprintf(pref, sizeof pref, "%s", g_want_udid[0] ? g_want_udid : g_bound_udid);
    pthread_mutex_unlock(&g_bind_mu);
    sleep(1);                    /* do not spin if the device drops again immediately */
    relaunch(pref[0] ? pref : NULL);
    _exit(1);                    /* the relaunch failed; exiting beats serving a dead session */
}

/* A libimobiledevice call in bring-up hung past the watchdog (imd.c). That thread cannot be
 * cancelled, but the process can start over: relaunch with the same preference, and tell the new
 * image that this device already failed (RPLAY_BACKOFF_UDID) so it tries any other attached phone
 * first instead of hanging on the same one again. The API is back within a second or two. Runs on
 * the watchdog thread, never in a signal handler. */
void cdhost_bringup_timed_out(void)
{
    char want[128], stuck[64];
    pthread_mutex_lock(&g_bind_mu);
    snprintf(want, sizeof want, "%s", g_want_udid);
    snprintf(stuck, sizeof stuck, "%s", g_binding_udid);
    pthread_mutex_unlock(&g_bind_mu);
    if (stuck[0]) env_for_child("RPLAY_BACKOFF_UDID", stuck);
    relaunch(want[0] ? want : NULL);
    _exit(2);                    /* the relaunch failed: exit as the watchdog always did */
}

/* Ctrl-C must end the daemon, every time.
 *
 * Default dispositions would do that, except that this process re-executes itself to switch
 * devices (cdhost_rebind) and an exec inherits both the signal mask and any SIG_IGN from the
 * image before it -- and once, a rebound daemon sat through Ctrl-C and a root `kill -INT`
 * (2026-08-23). Rather than argue about which ancestor ignored what, start from a known state:
 * unblock everything and own the terminating signals. The handler does not unwind: the kernel
 * releases the utun, the sockets and the threads on _exit, and there is nothing else to keep. */
static void on_terminate(int sig)
{
    static const char msg[] = "\n  signal received; cdhost exiting\n";
#ifdef _WIN32
    if (_write(2, msg, (unsigned)(sizeof msg - 1))) { /* ignore */ }
#else
    if (write(2, msg, sizeof msg - 1)) { /* ignore */ }
#endif
    _exit(128 + sig);
}

#ifdef _WIN32
/* Windows delivers Ctrl-C, Ctrl-Break and closing the console window as console control events,
 * on a thread of their own; the CRT's SIGINT emulation covers only the first. Handle all of them
 * the way SIGINT/SIGTERM are handled on POSIX -- say so and exit, leaving the OS to reclaim the
 * sockets. (A close/logoff/shutdown event kills the process shortly after the handler returns
 * anyway; exiting at once keeps the message and the exit code deterministic.) */
static BOOL WINAPI on_console_event(DWORD ev)
{
    switch (ev) {
    case CTRL_C_EVENT:        on_terminate(SIGINT);  return TRUE;
    case CTRL_BREAK_EVENT:    on_terminate(SIGTERM); return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: on_terminate(SIGTERM); return TRUE;
    default:                  return FALSE;
    }
}
#endif

static void own_signals(void)
{
#ifdef _WIN32
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);
    signal(SIGHUP, on_terminate);
    /* Ignore SIGPIPE here, at startup, not where the first socket is written.
     *
     * api_serve() sets this too, but that runs at Layer 4 -- after the packet pump threads have
     * been running since Layer 3a, through RSD enumeration and the DDI mount. A tunnel write to a
     * closed socket anywhere in that window raised SIGPIPE at its DEFAULT disposition and killed
     * the process instantly, with no "tunnel died" line and no other trace: a silent exit that
     * looks exactly like the bug #7 self-exit from outside but is not it. Disposition is
     * process-wide, so setting it once here covers every thread and every layer. */
    signal(SIGPIPE, SIG_IGN);
#endif
}

/* Wait until a device can be bound, then bring it up through the CoreDeviceProxy handshake.
 *
 * The control API is already serving while this runs, so a GUI started before the phone is
 * plugged in -- or on a machine without Apple's mux at all -- gets a live daemon that can say
 * which of those it is (list_devices: usbmuxd, engine_state) instead of a refused connection.
 * Polls usbmux every 2 s; a failed bring-up goes back to waiting (5 s) rather than exiting.
 * Returns 0 with the tunnel up, or -1 only under --no-wait. */
static int wait_and_bind(int no_wait, conn_t *tun, char *udid, size_t udidlen,
                         char *devname, size_t dnlen, char *prodver, size_t pvlen,
                         char *addr, size_t addrlen, char *ours, size_t ourslen,
                         long *rsd, long *mtu)
{
    printf("\n== Layer 0: usbmux (libusbmuxd) ==\n");
    int last_note = -1;          /* 0 = no usbmuxd, 1 = no device, 2 = no matching device, 3 = backing off */
    for (;;) {
        api_device devs[API_MAX_DEVICES];
        int n = usbmux_enumerate(devs, API_MAX_DEVICES);
        time_t now = time(NULL);

        char want[128], target[64] = "";
        int backing = 0;
        pthread_mutex_lock(&g_bind_mu);
        if (n >= 0) backoff_forget_detached(devs, n);
        snprintf(want, sizeof want, "%s", g_want_udid);
        /* Devices still backing off after a failed bring-up sit this round out, so another
         * attached phone gets its turn instead of the same failure repeating. */
        api_device elig[API_MAX_DEVICES];
        int ne = 0;
        for (int i = 0; i < n; i++) {
            if (backoff_blocked(devs[i].udid, now)) { backing++; continue; }
            elig[ne++] = devs[i];
        }
        int chosen = ne > 0 ? imd_pick_device(elig, ne, want) : -1;
        if (chosen >= 0) {
            snprintf(target, sizeof target, "%s", elig[chosen].udid);
            snprintf(g_binding_udid, sizeof g_binding_udid, "%s", target);
            api_set_engine_state(API_ENGINE_BINDING);
        }
        pthread_mutex_unlock(&g_bind_mu);

        if (chosen < 0) {
            int note = n < 0 ? 0 : n == 0 ? 1 : backing ? 3 : 2;
            if (note != last_note) {
                if (note == 0)
                    printf("  usbmuxd is not reachable -- %s\n  waiting (the API is up; list_devices "
                           "reports \"usbmuxd\":false)\n", USBMUXD_MISSING_MESSAGE);
                else if (note == 1)
                    printf("  no devices -- waiting for one to be attached (polling usbmux every 2 s)\n");
                else if (note == 3)
                    printf("  %d device(s) attached, %d backing off after a failed bring-up -- waiting\n",
                           n, backing);
                else
                    printf("  %d device(s) attached, none matching %s -- waiting\n", n, want);
                last_note = note;
            }
            if (no_wait) {
                fprintf(stderr, note == 0 ? "usbmuxd is not reachable\n" : "no devices\n");
                return -1;
            }
            sleep(2);
            continue;
        }
        last_note = -1;

        /* Bind exactly the device chosen above, by its full udid. */
        void *idev_conn = NULL;
        const char *failed = NULL;
        if (imd_bringup(target, &idev_conn, udid, udidlen, devname, dnlen, prodver, pvlen) != 0) {
            failed = "bring-up failed (lockdown/CoreDeviceProxy) -- is the device unlocked and "
                     "trusted, and on iOS 17 or later?";
        } else {
            tun->idev = idev_conn;
            if (tunnel_handshake_full(tun, addr, addrlen, ours, ourslen, rsd, mtu) != 0) {
                fprintf(stderr, "  tunnel handshake failed\n");
                imd_conn_close(idev_conn);
                tun->idev = NULL;
                failed = "CoreDeviceProxy tunnel handshake failed";
            }
        }
        if (!failed) {
            pthread_mutex_lock(&g_bind_mu);
            backoff_clear(target);
            snprintf(g_bound_udid, sizeof g_bound_udid, "%s", target);
            pthread_mutex_unlock(&g_bind_mu);
            printf("  handshake: us=%s device=%s serverRSDPort=%ld mtu=%ld\n", ours, addr, *rsd, *mtu);
            return 0;
        }

        pthread_mutex_lock(&g_bind_mu);
        int delay = backoff_fail(target, 0);
        g_binding_udid[0] = 0;
        api_set_engine_state(API_ENGINE_WAITING);
        pthread_mutex_unlock(&g_bind_mu);
        char msg[256];
        snprintf(msg, sizeof msg, "%s: %s", target, failed);
        api_set_engine_error(msg);
        if (no_wait) return -1;
        printf("  %s\n  back to waiting; this device is retried in %d s (others first)\n", msg, delay);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: a daemon's layer log should appear live */
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    /* 1 ms timer resolution. The default 15.6 ms tick rounds every short sleep up to it, which
     * coarsens swipe pacing (a 20-step gesture over 300 ms) and the RTCP timers. */
    timeBeginPeriod(1);
#endif
    own_signals();

    /* Flags rather than environment only, because sudo strips the environment.
     *
     * `RPLAY_UDID=... sudo ./cdhost` sets the variable for sudo, which then discards it, so the
     * daemon binds whatever it would have anyway -- silently, and three runs were lost to that
     * before anyone noticed. `sudo env VAR=... ./cdhost` works, but a flag cannot be got wrong.
     * The environment variables still work for anything already using them. */
    /* The absolute path to THIS executable, for the device-switch re-exec (cdhost_rebind).
     * argv[0] is unreliable under launchd -- the SMAppService daemon is started with the relative
     * BundleProgram path "Contents/MacOS/cdhost" and an unknown CWD, so realpath(argv[0]) fails and
     * execv then gets "No such file or directory". _NSGetExecutablePath always returns the real
     * path. */
    {
#ifdef _WIN32
        if (!GetModuleFileNameA(NULL, g_self, sizeof(g_self))) {
            snprintf(g_self, sizeof g_self, "%s", argv[0]);
        }
#elif defined(__APPLE__)
        char raw[PATH_MAX];
        uint32_t sz = sizeof raw;
        if (_NSGetExecutablePath(raw, &sz) == 0 && realpath(raw, g_self)) {
            /* got it */
        } else if (!realpath(argv[0], g_self)) {
            snprintf(g_self, sizeof g_self, "%s", argv[0]);
        }
#else
        /* Linux: /proc/self/exe is the same always-real path the dyld call provides on macOS. */
        if (!realpath("/proc/self/exe", g_self)) {
            if (!realpath(argv[0], g_self)) snprintf(g_self, sizeof g_self, "%s", argv[0]);
        }
#endif
    }

    for (int i = 1; i < argc; i++) {
        int matched = 0;
        for (int f = 0; f < N_FLAGS; f++) {
            if (strcmp(argv[i], g_flags[f].flag)) continue;
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", g_flags[f].flag);
                return 2;
            }
            setenv(g_flags[f].env, argv[++i], 1);
            matched = 1;
            break;
        }
        if (matched) continue;
        if (!strcmp(argv[i], "--dump-services")) {
            setenv("RPLAY_DUMP_SERVICES", "1", 1);
        } else if (!strcmp(argv[i], "--no-wait")) {
            setenv("RPLAY_NO_WAIT", "1", 1);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: cdhost [--dump-services] [--no-wait]");
            for (int f = 0; f < N_FLAGS; f++) printf(" [%s <value>]", g_flags[f].flag);
            printf("\n\n");
            for (int f = 0; f < N_FLAGS; f++)
                printf("  %-17s %s\n", g_flags[f].flag, g_flags[f].help);
            printf("  %-17s %s\n", "--dump-services",
                   "print every service RSD advertises, with ports.");
            printf("  %-17s %s\n", "--no-wait",
                   "exit if no device can be bound, instead of serving the API and\n"
                   "                    waiting for one (the old behaviour; for scripts).");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    /* Say what is overridden, every time. A run whose override did not apply looks exactly like a
     * device ignoring it, and that mistake has already cost one wrong conclusion here. */
    for (int f = 0; f < N_FLAGS; f++) {
        const char *v = getenv(g_flags[f].env);
        if (v && *v) printf("%s = %s\n", g_flags[f].env, v);
    }
    {
        const char *w = getenv("RPLAY_UDID");
        snprintf(g_want_udid, sizeof g_want_udid, "%s", w ? w : "");
        /* Set by the bring-up watchdog before it relaunched: that device hung, so start it in
         * backoff and let any other attached phone go first. Consumed here, not inherited further. */
        const char *b = getenv("RPLAY_BACKOFF_UDID");
        if (b && *b) {
            int d = backoff_fail(b, 1);
            printf("  %s hung the previous bring-up; trying it again in %d s, other devices first\n", b, d);
            env_for_child("RPLAY_BACKOFF_UDID", NULL);
        }
    }
    const char *nw = getenv("RPLAY_NO_WAIT");
    int no_wait = nw && nw[0] == '1';

    /* Layers 0-2 now run on libimobiledevice (host-c/imd.c): usbmux discovery, the pair-record
     * TLS session, device queries and StartService(CoreDeviceProxy), returning a connected TLS'd
     * channel. This is the piece that did not port -- the usbmux unix socket and lockdown's TLS
     * are macOS-specific; libusbmuxd/libimobiledevice speak the same protocols on Linux/Windows.
     * The CoreDeviceProxy handshake and the packet pump below are unchanged: they run over conn_t,
     * which now wraps the idevice connection. */
    static char udid[128] = {0}, devname[256] = "?", prodver[64] = "?";
    conn_t tun = { -1, NULL, NULL };
    char addr[128] = "?", ours[128] = "?"; long rsd = -1, mtu = 0;

    /* Attach to a tunnel someone else already built, instead of building one over usbmux.
     *
     * Opt in with RPLAY_ATTACH_TUNNEL=1. This is the path for a device that is reachable but has
     * no usbmux entry -- a phone paired to Device Hub over CoreDevice RemotePairing, which never
     * registers with usbmuxd. Layers 0-3a are skipped wholesale: the tunnel exists, it is routed,
     * and everything downstream is plain sockets over it. */
    int want_attach = getenv("RPLAY_ATTACH_TUNNEL") && getenv("RPLAY_ATTACH_TUNNEL")[0] == '1';
#ifdef _WIN32
    if (want_attach) {
        /* It finds the tunnel by parsing `ifconfig` for a utun and scans it with kernel sockets:
         * there is no such interface on Windows -- nothing there builds a routed CoreDevice tunnel
         * the way Device Hub does on a Mac -- so the mode has nothing to attach to. */
        fprintf(stderr, "RPLAY_ATTACH_TUNNEL is not supported on Windows: it attaches to a macOS "
                        "utun built by Device Hub/Xcode. Unset it to bind a device over usbmux.\n");
        return 2;
    }
#endif

    /* Userspace TCP/IP (lwIP) or the kernel tunnel. The kernel path is the macOS utun, which needs
     * root; everywhere else there is no kernel path at all, so userspace is the default there and
     * RPLAY_USERSPACE_NET=0 is refused up front rather than after a whole bring-up. */
    {
        const char *un = getenv("RPLAY_USERSPACE_NET");
#ifdef __APPLE__
        g_userspace = un && un[0] == '1';
#else
        g_userspace = !(un && un[0] == '0');
#endif
        if (want_attach) g_userspace = 0;   /* an attached tunnel is already routed for kernel sockets */
#ifndef __APPLE__
        if (!g_userspace && !want_attach) {
            fprintf(stderr, "  RPLAY_USERSPACE_NET=0 asks for the kernel tunnel, which is macOS-only "
                            "(utun) -- unset it to use the userspace stack\n");
            return 1;
        }
#endif
        usernet_enable(g_userspace);
    }

    /* The control API first, before any device: a GUI started now gets answers (device list, the
     * usbmuxd state, select_device) while the engine waits for a phone. Same ports and same JSON
     * contract as the Python engine. A daemon that cannot serve is not a daemon: if the ports are
     * taken (another cdhost), stop here rather than run on answering nobody. */
    printf("\n== Layer 4: daemon (control API; up before any device is bound) ==\n");
    static api_session session;
    if (api_start(&session) != 0) {
        fprintf(stderr, "  api server did not start; shutting down\n");
        return 1;
    }
    /* A relaunched engine has its ports now; a later rebind sets the marker again. */
#ifdef _WIN32
    _putenv_s("RPLAY_REBOUND", "");
    SetEnvironmentVariableA("RPLAY_REBOUND", NULL);
#else
    unsetenv("RPLAY_REBOUND");
#endif

    int attached = 0;
    if (want_attach) {
        api_set_engine_state(API_ENGINE_BINDING);
        if (attach_existing_tunnel(addr, sizeof addr, ours, sizeof ours, &rsd) != 0) return 1;
        attached = 1;
        snprintf(devname, sizeof devname, "attached");
    }

    if (!attached) {
        if (wait_and_bind(no_wait, &tun, udid, sizeof udid, devname, sizeof devname,
                          prodver, sizeof prodver, addr, sizeof addr, ours, sizeof ours,
                          &rsd, &mtu) != 0)
            return 1;
    }

    if (attached) {
        /* Nothing to bring up: the tunnel is already routed by whoever created it, so kernel
         * sockets reach the device as they are. No utun, no packet pump, and therefore none of
         * bug #7's self-exit either -- if that tunnel dies, its owner rebuilds it. */
        printf("\n== Layer 3a: skipped (using an existing tunnel) ==\n");
    } else if (g_userspace) {
        /* Userspace TCP/IP over the tunnel -- no root, no utun, no TUN driver (the Linux/Windows
         * and App Store path). lwIP runs the IP stack in-process over the CoreDeviceProxy
         * connection; every tunnel connection below opens an lwIP socket instead of a kernel one. */
        printf("\n== Layer 3a: userspace TCP/IP (lwIP, no root) ==\n");
        if (usernet_start(tun.idev, ours, addr) != 0) { fprintf(stderr, "  usernet_start failed\n"); return 1; }
        printf("  lwIP netif up over the tunnel\n");
    } else {
#ifndef __APPLE__
        /* Unreachable: refused above, before the API came up. */
        return 1;
#else
        printf("\n== Layer 3a: utun + packet pump ==\n");
        if (geteuid() != 0) {
            printf("  not root — cannot create a utun. Re-run with sudo, or set RPLAY_USERSPACE_NET=1\n");
            printf("      sudo %s\n", "./cdhost");
            printf("\nLayers 0-2 verified in C against the device.\n");
            return 0;
        }
        char ifname[64] = {0};
        int utun = utun_open(ifname, sizeof ifname);
        if (utun < 0) { perror("  utun_open"); return 1; }
        printf("  utun: %s\n", ifname);
        if (utun_configure(ifname, ours, addr, mtu) < 0) {
            fprintf(stderr, "  could not configure %s\n", ifname);
            return 1;
        }
        static pump_t pump;
        pump.utun = utun; pump.tun = &tun; pump.failed = 0; pump.reason = NULL;
        pthread_t t1, t2;
        pthread_create(&t1, NULL, pump_host_to_device, &pump);
        pthread_create(&t2, NULL, pump_device_to_host, &pump);
        printf("  pump running\n");
#endif /* __APPLE__ */
    }

    sleep(1);
    if (rsd_reachable(addr, rsd) == 0)
        printf("  ✅ RSD reachable at [%s]:%ld — the tunnel is live\n", addr, rsd);
    else
        printf("  ✗ could not connect to [%s]:%ld\n", addr, rsd);

    printf("\n== Layer 3b: RSD over RemoteXPC ==\n");
    session.udid = udid;
    session.device_name = devname;
    session.product_version = prodver;
    session.tunnel_addr = addr;
    session.our_addr = ours;
    session.rsd_port = rsd;
    /* ONE identity for this receiver. It goes into the offer as field 5.1, the device echoes it
     * back as RemoteSSRC, and every RTCP packet must carry it. Deriving both from this single
     * value is what stops them drifting apart -- when they did, the device silently discarded
     * every ack, PLI and FIR we sent. */
    session.ssrc = (uint32_t)(time(NULL) ^ (uintptr_t)&session);
    if (!session.ssrc) session.ssrc = 1;
    {
        const char *k = getenv("RPLAY_KEYFRAME_EVERY_S");
        session.keyframe_every_s = k ? atof(k) : 0.0;
    }
    if (rsd_enumerate(addr, rsd, &session) < 0)
        fprintf(stderr, "  service discovery failed\n");

    /* Self-activation: iOS 17+ discards the developer disk image on every reboot, and without it
     * the screen/control services stay silent. Mount it ourselves rather than requiring Xcode or
     * Device Hub -- the whole point of shipping this daemon (host/ddi_mount.py proved the flow,
     * ddi.c is the C port). Only when the phone is new enough to need it and does not already have
     * one; a locked phone is reported, not retried, since the mount needs it unlocked. */
    /* Where a shipped DDI would be: Contents/Resources/iOS_DDI, two dirs up from the executable
     * (Contents/MacOS/cdhost). NULL when that folder is absent (dev builds), which makes ddi.c
     * fall back to the system Xcode location. On Windows ddi.c looks beside the .exe itself
     * (backslash paths, %LOCALAPPDATA%), so there is no bundle to compute here. */
#ifndef _WIN32
    static char bundle_ddi[1400];
    { char exe[1200]; snprintf(exe, sizeof exe, "%s", g_self);
      char *macos = strrchr(exe, '/'); if (macos) *macos = 0;      /* .../Contents/MacOS */
      char *contents = strrchr(exe, '/'); if (contents) *contents = 0;  /* .../Contents */
      snprintf(bundle_ddi, sizeof bundle_ddi, "%s/Resources/iOS_DDI", exe);
      struct stat st; char probe[1500];
      snprintf(probe, sizeof probe, "%s/Restore/BuildManifest.plist", bundle_ddi);
      session.ddi_dir = (stat(probe, &st) == 0) ? bundle_ddi : NULL; }
#else
    session.ddi_dir = NULL;
#endif

    if (session.mounter_port && prodver[0] && atoi(prodver) >= 17) {
        printf("\n== Layer 3c: developer disk image ==\n");
        int drc = cdhost_ddi_activate(addr, session.mounter_port, session.ddi_dir);
        if (drc == RP_DDI_ALREADY)      printf("  already mounted\n");
        else if (drc == RP_DDI_OK)      printf("  mounted (no Xcode/Device Hub needed)\n");
        else if (drc == RP_DDI_LOCKED)  printf("  the device is locked -- unlock it, then reconnect\n");
        else if (drc == RP_DDI_NO_DDI)  printf("  no DDI files: run scripts/fetch-ddi.sh (or set RPLAY_DDI), or mount via Xcode/Device Hub\n");
        else                            printf("  could not mount the DDI (rc=%d); mirroring may not work\n", drc);
    }

    /* Bound: every method is available from here, and viewers that connected while the engine
     * was waiting get their stream started now. */
    api_set_engine_error(NULL);
    api_set_engine_state(API_ENGINE_READY);
    printf("\n== ready: %s (%s) is bound ==\n", devname, udid[0] ? udid : "attached tunnel");

    /* The API serves until the process is killed; a dead tunnel exits via pump_die (utun path)
     * or restarts via cdhost_tunnel_died (userspace). Reaching past api_wait means the accept
     * loop itself stopped. */
    api_wait();
    fprintf(stderr, "  api server stopped; shutting down\n");
    return 1;
}
