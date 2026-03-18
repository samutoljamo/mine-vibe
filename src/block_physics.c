#include "block_physics.h"

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
    /* FNV-1a mix for 64-bit key */
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

void block_physics_update(BlockPhysics* bp, World* world,
                          vec3 player_pos, float dt) {
    (void)world; (void)player_pos;
    bp->gravity_accum += dt;
    bp->water_accum   += dt;
    /* Ticks implemented in Tasks 6 and 7 */
}
