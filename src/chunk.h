#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
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

typedef struct BoundaryDelta {
    uint8_t  face;       /* 0=+X, 1=-X, 2=+Z, 3=-Z (horizontal only) */
    uint8_t  axis_coord; /* 0..15 along the boundary's other horizontal axis */
    uint16_t y;          /* 0..CHUNK_Y-1 */
    uint8_t  new_light;  /* packed sky+block nibble */
} BoundaryDelta;

typedef struct Chunk {
    int32_t          cx, cz;
    _Atomic int      state;
    BlockID          blocks[CHUNK_BLOCKS];
    ChunkMesh        mesh;
    uint8_t*         meta;          /* lazily allocated; NULL if unused */
    uint8_t*         lights;        /* lazily allocated; packed [block:4][sky:4] */
    uint16_t         pending_delta_count;
    uint16_t         pending_delta_cap;
    BoundaryDelta*   pending_deltas; /* malloc'd; NULL if cap == 0 */
    bool             needs_remesh;  /* set on block change; cleared on remesh submit */
    bool             needs_relight; /* set when neighbor wrote pending_deltas */
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
        if (!c->meta) {
            fprintf(stderr, "chunk_ensure_meta: out of memory\n");
            abort();
        }
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

/* Ensure lights array is allocated. Call before any light write. */
static inline void chunk_ensure_lights(Chunk* c) {
    if (!c->lights) {
        c->lights = calloc(CHUNK_BLOCKS, 1);
        if (!c->lights) {
            fprintf(stderr, "chunk_ensure_lights: out of memory\n");
            abort();
        }
    }
}

static inline uint8_t chunk_get_skylight(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return 0;
    if (!c->lights) return 0;
    return c->lights[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] & 0x0F;
}

static inline uint8_t chunk_get_blocklight(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return 0;
    if (!c->lights) return 0;
    return (c->lights[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] >> 4) & 0x0F;
}

static inline void chunk_set_skylight(Chunk* c, int x, int y, int z, uint8_t v) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return;
    chunk_ensure_lights(c);
    int idx = x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z;
    c->lights[idx] = (uint8_t)((c->lights[idx] & 0xF0) | (v & 0x0F));
}

static inline void chunk_set_blocklight(Chunk* c, int x, int y, int z, uint8_t v) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return;
    chunk_ensure_lights(c);
    int idx = x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z;
    c->lights[idx] = (uint8_t)((c->lights[idx] & 0x0F) | ((v & 0x0F) << 4));
}

#endif
