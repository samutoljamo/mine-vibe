# Block Physics Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add gravity-affected blocks (sand falls) and cellular automata water (fills containers, equalizes, dissipates) within a 64-block player radius using a dirty-set scheduler.

**Architecture:** A `BlockPhysics` module maintains two `PosSet` hash sets (gravity and water active positions) and fires independent ticks at 10 Hz (gravity) and 5 Hz (water). All physics writes go through `world_set_block()` / `world_set_meta()` which enforce the chunk-state safety rule (skip writes to chunks being meshed). Chunks with modified blocks are re-meshed via the existing worker pool.

**Tech Stack:** C11, existing Vulkan/chunk/world infrastructure, cglm for vec3.

**Spec:** `docs/superpowers/specs/2026-03-18-block-physics-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `src/block.h` | Modify | Add `is_gravity` to `BlockDef`; add `block_is_gravity()` |
| `src/block.c` | Modify | Set `is_gravity = true` for `BLOCK_SAND` |
| `src/chunk.h` | Modify | Add `uint8_t* meta`, `bool needs_remesh`; add `chunk_ensure_meta()`, `chunk_get_meta()`, `chunk_set_meta()` |
| `src/chunk.c` | Modify | Lazy-allocate `meta`; free in `chunk_destroy()`; init `needs_remesh = false` |
| `src/world.h` | Modify | Add `world_set_block()`, `world_set_meta()`, `world_get_meta()`; update `world_update()` signature |
| `src/world.c` | Modify | Implement write API; remesh trigger; seed physics from generate results |
| `src/worldgen.c` | Modify | Initialize `meta[i] = 255` for every `BLOCK_WATER` placed |
| `src/block_physics.h` | Create | `PosSet`, `BlockPhysics` struct, full public API |
| `src/block_physics.c` | Create | PosSet impl, gravity tick, water tick, scheduler |
| `src/mesher.h` | Modify | Add `meta` parameter to `mesher_build()` |
| `src/mesher.c` | Modify | Accept `meta` snapshot; partial-height water top face |
| `src/main.c` | Modify | Instantiate `BlockPhysics`; call `block_physics_update()` |
| `CMakeLists.txt` | Modify | Add `block_physics.c` to sources; add `test_block_physics` test target |
| `src/test_block_physics.c` | Create | Unit tests for PosSet and physics tick logic |

---

## Task 1: Block data model — add `is_gravity`

**Files:**
- Modify: `src/block.h`
- Modify: `src/block.c`

- [ ] **Step 1: Add `is_gravity` to `BlockDef` and add `block_is_gravity()` inline**

In `src/block.h`, add `is_gravity` field after `is_transparent` and add the inline:

```c
typedef struct BlockDef {
    const char* name;
    bool        is_solid;
    bool        is_transparent;
    bool        is_gravity;      /* falls when unsupported */
    uint8_t     tex_top;
    uint8_t     tex_side;
    uint8_t     tex_bottom;
} BlockDef;
```

Also add after the existing `block_is_transparent()` inline:

```c
static inline bool block_is_gravity(BlockID id) {
    return block_get_def(id)->is_gravity;
}
```

- [ ] **Step 2: Set `is_gravity = true` for sand in `src/block.c`**

Change the sand entry from:
```c
[BLOCK_SAND]    = { "sand",    true,  false, 4,  4,  4 },
```
to:
```c
[BLOCK_SAND]    = { "sand",    true,  false, true, 4,  4,  4 },
```

All other blocks get `false` (already their zero-initialized default):
```c
static const BlockDef block_defs[BLOCK_COUNT] = {
    [BLOCK_AIR]     = { "air",     false, true,  false, 0,  0,  0 },
    [BLOCK_STONE]   = { "stone",   true,  false, false, 0,  0,  0 },
    [BLOCK_DIRT]    = { "dirt",    true,  false, false, 1,  1,  1 },
    [BLOCK_GRASS]   = { "grass",   true,  false, false, 2,  3,  1 },
    [BLOCK_SAND]    = { "sand",    true,  false, true,  4,  4,  4 },
    [BLOCK_WOOD]    = { "wood",    true,  false, false, 5,  6,  5 },
    [BLOCK_LEAVES]  = { "leaves",  true,  true,  false, 7,  7,  7 },
    [BLOCK_WATER]   = { "water",   false, true,  false, 16, 16, 16 },
    [BLOCK_BEDROCK] = { "bedrock", true,  false, false, 17, 17, 17 },
};
```

- [ ] **Step 3: Build to verify**

```bash
cd /var/home/samu/minecraft && cmake --build build/ 2>&1 | tail -5
```
Expected: build succeeds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/block.h src/block.c
git commit -m "feat: add is_gravity field to BlockDef, set true for sand"
```

---

## Task 2: Chunk data model — add `meta` and `needs_remesh`

**Files:**
- Modify: `src/chunk.h`
- Modify: `src/chunk.c`

- [ ] **Step 1: Add fields and inline helpers to `src/chunk.h`**

Add `uint8_t* meta` and `bool needs_remesh` to the `Chunk` struct (after `mesh`):

```c
typedef struct Chunk {
    int32_t          cx, cz;
    _Atomic int      state;
    BlockID          blocks[CHUNK_BLOCKS];
    ChunkMesh        mesh;
    uint8_t*         meta;         /* lazily allocated; NULL if unused */
    bool             needs_remesh; /* set on block change; cleared on remesh submit */
} Chunk;
```

Add these inline helpers after the existing `chunk_set_block()`:

```c
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
```

Add `#include <stdlib.h>` at the top of chunk.h (needed for `calloc` in the inline).

- [ ] **Step 2: Update `chunk_create()` and `chunk_destroy()` in `src/chunk.c`**

