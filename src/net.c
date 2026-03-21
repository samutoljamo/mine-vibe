#include "net.h"
#include <stdio.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>   /* QueryPerformanceCounter */
#  pragma comment(lib, "ws2_32.lib")

static void wsa_init(void)
{
    static int done = 0;
    if (!done) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        done = 1;
    }
}

static int make_nonblocking(SOCKET s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
}

int net_socket_server(uint16_t port)
{
    wsa_init();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return -1;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }
    make_nonblocking(s);
    return (int)(uintptr_t)s;
}

int net_socket_client(void)
{
    wsa_init();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return -1;
    make_nonblocking(s);
    return (int)(uintptr_t)s;
}

void net_socket_close(int fd)
{
    if (fd >= 0) closesocket((SOCKET)(uintptr_t)fd);
}

int net_send(int fd, const void* buf, int len,
             const struct sockaddr_in* addr)
{
    SOCKET s = (SOCKET)(uintptr_t)fd;
    int n = sendto(s, (const char*)buf, len, 0,
                   (const struct sockaddr*)addr, (int)sizeof(*addr));
    if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return 0;
    return n;
}

int net_recv(int fd, void* buf, int len, struct sockaddr_in* from)
{
    SOCKET s = (SOCKET)(uintptr_t)fd;
    int fromlen = (int)sizeof(*from);
    int n = recvfrom(s, (char*)buf, len, 0, (struct sockaddr*)from, &fromlen);
    if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return 0;
    return n;
}

double net_time(void)
{
    static LARGE_INTEGER freq = {{0, 0}};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

#else  /* POSIX */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static int make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_socket_server(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    make_nonblocking(fd);
    return fd;
}

int net_socket_client(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    make_nonblocking(fd);
    return fd;
}

void net_socket_close(int fd)
{
    if (fd >= 0) close(fd);
}

int net_send(int fd, const void* buf, int len,
             const struct sockaddr_in* addr)
{
    int n = (int)sendto(fd, buf, (size_t)len, 0,
                        (const struct sockaddr*)addr, sizeof(*addr));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}

int net_recv(int fd, void* buf, int len, struct sockaddr_in* from)
{
    socklen_t fromlen = sizeof(*from);
    int n = (int)recvfrom(fd, buf, (size_t)len, 0,
                           (struct sockaddr*)from, &fromlen);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}

double net_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif /* _WIN32 */
