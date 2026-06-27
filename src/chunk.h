#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "block.h"
#include "chunk_mesh.h"
#include "platform_thread.h"

#define CHUNK_X 16
#define CHUNK_Y 256
#define CHUNK_Z 16
#define CHUNK_BLOCKS (CHUNK_X * CHUNK_Y * CHUNK_Z)

typedef enum ChunkState {
    CHUNK_UNLOADED = 0,
    CHUNK_GENERATING,
    CHUNK_GENERATED,
    CHUNK_LIGHTING,
    CHUNK_LIT,
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
    /* meta/lights are lazily allocated and may be first-touched by concurrent
     * worker threads, so the pointers are atomic and published via a CAS in
     * chunk_ensure_meta/chunk_ensure_lights (see below). Plain reads elsewhere
     * (mesher, snapshot) read a fully-constructed array; the atomic type makes
     * those loads well-defined. NULL until first allocation. */
    _Atomic(uint8_t*) meta;        /* lazily allocated; NULL if unused */
    /* Lighting fields. Concurrency contract:
     *   - lights[]: written by lighting worker during CHUNK_LIGHTING; read
     *     by mesher worker during CHUNK_MESHING; read-only after; main-thread
     *     in-place writes (block-change relight) are gated on state != LIGHTING/MESHING.
     *   - pending_deltas / pending_delta_count / pending_delta_cap / needs_relight:
     *     guarded by pending_mutex. Multiple workers may concurrently push_boundary_delta
     *     onto a shared neighbor; the mutex serializes their realloc/append. */
    PT_Mutex         pending_mutex;
    _Atomic(uint8_t*) lights;
    uint32_t         pending_delta_count;
    uint32_t         pending_delta_cap;
    BoundaryDelta*   pending_deltas;
    bool             needs_remesh;
    bool             needs_relight;
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

/* Atomically publish a lazily-allocated CHUNK_BLOCKS byte array into *slot.
 * Returns the live array (either ours or the one a racing thread published).
 * Cheap on the hot path: a single relaxed-ish load and early return once the
 * array exists; the calloc + CAS only run on the first touch. */
static inline uint8_t* chunk_lazy_alloc(_Atomic(uint8_t*)* slot,
                                        const char* what) {
    uint8_t* p = atomic_load_explicit(slot, memory_order_acquire);
    if (p) return p;
    uint8_t* fresh = calloc(CHUNK_BLOCKS, 1);
    if (!fresh) {
        fprintf(stderr, "%s: out of memory\n", what);
        abort();
    }
    uint8_t* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            slot, &expected, fresh,
            memory_order_acq_rel, memory_order_acquire)) {
        return fresh;            /* we won the race */
    }
    free(fresh);                 /* another thread published first */
    return expected;             /* CAS loaded the winner into expected */
}

/* Ensure meta array is allocated. Call before any meta write. */
static inline void chunk_ensure_meta(Chunk* c) {
    chunk_lazy_alloc(&c->meta, "chunk_ensure_meta");
}

static inline uint8_t chunk_get_meta(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return 0;
    uint8_t* meta = atomic_load_explicit(&c->meta, memory_order_acquire);
    if (!meta) return 0;
    return meta[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
}

static inline void chunk_set_meta(Chunk* c, int x, int y, int z, uint8_t val) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return;
    uint8_t* meta = chunk_lazy_alloc(&c->meta, "chunk_set_meta");
    meta[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] = val;
}

/* Ensure lights array is allocated. Call before any light write. */
static inline void chunk_ensure_lights(Chunk* c) {
    chunk_lazy_alloc(&c->lights, "chunk_ensure_lights");
}

static inline uint8_t chunk_get_skylight(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return 0;
    uint8_t* lights = atomic_load_explicit(&c->lights, memory_order_acquire);
    if (!lights) return 0;
    return lights[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] & 0x0F;
}

static inline uint8_t chunk_get_blocklight(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return 0;
    uint8_t* lights = atomic_load_explicit(&c->lights, memory_order_acquire);
    if (!lights) return 0;
    return (lights[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] >> 4) & 0x0F;
}

static inline void chunk_set_skylight(Chunk* c, int x, int y, int z, uint8_t v) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return;
    uint8_t* lights = chunk_lazy_alloc(&c->lights, "chunk_set_skylight");
    int idx = x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z;
    lights[idx] = (uint8_t)((lights[idx] & 0xF0) | (v & 0x0F));
}

static inline void chunk_set_blocklight(Chunk* c, int x, int y, int z, uint8_t v) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return;
    uint8_t* lights = chunk_lazy_alloc(&c->lights, "chunk_set_blocklight");
    int idx = x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z;
    lights[idx] = (uint8_t)((lights[idx] & 0x0F) | ((v & 0x0F) << 4));
}

#endif