`chunk_create()` uses `calloc` so `meta` and `needs_remesh` zero-initialize automatically — no change needed there.

Update `chunk_destroy()` to free `meta`:

```c
void chunk_destroy(Chunk* chunk) {
    free(chunk->meta);
    free(chunk);
}
```

- [ ] **Step 3: Build to verify**

```bash
cmake --build build/ 2>&1 | tail -5
```
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/chunk.h src/chunk.c
git commit -m "feat: add meta array and needs_remesh flag to Chunk"
```

---

## Task 3: World write API — `world_set_block`, `world_set_meta`, `world_get_meta`

**Files:**
- Modify: `src/world.h`
- Modify: `src/world.c`

- [ ] **Step 1: Declare the new functions in `src/world.h`**

Add after `world_get_block()`:

```c
/* Physics write API.
 * Returns true if the write was applied, false if deferred
 * (chunk not loaded or currently being meshed by a worker).
 * Caller must re-queue the position if false is returned. */
bool    world_set_block(World* world, int x, int y, int z, BlockID id);
bool    world_set_meta (World* world, int x, int y, int z, uint8_t level);
uint8_t world_get_meta (World* world, int x, int y, int z);
```

- [ ] **Step 2: Implement the three functions in `src/world.c`**

Use the same floor-division pattern as `world_get_block()`. Add at the bottom of world.c, after the existing `world_get_block()`:

```c
/* Resolves world (x,z) to chunk coords using floor division (matching world_get_block). */
static void world_to_chunk(int x, int z, int* cx, int* cz, int* lx, int* lz) {
    *cx = (x < 0) ? (x - 15) / 16 : x / 16;
    *cz = (z < 0) ? (z - 15) / 16 : z / 16;
    *lx = ((x % 16) + 16) % 16;
    *lz = ((z % 16) + 16) % 16;
}

bool world_set_block(World* world, int x, int y, int z, BlockID id) {
    if (y < 0 || y >= CHUNK_Y) return false;

    int cx, cz, lx, lz;
    world_to_chunk(x, z, &cx, &cz, &lx, &lz);

    Chunk* chunk = chunk_map_get(&world->map, cx, cz);
    if (!chunk) return false;

    int state = atomic_load(&chunk->state);
    if (state == CHUNK_MESHING) return false; /* deferred — safe write rule */
    if (state < CHUNK_GENERATED) return false;

    chunk_set_block(chunk, lx, y, lz, id);
    chunk->needs_remesh = true;
    return true;
}

bool world_set_meta(World* world, int x, int y, int z, uint8_t level) {
    if (y < 0 || y >= CHUNK_Y) return false;

    int cx, cz, lx, lz;
    world_to_chunk(x, z, &cx, &cz, &lx, &lz);

    Chunk* chunk = chunk_map_get(&world->map, cx, cz);
    if (!chunk) return false;

    int state = atomic_load(&chunk->state);
    if (state == CHUNK_MESHING) return false;
    if (state < CHUNK_GENERATED) return false;

    chunk_set_meta(chunk, lx, y, lz, level);
    chunk->needs_remesh = true;
    return true;
}

uint8_t world_get_meta(World* world, int x, int y, int z) {
    if (y < 0 || y >= CHUNK_Y) return 0;

    int cx, cz, lx, lz;
    world_to_chunk(x, z, &cx, &cz, &lx, &lz);

    Chunk* chunk = chunk_map_get(&world->map, cx, cz);
    if (!chunk) return 0;
    if (atomic_load(&chunk->state) < CHUNK_GENERATED) return 0;

    return chunk_get_meta(chunk, lx, y, lz);
}
```

- [ ] **Step 3: Build to verify**

```bash
cmake --build build/ 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add src/world.h src/world.c
git commit -m "feat: add world_set_block, world_set_meta, world_get_meta with chunk-state safety"
```

---

## Task 4: Worldgen water meta initialization

**Files:**
- Modify: `src/worldgen.c`

- [ ] **Step 1: Initialize `meta = 255` for all worldgen-placed water blocks**

In `worldgen_generate()`, the water block is placed at line 217:
```c
} else if (y <= SEA_LEVEL && y > h) {
    block = BLOCK_WATER;
}
```

After the main terrain fill loop (the `chunk_set_block` call at the end of the y-loop body), add a second pass to initialize meta for water blocks. The cleanest way is to add it right after the terrain fill double-loop ends:

```c
    /* Fill terrain layers */
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int h = height_map[x][z];
            bool is_beach = (h <= SEA_LEVEL + 1 && h >= SEA_LEVEL - 2);

            for (int y = 0; y < CHUNK_Y; y++) {
                BlockID block = BLOCK_AIR;

                if (y == 0) {
                    block = BLOCK_BEDROCK;
                } else if (y < 10) {
                    int r = hash_pos(base_x + x, y * 7919 + base_z + z, seed);
                    block = (r % 3 == 0) ? BLOCK_BEDROCK : BLOCK_STONE;
                } else if (y < h - 3) {
                    block = BLOCK_STONE;
                } else if (y < h) {
                    block = is_beach ? BLOCK_SAND : BLOCK_DIRT;
                } else if (y == h) {
                    block = is_beach ? BLOCK_SAND : BLOCK_GRASS;
                } else if (y <= SEA_LEVEL && y > h) {
                    block = BLOCK_WATER;
                }

                chunk_set_block(chunk, x, y, z, block);

                /* Water blocks placed by worldgen are permanent sources */
                if (block == BLOCK_WATER) {
                    chunk_set_meta(chunk, x, y, z, WATER_SOURCE_LEVEL);
                }
            }
        }
    }
```

The `WATER_SOURCE_LEVEL` constant (255) will be defined in `block_physics.h`. For now, just use the literal `255` and add the `#include "block_physics.h"` once that file is created. To keep this task self-contained, use the literal `255` and add the include in Task 6 when block_physics.h exists.

