#include "net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

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
