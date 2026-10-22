#ifndef RP_COMPAT_H
#define RP_COMPAT_H

/* Kernel-socket portability. Two rules this header exists to enforce:
 *
 *  1. A kernel socket is closed with sock_close(), never close(). On Windows a SOCKET is a kernel
 *     handle, not a CRT file descriptor, so close() on it is wrong -- and the old blanket
 *     `#define close(x) closesocket(x)` was worse: it also turned close() on an lwIP fd (768+)
 *     into closesocket(768), which on Windows can be a DIFFERENT, live Winsock handle.
 *  2. A tunnel connection (anything from tun_connect / usernet_*) is closed with tun_close() and
 *     read/written with tun_read()/tun_write() -- see usernet.h. Never sock_close() it.
 *
 * Socket errors come from sock_errno()/sock_strerror(), because Winsock reports them through
 * WSAGetLastError() and leaves errno untouched. */

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

typedef intptr_t ssize_t;
#define sleep(x) Sleep((DWORD)((x)*1000))
#define usleep(x) Sleep((DWORD)((x)/1000))
#define mkdir(p, m) _mkdir(p)

static inline int sock_close(int fd) { return closesocket((SOCKET)fd); }
static inline int sock_errno(void) { return WSAGetLastError(); }

/* Winsock's codes are not CRT errno values, so strerror() would print nonsense for them. */
static inline const char *sock_strerror(int err)
{
    static __thread char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             (DWORD)err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
                             (DWORD)sizeof buf, NULL);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '.')) buf[--n] = 0;
    if (!n) snprintf(buf, sizeof buf, "Winsock error %d", err);
    return buf;
}

/* Keep a socket out of any process this one spawns (the device-rebind relaunch). */
static inline void sock_noinherit(int fd) { SetHandleInformation((HANDLE)(intptr_t)fd, HANDLE_FLAG_INHERIT, 0); }

static inline int set_nonblocking(int fd) {
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
}

static inline int is_sock_wouldblock(void) {
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

static inline int sock_close(int fd) { return close(fd); }
static inline int sock_errno(void) { return errno; }
static inline const char *sock_strerror(int err) { return strerror(err); }
static inline void sock_noinherit(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

static inline int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static inline int is_sock_wouldblock(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

#endif

#endif /* RP_COMPAT_H */