For now write it as:
```c
                if (block == BLOCK_WATER) {
                    chunk_set_meta(chunk, x, y, z, 255); /* source level */
                }
```

- [ ] **Step 2: Build to verify**

```bash
cmake --build build/ 2>&1 | tail -5
```

- [ ] **Step 3: Commit**

```bash
git add src/worldgen.c
git commit -m "feat: initialize water meta to 255 (source level) in worldgen"
```

---

## Task 5: PosSet — open-addressing hash set with unit tests

**Files:**
- Create: `src/block_physics.h`
- Create: `src/block_physics.c` (PosSet section only)
- Create: `src/test_block_physics.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add test target to `CMakeLists.txt`**

After the closing `endif()` at the end of CMakeLists.txt, add:

```cmake
# ---- Unit tests (no Vulkan dependency) ----
enable_testing()
add_executable(test_block_physics
    src/test_block_physics.c
    src/block_physics.c
    src/block.c
)
target_include_directories(test_block_physics PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${fastnoiselite_SOURCE_DIR}/C
)
target_link_libraries(test_block_physics PRIVATE cglm m)
add_test(NAME block_physics COMMAND test_block_physics)
```

- [ ] **Step 2: Create failing test for PosSet in `src/test_block_physics.c`**

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- Minimal stubs so block_physics.c compiles without world.h (Vulkan) ---- */
typedef struct World World;
typedef unsigned char BlockID;

/* These will be needed later for physics tick tests */
BlockID world_get_block(World* w, int x, int y, int z)  { (void)w;(void)x;(void)y;(void)z; return 0; }
int     world_set_block(World* w, int x, int y, int z, BlockID id) { (void)w;(void)x;(void)y;(void)z;(void)id; return 1; }
unsigned char world_get_meta(World* w, int x, int y, int z) { (void)w;(void)x;(void)y;(void)z; return 0; }
int     world_set_meta(World* w, int x, int y, int z, unsigned char v) { (void)w;(void)x;(void)y;(void)z;(void)v; return 1; }

#include "block_physics.h"

/* ---- PosSet tests ---- */

static void test_posset_insert_contains(void) {
    PosSet s;
    posset_init(&s);

    assert(!posset_contains(&s, 0, 0, 0));
    posset_insert(&s, 0, 0, 0);
    assert(posset_contains(&s, 0, 0, 0));

    posset_insert(&s, -100, 64, 200);
    assert(posset_contains(&s, -100, 64, 200));
    assert(!posset_contains(&s, -100, 64, 201));

    /* Negative coordinates */
    posset_insert(&s, -1, 0, -1);
    assert(posset_contains(&s, -1, 0, -1));
    assert(!posset_contains(&s, 1, 0, 1));

    assert(posset_count(&s) == 3);
    posset_destroy(&s);
    printf("PASS: test_posset_insert_contains\n");
}

static void test_posset_remove(void) {
    PosSet s;
    posset_init(&s);

    posset_insert(&s, 10, 5, 20);
    assert(posset_contains(&s, 10, 5, 20));
    posset_remove(&s, 10, 5, 20);
    assert(!posset_contains(&s, 10, 5, 20));
    assert(posset_count(&s) == 0);

    /* Remove non-existent — should be a no-op */
    posset_remove(&s, 99, 99, 99);
    assert(posset_count(&s) == 0);

    posset_destroy(&s);
    printf("PASS: test_posset_remove\n");
}

static void test_posset_iterate(void) {
    PosSet s;
    posset_init(&s);

    posset_insert(&s, 1, 2, 3);
    posset_insert(&s, 4, 5, 6);
    posset_insert(&s, 7, 8, 9);

    int found_count = 0;
    int idx = 0;
    int x, y, z;
    while (posset_iter_next(&s, &idx, &x, &y, &z)) {
        found_count++;
        idx++;
    }
    assert(found_count == 3);

    posset_destroy(&s);
    printf("PASS: test_posset_iterate\n");
}

static void test_posset_duplicate_insert(void) {
    PosSet s;
    posset_init(&s);

    posset_insert(&s, 5, 5, 5);
    posset_insert(&s, 5, 5, 5); /* duplicate */
    assert(posset_count(&s) == 1);

    posset_destroy(&s);
    printf("PASS: test_posset_duplicate_insert\n");
}

static void test_posset_rehash(void) {
    PosSet s;
    posset_init(&s);

    /* Insert enough to trigger rehash (> 70% of 4096 = 2868) */
    for (int i = 0; i < 3000; i++) {
        posset_insert(&s, i, i % 256, i * 2);
    }
    assert(posset_count(&s) == 3000);

    for (int i = 0; i < 3000; i++) {
        assert(posset_contains(&s, i, i % 256, i * 2));
    }

    posset_destroy(&s);
    printf("PASS: test_posset_rehash\n");
}

int main(void) {
    test_posset_insert_contains();
    test_posset_remove();
    test_posset_iterate();
    test_posset_duplicate_insert();
    test_posset_rehash();
    printf("All PosSet tests passed.\n");
    return 0;
}
```

- [ ] **Step 3: Run test — expect compile failure (no block_physics.h yet)**

```bash
cmake --build build/ --target test_block_physics 2>&1 | head -20
```
Expected: error — `block_physics.h` not found.

- [ ] **Step 4: Create `src/block_physics.h`**

```c
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

typedef struct {
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
```

- [ ] **Step 5: Create `src/block_physics.c` — PosSet implementation only**

```c
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
    /* Ticks implemented in Tasks 7 and 8 */
}
```

- [ ] **Step 6: Build and run tests**

