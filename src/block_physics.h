#ifndef BLOCK_PHYSICS_H
#define BLOCK_PHYSICS_H

#include <stdint.h>
#include <stdbool.h>
#include <cglm/cglm.h>
#include "block.h"

/* Physics constants */
#define GRAVITY_TICK_HZ       10
#define WATER_TICK_HZ         5
#define PHYSICS_RADIUS        64
#define WATER_DISSIPATION     2
#define WATER_SOURCE_LEVEL    255
#define POSSET_INIT_CAPACITY  4096

/* Forward declaration — avoids pulling in world.h (Vulkan transitive deps) */
typedef struct World World;

/* ------------------------------------------------------------------ */
/*  PosSet: open-addressing hash set of world positions               */
/*                                                                    */
/*  Key packing: x and z biased by 2^20 and packed into 21-bit        */
/*  fields; y packed into low 8 bits. Valid keys use only bits 0-49.  */
/*  Sentinels INT64_MAX (empty) and -1 (tombstone) use higher bits.   */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t* keys;       /* backing array; INT64_MAX = empty, -1 = tombstone */
    int      capacity;
    int      count;      /* live entries */
    int      tombstones;
} PosSet;

void posset_init(PosSet* s);
void posset_destroy(PosSet* s);
void posset_insert(PosSet* s, int x, int y, int z);
bool posset_contains(const PosSet* s, int x, int y, int z);
void posset_remove(PosSet* s, int x, int y, int z);
int  posset_count(const PosSet* s);

/* Iteration. Start with *idx = 0. Call repeatedly; returns true and
 * fills x/y/z for each live entry. Caller increments *idx by 1 each
 * call. Returns false when all entries have been visited. */
bool posset_iter_next(const PosSet* s, int* idx, int* x, int* y, int* z);

/* ------------------------------------------------------------------ */
/*  BlockPhysics                                                       */
/* ------------------------------------------------------------------ */

typedef struct BlockPhysics {
    PosSet gravity_active;
    PosSet water_active;
    float  gravity_accum;
    float  water_accum;
} BlockPhysics;

void block_physics_init(BlockPhysics* bp);
void block_physics_destroy(BlockPhysics* bp);

/* Call whenever any block changes (placed, removed, moved by physics).
 * Adds pos and 6 face-neighbors to both active sets. */
void block_physics_notify(BlockPhysics* bp, int x, int y, int z);

/* Called once per frame from the game loop. Fires gravity and water
 * ticks independently based on their accumulators. */
void block_physics_update(BlockPhysics* bp, World* world,
                          vec3 player_pos, float dt);

#endif
