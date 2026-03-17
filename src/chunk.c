#include "chunk.h"
#include <stdlib.h>
#include <string.h>

Chunk* chunk_create(int32_t cx, int32_t cz) {
    Chunk* c = calloc(1, sizeof(Chunk));
    c->cx = cx;
    c->cz = cz;
    atomic_store(&c->state, CHUNK_UNLOADED);
    return c;
}

void chunk_destroy(Chunk* chunk) {
    free(chunk);
}
