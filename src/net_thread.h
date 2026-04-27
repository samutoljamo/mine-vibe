#ifndef NET_THREAD_H
#define NET_THREAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <netinet/in.h>
#endif
#include "platform_thread.h"
#include <stdatomic.h>

#define NET_THREAD_MAX_MSG 512   /* max bytes per queued message */

/* A message in either queue */
typedef struct NetMsg {
    uint8_t             data[NET_THREAD_MAX_MSG];
    int                 len;
    struct sockaddr_in  addr;
    double              recv_time; /* net_time() at receive; 0 for outbound */
    struct NetMsg*      next;
} NetMsg;

/* Thread + queues */
typedef struct {
    int          fd;
    PT_Thread    thread;
    _Atomic bool running;

    /* Inbound: network → game logic */
    NetMsg*      in_head;
    NetMsg*      in_tail;
    PT_Mutex     in_mutex;

    /* Outbound: game logic → network */
    NetMsg*      out_head;
    NetMsg*      out_tail;
    PT_Mutex     out_mutex;
} NetThread;

/* fd must already be open and non-blocking (from net_socket_server/client) */
bool net_thread_start(NetThread* nt, int fd);
void net_thread_stop(NetThread* nt);

/* Push a message to send. Copies data. */
void net_thread_push_outbound(NetThread* nt, const void* data, int len,
                               const struct sockaddr_in* addr);

/* Pop a received message. Returns NULL if queue empty.
 * Caller must free() the returned NetMsg. */
NetMsg* net_thread_pop_inbound(NetThread* nt);

#endif /* NET_THREAD_H */
