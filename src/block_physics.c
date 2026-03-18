#include "block_physics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  World API forward declarations                                     */
/*  Declared here (not via world.h) to avoid Vulkan transitive deps.  */
/* ------------------------------------------------------------------ */

BlockID world_get_block(World* world, int x, int y, int z);
bool    world_set_block(World* world, int x, int y, int z, BlockID id);
uint8_t world_get_meta(World* world, int x, int y, int z);
bool    world_set_meta(World* world, int x, int y, int z, uint8_t level);

/* ------------------------------------------------------------------ */
/*  PosSet internals                                                   */
/* ------------------------------------------------------------------ */

#define POSSET_EMPTY      INT64_MAX
#define POSSET_TOMBSTONE  ((int64_t)-1)
#define POSSET_X_BIAS     (1 << 20)
#define POSSET_Z_BIAS     (1 << 20)

static inline int64_t pos_pack(int x, int y, int z) {
    uint64_t ux = (uint64_t)(x + POSSET_X_BIAS) & 0x1FFFFF; /* 21 bits */
    uint64_t uy = (uint64_t)(unsigned char)y;                /* 8 bits  */
    uint64_t uz = (uint64_t)(z + POSSET_Z_BIAS) & 0x1FFFFF; /* 21 bits */
    return (int64_t)((ux << 29) | (uz << 8) | uy);
}

static inline void pos_unpack(int64_t key, int* x, int* y, int* z) {
    uint64_t k = (uint64_t)key;
    *y = (int)(k & 0xFF);
    *z = (int)((k >> 8) & 0x1FFFFF) - POSSET_Z_BIAS;
    *x = (int)((k >> 29) & 0x1FFFFF) - POSSET_X_BIAS;
}

static inline int posset_slot(const PosSet* s, int64_t key) {
    /* splitmix64 finalizer — avalanches all 64 bits */
    uint64_t h = (uint64_t)key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (int)(h & (uint64_t)(s->capacity - 1));
}

void posset_init(PosSet* s) {
    s->capacity   = POSSET_INIT_CAPACITY;
    s->count      = 0;
    s->tombstones = 0;
    s->keys = malloc((size_t)s->capacity * sizeof(int64_t));
    if (!s->keys) { fprintf(stderr, "posset_init: out of memory\n"); abort(); }
    for (int i = 0; i < s->capacity; i++) s->keys[i] = POSSET_EMPTY;
}

void posset_destroy(PosSet* s) {
    free(s->keys);
    s->keys = NULL;
    s->capacity = s->count = s->tombstones = 0;
}

static void posset_rehash(PosSet* s, int new_cap) {
    int64_t* old_keys = s->keys;
    int      old_cap  = s->capacity;

    s->capacity   = new_cap;
    s->count      = 0;
    s->tombstones = 0;
    s->keys = malloc((size_t)new_cap * sizeof(int64_t));
    if (!s->keys) { fprintf(stderr, "posset_rehash: out of memory\n"); abort(); }
    for (int i = 0; i < new_cap; i++) s->keys[i] = POSSET_EMPTY;

    for (int i = 0; i < old_cap; i++) {
        int64_t k = old_keys[i];
        if (k == POSSET_EMPTY || k == POSSET_TOMBSTONE) continue;
        /* Re-insert */
        int slot = posset_slot(s, k);
        while (s->keys[slot] != POSSET_EMPTY) {
            slot = (slot + 1) & (s->capacity - 1);
        }
        s->keys[slot] = k;
        s->count++;
    }
    free(old_keys);
}

void posset_insert(PosSet* s, int x, int y, int z) {
    /* Rehash if load (live + tombstone) exceeds 70% */
    if ((s->count + s->tombstones) * 10 >= s->capacity * 7) {
        posset_rehash(s, s->capacity * 2);
    }

    int64_t key  = pos_pack(x, y, z);
    int     slot = posset_slot(s, key);
    int     first_tomb = -1;

    for (;;) {
        int64_t k = s->keys[slot];
        if (k == key) return; /* already present */
        if (k == POSSET_TOMBSTONE && first_tomb < 0) first_tomb = slot;
        if (k == POSSET_EMPTY) {
            int dest = (first_tomb >= 0) ? first_tomb : slot;
            if (first_tomb >= 0) s->tombstones--;
            s->keys[dest] = key;
            s->count++;
            return;
        }
        slot = (slot + 1) & (s->capacity - 1);
    }
}

bool posset_contains(const PosSet* s, int x, int y, int z) {
    int64_t key  = pos_pack(x, y, z);
    int     slot = posset_slot(s, key);

    for (;;) {
        int64_t k = s->keys[slot];
        if (k == key) return true;
        if (k == POSSET_EMPTY) return false;
        slot = (slot + 1) & (s->capacity - 1);
    }
}