```bash
cmake --build build/ --target test_block_physics 2>&1 | tail -5
cd build && ctest -V -R block_physics 2>&1 | tail -20
cd ..
```
Expected: all 5 PosSet tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/block_physics.h src/block_physics.c src/test_block_physics.c CMakeLists.txt
git commit -m "feat: add PosSet hash set and BlockPhysics skeleton with passing unit tests"
```

---

## Task 6: Gravity tick — sand falls at 10 Hz

**Files:**
- Modify: `src/block_physics.c` — implement `gravity_tick()`, call from `block_physics_update()`
- Modify: `src/test_block_physics.c` — add gravity tests

- [ ] **Step 1: Write failing gravity tests — add to `test_block_physics.c`**

Replace the stub world functions at the top of `test_block_physics.c` with a small mock world, and add test cases:

```c
/* ---- Mock world for physics tests ---- */
#define MW 8
#define MH 16
#define MD 8

static BlockID  mock_b[MH][MD][MW];   /* [y][z][x] */
static uint8_t  mock_m[MH][MD][MW];

static void mock_reset(void) {
    memset(mock_b, 0, sizeof(mock_b));
    memset(mock_m, 0, sizeof(mock_m));
}

BlockID world_get_block(World* w, int x, int y, int z) {
    (void)w;
    if (x<0||x>=MW||y<0||y>=MH||z<0||z>=MD) return 0; /* BLOCK_AIR */
    return mock_b[y][z][x];
}
bool world_set_block(World* w, int x, int y, int z, BlockID id) {
    (void)w;
    if (x<0||x>=MW||y<0||y>=MH||z<0||z>=MD) return 0;
    mock_b[y][z][x] = id;
    return 1;
}
uint8_t world_get_meta(World* w, int x, int y, int z) {
    (void)w;
    if (x<0||x>=MW||y<0||y>=MH||z<0||z>=MD) return 0;
    return mock_m[y][z][x];
}
bool world_set_meta(World* w, int x, int y, int z, uint8_t v) {
    (void)w;
    if (x<0||x>=MW||y<0||y>=MH||z<0||z>=MD) return 0;
    mock_m[y][z][x] = v;
    return 1;
}
```

Then add gravity test functions (before `main()`):

```c
static void test_gravity_sand_falls_one_step(void) {
    mock_reset();
    /* Place sand at (2,5,2) with air below at (2,4,2) */
    mock_b[5][2][2] = BLOCK_SAND;

    BlockPhysics bp;
    block_physics_init(&bp);
    posset_insert(&bp.gravity_active, 2, 5, 2);

    /* Manually fire one gravity tick */
    float player_pos[3] = {2.0f, 5.0f, 2.0f};
    /* Set accum past threshold so tick fires */
    bp.gravity_accum = 1.0f / GRAVITY_TICK_HZ + 0.001f;
    block_physics_update(&bp, NULL, player_pos, 0.0f);

    assert(mock_b[5][2][2] == BLOCK_AIR);  /* sand left */
    assert(mock_b[4][2][2] == BLOCK_SAND); /* sand arrived */

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_sand_falls_one_step\n");
}

static void test_gravity_sand_stable_on_solid(void) {
    mock_reset();
    mock_b[5][2][2] = BLOCK_SAND;
    mock_b[4][2][2] = BLOCK_STONE; /* solid below */

    BlockPhysics bp;
    block_physics_init(&bp);
    posset_insert(&bp.gravity_active, 2, 5, 2);

    float player_pos[3] = {2.0f, 5.0f, 2.0f};
    bp.gravity_accum = 1.0f / GRAVITY_TICK_HZ + 0.001f;
    block_physics_update(&bp, NULL, player_pos, 0.0f);

    assert(mock_b[5][2][2] == BLOCK_SAND); /* sand didn't move */
    assert(posset_count(&bp.gravity_active) == 0); /* settled */

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_sand_stable_on_solid\n");
}

static void test_gravity_sand_displaces_water(void) {
    mock_reset();
    mock_b[5][2][2] = BLOCK_SAND;
    mock_b[4][2][2] = BLOCK_WATER;
    mock_m[4][2][2] = 200; /* water level */

    BlockPhysics bp;
    block_physics_init(&bp);
    posset_insert(&bp.gravity_active, 2, 5, 2);

    float player_pos[3] = {2.0f, 5.0f, 2.0f};
    bp.gravity_accum = 1.0f / GRAVITY_TICK_HZ + 0.001f;
    block_physics_update(&bp, NULL, player_pos, 0.0f);

    assert(mock_b[4][2][2] == BLOCK_SAND);  /* sand now at water position */
    assert(mock_b[5][2][2] == BLOCK_WATER); /* water displaced upward */
    assert(mock_m[5][2][2] == 200);         /* water level preserved */

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_sand_displaces_water\n");
}
```

In `main()`, add the calls:
```c
    test_gravity_sand_falls_one_step();
    test_gravity_sand_stable_on_solid();
    test_gravity_sand_displaces_water();
```

- [ ] **Step 2: Run tests — expect failures**

```bash
cmake --build build/ --target test_block_physics && cd build && ctest -V -R block_physics 2>&1 | tail -20; cd ..
```
Expected: gravity tests fail (tick is a no-op stub).

- [ ] **Step 3: Implement `gravity_tick()` in `src/block_physics.c`**

Replace the `block_physics_update()` stub with the real implementation. Add `gravity_tick()` as a static function, and call it from `block_physics_update()`:

```c
/* ------------------------------------------------------------------ */
/*  Gravity tick (10 Hz)                                              */
/* ------------------------------------------------------------------ */

