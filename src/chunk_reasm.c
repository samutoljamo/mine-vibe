#include "chunk_reasm.h"
#include <string.h>

void chunk_reasm_init(ChunkReasmRing* ring)
{
    memset(ring, 0, sizeof(*ring));
}

int chunk_reasm_active_count(const ChunkReasmRing* ring)
{
    int n = 0;
    for (int i = 0; i < CHUNK_REASM_SLOTS; i++)
        if (ring->slots[i].active) n++;
    return n;
}

/* Find the active slot for msg_id, or NULL. */
static ChunkReasmSlot* find_slot(ChunkReasmRing* ring, uint16_t msg_id)
{
    for (int i = 0; i < CHUNK_REASM_SLOTS; i++)
        if (ring->slots[i].active && ring->slots[i].msg_id == msg_id)
            return &ring->slots[i];
    return NULL;
}

/* Acquire a slot for a brand-new msg_id: reuse a free slot if one exists, else
 * evict the least-recently-touched active slot (bounded memory, oldest-out). */
static ChunkReasmSlot* acquire_slot(ChunkReasmRing* ring)
{
    ChunkReasmSlot* victim = NULL;
    for (int i = 0; i < CHUNK_REASM_SLOTS; i++) {
        ChunkReasmSlot* s = &ring->slots[i];
        if (!s->active) return s;                 /* free slot: prefer it */
        if (!victim || s->touch < victim->touch)  /* track oldest */
            victim = s;
    }
    return victim;   /* all busy: evict oldest */
}

bool chunk_reasm_feed(ChunkReasmRing* ring,
                      uint16_t msg_id, uint16_t index, uint16_t total,
                      const uint8_t* frag, size_t flen,
                      uint8_t* out, size_t out_cap, size_t* out_len)
{
    /* Validate the fragment geometry before touching any slot. */
    if (total < 1 || total > CHUNK_DATA_FRAG_MAX) return false;
    if (index >= total) return false;
    if (flen > CHUNK_DATA_FRAG_BYTES) return false;

    size_t doff = (size_t)index * CHUNK_DATA_FRAG_BYTES;
    if (doff + flen > sizeof(((ChunkReasmSlot*)0)->data)) return false;

    ChunkReasmSlot* s = find_slot(ring, msg_id);
    if (s && s->total != total) {
        /* Same msg_id but a different total: the wire is inconsistent (or the
         * msg_id space wrapped onto a stale slot). Restart this slot clean. */
        s->active = false;
        s = NULL;
    }
    if (!s) {
        s = acquire_slot(ring);
        memset(s, 0, sizeof(*s));
        s->active = true;
        s->msg_id = msg_id;
        s->total  = total;
    }

    s->touch = ++ring->clock;   /* mark recently used (resets LRU age) */

    /* Store the fragment (idempotent for duplicates). */
    memcpy(s->data + doff, frag, flen);
    if (!s->got[index]) {
        s->got[index] = 1;
        s->received++;
    }
    /* The last fragment fixes the total body length (earlier frags are full
     * CHUNK_DATA_FRAG_BYTES). */
    if (index == (uint16_t)(total - 1))
        s->total_len = doff + flen;

    if (s->received == s->total) {
        /* Column complete. Hand it back and free the slot. */
        bool ok = false;
        if (s->total_len <= out_cap) {
            memcpy(out, s->data, s->total_len);
            *out_len = s->total_len;
            ok = true;
        }
        s->active = false;
        return ok;
    }
    return false;
}
