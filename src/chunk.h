#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include "block.h"
#include "chunk_mesh.h"

#define CHUNK_X 16
#define CHUNK_Y 256
#define CHUNK_Z 16
#define CHUNK_BLOCKS (CHUNK_X * CHUNK_Y * CHUNK_Z)

typedef enum ChunkState {
    CHUNK_UNLOADED = 0,
    CHUNK_GENERATING,
    CHUNK_GENERATED,
    CHUNK_MESHING,
    CHUNK_READY,
} ChunkState;

typedef struct Chunk {
    int32_t          cx, cz;
    _Atomic int      state;
    BlockID          blocks[CHUNK_BLOCKS];
    ChunkMesh        mesh;
    uint8_t*         meta;         /* lazily allocated; NULL if unused */
    bool             needs_remesh; /* set on block change; cleared on remesh submit */
} Chunk;

Chunk* chunk_create(int32_t cx, int32_t cz);
void   chunk_destroy(Chunk* chunk);

static inline BlockID chunk_get_block(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return BLOCK_AIR;
    return c->blocks[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
}

static inline void chunk_set_block(Chunk* c, int x, int y, int z, BlockID id) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z) return;
    c->blocks[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] = id;
}

/* Ensure meta array is allocated. Call before any meta write. */
static inline void chunk_ensure_meta(Chunk* c) {
    if (!c->meta) {
        c->meta = calloc(CHUNK_BLOCKS, 1);
    }
}

static inline uint8_t chunk_get_meta(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return 0;
    if (!c->meta) return 0;
    return c->meta[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
}

static inline void chunk_set_meta(Chunk* c, int x, int y, int z, uint8_t val) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return;
    chunk_ensure_meta(c);
    c->meta[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] = val;
}

#endif
