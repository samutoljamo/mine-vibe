#include "chunk.h"
#include <stdlib.h>
#include <string.h>

Chunk* chunk_create(int32_t cx, int32_t cz) {
    Chunk* c = calloc(1, sizeof(Chunk));
    c->cx = cx;
    c->cz = cz;
    atomic_store(&c->state, CHUNK_UNLOADED);
    pt_mutex_init(&c->pending_mutex);
    return c;
}

void chunk_destroy(Chunk* chunk) {
    pt_mutex_destroy(&chunk->pending_mutex);
    /* No concurrent access at destroy time; load the atomic slots once. */
    free(atomic_load_explicit(&chunk->meta, memory_order_relaxed));
    free(atomic_load_explicit(&chunk->lights, memory_order_relaxed));
    free(chunk->pending_deltas);
    free(chunk);
}