void posset_remove(PosSet* s, int x, int y, int z) {
    int64_t key  = pos_pack(x, y, z);
    int     slot = posset_slot(s, key);

    for (;;) {
        int64_t k = s->keys[slot];
        if (k == key) {
            s->keys[slot] = POSSET_TOMBSTONE;
            s->count--;
            s->tombstones++;
            return;
        }
        if (k == POSSET_EMPTY) return; /* not found */
        slot = (slot + 1) & (s->capacity - 1);
    }
}

int posset_count(const PosSet* s) {
    return s->count;
}

bool posset_iter_next(const PosSet* s, int* idx, int* x, int* y, int* z) {
    while (*idx < s->capacity) {
        int64_t k = s->keys[*idx];
        if (k != POSSET_EMPTY && k != POSSET_TOMBSTONE) {
            pos_unpack(k, x, y, z);
            return true;
        }
        (*idx)++;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  BlockPhysics — stubs only (ticks implemented in later tasks)      */
/* ------------------------------------------------------------------ */

void block_physics_init(BlockPhysics* bp) {
    posset_init(&bp->gravity_active);
    posset_init(&bp->water_active);
    bp->gravity_accum = 0.0f;
    bp->water_accum   = 0.0f;
}

void block_physics_destroy(BlockPhysics* bp) {
    posset_destroy(&bp->gravity_active);
    posset_destroy(&bp->water_active);
}

void block_physics_notify(BlockPhysics* bp, int x, int y, int z) {
    /* Add position and its 6 face-neighbors to both active sets */
    static const int dx[7] = {0,  1, -1,  0,  0,  0,  0};
    static const int dy[7] = {0,  0,  0,  1, -1,  0,  0};
    static const int dz[7] = {0,  0,  0,  0,  0,  1, -1};
    for (int i = 0; i < 7; i++) {
        posset_insert(&bp->gravity_active, x + dx[i], y + dy[i], z + dz[i]);
        posset_insert(&bp->water_active,   x + dx[i], y + dy[i], z + dz[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Gravity tick                                                       */
/* ------------------------------------------------------------------ */

static void gravity_tick(BlockPhysics* bp, World* world, vec3 player_pos)
{
    /* Build the next dirty set from scratch: only positions that still
     * need checking survive into the next tick.                       */
    PosSet next;
    posset_init(&next);

    /* Track positions that received a block this tick — skip if revisited */
    PosSet moved;
    posset_init(&moved);

    int idx = 0, x, y, z;
    while (posset_iter_next(&bp->gravity_active, &idx, &x, &y, &z)) {
        idx++;

        /* Skip positions that received a block earlier in this same tick */
        if (posset_contains(&moved, x, y, z))
            continue;

        /* Radius check (XZ plane only) */
        float fdx = (float)x - player_pos[0];
        float fdz = (float)z - player_pos[2];
        if (fdx * fdx + fdz * fdz > (float)(PHYSICS_RADIUS * PHYSICS_RADIUS))
            continue; /* outside simulation radius — drop */

        BlockID block = world_get_block(world, x, y, z);
        if (!block_is_gravity(block))
            continue; /* not a gravity block — settled or replaced */

        if (y <= 0)
            continue; /* at bedrock floor — nowhere to fall */

        BlockID below = world_get_block(world, x, y - 1, z);
        if (below != BLOCK_AIR) {
            continue; /* supported — drop from dirty set */
        }

        /* Move block down one step */
        if (!world_set_block(world, x, y - 1, z, block)) {
            /* Chunk busy (meshing) — retry next tick */
            posset_insert(&next, x, y, z);
            continue;
        }
        if (!world_set_block(world, x, y, z, BLOCK_AIR)) {
            /* Roll back destination write — source chunk is busy */
            world_set_block(world, x, y - 1, z, BLOCK_AIR);
            posset_insert(&next, x, y, z);
            continue;
        }

        /* Record destination as moved-into this tick */
        posset_insert(&moved, x, y - 1, z);

        /* New position may still be unsupported */
        posset_insert(&next, x, y - 1, z);
        /* Slot above is now AIR — block above might start falling */
        posset_insert(&next, x, y + 1, z);
    }

    posset_destroy(&moved);
    posset_destroy(&bp->gravity_active);
    bp->gravity_active = next;
}

/* ------------------------------------------------------------------ */
/*  Water cellular automata tick                                       */
/* ------------------------------------------------------------------ */

/* Horizontal neighbor offsets */
static const int W_HX[4] = { 1, -1,  0,  0 };
static const int W_HZ[4] = { 0,  0,  1, -1 };

/* Try to place/update water at (x,y,z) with the given level.
 * Skips solid blocks and source blocks.  If successful, inserts the
 * position into *next so the new cell is processed on future ticks. */
static void water_spread_to(PosSet* next, World* world,
                             int x, int y, int z, uint8_t level)
{
    if (level == 0) return;

    BlockID nb = world_get_block(world, x, y, z);
    if (nb != BLOCK_AIR && nb != BLOCK_WATER) return; /* solid */

    if (nb == BLOCK_WATER) {
        uint8_t cur = world_get_meta(world, x, y, z);
        if (cur == WATER_SOURCE_LEVEL) return; /* never overwrite sources */
        if (cur >= level) return;              /* already at or above */
    }

    if (!world_set_block(world, x, y, z, BLOCK_WATER)) return;
    world_set_meta(world, x, y, z, level);
    posset_insert(next, x, y, z);
}

static void water_tick(BlockPhysics* bp, World* world, vec3 player_pos)
{
    PosSet next;
    posset_init(&next);

    /* Track positions that received water this tick — skip if revisited */
    PosSet moved;
    posset_init(&moved);

    int idx = 0, x, y, z;
    while (posset_iter_next(&bp->water_active, &idx, &x, &y, &z)) {
        idx++;

        /* Skip positions that received water earlier in this same tick */
        if (posset_contains(&moved, x, y, z))
            continue;

        /* Radius check (XZ plane) */
        float fdx = (float)x - player_pos[0];
        float fdz = (float)z - player_pos[2];
        if (fdx * fdx + fdz * fdz > (float)(PHYSICS_RADIUS * PHYSICS_RADIUS))
            continue;

        BlockID block = world_get_block(world, x, y, z);
        if (block != BLOCK_WATER) continue; /* block was replaced */

        uint8_t level = world_get_meta(world, x, y, z);

        uint8_t spread_down; /* level given to downward flow    */
        uint8_t spread_side; /* level given to horizontal flow  */

        if (level == WATER_SOURCE_LEVEL) {
            /* Source block: permanent, spreads at WATER_SOURCE_LEVEL-1 */
            spread_down = WATER_SOURCE_LEVEL - 1;
            spread_side = WATER_SOURCE_LEVEL - 2;
            posset_insert(&next, x, y, z); /* sources stay active forever */
        } else {
            /* Flowing block: dissipate each tick */
            int new_level = (int)level - WATER_DISSIPATION;
            if (new_level <= 0) {
                /* Water has dissipated — remove block */
                world_set_block(world, x, y, z, BLOCK_AIR);
                world_set_meta(world, x, y, z, 0);
                /* Notify above (water gone — gravity block may fall) */
                posset_insert(&next, x, y + 1, z);
                posset_insert(&bp->gravity_active, x, y + 1, z);
                continue;
            }
            world_set_meta(world, x, y, z, (uint8_t)new_level);
            spread_down = (uint8_t)new_level;
            spread_side = (new_level > 1) ? (uint8_t)(new_level - 1) : 0;
            posset_insert(&next, x, y, z); /* keep alive while level > 0 */
        }

        /* Flow down (keeps full level — gravity has priority) */
        if (y > 0) {
            BlockID below = world_get_block(world, x, y - 1, z);
            if (below == BLOCK_AIR || (below == BLOCK_WATER &&
                world_get_meta(world, x, y - 1, z) < spread_down &&
                world_get_meta(world, x, y - 1, z) != WATER_SOURCE_LEVEL)) {
                water_spread_to(&next, world, x, y - 1, z, spread_down);
                /* Mark destination to prevent re-processing in same tick */
                posset_insert(&moved, x, y - 1, z);
            }
        }

        /* Horizontal spread (loses 1 level per step) */
        for (int d = 0; d < 4; d++) {
            int sx = x + W_HX[d], sz = z + W_HZ[d];
            water_spread_to(&next, world, sx, y, sz, spread_side);
            /* Mark destination to prevent re-processing in same tick */
            if (spread_side > 0)
                posset_insert(&moved, sx, y, sz);
        }
    }

    posset_destroy(&moved);
    posset_destroy(&bp->water_active);
    bp->water_active = next;
}

/* ------------------------------------------------------------------ */
/*  BlockPhysics update                                                */
/* ------------------------------------------------------------------ */

void block_physics_update(BlockPhysics* bp, World* world,
                          vec3 player_pos, float dt)
{
    bp->gravity_accum += dt;
    bp->water_accum   += dt;

    const float gravity_interval = 1.0f / (float)GRAVITY_TICK_HZ;
    while (bp->gravity_accum >= gravity_interval) {
        gravity_tick(bp, world, player_pos);
        bp->gravity_accum -= gravity_interval;
    }

    const float water_interval = 1.0f / (float)WATER_TICK_HZ;
    while (bp->water_accum >= water_interval) {
        water_tick(bp, world, player_pos);
        bp->water_accum -= water_interval;
    }
}