static void gravity_tick(BlockPhysics* bp, World* world, vec3 player_pos) {
    /* Collect positions to process (snapshot — avoid modifying set while iterating) */
    int idx = 0, x, y, z;

    /* We iterate and build a list of actions, then apply them */
    /* Max active positions in radius: safe upper bound ~200K, but practical ~1K */
    /* Use simple approach: iterate, apply, next entry */
    idx = 0;
    while (posset_iter_next(&bp->gravity_active, &idx, &x, &y, &z)) {
        /* Radius check */
        float dx = (float)x - player_pos[0];
        float dz = (float)z - player_pos[2];
        if (dx*dx + dz*dz > (float)(PHYSICS_RADIUS * PHYSICS_RADIUS)) {
            idx++;
            continue;
        }

        BlockID block = world_get_block(world, x, y, z);

        /* Only process gravity blocks */
        if (!block_is_gravity(block)) {
            posset_remove(&bp->gravity_active, x, y, z);
            /* Do not increment idx — remove shifted things, re-check same slot */
            /* Actually PosSet tombstones are in-place; idx still valid. */
            idx++;
            continue;
        }

        BlockID below = world_get_block(world, x, y - 1, z);

        if (below == BLOCK_AIR) {
            /* Fall down one block */
            bool ok_clear = world_set_block(world, x, y, z, BLOCK_AIR);
            bool ok_land  = world_set_block(world, x, y - 1, z, block);
            if (ok_clear && ok_land) {
                posset_remove(&bp->gravity_active, x, y, z);
                posset_insert(&bp->gravity_active, x, y - 1, z); /* keep falling */
                block_physics_notify(bp, x, y, z); /* neighbors may be affected */
            }
            /* If write deferred (chunk meshing), leave in set for next tick */
        } else if (below == BLOCK_WATER) {
            /* Displace water: swap sand and water, preserve water meta */
            uint8_t water_level = world_get_meta(world, x, y - 1, z);
            bool ok1 = world_set_block(world, x, y - 1, z, block);    /* sand goes down */
            bool ok2 = world_set_block(world, x, y, z, BLOCK_WATER);  /* water goes up */
            bool ok3 = world_set_meta(world, x, y, z, water_level);   /* preserve level */
            if (ok1 && ok2 && ok3) {
                posset_remove(&bp->gravity_active, x, y, z);
                posset_insert(&bp->gravity_active, x, y - 1, z); /* sand keeps falling */
                posset_insert(&bp->water_active, x, y, z);       /* displaced water flows */
            }
        } else {
            /* Solid or other — stable */
            posset_remove(&bp->gravity_active, x, y, z);
        }
        idx++;
    }
}

/* ------------------------------------------------------------------ */
/*  block_physics_update                                              */
/* ------------------------------------------------------------------ */

void block_physics_update(BlockPhysics* bp, World* world,
                          vec3 player_pos, float dt) {
    bp->gravity_accum += dt;
    bp->water_accum   += dt;

    const float gravity_dt = 1.0f / (float)GRAVITY_TICK_HZ;
    const float water_dt   = 1.0f / (float)WATER_TICK_HZ;

    while (bp->gravity_accum >= gravity_dt) {
        gravity_tick(bp, world, player_pos);
        bp->gravity_accum -= gravity_dt;
    }

    while (bp->water_accum >= water_dt) {
        /* water_tick implemented in Task 8 */
        bp->water_accum -= water_dt;
    }
}
```

- [ ] **Step 4: Run tests — expect all to pass**

```bash
cmake --build build/ --target test_block_physics && cd build && ctest -V -R block_physics 2>&1 | tail -20; cd ..
```
Expected: all PosSet tests + all 3 gravity tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/block_physics.c src/test_block_physics.c
git commit -m "feat: implement gravity tick for sand at 10Hz with unit tests"
```

---

## Task 7: Water tick — cellular automata at 5 Hz

**Files:**
- Modify: `src/block_physics.c` — implement `water_tick()`
- Modify: `src/test_block_physics.c` — add water tests

- [ ] **Step 1: Write failing water tests — add to `test_block_physics.c`**

Add these test functions:

```c
static void test_water_flows_down_to_air(void) {
    mock_reset();
    mock_b[5][2][2] = BLOCK_WATER;
    mock_m[5][2][2] = 100;
    /* y=4 is air */

    BlockPhysics bp;
    block_physics_init(&bp);
    posset_insert(&bp.water_active, 2, 5, 2);

    float player_pos[3] = {2.0f, 5.0f, 2.0f};
    bp.water_accum = 1.0f / WATER_TICK_HZ + 0.001f;
    block_physics_update(&bp, NULL, player_pos, 0.0f);

    assert(mock_b[4][2][2] == BLOCK_WATER); /* water flowed down */
    assert(mock_m[4][2][2] == 100);         /* level transferred */

    block_physics_destroy(&bp);
    printf("PASS: test_water_flows_down_to_air\n");
}

static void test_water_source_does_not_dissipate(void) {
    mock_reset();
    mock_b[5][2][2] = BLOCK_WATER;
    mock_m[5][2][2] = WATER_SOURCE_LEVEL; /* source block */
    /* Surrounded by solid — no flow possible */
    mock_b[4][2][2] = BLOCK_STONE;
    mock_b[6][2][2] = BLOCK_STONE;
    mock_b[5][2][1] = BLOCK_STONE;
    mock_b[5][2][3] = BLOCK_STONE;
    mock_b[5][1][2] = BLOCK_STONE;
    mock_b[5][3][2] = BLOCK_STONE;

    BlockPhysics bp;
    block_physics_init(&bp);
    posset_insert(&bp.water_active, 2, 5, 2);

    float player_pos[3] = {2.0f, 5.0f, 2.0f};
    /* Fire many ticks */
    for (int i = 0; i < 20; i++) {
        bp.water_accum = 1.0f / WATER_TICK_HZ + 0.001f;
        block_physics_update(&bp, NULL, player_pos, 0.0f);
    }

    assert(mock_b[5][2][2] == BLOCK_WATER);         /* source still exists */
    assert(mock_m[5][2][2] == WATER_SOURCE_LEVEL);  /* level unchanged */

    block_physics_destroy(&bp);
    printf("PASS: test_water_source_does_not_dissipate\n");
}

static void test_water_non_source_dissipates(void) {
    mock_reset();
    mock_b[5][2][2] = BLOCK_WATER;
    mock_m[5][2][2] = 4; /* small non-source level */
    /* Surround with solid so no horizontal flow */
    mock_b[4][2][2] = BLOCK_STONE;
    mock_b[5][2][1] = BLOCK_STONE;
    mock_b[5][2][3] = BLOCK_STONE;
    mock_b[5][1][2] = BLOCK_STONE;
    mock_b[5][3][2] = BLOCK_STONE;

    BlockPhysics bp;
    block_physics_init(&bp);
    posset_insert(&bp.water_active, 2, 5, 2);

    float player_pos[3] = {2.0f, 5.0f, 2.0f};
    /* Fire 3 ticks: level 4 → 2 → 0 (becomes air) */
    for (int i = 0; i < 3; i++) {
        bp.water_accum = 1.0f / WATER_TICK_HZ + 0.001f;
        block_physics_update(&bp, NULL, player_pos, 0.0f);
    }

    assert(mock_b[5][2][2] == BLOCK_AIR); /* water evaporated */

    block_physics_destroy(&bp);
    printf("PASS: test_water_non_source_dissipates\n");
}
```

