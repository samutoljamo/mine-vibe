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

/* ------------------------------------------------------------------ */
/*  Fragmentation                                                      */
/* ------------------------------------------------------------------ */

static void wr_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t rd_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint16_t reliable_fragment_count(size_t len)
{
    if (len <= RELIABLE_FRAG_CHUNK) return 1;
    size_t n = (len + RELIABLE_FRAG_CHUNK - 1) / RELIABLE_FRAG_CHUNK;
    return (uint16_t)n;
}

size_t reliable_fragment_build(uint8_t* out, uint16_t msg_id,
                               uint16_t index, uint16_t total,
                               const uint8_t* data, size_t len)
{
    size_t off    = (size_t)index * RELIABLE_FRAG_CHUNK;
    size_t remain = (off < len) ? (len - off) : 0;
    size_t chunk  = remain < RELIABLE_FRAG_CHUNK ? remain : RELIABLE_FRAG_CHUNK;

    out[0] = RELIABLE_FRAG_MAGIC;
    wr_u16(out + 1, msg_id);
    wr_u16(out + 3, index);
    wr_u16(out + 5, total);
    if (chunk) memcpy(out + RELIABLE_FRAG_HEADER, data + off, chunk);
    return RELIABLE_FRAG_HEADER + chunk;
}

bool reliable_packet_is_fragment(const uint8_t* packet, size_t len)
{
    return len >= RELIABLE_FRAG_HEADER && packet[0] == RELIABLE_FRAG_MAGIC;
}

void reliable_reassemble_init(ReliableReassembler* re)
{
    memset(re, 0, sizeof(*re));
}

bool reliable_reassemble_feed(ReliableReassembler* re,
                              const uint8_t* packet, size_t len,
                              uint8_t* out, size_t out_cap, size_t* out_len)
{
    if (!reliable_packet_is_fragment(packet, len)) return false;

    uint16_t msg_id = rd_u16(packet + 1);
    uint16_t index  = rd_u16(packet + 3);
    uint16_t total  = rd_u16(packet + 5);
    size_t   chunk  = len - RELIABLE_FRAG_HEADER;

    if (total == 0 || total > RELIABLE_FRAG_MAX) return false;
    if (index >= total)                          return false;
    if (chunk > RELIABLE_FRAG_CHUNK)             return false;

    /* A new message id (or a fresh reassembler) starts a new assembly. A
     * fragment belonging to an already-completed message is harmless: re-
     * storing the same bytes leaves the buffer correct. */
    if (!re->active || re->msg_id != msg_id || re->total != total) {
        memset(re, 0, sizeof(*re));
        re->active = true;
        re->msg_id = msg_id;
        re->total  = total;
    }

    size_t off = (size_t)index * RELIABLE_FRAG_CHUNK;
    if (off + chunk > sizeof(re->data)) return false;
    if (chunk) memcpy(re->data + off, packet + RELIABLE_FRAG_HEADER, chunk);

    if (!re->got[index]) {
        re->got[index] = 1;
        re->received++;
    }
    /* The last fragment fixes the total length (earlier fragments are full
     * chunks). Record it whenever we have that fragment. */
    if (index == (uint16_t)(total - 1))
        re->total_len = off + chunk;

    if (re->received != re->total) return false;

    /* Complete. */
    size_t final_len = re->total_len;
    if (final_len > out_cap) return false;
    if (final_len) memcpy(out, re->data, final_len);
    if (out_len) *out_len = final_len;
    return true;
}

uint16_t reliable_send_fragmented(ReliableChannel* ch, int fd,
                                  const struct sockaddr_in* addr,
                                  const uint8_t* data, size_t len)
{
    if (len > RELIABLE_FRAG_MAX_PAYLOAD) return 0;

    uint16_t total  = reliable_fragment_count(len);
    /* Distinct message id so the receiver can tell fragmented messages apart.
     * Derived from the channel's sequence space; the exact value only needs to
     * change between concurrent in-flight messages. */
    uint16_t msg_id = ch->next_seq;

    for (uint16_t i = 0; i < total; i++) {
        uint8_t frag[RELIABLE_MAX_PAYLOAD];
        size_t flen = reliable_fragment_build(frag, msg_id, i, total, data, len);
        reliable_send(ch, fd, addr, frag, (uint16_t)flen);
    }
    return total;
}
