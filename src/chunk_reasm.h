#ifndef CHUNK_REASM_H
#define CHUNK_REASM_H

/* ------------------------------------------------------------------ */
/*  Per-msg_id chunk-column reassembly ring                            */
/*                                                                     */
/*  A streamed chunk column (PKT_CHUNK_DATA) is split into several     */
/*  fragments, each an independently acked + retransmitted reliable    */
/*  packet carrying a {msg_id,index,total} subheader. The receiver     */
/*  must rebuild a column at CHUNK_DATA_FRAG_BYTES stride.             */
/*                                                                     */
/*  A single in-flight slot is not enough under loss: a retransmitted  */
/*  fragment of an OLDER column can arrive interleaved with a NEWER    */
/*  column's fragments. With one slot the new msg_id resets the slot   */
/*  and the old column is dropped with no self-heal (or vice-versa).   */
/*                                                                     */
/*  This ring keeps a few concurrent partial columns, keyed by msg_id, */
/*  so interleaved multi-fragment columns reassemble independently.    */
/*  Memory is bounded: when all slots are busy and a brand-new msg_id  */
/*  arrives, the OLDEST (least-recently-touched) slot is evicted.      */
/*                                                                     */
/*  The module is pure (no sockets, no Vulkan): it is unit tested in   */
/*  tests/test_net.c. The caller feeds raw fragment payloads and gets  */
/*  a completed column back to RLE-decode.                             */
/* ------------------------------------------------------------------ */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "net.h"   /* CHUNK_DATA_FRAG_MAX, CHUNK_DATA_FRAG_BYTES */

/* Concurrent in-flight columns. A handful is plenty: the server streams at most
 * SERVER_STREAM_BUDGET (4) new columns per tick, and only a few can be partially
 * received at once under realistic loss/reorder. Keep it small to bound memory
 * (each slot holds a full worst-case column buffer). */
#define CHUNK_REASM_SLOTS 6

typedef struct {
    bool     active;
    uint16_t msg_id;
    uint16_t total;
    uint16_t received;
    uint64_t touch;                          /* LRU stamp; higher = newer */
    uint8_t  got[CHUNK_DATA_FRAG_MAX];
    size_t   total_len;                      /* known once the last frag lands */
    uint8_t  data[CHUNK_DATA_FRAG_MAX * CHUNK_DATA_FRAG_BYTES];
} ChunkReasmSlot;

typedef struct {
    ChunkReasmSlot slots[CHUNK_REASM_SLOTS];
    uint64_t       clock;                    /* monotonically bumped per feed */
} ChunkReasmRing;

/* Reset the ring to empty. */
void chunk_reasm_init(ChunkReasmRing* ring);

/* Feed one fragment into the ring.
 *   msg_id/index/total: the fragment subheader (already parsed + validated by
 *     the caller against CHUNK_DATA_FRAG_MAX etc).
 *   frag/flen: the fragment payload bytes (flen <= CHUNK_DATA_FRAG_BYTES).
 *
 * Returns true exactly once, when this fragment COMPLETES a column: it then
 * writes the assembled body to `out` (capacity `out_cap`) and its length to
 * `*out_len`, and frees the slot. Returns false while a column is still partial
 * (or on a malformed/oversized fragment, which is ignored).
 *
 * Out-of-order and duplicate fragments are handled. A duplicate fragment of an
 * already-completed-and-evicted column simply starts a fresh slot and waits for
 * the rest (harmless). */
bool chunk_reasm_feed(ChunkReasmRing* ring,
                      uint16_t msg_id, uint16_t index, uint16_t total,
                      const uint8_t* frag, size_t flen,
                      uint8_t* out, size_t out_cap, size_t* out_len);

/* Count of currently-active (partial) slots — for tests/diagnostics. */
int chunk_reasm_active_count(const ChunkReasmRing* ring);

#endif /* CHUNK_REASM_H */