Add to `main()`:
```c
    test_water_flows_down_to_air();
    test_water_source_does_not_dissipate();
    test_water_non_source_dissipates();
```

- [ ] **Step 2: Run tests — expect water tests to fail**

```bash
cmake --build build/ --target test_block_physics && cd build && ctest -V -R block_physics 2>&1 | tail -20; cd ..
```
Expected: water tests fail.

- [ ] **Step 3: Implement `water_tick()` in `src/block_physics.c`**

Add `water_tick()` as a static function above `block_physics_update()`, and call it in the existing water accumulator loop:

```c
/* ------------------------------------------------------------------ */
/*  Water tick (5 Hz)                                                 */
/* ------------------------------------------------------------------ */

static void water_tick(BlockPhysics* bp, World* world, vec3 player_pos) {
    static const int hor_dx[4] = { 1, -1,  0,  0 };
    static const int hor_dz[4] = { 0,  0,  1, -1 };

    int idx = 0, x, y, z;
    while (posset_iter_next(&bp->water_active, &idx, &x, &y, &z)) {
        float ddx = (float)x - player_pos[0];
        float ddz = (float)z - player_pos[2];
        if (ddx*ddx + ddz*ddz > (float)(PHYSICS_RADIUS * PHYSICS_RADIUS)) {
            idx++;
            continue;
        }

        BlockID block = world_get_block(world, x, y, z);
        if (block != BLOCK_WATER) {
            posset_remove(&bp->water_active, x, y, z);
            idx++;
            continue;
        }

        uint8_t level = world_get_meta(world, x, y, z);

        /* Step 1: Source refresh */
        if (level == WATER_SOURCE_LEVEL) {
            /* Source is permanent — skip dissipation */
            /* Still do flow steps below */
        }

        /* Step 2: Downward flow */
        BlockID below_b = world_get_block(world, x, y - 1, z);
        if (below_b == BLOCK_AIR) {
            world_set_block(world, x, y - 1, z, BLOCK_WATER);
            world_set_meta(world, x, y - 1, z, level);
            posset_insert(&bp->water_active, x, y - 1, z);
        } else if (below_b == BLOCK_WATER) {
            uint8_t below_level = world_get_meta(world, x, y - 1, z);
            if (below_level < level) {
                uint8_t transfer = (uint8_t)((level - below_level) / 2);
                if (transfer > 0) {
                    uint8_t new_below = (uint8_t)(below_level + transfer);
                    /* Cap at source level */
                    if (new_below > WATER_SOURCE_LEVEL) new_below = WATER_SOURCE_LEVEL;
                    world_set_meta(world, x, y - 1, z, new_below);
                    posset_insert(&bp->water_active, x, y - 1, z);
                }
            }
        }

        /* Step 3: Horizontal equalization */
        for (int d = 0; d < 4; d++) {
            int nx = x + hor_dx[d];
            int nz = z + hor_dz[d];
            BlockID nb = world_get_block(world, nx, y, nz);

            if (nb == BLOCK_AIR && level > 1) {
                world_set_block(world, nx, y, nz, BLOCK_WATER);
                world_set_meta(world, nx, y, nz, 1);
                posset_insert(&bp->water_active, nx, y, nz);
            } else if (nb == BLOCK_WATER) {
                uint8_t nlevel = world_get_meta(world, nx, y, nz);
                if (nlevel < level - 1) {
                    uint8_t transfer = (uint8_t)((level - nlevel) / 2);
                    if (transfer > 0) {
                        uint8_t new_n = (uint8_t)(nlevel + transfer);
                        if (new_n > WATER_SOURCE_LEVEL) new_n = WATER_SOURCE_LEVEL;
                        world_set_meta(world, nx, y, nz, new_n);
                        posset_insert(&bp->water_active, nx, y, nz);
                    }
                }
            }
        }

        /* Step 4: Dissipation (non-source only, AFTER flow) */
        if (level != WATER_SOURCE_LEVEL) {
            if (level <= WATER_DISSIPATION) {
                world_set_block(world, x, y, z, BLOCK_AIR);
                world_set_meta(world, x, y, z, 0);
                block_physics_notify(bp, x, y, z);
                posset_remove(&bp->water_active, x, y, z);
            } else {
                uint8_t new_level = (uint8_t)(level - WATER_DISSIPATION);
                world_set_meta(world, x, y, z, new_level);
            }
        }

        idx++;
    }
}
```

