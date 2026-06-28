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

/* Window must be large enough that a tick's worth of chunk-stream fragments
 * (SERVER_STREAM_BUDGET columns × a few fragments each) plus ordinary reliable
 * traffic all stay in-flight without evicting unacked entries. */
#define RELIABLE_WINDOW      64
#define RELIABLE_TIMEOUT     0.1   /* seconds before retransmit */
/* Sized so a streamed chunk column fits in a handful of fragments (a real RLE
 * column is ~6-8 KB). Stays safely under NET_MAX_PACKET (1400) and the net
 * thread's per-message buffer (NET_THREAD_MAX_MSG). */
#define RELIABLE_MAX_PAYLOAD 1200

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

/* ------------------------------------------------------------------ */
/*  Reliable message fragmentation                                     */
/* ------------------------------------------------------------------ */
/* Reliable payloads are normally capped at RELIABLE_MAX_PAYLOAD. For larger
 * messages (e.g. future inventory/crafting/chunk sync) an OPTIONAL fragmented
 * path splits the payload into several reliable packets, each carrying a small
 * fragment header, that the receiver reassembles.
 *
 * This is additive: small messages still use reliable_send / reliable_on_recv
 * exactly as before and are never fragmented, so existing callers need no
 * changes. Fragment packets are self-identifying via a magic first byte that no
 * existing packet type uses (packet types are < 64, the magic is 0xFE), so a
 * receiver can distinguish a fragment from a normal payload.                  */

#define RELIABLE_FRAG_MAGIC      0xFEu
/* Wire header: magic(1) + msg_id(2) + index(2) + total(2). */
#define RELIABLE_FRAG_HEADER     7
#define RELIABLE_FRAG_CHUNK      (RELIABLE_MAX_PAYLOAD - RELIABLE_FRAG_HEADER)
/* Max fragments per message; bounds the reassembly bitmap. */
#define RELIABLE_FRAG_MAX        64
/* Largest payload the fragmented path can carry. */
#define RELIABLE_FRAG_MAX_PAYLOAD (RELIABLE_FRAG_CHUNK * RELIABLE_FRAG_MAX)

/* Reassembly buffer for one in-flight fragmented message. A single instance
 * tracks the most recent msg_id seen; a new msg_id resets the buffer. */
typedef struct {
    bool     active;
    uint16_t msg_id;
    uint16_t total;
    uint16_t received;                 /* count of distinct fragments stored  */
    uint8_t  got[RELIABLE_FRAG_MAX];   /* per-index received flag             */
    size_t   total_len;                /* known once all fragments arrive     */
    uint8_t  data[RELIABLE_FRAG_MAX_PAYLOAD];
} ReliableReassembler;

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

/* ---- Fragmentation helpers (pure, socket-free) ---- */

/* Number of fragments a payload of `len` bytes splits into. Always >= 1
 * (a zero-length or small payload is "one fragment"). */
uint16_t reliable_fragment_count(size_t len);

/* Build fragment `index` of `total` for `data`/`len` into `out` (which must be
 * at least RELIABLE_MAX_PAYLOAD bytes). Returns the fragment packet size,
 * including the fragment header. */
size_t reliable_fragment_build(uint8_t* out, uint16_t msg_id,
                               uint16_t index, uint16_t total,
                               const uint8_t* data, size_t len);

/* True if `packet`/`len` is a fragment packet (begins with the fragment
 * magic and is at least a full fragment header long). */
bool reliable_packet_is_fragment(const uint8_t* packet, size_t len);

/* Reset a reassembler to empty. */
void reliable_reassemble_init(ReliableReassembler* re);

/* Feed one fragment packet into the reassembler. Out-of-order and duplicate
 * fragments are handled. Returns true once the full message is reassembled,
 * writing it to `out` (capacity `out_cap`) and its length to `*out_len`.
 * Returns false while still waiting for more fragments (or on a malformed /
 * oversized fragment). */
bool reliable_reassemble_feed(ReliableReassembler* re,
                              const uint8_t* packet, size_t len,
                              uint8_t* out, size_t out_cap, size_t* out_len);

/* ---- Fragmented send (socket path) ---- */

/* Send `data`/`len` reliably, fragmenting if it exceeds RELIABLE_MAX_PAYLOAD.
 * Each fragment goes through reliable_send (so each is independently acked and
 * retransmitted). For len <= RELIABLE_FRAG_CHUNK this still wraps the payload
 * in a single fragment, so the receiver must route fragment packets through a
 * ReliableReassembler. Use plain reliable_send for messages that the existing
 * unfragmented receive path must handle. Returns the number of fragments sent,
 * or 0 if `len` exceeds RELIABLE_FRAG_MAX_PAYLOAD. */
uint16_t reliable_send_fragmented(ReliableChannel* ch, int fd,
                                  const struct sockaddr_in* addr,
                                  const uint8_t* data, size_t len);

#endif /* RELIABLE_H */
