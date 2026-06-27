#include "net_thread.h"
#include "net.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void* thread_func(void* arg)
{
    NetThread* nt = (NetThread*)arg;
    uint8_t buf[NET_THREAD_MAX_MSG];
    double next_drop_report = 0.0;

    while (atomic_load(&nt->running)) {
        /* Drain inbound socket */
        struct sockaddr_in from;
        int n;
        while ((n = net_recv(nt->fd, buf, sizeof(buf), &from)) > 0) {
            NetMsg* msg = malloc(sizeof(NetMsg));
            if (!msg) {
                /* Dropped an inbound packet: queue semantics unchanged, but
                 * count it so the loss is observable rather than silent. */
                atomic_fetch_add(&nt->dropped, 1ull);
                continue;
            }
            memcpy(msg->data, buf, (size_t)n);
            msg->len       = n;
            msg->addr      = from;
            msg->recv_time = net_time();
            msg->next      = NULL;

            pt_mutex_lock(&nt->in_mutex);
            if (nt->in_tail) nt->in_tail->next = msg;
            else             nt->in_head = msg;
            nt->in_tail = msg;
            pt_mutex_unlock(&nt->in_mutex);
        }

        /* Drain outbound queue */
        pt_mutex_lock(&nt->out_mutex);
        NetMsg* out = nt->out_head;
        nt->out_head = nt->out_tail = NULL;
        pt_mutex_unlock(&nt->out_mutex);

        while (out) {
            net_send(nt->fd, out->data, out->len, &out->addr);
            NetMsg* next = out->next;
            free(out);
            out = next;
        }

        /* Periodically surface any packet loss (at most ~once per second). */
        {
            unsigned long long dropped = atomic_load(&nt->dropped);
            if (dropped != nt->dropped_reported) {
                double now = net_time();
                if (now >= next_drop_report) {
                    fprintf(stderr,
                            "net_thread: dropped %llu packet(s) total "
                            "(allocation failure under load)\n", dropped);
                    nt->dropped_reported = dropped;
                    next_drop_report = now + 1.0;
                }
            }
        }

        /* ~1ms sleep to avoid burning CPU */
        pt_sleep_ms(1);
    }
    return NULL;
}

bool net_thread_start(NetThread* nt, int fd)
{
    nt->fd       = fd;
    nt->in_head  = nt->in_tail  = NULL;
    nt->out_head = nt->out_tail = NULL;
    atomic_store(&nt->dropped, 0ull);
    nt->dropped_reported = 0;
    pt_mutex_init(&nt->in_mutex);
    pt_mutex_init(&nt->out_mutex);
    atomic_store(&nt->running, true);
    if (pt_thread_create(&nt->thread, thread_func, nt) != 0) {
        /* Thread did not start — undo init so the caller sees a clean failure. */
        atomic_store(&nt->running, false);
        pt_mutex_destroy(&nt->in_mutex);
        pt_mutex_destroy(&nt->out_mutex);
        fprintf(stderr, "net_thread_start: failed to create network thread\n");
        return false;
    }
    return true;
}

void net_thread_stop(NetThread* nt)
{
    atomic_store(&nt->running, false);
    pt_thread_join(nt->thread); /* pt_thread_join takes PT_Thread by value */
    /* Drain remaining queued messages — do this BEFORE destroying mutexes */
    NetMsg* m;
    while ((m = net_thread_pop_inbound(nt))) free(m);
    pt_mutex_lock(&nt->out_mutex);
    m = nt->out_head;
    nt->out_head = nt->out_tail = NULL;
    pt_mutex_unlock(&nt->out_mutex);
    while (m) { NetMsg* n = m->next; free(m); m = n; }
    /* Destroy mutexes AFTER draining */
    pt_mutex_destroy(&nt->in_mutex);
    pt_mutex_destroy(&nt->out_mutex);
}

void net_thread_push_outbound(NetThread* nt, const void* data, int len,
                                const struct sockaddr_in* addr)
{
    if (len <= 0 || len > NET_THREAD_MAX_MSG) return;
    NetMsg* msg = malloc(sizeof(NetMsg));
    if (!msg) {
        /* Dropped an outbound packet — count it so the loss is observable
         * (the thread loop emits a periodic warning). */
        atomic_fetch_add(&nt->dropped, 1ull);
        return;
    }
    memcpy(msg->data, data, (size_t)len);
    msg->len  = len;
    msg->addr = *addr;
    msg->next = NULL;

    pt_mutex_lock(&nt->out_mutex);
    if (nt->out_tail) nt->out_tail->next = msg;
    else              nt->out_head = msg;
    nt->out_tail = msg;
    pt_mutex_unlock(&nt->out_mutex);
}

NetMsg* net_thread_pop_inbound(NetThread* nt)
{
    pt_mutex_lock(&nt->in_mutex);
    NetMsg* msg = nt->in_head;
    if (msg) {
        nt->in_head = msg->next;
        if (!nt->in_head) nt->in_tail = NULL;
    }
    pt_mutex_unlock(&nt->in_mutex);
    return msg;
}