In `block_physics_update()`, replace the water stub:
```c
    while (bp->water_accum >= water_dt) {
        water_tick(bp, world, player_pos);
        bp->water_accum -= water_dt;
    }
```

- [ ] **Step 4: Run all tests**

```bash
cmake --build build/ --target test_block_physics && cd build && ctest -V -R block_physics 2>&1 | tail -30; cd ..
```
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/block_physics.c src/test_block_physics.c
git commit -m "feat: implement water cellular automata tick at 5Hz with unit tests"
```

---

## Task 8: World remesh pipeline + physics seeding

**Files:**
- Modify: `src/world.h` — update `world_update()` signature
- Modify: `src/world.c` — remesh trigger; physics seeding from generate results
- Modify: `src/main.c` — update call site

- [ ] **Step 1: Update `world_update()` signature in `src/world.h`**

Change:
```c
void world_update(World* world, vec3 player_pos);
```
to:
```c
#include "block_physics.h"  /* for BlockPhysics* parameter */
void world_update(World* world, vec3 player_pos, BlockPhysics* physics);
```

- [ ] **Step 2: Add remesh trigger and physics seeding in `src/world.c`**

Add `#include "block_physics.h"` near the top of world.c with the other includes.

**2a — Seed physics from generate results (Step 1 of `world_update()`):**

In the generate result branch (currently a no-op comment), add the seeding scan:

```c
            } else {
                /* Generate result — seed block physics active sets */
                Chunk* c = chunk;
                if (physics) {
                    int base_x = c->cx * CHUNK_X;
                    int base_z = c->cz * CHUNK_Z;
                    for (int bx = 0; bx < CHUNK_X; bx++) {
                        for (int bz = 0; bz < CHUNK_Z; bz++) {
                            for (int by = 0; by < CHUNK_Y; by++) {
                                BlockID b = chunk_get_block(c, bx, by, bz);
                                if (b == BLOCK_WATER || block_is_gravity(b)) {
                                    block_physics_notify(physics,
                                        base_x + bx, by, base_z + bz);
                                }
                            }
                        }
                    }
                }
            }
```

**2b — Remesh trigger (add new Step 4b between existing Steps 4 and 5):**

After the closing `}` of Step 4 (submit meshing) and before Step 5 (build render array), add:

```c
    /* ---- Step 4b: Re-mesh CHUNK_READY chunks flagged needs_remesh ---- */
    {
        uint32_t idx = 0;
        Chunk* chunk;
        while ((chunk = chunk_map_iter(&world->map, &idx)) != NULL) {
            if (!chunk->needs_remesh) continue;
            if (atomic_load(&chunk->state) != CHUNK_READY) continue;

            chunk->needs_remesh = false;
            atomic_store(&chunk->state, CHUNK_MESHING);

            /* Extract boundary slices */
            size_t slice_size = 16 * CHUNK_Y * sizeof(BlockID);
            Chunk* nx_pos = chunk_map_get(&world->map, chunk->cx + 1, chunk->cz);
            Chunk* nx_neg = chunk_map_get(&world->map, chunk->cx - 1, chunk->cz);
            Chunk* nz_pos = chunk_map_get(&world->map, chunk->cx, chunk->cz + 1);
            Chunk* nz_neg = chunk_map_get(&world->map, chunk->cx, chunk->cz - 1);

            BlockID* b_pos_x = nx_pos ? malloc(slice_size) : NULL;
            BlockID* b_neg_x = nx_neg ? malloc(slice_size) : NULL;
            BlockID* b_pos_z = nz_pos ? malloc(slice_size) : NULL;
            BlockID* b_neg_z = nz_neg ? malloc(slice_size) : NULL;

            if (b_pos_x) mesher_extract_boundary(nx_pos, 0, b_pos_x);
            if (b_neg_x) mesher_extract_boundary(nx_neg, 1, b_neg_x);
            if (b_pos_z) mesher_extract_boundary(nz_pos, 2, b_pos_z);
            if (b_neg_z) mesher_extract_boundary(nz_neg, 3, b_neg_z);

            /* Snapshot meta for thread-safe mesher access */
            uint8_t* meta_snap = NULL;
            if (chunk->meta) {
                meta_snap = malloc(CHUNK_BLOCKS);
                memcpy(meta_snap, chunk->meta, CHUNK_BLOCKS);
            }

            WorkItem* wi = calloc(1, sizeof(WorkItem));
            wi->type            = WORK_MESH;
            wi->chunk           = chunk;
            wi->boundary_pos_x  = b_pos_x;
            wi->boundary_neg_x  = b_neg_x;
            wi->boundary_pos_z  = b_pos_z;
            wi->boundary_neg_z  = b_neg_z;
            wi->meta_snapshot   = meta_snap;
            submit_work(world, wi);
        }
    }
```

Also add `meta_snapshot` to `WorkItem` and thread cleanup (see Task 9 — mesher changes).

- [ ] **Step 3: Update `src/main.c` call site**

Change:
```c
world_update(world, g_player.position);
```
to:
```c
block_physics_update(&g_physics, world, g_player.position, dt);
world_update(world, g_player.position, &g_physics);
```

Also add at the top (before `main()`):
```c
static BlockPhysics g_physics;
```

And in `main()`, after `player_init()`:
```c
block_physics_init(&g_physics);
```

And before `world_destroy()`:
```c
block_physics_destroy(&g_physics);
```

Add `#include "block_physics.h"` to the includes.

- [ ] **Step 4: Build to verify**

