#include "reliable.h"
#include "net.h"
#include <string.h>

void reliable_init(ReliableChannel* ch)
{
    memset(ch, 0, sizeof(*ch));
}

uint16_t reliable_send(ReliableChannel* ch, int fd,
                        const struct sockaddr_in* addr,
                        const uint8_t* data, uint16_t len)
{
    uint16_t seq = ch->next_seq++;

    /* Find a free slot */
    int slot = seq % RELIABLE_WINDOW;
    /* If slot is in use (window full), evict — packet loss acceptable here */
    ReliableEntry* e = &ch->send_buf[slot];
    if (len > RELIABLE_MAX_PAYLOAD) len = RELIABLE_MAX_PAYLOAD;
    memcpy(e->data, data, len);
    e->len       = len;
    e->seq       = seq;
    e->sent_time = net_time();
    e->in_use    = true;

    net_send(fd, data, len, addr);
    return seq;
}

bool reliable_on_recv(ReliableChannel* ch, uint16_t seq,
                      uint16_t ack, uint16_t ack_bits)
{
    bool is_new = false;

    /* Update receive tracking */
    if (!ch->recv_any) {
        ch->last_recv_seq = seq;
        ch->recv_bits     = 0;
        ch->recv_any      = true;
        is_new            = true;
    } else {
        int16_t diff = (int16_t)(seq - ch->last_recv_seq);
        if (diff > 0) {
            if (diff >= 16) ch->recv_bits = 0;
            else            ch->recv_bits <<= diff;
            ch->recv_bits |= (1u << (diff - 1)); /* mark previous */
            ch->last_recv_seq = seq;
            is_new            = true;
        } else if (diff < 0 && diff > -16) {
            uint16_t bit = (uint16_t)(1u << (-diff - 1));
            if (!(ch->recv_bits & bit)) {
                ch->recv_bits |= bit;
                is_new = true;
            }
        }
        /* diff == 0: exact retransmission of last seq — is_new stays false */
    }

    /* Process ACKs — remove confirmed entries from send_buf */
    for (int i = 0; i < RELIABLE_WINDOW; i++) {
        ReliableEntry* e = &ch->send_buf[i];
        if (!e->in_use) continue;
        int16_t diff = (int16_t)(ack - e->seq);
        if (diff == 0) {
            e->in_use = false;
        } else if (diff > 0 && diff <= 16) {
            if (ack_bits & (1u << (diff - 1)))
                e->in_use = false;
        }
    }

    return is_new;
}

void reliable_fill_ack(const ReliableChannel* ch,
                        uint16_t* out_ack, uint16_t* out_ack_bits)
{
    *out_ack      = ch->recv_any ? ch->last_recv_seq : 0;
    *out_ack_bits = ch->recv_bits;
}

void reliable_tick(ReliableChannel* ch, int fd,
                    const struct sockaddr_in* addr)
{
    double now = net_time();
    for (int i = 0; i < RELIABLE_WINDOW; i++) {
        ReliableEntry* e = &ch->send_buf[i];
        if (e->in_use && (now - e->sent_time) > RELIABLE_TIMEOUT) {
            net_send(fd, e->data, e->len, addr);
            e->sent_time = now;
        }
    }
}
