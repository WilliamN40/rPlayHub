/* tunio.c -- the kernel-vs-lwIP routing for tunnel connections. Kept apart from usernet.c because
 * lwIP's <lwip/sockets.h> and the system <sys/socket.h> both define struct sockaddr et al. and
 * cannot share a translation unit. This file uses the system headers and calls into usernet.c
 * (lwIP) only through the plain functions in usernet.h. */
#include "usernet.h"

#include "compat.h"
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

static int g_on;
void usernet_enable(int on) { g_on = on; }
int usernet_is_on(void) { return g_on; }

int tun_connect(const char *addr, int port, int timeout_s)
{
    if (g_on) return usernet_connect(addr, port);
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) return -1;
    int fd = (int)socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef _WIN32
    DWORD ms = (timeout_s > 0 ? timeout_s : 10) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { .tv_sec = timeout_s > 0 ? timeout_s : 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { sock_close(fd); return -1; }
    return fd;
}

long tun_read(int fd, void *buf, size_t n)  { return usernet_owns(fd) ? usernet_read(fd, buf, n) : (long)recv(fd, buf, n, 0); }
long tun_write(int fd, const void *buf, size_t n) { return usernet_owns(fd) ? usernet_write(fd, buf, n) : (long)send(fd, buf, n, 0); }
void tun_close(int fd) { if (fd < 0) return; if (usernet_owns(fd)) usernet_close(fd); else sock_close(fd); }

long tun_read_timeout(int fd, void *buf, size_t n, int ms)
{
    if (usernet_owns(fd)) return usernet_read_timeout(fd, buf, n, ms);
    /* Kernel socket: wait for readability rather than setting SO_RCVTIMEO, which on Windows
     * leaves a timed-out connection "in an indeterminate state". */
#ifdef _WIN32
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET((SOCKET)fd, &rd);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(0, &rd, NULL, NULL, &tv);
    /* Winsock reports through WSAGetLastError, never errno. */
    if (r < 0) return WSAGetLastError() == WSAEINTR ? TUN_TIMEOUT : -1;
#else
    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r = poll(&p, 1, ms);
    if (r < 0) return errno == EINTR ? TUN_TIMEOUT : -1;
#endif
    if (r == 0) return TUN_TIMEOUT;
    return (long)recv(fd, buf, n, 0);
}