```bash
cmake --build build/ 2>&1 | tail -10
```
Expected: clean build. There will be a compile error about `wi->meta_snapshot` — that field doesn't exist yet. Move to Task 9 to add it.

---

## Task 9: Mesher meta snapshot + partial-height water surface

**Files:**
- Modify: `src/world.c` — add `meta_snapshot` to `WorkItem`; pass to mesher; free in worker
- Modify: `src/mesher.h` — add `meta` parameter to `mesher_build()`
- Modify: `src/mesher.c` — use meta for water top face height

- [ ] **Step 1: Add `meta_snapshot` to `WorkItem` in `src/world.c`**

In the `WorkItem` struct definition:

```c
typedef struct WorkItem {
    WorkType         type;
    Chunk*           chunk;
    int              seed;
    BlockID*         boundary_pos_x;
    BlockID*         boundary_neg_x;
    BlockID*         boundary_pos_z;
    BlockID*         boundary_neg_z;
    uint8_t*         meta_snapshot;  /* NEW: malloc'd copy of chunk->meta, or NULL */
    struct WorkItem* next;
} WorkItem;
```

Update the initial meshing submission in Step 4 of `world_update()` to also snapshot meta:

In the existing mesh submission loop (Step 4), after the boundary slice allocations and before `WorkItem* wi = calloc(...)`, add:
```c
            uint8_t* meta_snap = NULL;
            if (chunk->meta) {
                meta_snap = malloc(CHUNK_BLOCKS);
                memcpy(meta_snap, chunk->meta, CHUNK_BLOCKS);
            }
```
And in the WorkItem initialization:
```c
            wi->meta_snapshot   = meta_snap;
```

In `world_destroy()`, free `meta_snapshot` in the work item cleanup loop:
```c
        if (wi->type == WORK_MESH) {
            free(wi->boundary_pos_x);
            free(wi->boundary_neg_x);
            free(wi->boundary_pos_z);
            free(wi->boundary_neg_z);
            free(wi->meta_snapshot);  /* NEW */
        }
```

In `worker_func()`, in the `WORK_MESH` branch, pass `meta_snapshot` to the mesher and free it after:

```c
            MeshData* md = malloc(sizeof(MeshData));
            mesh_data_init(md);
            mesher_build(item->chunk, &neighbors, item->meta_snapshot, md);  /* add param */

            free(item->boundary_pos_x);
            free(item->boundary_neg_x);
            free(item->boundary_pos_z);
            free(item->boundary_neg_z);
            free(item->meta_snapshot);  /* NEW */
```

- [ ] **Step 2: Update `mesher_build()` signature in `src/mesher.h`**

Change:
```c
void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors, MeshData* out);
```
to:
```c
void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors,
                  const uint8_t* meta, MeshData* out);
```

- [ ] **Step 3: Update `mesher_build()` in `src/mesher.c`**

Update the signature to accept `const uint8_t* meta`.

In the face-emission loop, for the `+Y` face (face index 2) of `BLOCK_WATER`, adjust the top Y coordinate to reflect the water level. Find the `case 2: /* +Y */` block and replace it:

```c
                    case 2: /* +Y */
                    {
                        float y_top;
                        if (block == BLOCK_WATER && meta) {
                            int flat_idx = x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z;
                            uint8_t level = meta[flat_idx];
                            y_top = fy + (level > 0 ? (float)level / 255.0f : 1.0f);
                        } else {
                            y_top = fy + 1.0f;
                        }
                        pos[0][0] = fx;   pos[0][1] = y_top; pos[0][2] = fz;
                        pos[1][0] = fx+1; pos[1][1] = y_top; pos[1][2] = fz;
                        pos[2][0] = fx+1; pos[2][1] = y_top; pos[2][2] = fz+1;
                        pos[3][0] = fx;   pos[3][1] = y_top; pos[3][2] = fz+1;
                        break;
                    }
```

- [ ] **Step 4: Build and run**

```bash
cmake --build build/ 2>&1 | tail -10
```
Expected: clean build.

```bash
./build/minecraft
```
Expected: game runs. In a world with water, the water surface should render at correct heights. Sand should fall when unsupported.

- [ ] **Step 5: Commit**

```bash
git add src/world.c src/world.h src/mesher.h src/mesher.c src/main.c src/block_physics.h src/block_physics.c CMakeLists.txt
git commit -m "feat: wire block physics into game loop with remesh pipeline and partial-height water surface"
```

---

## Task 10: Smoke tests — verify end-to-end behavior

- [ ] **Step 1: Run unit test suite**

```bash
cd build && ctest -V 2>&1; cd ..
```
Expected: all tests pass.

- [ ] **Step 2: Run the game and verify sand physics**

```bash
./build/minecraft
```

Walk to a sand block on a beach. In a future build when block placement is implemented, placing a sand block above air should cause it to fall. For now, verify:
- Game starts without crashes
- Water at sea level renders with partial-height surface on flowing water
- No visual glitches or assertion failures in stderr

- [ ] **Step 3: Commit any fixups**

If any issues were found and fixed in Step 2, commit them:

```bash
git add -p
git commit -m "fix: block physics integration fixups from smoke testing"
```

---

## Quick Reference

**Build:**
```bash
cmake --build build/
```

**Run unit tests:**
```bash
cd build && ctest -V -R block_physics; cd ..
```

**Run game:**
```bash
./build/minecraft
```

**Key constants** (in `src/block_physics.h`):
- `GRAVITY_TICK_HZ 10` — sand falls 10 blocks/second
- `WATER_TICK_HZ 5` — water spreads every 200ms
- `PHYSICS_RADIUS 64` — simulation bubble radius in blocks
- `WATER_DISSIPATION 2` — level units lost per non-source water tick
- `WATER_SOURCE_LEVEL 255` — permanent water source marker
