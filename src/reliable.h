#ifndef RELIABLE_H
#define RELIABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <netinet/in.h>
#endif

#define RELIABLE_WINDOW      32
#define RELIABLE_TIMEOUT     0.1   /* seconds before retransmit */
#define RELIABLE_MAX_PAYLOAD 256

typedef struct {
    uint8_t  data[RELIABLE_MAX_PAYLOAD];
    uint16_t len;
    uint16_t seq;
    double   sent_time;
    bool     in_use;
} ReliableEntry;

typedef struct {
    /* Send side */
    ReliableEntry send_buf[RELIABLE_WINDOW];
    uint16_t      next_seq;

    /* Receive side — track which seqs we've seen, for ack_bits */
    uint16_t      last_recv_seq;
    uint16_t      recv_bits;      /* bit i: ack-1-i was received */
    bool          recv_any;       /* false until first packet arrives */
} ReliableChannel;

void reliable_init(ReliableChannel* ch);

/* Queue a reliable message. Sends immediately; also stores for retransmit.
 * fd/addr: the socket to send on. seq is assigned from ch->next_seq.
 * Returns the assigned seq. */
uint16_t reliable_send(ReliableChannel* ch, int fd,
                        const struct sockaddr_in* addr,
                        const uint8_t* data, uint16_t len);

/* Call when any packet arrives. Updates recv tracking for ack generation.
 * Also processes ack/ack_bits to remove confirmed entries from send_buf.
 * Returns true if this is the first time this seq was seen (not a duplicate). */
bool reliable_on_recv(ReliableChannel* ch, uint16_t seq,
                      uint16_t ack, uint16_t ack_bits);

/* Populate ack and ack_bits fields in an outgoing packet header based on
 * what we have received from the remote side. */
void reliable_fill_ack(const ReliableChannel* ch,
                        uint16_t* out_ack, uint16_t* out_ack_bits);

/* Retransmit any send_buf entries older than RELIABLE_TIMEOUT.
 * Call once per tick. */
void reliable_tick(ReliableChannel* ch, int fd,
                    const struct sockaddr_in* addr);

#endif /* RELIABLE_H */
