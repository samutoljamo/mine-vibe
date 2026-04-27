# Lighting Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current per-face N·L sun shading with a proper voxel lighting system: per-block skylight propagation, per-vertex ambient occlusion, and 4-corner-averaged smooth lighting.

**Architecture:** New per-chunk `lights` byte array (packed `[block:4][sky:4]`). New `lighting.c` module with sky column pass + horizontal BFS + boundary-delta queue for cross-chunk propagation. New chunk states `LIGHTING`/`LIT` between `GENERATED` and `MESHING`, with a new `WORK_LIGHT` worker job. Mesher reads neighbor light slices, computes real AO, and emits per-vertex light + AO. Shader replaces N·L with per-vertex light texture lookup.

**Tech Stack:** C11, CMake/ctest, Vulkan via volk, GLSL, distrobox (cyberismo container) for all builds and tests.

**Spec:** `docs/superpowers/specs/2026-04-27-lighting-foundation-design.md`

---

## File Map

| File | Action | Purpose |
|------|--------|---------|
| `src/block.h` | Modify | Add `light_absorb`, `light_emit` to `BlockDef` |
| `src/block.c` | Modify | Fill new fields in `block_defs` table |
| `src/chunk.h` | Modify | Add `lights` array, `pending_deltas`, light accessors, `BoundaryDelta` |
| `src/chunk.c` | Modify | Free `lights` and `pending_deltas` on destroy |
| `src/lighting.h` | Create | Public API: `lighting_initial_pass`, `lighting_on_block_changed`, `lighting_consume_pending` |
| `src/lighting.c` | Create | Sky column pass, horizontal BFS, removal/addition BFS, delta queue |
| `src/mesher.h` | Modify | Extend `ChunkNeighbors` with light slices; declare `mesher_extract_light_boundary` |
| `src/mesher.c` | Modify | Real AO, smooth corner-light sampling, emit per-vertex light, light boundary extractor |
| `src/vertex.h` | Modify | Add `light` byte to `BlockVertex`, attribute descriptor |
| `shaders/block.vert` | Modify | Read `in_light`, drop N·L computation |
| `shaders/block.frag` | Modify | Apply `MIN_BRIGHT` floor, drop N·L modulation |
| `src/world.c` | Modify | Lighting state machine, `WORK_LIGHT` worker job, light boundary slices, inline relight in `world_set_block` |
| `tests/test_lighting.c` | Create | TDD tests for sky pass, BFS, cross-chunk, block-change, AO, smooth light |
| `tests/test_mesher.c` | Modify | New tests for AO and per-vertex light |
| `CMakeLists.txt` | Modify | Add `lighting.c` to library, add `test_lighting` target |

---

## Build commands (always run inside distrobox)

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake -S . -B build && cmake --build build -j"
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && ctest --test-dir build --output-on-failure"
```

For a single test target:
```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest -R lighting --output-on-failure"
```

Do NOT run `./build/minecraft` from inside distrobox — the game runs on the host. To smoke-test, exit distrobox and run `./build/minecraft` on the host. (Memory: distrobox is for building only.)

---

## Task 1: Add `light_absorb` and `light_emit` to BlockDef

**Files:**
- Modify: `src/block.h`
- Modify: `src/block.c`
- Test: `tests/test_lighting.c` (created in this task)
- Modify: `CMakeLists.txt` (add `test_lighting` target)

- [ ] **Step 1: Create `tests/test_lighting.c` with the first test**

```c
#include <assert.h>
#include <stdio.h>
#include "../src/block.h"

static void test_block_light_absorb_values(void)
{
    /* Air transmits fully. */
    assert(block_get_def(BLOCK_AIR)->light_absorb == 0);
    assert(block_get_def(BLOCK_AIR)->light_emit   == 0);

    /* Leaves and water dim slightly. */
    assert(block_get_def(BLOCK_LEAVES)->light_absorb == 2);
    assert(block_get_def(BLOCK_WATER)->light_absorb  == 2);

    /* Solid opaque blocks fully absorb. */
    assert(block_get_def(BLOCK_STONE)->light_absorb   == 15);
    assert(block_get_def(BLOCK_DIRT)->light_absorb    == 15);
    assert(block_get_def(BLOCK_GRASS)->light_absorb   == 15);
    assert(block_get_def(BLOCK_SAND)->light_absorb    == 15);
    assert(block_get_def(BLOCK_WOOD)->light_absorb    == 15);
    assert(block_get_def(BLOCK_BEDROCK)->light_absorb == 15);

    /* Spec 1: no block emits; spec 2 will fill these in. */
    assert(block_get_def(BLOCK_STONE)->light_emit == 0);

    printf("PASS: test_block_light_absorb_values\n");
}

int main(void)
{
    test_block_light_absorb_values();
    return 0;
}
```

- [ ] **Step 2: Add the test target to `CMakeLists.txt`**

After the `test_mesher` block (around line 252), append:

```cmake
add_executable(test_lighting
    tests/test_lighting.c
    src/block.c
)
target_include_directories(test_lighting PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_lighting PRIVATE m)
endif()
add_test(NAME lighting COMMAND test_lighting)
```

- [ ] **Step 3: Build to verify it fails to compile**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake -S . -B build && cmake --build build --target test_lighting"`
Expected: compile error — `BlockDef` has no field named `light_absorb`.

- [ ] **Step 4: Add the new fields to `BlockDef` in `src/block.h`**

```c
typedef struct BlockDef {
    const char* name;
    bool        is_solid;
    bool        is_transparent;
    bool        is_gravity;      /* falls when unsupported */
    uint8_t     light_absorb;    /* 0 = transmits fully, 15 = opaque to light */
    uint8_t     light_emit;      /* 0 in spec 1; spec 2 adds emitting blocks */
    uint8_t     tex_top;
    uint8_t     tex_side;
    uint8_t     tex_bottom;
} BlockDef;
```

- [ ] **Step 5: Update the `block_defs` table in `src/block.c`**

Replace the entire table with:

```c
static const BlockDef block_defs[BLOCK_COUNT] = {
    /*                  solid  transp gravity absorb emit  top side bottom */
    [BLOCK_AIR]     = { "air",     false, true,  false, 0,  0, 0,  0,  0 },
    [BLOCK_STONE]   = { "stone",   true,  false, false, 15, 0, 0,  0,  0 },
    [BLOCK_DIRT]    = { "dirt",    true,  false, false, 15, 0, 1,  1,  1 },
    [BLOCK_GRASS]   = { "grass",   true,  false, false, 15, 0, 2,  3,  1 },
    [BLOCK_SAND]    = { "sand",    true,  false, true,  15, 0, 4,  4,  4 },
    [BLOCK_WOOD]    = { "wood",    true,  false, false, 15, 0, 5,  6,  5 },
    [BLOCK_LEAVES]  = { "leaves",  true,  true,  false, 2,  0, 7,  7,  7 },
    [BLOCK_WATER]   = { "water",   false, true,  false, 2,  0, 16, 16, 16 },
    [BLOCK_BEDROCK] = { "bedrock", true,  false, false, 15, 0, 17, 17, 17 },
};
```

- [ ] **Step 6: Build and run the test**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest -R lighting --output-on-failure"`
Expected: `PASS: test_block_light_absorb_values`

- [ ] **Step 7: Build the full project to confirm no other code broke**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build -j"`
Expected: clean build.

- [ ] **Step 8: Commit**

```bash
git add src/block.h src/block.c tests/test_lighting.c CMakeLists.txt
git commit -m "feat(lighting): add light_absorb and light_emit to BlockDef"
```

---

## Task 2: Add `lights` array and accessors to `Chunk`

**Files:**
- Modify: `src/chunk.h`
- Modify: `src/chunk.c`
- Modify: `tests/test_lighting.c`
- Modify: `CMakeLists.txt` (link `chunk.c` into `test_lighting`)

- [ ] **Step 1: Add tests to `tests/test_lighting.c`**

Insert before `int main(...)`:

```c
#include "../src/chunk.h"

static void test_chunk_light_lazy_alloc_and_pack(void)
{
    Chunk* c = chunk_create(0, 0);

    /* Before any write, lights is NULL but reads return 0. */
    assert(c->lights == NULL);
    assert(chunk_get_skylight(c, 0, 0, 0)   == 0);
    assert(chunk_get_blocklight(c, 0, 0, 0) == 0);

    /* First write allocates lights. */
    chunk_set_skylight(c, 1, 2, 3, 15);
    assert(c->lights != NULL);
    assert(chunk_get_skylight(c, 1, 2, 3)   == 15);
    assert(chunk_get_blocklight(c, 1, 2, 3) == 0);

    /* Block-light nibble does not stomp sky nibble. */
    chunk_set_blocklight(c, 1, 2, 3, 9);
    assert(chunk_get_skylight(c, 1, 2, 3)   == 15);
    assert(chunk_get_blocklight(c, 1, 2, 3) == 9);

    /* Setting sky again does not stomp block. */
    chunk_set_skylight(c, 1, 2, 3, 4);
    assert(chunk_get_skylight(c, 1, 2, 3)   == 4);
    assert(chunk_get_blocklight(c, 1, 2, 3) == 9);

    /* Out-of-range writes are no-ops. */
    chunk_set_skylight(c, -1, 0, 0, 15);
    chunk_set_skylight(c,  0, -1, 0, 15);
    chunk_set_skylight(c,  0, 0, -1, 15);
    assert(chunk_get_skylight(c, 0, 0, 0) == 0);

    chunk_destroy(c);
    printf("PASS: test_chunk_light_lazy_alloc_and_pack\n");
}
```

In `main`, add the call before `return`:

```c
    test_chunk_light_lazy_alloc_and_pack();
```

- [ ] **Step 2: Add `chunk.c` to the test_lighting target in `CMakeLists.txt`**

Update the `test_lighting` target sources to:

```cmake
add_executable(test_lighting
    tests/test_lighting.c
    src/block.c
    src/chunk.c
)
```

- [ ] **Step 3: Build to verify it fails**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_lighting"`
Expected: compile error — `Chunk` has no `lights` field, no `chunk_get_skylight` declaration.

- [ ] **Step 4: Update `src/chunk.h`**

Add the `BoundaryDelta` struct and extend `Chunk`. Insert before `typedef struct Chunk`:

```c
typedef struct BoundaryDelta {
    uint8_t  face;       /* 0=+X, 1=-X, 2=+Z, 3=-Z (horizontal only) */
    uint8_t  axis_coord; /* 0..15 along the boundary's other horizontal axis */
    uint16_t y;          /* 0..CHUNK_Y-1 */
    uint8_t  new_light;  /* packed sky+block nibble */
} BoundaryDelta;
```

Extend the `Chunk` struct:

```c
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
```

Add accessor declarations + inline helpers after the existing `chunk_set_meta`:

```c
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
```

- [ ] **Step 5: Update `src/chunk.c` to free new buffers**

Replace `chunk_destroy`:

```c
void chunk_destroy(Chunk* chunk) {
    free(chunk->meta);
    free(chunk->lights);
    free(chunk->pending_deltas);
    free(chunk);
}
```

- [ ] **Step 6: Build and run the test**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest -R lighting --output-on-failure"`
Expected: both tests pass.

- [ ] **Step 7: Build the full project**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build -j"`
Expected: clean build.

- [ ] **Step 8: Commit**

```bash
git add src/chunk.h src/chunk.c tests/test_lighting.c CMakeLists.txt
git commit -m "feat(lighting): add per-chunk lights array and BoundaryDelta queue"
```

---

## Task 3: Lighting module — sky column pass

**Files:**
- Create: `src/lighting.h`
- Create: `src/lighting.c`
- Modify: `tests/test_lighting.c`
- Modify: `CMakeLists.txt` (add `lighting.c` to executable + test target)

- [ ] **Step 1: Add tests to `tests/test_lighting.c`**

Add include at the top:

```c
#include "../src/lighting.h"
```

Add helper + tests above `main`:

```c
/* Helper: empty (all-air) chunk with optional pillar. */
static Chunk* make_chunk_with_pillar(int px, int pz, int top_y, BlockID b)
{
    Chunk* c = chunk_create(0, 0);
    /* Already calloc'd to BLOCK_AIR (0). */
    if (top_y >= 0) {
        for (int y = 0; y <= top_y; y++) {
            chunk_set_block(c, px, y, pz, b);
        }
    }
    return c;
}

static void test_sky_column_empty_chunk(void)
{
    Chunk* c = make_chunk_with_pillar(0, 0, -1, BLOCK_AIR); /* no pillar */
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };

    lighting_initial_pass(c, &nb);

    /* Every cell sees full sky. */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                assert(chunk_get_skylight(c, x, y, z) == 15);

    chunk_destroy(c);
    printf("PASS: test_sky_column_empty_chunk\n");
}

/* Pillar cell itself is opaque → sky=0; cell above pillar sees sky=15.
 * Both assertions are BFS-stable:
 *   - sky_column_pass never writes into a cell whose absorb=15 except as 0.
 *   - horizontal_bfs (Task 4) computes new_sky = step_light(neighbor, 15) = 0,
 *     so it never raises stone cells. The cell above is in an open column
 *     and stays 15. */
static void test_sky_column_at_pillar(void)
{
    /* Stone pillar at (5, *, 7) reaching y=64. */
    Chunk* c = make_chunk_with_pillar(5, 7, 64, BLOCK_STONE);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };

    lighting_initial_pass(c, &nb);

    /* Cells above the pillar see sky. */
    for (int y = 65; y < CHUNK_Y; y++)
        assert(chunk_get_skylight(c, 5, y, 7) == 15);

    /* The pillar cells themselves are opaque (absorb=15) → sky=0. */
    for (int y = 0; y <= 64; y++)
        assert(chunk_get_skylight(c, 5, y, 7) == 0);

    chunk_destroy(c);
    printf("PASS: test_sky_column_at_pillar\n");
}

/* Leaves cell itself absorbs 2; the cell above sees full sky.
 * Both assertions are BFS-stable:
 *   - The cell above is in an open column at sky=15; horizontal BFS sees
 *     no neighbor brighter than 15, so it stays.
 *   - The leaves cell's sky after column pass is 13. After BFS, neighbors
 *     at the same y in open air are sky=15; BFS computes
 *     step_light(15, 2) = 13 into the leaves cell, which is not greater
 *     than the existing 13, so no change. */
static void test_sky_column_through_leaves(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 3, 100, 3, BLOCK_LEAVES);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };

    lighting_initial_pass(c, &nb);

    /* Above leaves: 15. */
    assert(chunk_get_skylight(c, 3, 101, 3) == 15);
    /* Leaves cell: 15 - absorb(2) = 13. */
    assert(chunk_get_skylight(c, 3, 100, 3) == 13);

    chunk_destroy(c);
    printf("PASS: test_sky_column_through_leaves\n");
}
```

In `main` add:

```c
    test_sky_column_empty_chunk();
    test_sky_column_under_solid();
    test_sky_column_through_leaves();
```

- [ ] **Step 2: Add `lighting.c` to test target and main executable in `CMakeLists.txt`**

Add `src/lighting.c` to the `add_executable(minecraft ...)` source list (insert near `src/world.c`):

```cmake
    src/world.c
    src/lighting.c
```

Update the `test_lighting` target:

```cmake
add_executable(test_lighting
    tests/test_lighting.c
    src/block.c
    src/chunk.c
    src/lighting.c
)
```

- [ ] **Step 3: Build to verify it fails**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake -S . -B build && cmake --build build --target test_lighting"`
Expected: error — `lighting.h` not found.

- [ ] **Step 4: Create `src/lighting.h`**

```c
#ifndef LIGHTING_H
#define LIGHTING_H

#include <stdint.h>
#include "block.h"

typedef struct Chunk Chunk;

typedef struct LightingNeighbors {
    Chunk* neg_x;
    Chunk* pos_x;
    Chunk* neg_z;
    Chunk* pos_z;
} LightingNeighbors;

/* Run the initial lighting pass for a freshly generated chunk.
 * Requires neighbors to be at least GENERATED so we can read their
 * block data (NULL neighbors are treated as fully-sky-lit at the
 * boundary, falling back gracefully at world edges).
 *
 * Updates this chunk's lights array and queues boundary deltas onto
 * neighbor chunks for them to pick up via lighting_consume_pending. */
void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb);

/* Consume any pending boundary deltas accumulated on this chunk by
 * neighbors. Runs targeted addition-BFS to apply them. Cheap if the
 * pending queue is empty. */
void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb);

/* Block-change relight for an in-place modification at local (x,y,z).
 * Walks removal-BFS (if light dropped) followed by addition-BFS (if
 * light rose) within the chunk and queues neighbor deltas as needed. */
void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id);

#endif
```

- [ ] **Step 5: Create `src/lighting.c` with sky column pass only**

```c
#include "lighting.h"
#include "chunk.h"
#include <stdlib.h>
#include <string.h>

/* Step the light value through a block of given absorption.
 * cost = max(1, absorb): air costs 1 per step, leaves cost 2, etc. */
static inline uint8_t step_light(uint8_t v, uint8_t absorb)
{
    uint8_t cost = absorb < 1 ? 1 : absorb;
    return v <= cost ? 0 : (uint8_t)(v - cost);
}

/* Sky column pass: for each (x,z), walk y from CHUNK_Y-1 down. Track
 * remaining sky light. When remaining > 0, write it; otherwise write 0. */
static void sky_column_pass(Chunk* c)
{
    for (int z = 0; z < CHUNK_Z; z++) {
        for (int x = 0; x < CHUNK_X; x++) {
            uint8_t sky = 15;
            for (int y = CHUNK_Y - 1; y >= 0; y--) {
                BlockID b = chunk_get_block(c, x, y, z);
                uint8_t absorb = block_get_def(b)->light_absorb;
                if (absorb > 0) {
                    sky = step_light(sky, absorb);
                }
                chunk_set_skylight(c, x, y, z, sky);
            }
        }
    }
}

void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb)
{
    (void)nb; /* horizontal BFS + cross-chunk added in later tasks */
    sky_column_pass(c);
}

void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb)
{
    (void)c; (void)nb;
    /* Implemented in Task 5. */
}

void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id)
{
    (void)c; (void)nb; (void)x; (void)y; (void)z;
    (void)old_id; (void)new_id;
    /* Implemented in Task 6. */
}
```

- [ ] **Step 6: Run the test**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest -R lighting --output-on-failure"`
Expected: all 3 sky-column tests pass.

- [ ] **Step 7: Build the full project**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build -j"`
Expected: clean build.

- [ ] **Step 8: Commit**

```bash
git add src/lighting.h src/lighting.c tests/test_lighting.c CMakeLists.txt
git commit -m "feat(lighting): sky column pass with leaves/water absorption"
```

---

## Task 4: Lighting module — horizontal BFS within a chunk

**Files:**
- Modify: `src/lighting.c`
- Modify: `tests/test_lighting.c`

- [ ] **Step 1: Add tests to `tests/test_lighting.c`**

Add before `main`:

```c
static void test_bfs_through_doorway(void)
{
    /* Floor of stone at y=10. Wall of stone at z=8 from y=10..15.
     * One opening at (5, 11..15, 8) so light leaks south. */
    Chunk* c = chunk_create(0, 0);
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 10, z, BLOCK_STONE);
    for (int x = 0; x < CHUNK_X; x++)
        for (int y = 10; y <= 15; y++)
            chunk_set_block(c, x, y, 8, BLOCK_STONE);
    /* Knock out a 1x5 doorway. */
    for (int y = 11; y <= 15; y++)
        chunk_set_block(c, 5, y, 8, BLOCK_AIR);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Inside the doorway, sky-15 enters from above. */
    assert(chunk_get_skylight(c, 5, 15, 8) == 15);

    /* One step away from the doorway opening (still under the wall but in
     * open space at z=9, y=11): light is 14 (one step of cost 1). */
    assert(chunk_get_skylight(c, 5, 11, 9) == 14);

    /* Light falls off as we move further south under the overhang. */
    assert(chunk_get_skylight(c, 5, 11, 10) == 13);
    assert(chunk_get_skylight(c, 5, 11, 11) == 12);

    chunk_destroy(c);
    printf("PASS: test_bfs_through_doorway\n");
}

static void test_bfs_blocked_by_solid(void)
{
    /* Solid roof at y=15 covers everything; sky cannot penetrate. */
    Chunk* c = chunk_create(0, 0);
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 15, z, BLOCK_STONE);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Above the roof: 15. */
    assert(chunk_get_skylight(c, 8, 16, 8) == 15);

    /* Under the roof: 0 (no doorway). */
    for (int y = 0; y < 15; y++)
        assert(chunk_get_skylight(c, 8, y, 8) == 0);

    chunk_destroy(c);
    printf("PASS: test_bfs_blocked_by_solid\n");
}
```

In `main`:

```c
    test_bfs_through_doorway();
    test_bfs_blocked_by_solid();
```

- [ ] **Step 2: Run to confirm failures**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest -R lighting --output-on-failure"`
Expected: `test_bfs_through_doorway` fails (light at (5, 11, 9) is 0 because no horizontal BFS yet).

- [ ] **Step 3: Add the BFS to `src/lighting.c`**

Add a fixed-capacity ring queue at the top of the file:

```c
/* BFS queue for in-chunk propagation. Max queue size = CHUNK_BLOCKS,
 * but we cap it at 1<<16 since BFS revisits are bounded by light range. */
typedef struct LightCell {
    int16_t x, y, z;
    uint8_t light;
} LightCell;

#define LIGHT_QUEUE_CAP 65536

typedef struct LightQueue {
    LightCell cells[LIGHT_QUEUE_CAP];
    uint32_t  head;
    uint32_t  tail;
} LightQueue;

static void lq_init(LightQueue* q) { q->head = 0; q->tail = 0; }
static int  lq_empty(const LightQueue* q) { return q->head == q->tail; }
static void lq_push(LightQueue* q, int x, int y, int z, uint8_t light)
{
    LightCell* c = &q->cells[q->tail++ & (LIGHT_QUEUE_CAP - 1)];
    c->x = (int16_t)x; c->y = (int16_t)y; c->z = (int16_t)z; c->light = light;
}
static LightCell lq_pop(LightQueue* q)
{
    return q->cells[q->head++ & (LIGHT_QUEUE_CAP - 1)];
}
```

Add the BFS function:

```c
/* Horizontal/vertical BFS within a single chunk. Seeds: every cell whose
 * current sky-light value is greater than 0; for each, propagate to 6
 * neighbors with neighbor_sky = max(neighbor_sky, here_sky - cost).
 *
 * NULL-neighbor cells across chunk boundaries are clamped: we read the
 * neighbor block to compute absorb, but skip writing into the neighbor
 * chunk (cross-chunk propagation is handled in Task 5). */
static void horizontal_bfs(Chunk* c, const LightingNeighbors* nb)
{
    (void)nb; /* not used yet — Task 5 adds cross-chunk seeding */

    LightQueue q;
    lq_init(&q);

    /* Seed: every cell whose sky > 0 enters the queue. The BFS will
     * relax 6-neighbors. Many seeds is fine — duplicates are filtered by
     * the "only push when neighbor_sky strictly increases" check. */
    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                uint8_t s = chunk_get_skylight(c, x, y, z);
                if (s > 0) lq_push(&q, x, y, z, s);
            }
        }
    }

    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(&q)) {
        LightCell cell = lq_pop(&q);
        for (int f = 0; f < 6; f++) {
            int nx = cell.x + dx[f];
            int ny = cell.y + dy[f];
            int nz = cell.z + dz[f];
            if (ny < 0 || ny >= CHUNK_Y) continue;
            if (nx < 0 || nx >= CHUNK_X) continue; /* cross-chunk: Task 5 */
            if (nz < 0 || nz >= CHUNK_Z) continue;

            BlockID nb_block  = chunk_get_block(c, nx, ny, nz);
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            uint8_t cur = chunk_get_skylight(c, nx, ny, nz);
            if (new_sky > cur) {
                chunk_set_skylight(c, nx, ny, nz, new_sky);
                lq_push(&q, nx, ny, nz, new_sky);
            }
        }
    }
}
```

Wire it into `lighting_initial_pass`:

```c
void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb)
{
    sky_column_pass(c);
    horizontal_bfs(c, nb);
}
```

- [ ] **Step 4: Build and run the test**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_lighting && cd build && ctest -R lighting --output-on-failure"`
Expected: all 5 tests pass (3 from Task 3, 2 new).

- [ ] **Step 5: Commit**

```bash
git add src/lighting.c tests/test_lighting.c
git commit -m "feat(lighting): horizontal BFS for in-chunk light propagation"
```

---

## Task 5: Lighting module — cross-chunk boundary deltas

**Files:**
- Modify: `src/lighting.c`
- Modify: `tests/test_lighting.c`

- [ ] **Step 1: Add cross-chunk test**

Add to `tests/test_lighting.c` before `main`:

```c
/* Two chunks side by side. Chunk A (left) has a doorway opening into
 * the BFS field; Chunk B (right) is fully open with sky=15 already.
 * After A's pass, A's eastern boundary cells should match the propagated
 * values; B should have a needs_relight flag set so it picks up A's
 * boundary contribution next. */
static void test_cross_chunk_boundary_delta(void)
{
    Chunk* a = chunk_create(0, 0);
    Chunk* b = chunk_create(1, 0); /* +X neighbor */

    /* Seal A under a stone roof at y=15 except a doorway at +X edge. */
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(a, x, 15, z, BLOCK_STONE);
    chunk_set_block(a, CHUNK_X - 1, 15, 8, BLOCK_AIR); /* doorway */

    /* B is fully open (all air), pre-lit with sky=15 by its own pass. */
    LightingNeighbors nb_b = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(b, &nb_b);

    /* Now run A's pass with B as +X neighbor. */
    LightingNeighbors nb_a = { NULL, b, NULL, NULL };
    lighting_initial_pass(a, &nb_a);

    /* Inside A under the doorway: sky=15 directly under, falls off west. */
    assert(chunk_get_skylight(a, CHUNK_X - 1, 15, 8) == 15);
    /* One step west of doorway under roof: 14. */
    assert(chunk_get_skylight(a, CHUNK_X - 2, 14, 8) == 14);

    /* B's western boundary (x=0) should not yet be re-lit. The BFS only
     * RECORDS deltas onto B's pending queue. After consume_pending runs,
     * B's boundary cells are unchanged because they were already 15. */
    assert(chunk_get_skylight(b, 0, 15, 8) == 15);

    /* Reverse case: B has a column of solid blocks at x=0. After A lights
     * via the doorway, B's x=0 column under the column should rise from 0
     * to whatever propagates from A. */
    chunk_destroy(a);
    chunk_destroy(b);

    /* Reset for second sub-case. */
    a = chunk_create(0, 0);
    b = chunk_create(1, 0);

    /* B has solid pillar at x=0 from y=10..14 — fully shaded under it. */
    for (int y = 10; y <= 14; y++)
        chunk_set_block(b, 0, y, 8, BLOCK_STONE);

    /* A is fully open. */
    lighting_initial_pass(a, (LightingNeighbors[]){ {NULL, b, NULL, NULL} });
    lighting_initial_pass(b, (LightingNeighbors[]){ {a, NULL, NULL, NULL} });

    /* After both passes, B should be flagged needs_relight (A's bright
     * boundary at x=15 wants to push light into B at x=0). */
    assert(b->needs_relight == true);

    /* Consume pending on B and re-run BFS. */
    lighting_consume_pending(b, (LightingNeighbors[]){ {a, NULL, NULL, NULL} });

    /* B's cell at (0, 12, 8) is solid stone — light=0. But (0, 12, 7)
     * under the pillar's shade now sees A's bright neighbor and gets
     * sky=14 (one step from A's x=15 edge). */
    assert(chunk_get_skylight(b, 0, 12, 7) >= 13);

    chunk_destroy(a);
    chunk_destroy(b);
    printf("PASS: test_cross_chunk_boundary_delta\n");
}
```

In `main`:

```c
    test_cross_chunk_boundary_delta();
```

- [ ] **Step 2: Run to confirm failure**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_lighting && cd build && ctest -R lighting --output-on-failure"`
Expected: assertion fails — `needs_relight` is never set.

- [ ] **Step 3: Implement boundary delta queue helpers in `src/lighting.c`**

Add at the top of the file:

```c
/* Append a boundary delta to the neighbor's pending queue. Grows on demand. */
static void push_boundary_delta(Chunk* nb_chunk, uint8_t face,
                                uint8_t axis_coord, uint16_t y, uint8_t new_light)
{
    if (nb_chunk->pending_delta_count >= nb_chunk->pending_delta_cap) {
        uint16_t new_cap = nb_chunk->pending_delta_cap == 0
            ? 64 : (uint16_t)(nb_chunk->pending_delta_cap * 2);
        BoundaryDelta* tmp = realloc(nb_chunk->pending_deltas,
                                     new_cap * sizeof(BoundaryDelta));
        if (!tmp) return; /* drop delta on OOM — eventual consistency loss */
        nb_chunk->pending_deltas    = tmp;
        nb_chunk->pending_delta_cap = new_cap;
    }
    BoundaryDelta* d = &nb_chunk->pending_deltas[nb_chunk->pending_delta_count++];
    d->face       = face;
    d->axis_coord = axis_coord;
    d->y          = y;
    d->new_light  = new_light;
    nb_chunk->needs_relight = true;
}
```

- [ ] **Step 4: Make BFS write deltas across boundaries instead of skipping**

Replace the `if (nx < 0 || nx >= CHUNK_X) continue; /* cross-chunk: Task 5 */` and the `nz` equivalent with cross-chunk delta logic. Update `horizontal_bfs`:

```c
static void horizontal_bfs(Chunk* c, const LightingNeighbors* nb)
{
    LightQueue q;
    lq_init(&q);

    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                uint8_t s = chunk_get_skylight(c, x, y, z);
                if (s > 0) lq_push(&q, x, y, z, s);
            }
        }
    }

    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(&q)) {
        LightCell cell = lq_pop(&q);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;

            /* Cross-chunk: write a delta to the neighbor's pending queue. */
            Chunk* target = c;
            int    tx = nx_, tz = nz_;
            uint8_t out_face = 0xFF;

            if (nx_ < 0) {
                target = nb ? nb->neg_x : NULL;
                tx = CHUNK_X - 1;
                out_face = 1; /* into neighbor's east face */
            } else if (nx_ >= CHUNK_X) {
                target = nb ? nb->pos_x : NULL;
                tx = 0;
                out_face = 0;
            } else if (nz_ < 0) {
                target = nb ? nb->neg_z : NULL;
                tz = CHUNK_Z - 1;
                out_face = 3;
            } else if (nz_ >= CHUNK_Z) {
                target = nb ? nb->pos_z : NULL;
                tz = 0;
                out_face = 2;
            }

            BlockID nb_block;
            if (target == c) {
                nb_block = chunk_get_block(c, tx, ny_, tz);
            } else if (target) {
                nb_block = chunk_get_block(target, tx, ny_, tz);
            } else {
                continue; /* edge of world: nothing to propagate into */
            }
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            if (target == c) {
                uint8_t cur = chunk_get_skylight(c, tx, ny_, tz);
                if (new_sky > cur) {
                    chunk_set_skylight(c, tx, ny_, tz, new_sky);
                    lq_push(&q, tx, ny_, tz, new_sky);
                }
            } else {
                /* Record on neighbor's pending queue. axis_coord is the
                 * coordinate along the boundary (z for ±X, x for ±Z). */
                uint8_t axis = (out_face == 0 || out_face == 1)
                             ? (uint8_t)tz : (uint8_t)tx;
                uint8_t cur  = chunk_get_skylight(target, tx, ny_, tz);
                if (new_sky > cur) {
                    push_boundary_delta(target, out_face, axis,
                                        (uint16_t)ny_, new_sky);
                }
            }
        }
    }
}
```

- [ ] **Step 5: Implement `lighting_consume_pending`**

Replace the stub:

```c
void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb)
{
    if (c->pending_delta_count == 0) {
        c->needs_relight = false;
        return;
    }

    LightQueue q;
    lq_init(&q);

    /* Apply each pending delta directly into c->lights, then seed a queue
     * with the changed cells so addition-BFS spreads from them. */
    for (uint16_t i = 0; i < c->pending_delta_count; i++) {
        BoundaryDelta d = c->pending_deltas[i];
        int x, z;
        switch (d.face) {
        case 0: x = 0;            z = d.axis_coord; break;  /* +X face: write at x=0 */
        case 1: x = CHUNK_X - 1;  z = d.axis_coord; break;
        case 2: x = d.axis_coord; z = 0;            break;
        case 3: x = d.axis_coord; z = CHUNK_Z - 1;  break;
        default: continue;
        }
        uint8_t cur = chunk_get_skylight(c, x, d.y, z);
        uint8_t v   = d.new_light & 0x0F;
        if (v > cur) {
            chunk_set_skylight(c, x, d.y, z, v);
            lq_push(&q, x, d.y, z, v);
        }
    }

    c->pending_delta_count = 0;
    c->needs_relight       = false;

    /* Re-run the same propagation logic. We share BFS code by lifting
     * the inner relax loop into a helper; for simplicity here we duplicate
     * the relaxation step. */
    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(&q)) {
        LightCell cell = lq_pop(&q);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;

            Chunk* target = c;
            int tx = nx_, tz = nz_;
            uint8_t out_face = 0xFF;
            if (nx_ < 0)        { target = nb ? nb->neg_x : NULL; tx = CHUNK_X - 1; out_face = 1; }
            else if (nx_ >= CHUNK_X) { target = nb ? nb->pos_x : NULL; tx = 0; out_face = 0; }
            else if (nz_ < 0)        { target = nb ? nb->neg_z : NULL; tz = CHUNK_Z - 1; out_face = 3; }
            else if (nz_ >= CHUNK_Z) { target = nb ? nb->pos_z : NULL; tz = 0; out_face = 2; }

            if (!target) continue;

            BlockID nb_block = chunk_get_block(target, tx, ny_, tz);
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            if (target == c) {
                uint8_t cur = chunk_get_skylight(c, tx, ny_, tz);
                if (new_sky > cur) {
                    chunk_set_skylight(c, tx, ny_, tz, new_sky);
                    lq_push(&q, tx, ny_, tz, new_sky);
                }
            } else {
                uint8_t axis = (out_face == 0 || out_face == 1)
                             ? (uint8_t)tz : (uint8_t)tx;
                uint8_t cur  = chunk_get_skylight(target, tx, ny_, tz);
                if (new_sky > cur) {
                    push_boundary_delta(target, out_face, axis,
                                        (uint16_t)ny_, new_sky);
                }
            }
        }
    }
}
```

- [ ] **Step 6: Build and run**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_lighting && cd build && ctest -R lighting --output-on-failure"`
Expected: all 6 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/lighting.c tests/test_lighting.c
git commit -m "feat(lighting): cross-chunk boundary deltas and consume_pending"
```

---

## Task 6: Lighting module — block-change relight

**Files:**
- Modify: `src/lighting.c`
- Modify: `tests/test_lighting.c`

- [ ] **Step 1: Add tests**

Append to `tests/test_lighting.c` before `main`:

```c
/* Place an opaque block at a sky-exposed cell. Column under it should go dark. */
static void test_relight_place_opaque_at_sky(void)
{
    Chunk* c = chunk_create(0, 0);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);
    /* All cells are 15. */
    assert(chunk_get_skylight(c, 8, 64, 8) == 15);

    /* Place stone at y=128 column (8,8). */
    chunk_set_block(c, 8, 128, 8, BLOCK_STONE);
    lighting_on_block_changed(c, &nb, 8, 128, 8, BLOCK_AIR, BLOCK_STONE);

    /* Cells directly below should now be 0 (sky no longer reaches). */
    assert(chunk_get_skylight(c, 8, 127, 8) == 0);
    assert(chunk_get_skylight(c, 8, 64, 8)  == 0);
    /* Side cells refilled by horizontal BFS from neighbors. */
    assert(chunk_get_skylight(c, 7, 64, 8) == 15);
    assert(chunk_get_skylight(c, 8, 64, 7) == 15);

    chunk_destroy(c);
    printf("PASS: test_relight_place_opaque_at_sky\n");
}

/* Break an opaque block in a roof. Column below should re-light. */
static void test_relight_break_opaque_roof(void)
{
    Chunk* c = chunk_create(0, 0);

    /* Stone roof at y=20 over the whole chunk. */
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 20, z, BLOCK_STONE);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);
    assert(chunk_get_skylight(c, 5, 10, 5) == 0);

    /* Break the roof above (5, *, 5). */
    chunk_set_block(c, 5, 20, 5, BLOCK_AIR);
    lighting_on_block_changed(c, &nb, 5, 20, 5, BLOCK_STONE, BLOCK_AIR);

    /* Cells directly under the new hole get sky=15. */
    assert(chunk_get_skylight(c, 5, 19, 5) == 15);
    assert(chunk_get_skylight(c, 5, 10, 5) == 15);
    /* Adjacent cells under the rest of the roof get less than 15. */
    assert(chunk_get_skylight(c, 4, 19, 5) == 14);
    assert(chunk_get_skylight(c, 5, 19, 4) == 14);

    chunk_destroy(c);
    printf("PASS: test_relight_break_opaque_roof\n");
}
```

In `main`:

```c
    test_relight_place_opaque_at_sky();
    test_relight_break_opaque_roof();
```

- [ ] **Step 2: Run to confirm failure**

Expected: relight tests fail; the stub does nothing.

- [ ] **Step 3: Implement `lighting_on_block_changed` in `src/lighting.c`**

Replace the stub with:

```c
/* Removal-BFS: visit cells reachable from (x,y,z) whose only support was a
 * value <= the cell's old contribution. Zero them and queue brighter
 * surviving neighbors as re-propagation seeds. */
static void removal_bfs(Chunk* c, const LightingNeighbors* nb,
                        int x, int y, int z,
                        uint8_t old_value,
                        LightQueue* re_propagate)
{
    LightQueue rq;
    lq_init(&rq);
    lq_push(&rq, x, y, z, old_value);

    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(&rq)) {
        LightCell cell = lq_pop(&rq);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;
            if (nx_ < 0 || nx_ >= CHUNK_X) continue;  /* edge: spec 1 — local only */
            if (nz_ < 0 || nz_ >= CHUNK_Z) continue;

            uint8_t nb_sky = chunk_get_skylight(c, nx_, ny_, nz_);
            if (nb_sky == 0) continue;

            /* If this neighbor's light could have been sustained by us
             * (cell.light - cost == nb_sky), zero it and continue removal.
             * Otherwise it's brighter than we contributed — queue it as a
             * re-propagation seed. */
            BlockID b = chunk_get_block(c, nx_, ny_, nz_);
            uint8_t cost = block_get_def(b)->light_absorb;
            if (cost < 1) cost = 1;

            if (nb_sky < cell.light) {
                chunk_set_skylight(c, nx_, ny_, nz_, 0);
                lq_push(&rq, nx_, ny_, nz_, nb_sky);
            } else {
                lq_push(re_propagate, nx_, ny_, nz_, nb_sky);
            }
            (void)cost;
        }
    }

    (void)nb; /* cross-chunk removal is bounded; see Risks in spec */
}

/* Addition-BFS: relax outward from a queue of cells. Same code as the
 * inner loop of horizontal_bfs but operates on a caller-supplied queue. */
static void addition_bfs(Chunk* c, const LightingNeighbors* nb, LightQueue* q)
{
    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(q)) {
        LightCell cell = lq_pop(q);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;

            Chunk* target = c;
            int tx = nx_, tz = nz_;
            uint8_t out_face = 0xFF;
            if (nx_ < 0)             { target = nb ? nb->neg_x : NULL; tx = CHUNK_X - 1; out_face = 1; }
            else if (nx_ >= CHUNK_X) { target = nb ? nb->pos_x : NULL; tx = 0;            out_face = 0; }
            else if (nz_ < 0)        { target = nb ? nb->neg_z : NULL; tz = CHUNK_Z - 1; out_face = 3; }
            else if (nz_ >= CHUNK_Z) { target = nb ? nb->pos_z : NULL; tz = 0;            out_face = 2; }

            if (!target) continue;

            BlockID nb_block  = chunk_get_block(target, tx, ny_, tz);
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            if (target == c) {
                uint8_t cur = chunk_get_skylight(c, tx, ny_, tz);
                if (new_sky > cur) {
                    chunk_set_skylight(c, tx, ny_, tz, new_sky);
                    lq_push(q, tx, ny_, tz, new_sky);
                }
            } else {
                uint8_t axis = (out_face == 0 || out_face == 1)
                             ? (uint8_t)tz : (uint8_t)tx;
                uint8_t cur = chunk_get_skylight(target, tx, ny_, tz);
                if (new_sky > cur) {
                    push_boundary_delta(target, out_face, axis,
                                        (uint16_t)ny_, new_sky);
                }
            }
        }
    }
}

void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id)
{
    uint8_t old_absorb = block_get_def(old_id)->light_absorb;
    uint8_t new_absorb = block_get_def(new_id)->light_absorb;
    uint8_t cur_sky    = chunk_get_skylight(c, x, y, z);

    LightQueue add_q;
    lq_init(&add_q);

    if (new_absorb > old_absorb) {
        /* Block became more opaque: removal-BFS from the cell's old value. */
        chunk_set_skylight(c, x, y, z, 0);
        removal_bfs(c, nb, x, y, z, cur_sky, &add_q);
    }

    /* Re-evaluate this column's sky exposure. Walk up from y and re-run
     * sky_column_pass for column (x,z). Cheap — single column. */
    {
        uint8_t sky = 15;
        for (int yy = CHUNK_Y - 1; yy >= 0; yy--) {
            BlockID b = chunk_get_block(c, x, yy, z);
            uint8_t a = block_get_def(b)->light_absorb;
            if (a > 0) sky = step_light(sky, a);
            uint8_t was = chunk_get_skylight(c, x, yy, z);
            if (sky > was) {
                chunk_set_skylight(c, x, yy, z, sky);
                lq_push(&add_q, x, yy, z, sky);
            }
        }
    }

    /* Also seed addition from the changed cell's 6 neighbors so light
     * flows back in around a removed opaque. */
    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };
    for (int f = 0; f < 6; f++) {
        int nx_ = x + dx[f], ny_ = y + dy[f], nz_ = z + dz[f];
        if (ny_ < 0 || ny_ >= CHUNK_Y) continue;
        if (nx_ < 0 || nx_ >= CHUNK_X) continue;
        if (nz_ < 0 || nz_ >= CHUNK_Z) continue;
        uint8_t s = chunk_get_skylight(c, nx_, ny_, nz_);
        if (s > 0) lq_push(&add_q, nx_, ny_, nz_, s);
    }

    addition_bfs(c, nb, &add_q);
}
```

- [ ] **Step 4: Build and run**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_lighting && cd build && ctest -R lighting --output-on-failure"`
Expected: all 8 lighting tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/lighting.c tests/test_lighting.c
git commit -m "feat(lighting): block-change relight with removal/addition BFS"
```

---

## Task 7: Mesher — real AO computation

**Files:**
- Modify: `src/mesher.c`
- Modify: `tests/test_mesher.c`

- [ ] **Step 1: Add an AO unit test to `tests/test_mesher.c`**

Replace the file with:

```c
#include "mesher.h"
#include "chunk.h"
#include "block.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

static void test_solid_chunk_mesh(void)
{
    Chunk* chunk = chunk_create(0, 0);
    for (int y = 0; y < 64; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                chunk_set_block(chunk, x, y, z, BLOCK_STONE);
    atomic_store(&chunk->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(chunk, &nb, NULL, &md);

    assert(md.vertices != NULL);
    assert(md.indices  != NULL);
    assert(md.vertex_count > 0);
    assert(md.index_count  > 0);
    assert(md.index_count == md.vertex_count / 4 * 6);
    assert(md.vertex_cap >= md.vertex_count);
    assert(md.index_cap  >= md.index_count);

    mesh_data_free(&md);
    chunk_destroy(chunk);
    printf("PASS: test_solid_chunk_mesh\n");
}

/* Place a single isolated stone block; verify all 4 corners of every face
 * have AO=3 (no neighbors → no occlusion). */
static void test_ao_isolated_block(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* All 24 vertices (6 faces × 4 corners) of the block should have ao=3. */
    assert(md.vertex_count == 24);
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        assert(md.vertices[i].ao == 3);
    }

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_ao_isolated_block\n");
}

/* Place a stone block with one side-neighbor on top. The +Y face's vertex
 * adjacent to that neighbor should have AO < 3. */
static void test_ao_with_neighbor_on_top(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    /* Side neighbor at (9, 65, 8): touches the +X edge of the +Y face's
     * top vertex. */
    chunk_set_block(c, 9, 65, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* Find the +Y face vertices of the (8,64,8) block. The +Y face is
     * normal_id == 2. Quad has 4 vertices; the two with x≈9 (positive-X
     * side) should have AO < 3 because of the (9,65,8) neighbor. */
    int saw_occluded = 0;
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        if (md.vertices[i].normal != 2) continue;
        if (md.vertices[i].pos[1] < 64.5f || md.vertices[i].pos[1] > 65.5f) continue;
        /* The +Y face quad of (8,64,8) lives at y=65. Its 4 corners are
         * at x in {8,9} × z in {8,9}. */
        if (md.vertices[i].pos[0] > 8.5f) {
            assert(md.vertices[i].ao < 3); /* occluded by +X neighbor */
            saw_occluded = 1;
        } else {
            assert(md.vertices[i].ao == 3); /* the -X side is unoccluded */
        }
    }
    assert(saw_occluded);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_ao_with_neighbor_on_top\n");
}

int main(void)
{
    test_solid_chunk_mesh();
    test_ao_isolated_block();
    test_ao_with_neighbor_on_top();
    return 0;
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_mesher && cd build && ctest -R mesher --output-on-failure"`
Expected: `test_ao_with_neighbor_on_top` fails — current mesher emits `ao=3` always.

- [ ] **Step 3: Add an AO helper to `src/mesher.c`**

Insert before `mesher_build`:

```c
/* Standard 3-block AO test. side1, side2 are the two edge-adjacent
 * neighbors on the air side of the face; corner is the corner-adjacent
 * neighbor. Returns 0 (max occlusion) to 3 (none). */
static uint8_t ao_value(bool side1, bool side2, bool corner)
{
    if (side1 && side2) return 0;
    return (uint8_t)(3 - ((int)side1 + (int)side2 + (int)corner));
}

/* Lookup helper that returns true if the block at chunk-local (x,y,z),
 * accounting for crossing into a neighbor's BlockID slice, is solid
 * (i.e. would occlude AO/light). */
static bool is_solid_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_Y) return false;

    BlockID b;
    if (x < 0) {
        if (!nb || !nb->neg_x) return false;
        b = nb->neg_x[(z < 0 ? 0 : (z >= CHUNK_Z ? CHUNK_Z - 1 : z)) * CHUNK_Y + y];
    } else if (x >= CHUNK_X) {
        if (!nb || !nb->pos_x) return false;
        b = nb->pos_x[(z < 0 ? 0 : (z >= CHUNK_Z ? CHUNK_Z - 1 : z)) * CHUNK_Y + y];
    } else if (z < 0) {
        if (!nb || !nb->neg_z) return false;
        b = nb->neg_z[x * CHUNK_Y + y];
    } else if (z >= CHUNK_Z) {
        if (!nb || !nb->pos_z) return false;
        b = nb->pos_z[x * CHUNK_Y + y];
    } else {
        b = chunk_get_block(c, x, y, z);
    }
    return !block_is_transparent(b) && b != BLOCK_AIR;
}

/* For face dir (0=+X..5=-Z), fill ao[4] with computed AO for the 4 corners.
 * Vertex ordering matches the existing emit_quad order. */
static void compute_face_ao(const Chunk* c, const ChunkNeighbors* nb,
                            int x, int y, int z, int face, uint8_t ao[4])
{
    /* Per-face air-side neighbor offset and corner basis vectors.
     * For each of the 4 corners (matching emit_quad UV order), we sample
     * side1, side2, corner at offsets relative to the face's air block. */

    /* Air block one step in face direction. */
    static const int fdx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int fdy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int fdz[6] = { 0,  0,  0,  0,  1, -1 };

    /* Corner basis: u and v step in the face plane. Per face, define u
     * and v unit vectors (as integer offsets) and the 4 corners' (u,v)
     * sign combinations matching emit_quad's vertex order [v0..v3]. */
    int ux[6] = {0,0,1,1,-1,1}, uy[6] = {0,0,0,0,0,0}, uz[6] = {1,-1,0,0,0,0};
    int vx[6] = {0,0,0,0,0,0},  vy[6] = {1,1,0,0,1,1}, vz[6] = {0,0,1,-1,0,0};

    /* Corners (in emit_quad order):
     *   v0 = (-u, -v) i.e. -u/-v
     *   v1 = (+u, -v)
     *   v2 = (+u, +v)
     *   v3 = (-u, +v)
     * Verified against the pos[] tables in emit code. */
    int signs_u[4] = { -1, +1, +1, -1 };
    int signs_v[4] = { -1, -1, +1, +1 };

    int ax = x + fdx[face], ay = y + fdy[face], az = z + fdz[face];

    for (int i = 0; i < 4; i++) {
        int su = signs_u[i], sv = signs_v[i];
        bool side1 = is_solid_at(c, nb,
            ax + su * ux[face], ay + su * uy[face], az + su * uz[face]);
        bool side2 = is_solid_at(c, nb,
            ax + sv * vx[face], ay + sv * vy[face], az + sv * vz[face]);
        bool corner = is_solid_at(c, nb,
            ax + su * ux[face] + sv * vx[face],
            ay + su * uy[face] + sv * vy[face],
            az + su * uz[face] + sv * vz[face]);
        ao[i] = ao_value(side1, side2, corner);
    }
}
```

- [ ] **Step 4: Replace the hardcoded AO in `mesher_build`**

Find this line in `mesher_build` (currently `uint8_t ao[4] = {3, 3, 3, 3}; /* Default: no AO */`) and replace with:

```c
                    uint8_t ao[4];
                    compute_face_ao(chunk, neighbors, x, y, z, face, ao);
```

- [ ] **Step 5: Build and run**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_mesher && cd build && ctest -R mesher --output-on-failure"`
Expected: all 3 mesher tests pass.

- [ ] **Step 6: Verify the corner basis indices are correct by inspecting failures**

If `test_ao_with_neighbor_on_top` still fails, the `ux/uy/uz/vx/vy/vz` vectors are wrong for the +Y face. Cross-check against the `pos[]` table in `mesher_build`'s `case 2:` branch: the 4 corners of +Y face are at (x, y+1, z), (x+1, y+1, z), (x+1, y+1, z+1), (x, y+1, z+1) — confirming u=+X, v=+Z and corners follow the `signs_u/signs_v` pattern above.

If you needed to fix any face's basis vectors, re-run the test until it passes.

- [ ] **Step 7: Commit**

```bash
git add src/mesher.c tests/test_mesher.c
git commit -m "feat(lighting): real per-vertex AO in mesher"
```

---

## Task 8: Mesher — smooth lighting, vertex `light` byte, light boundary slices

**Files:**
- Modify: `src/vertex.h`
- Modify: `src/mesher.h`
- Modify: `src/mesher.c`
- Modify: `tests/test_mesher.c`

- [ ] **Step 1: Add a smooth-light test to `tests/test_mesher.c`**

Append before `int main`:

```c
/* With a fully-lit chunk (sky=15 everywhere via lighting_initial_pass),
 * every emitted vertex should have light=15. */
static void test_smooth_light_uniform(void)
{
    /* Chunk is empty except a single floating block. */
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    /* Manually flood the chunk with sky=15 (skip lighting module to keep
     * mesher tests independent). */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    for (uint32_t i = 0; i < md.vertex_count; i++) {
        assert(md.vertices[i].light == 15);
    }

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_smooth_light_uniform\n");
}

/* With sky=0 everywhere except one cell at light=12, a +Y face vertex
 * adjacent to that lit cell should average down toward 12. */
static void test_smooth_light_partial(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    /* Default sky=0 everywhere (no allocation). Set just one cell. */
    chunk_set_skylight(c, 8, 65, 8, 12); /* the air-side cell above the +Y face */

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* The +Y face should average non-zero contributions across 4 corners.
     * Only the face_block (8,65,8) has light=12; the side1/side2/corner
     * neighbors are all 0. With our averaging "non-zero only" rule, the
     * single non-zero contributor gives light=12 at every corner. */
    int saw = 0;
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        if (md.vertices[i].normal != 2) continue;
        assert(md.vertices[i].light == 12);
        saw++;
    }
    assert(saw == 4);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_smooth_light_partial\n");
}
```

In `main`:

```c
    test_smooth_light_uniform();
    test_smooth_light_partial();
```

- [ ] **Step 2: Run to confirm failure**

Expected: `BlockVertex` has no `light` field — compile error.

- [ ] **Step 3: Update `src/vertex.h`**

Replace `BlockVertex` definition and `vertex_attr_descs` with:

```c
typedef struct BlockVertex {
    float    pos[3];   /* 12 */
    float    uv[2];    /* 8 */
    uint8_t  normal;   /* 1 — 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z */
    uint8_t  ao;       /* 1 — 0..3 */
    uint8_t  light;    /* 1 — 0..15 (sky channel; spec 1) */
    uint8_t  _pad;     /* 1 — reserved (spec 2 may pack block-light here) */
} BlockVertex;

_Static_assert(sizeof(BlockVertex) == 24, "BlockVertex must be 24 bytes");
```

Update the attribute descs to 5 entries:

```c
static inline void vertex_attr_descs(VkVertexInputAttributeDescription out[5]) {
    out[0] = (VkVertexInputAttributeDescription){
        .location = 0, .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0,
    };
    out[1] = (VkVertexInputAttributeDescription){
        .location = 1, .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT, .offset = 12,
    };
    out[2] = (VkVertexInputAttributeDescription){
        .location = 2, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 20,
    };
    out[3] = (VkVertexInputAttributeDescription){
        .location = 3, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 21,
    };
    out[4] = (VkVertexInputAttributeDescription){
        .location = 4, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 22,
    };
}
```

- [ ] **Step 4: Update pipeline call sites that consume `vertex_attr_descs`**

Search for callers:

```bash
grep -n vertex_attr_descs src/
```

Each caller passes a stack array sized `[4]` and an `attributeDescriptionCount = 4`. Update both: array size `[5]` and count `5`.

For `src/pipeline.c` and `src/renderer.c` (or wherever the pipeline create info lives), find the block resembling:

```c
VkVertexInputAttributeDescription attrs[4];
vertex_attr_descs(attrs);
...
.vertexAttributeDescriptionCount = 4,
.pVertexAttributeDescriptions = attrs,
```

Change to `attrs[5]` and `vertexAttributeDescriptionCount = 5`.

- [ ] **Step 5: Extend `ChunkNeighbors` and `mesher.h`**

Update `src/mesher.h`:

```c
typedef struct ChunkNeighbors {
    const BlockID* pos_x; /* x=0 slice of +X neighbor */
    const BlockID* neg_x; /* x=15 slice of -X neighbor */
    const BlockID* pos_z; /* z=0 slice of +Z neighbor */
    const BlockID* neg_z; /* z=15 slice of -Z neighbor */
    const uint8_t* pos_x_lights; /* matching light slices; NULL = treat as 0 */
    const uint8_t* neg_x_lights;
    const uint8_t* pos_z_lights;
    const uint8_t* neg_z_lights;
} ChunkNeighbors;
```

Add to declarations:

```c
/* Extract a light slice from one face of a chunk. Out layout matches
 * the BlockID slice from mesher_extract_boundary. */
void mesher_extract_light_boundary(const Chunk* chunk, int face, uint8_t* out);
```

- [ ] **Step 6: Implement `mesher_extract_light_boundary` in `src/mesher.c`**

Append at the end of the file:

```c
void mesher_extract_light_boundary(const Chunk* chunk, int face, uint8_t* out)
{
    switch (face) {
    case 0: /* x=0 */
        for (int z = 0; z < CHUNK_Z; z++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[z * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[0 + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    case 1: /* x=15 */
        for (int z = 0; z < CHUNK_Z; z++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[z * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[(CHUNK_X - 1) + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    case 2: /* z=0 */
        for (int x = 0; x < CHUNK_X; x++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[x * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[x + 0 * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    case 3: /* z=15 */
        for (int x = 0; x < CHUNK_X; x++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[x * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[x + (CHUNK_Z - 1) * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    }
}
```

- [ ] **Step 7: Add light sampling helpers + `compute_face_light` in `src/mesher.c`**

Insert after the `is_solid_at` helper:

```c
/* Read packed [block:4][sky:4] light byte at (x,y,z), crossing into
 * neighbor light slices as needed. Returns 0 if neighbor has no slice. */
static uint8_t light_byte_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_Y) return 0;

    if (x < 0) {
        if (!nb || !nb->neg_x_lights) return 0;
        int zc = z < 0 ? 0 : (z >= CHUNK_Z ? CHUNK_Z - 1 : z);
        return nb->neg_x_lights[zc * CHUNK_Y + y];
    } else if (x >= CHUNK_X) {
        if (!nb || !nb->pos_x_lights) return 0;
        int zc = z < 0 ? 0 : (z >= CHUNK_Z ? CHUNK_Z - 1 : z);
        return nb->pos_x_lights[zc * CHUNK_Y + y];
    } else if (z < 0) {
        if (!nb || !nb->neg_z_lights) return 0;
        return nb->neg_z_lights[x * CHUNK_Y + y];
    } else if (z >= CHUNK_Z) {
        if (!nb || !nb->pos_z_lights) return 0;
        return nb->pos_z_lights[x * CHUNK_Y + y];
    } else {
        if (!c->lights) return 0;
        return c->lights[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
    }
}

static inline uint8_t sky_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    return light_byte_at(c, nb, x, y, z) & 0x0F;
}

/* Smooth corner light: average non-zero sky values from up to 4 cells
 * meeting at the corner on the air side of the face. Returns 0..15. */
static uint8_t corner_sky(uint8_t face_block, uint8_t side1, uint8_t side2, uint8_t corner)
{
    int sum = 0, count = 0;
    if (face_block) { sum += face_block; count++; }
    if (side1)      { sum += side1;      count++; }
    if (side2)      { sum += side2;      count++; }
    if (corner)     { sum += corner;     count++; }
    return count == 0 ? 0 : (uint8_t)(sum / count);
}

/* Per-corner smooth light. Same basis vectors as compute_face_ao. */
static void compute_face_light(const Chunk* c, const ChunkNeighbors* nb,
                               int x, int y, int z, int face, uint8_t light[4])
{
    static const int fdx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int fdy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int fdz[6] = { 0,  0,  0,  0,  1, -1 };

    int ux[6] = {0,0,1,1,-1,1}, uy[6] = {0,0,0,0,0,0}, uz[6] = {1,-1,0,0,0,0};
    int vx[6] = {0,0,0,0,0,0},  vy[6] = {1,1,0,0,1,1}, vz[6] = {0,0,1,-1,0,0};
    int signs_u[4] = { -1, +1, +1, -1 };
    int signs_v[4] = { -1, -1, +1, +1 };

    int ax = x + fdx[face], ay = y + fdy[face], az = z + fdz[face];

    uint8_t face_block = sky_at(c, nb, ax, ay, az);

    for (int i = 0; i < 4; i++) {
        int su = signs_u[i], sv = signs_v[i];
        uint8_t s1 = sky_at(c, nb,
            ax + su * ux[face], ay + su * uy[face], az + su * uz[face]);
        uint8_t s2 = sky_at(c, nb,
            ax + sv * vx[face], ay + sv * vy[face], az + sv * vz[face]);
        uint8_t cn = sky_at(c, nb,
            ax + su * ux[face] + sv * vx[face],
            ay + su * uy[face] + sv * vy[face],
            az + su * uz[face] + sv * vz[face]);
        light[i] = corner_sky(face_block, s1, s2, cn);
    }
}
```

- [ ] **Step 8: Wire the light into `emit_quad` and `mesher_build`**

Update `emit_quad` to take a `const uint8_t light[4]` arg:

```c
static void emit_quad(MeshData* md,
                      float pos[4][3],
                      float uv[4][2],
                      uint8_t normal_id,
                      const uint8_t ao[4],
                      const uint8_t light[4])
{
    ensure_capacity(md, 4, 6);
    uint32_t base = md->vertex_count;

    for (int i = 0; i < 4; i++) {
        BlockVertex* v = &md->vertices[md->vertex_count++];
        v->pos[0] = pos[i][0];
        v->pos[1] = pos[i][1];
        v->pos[2] = pos[i][2];
        v->uv[0]  = uv[i][0];
        v->uv[1]  = uv[i][1];
        v->normal = normal_id;
        v->ao     = ao[i];
        v->light  = light[i];
        v->_pad   = 0;
    }

    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 3;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
    } else {
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
    }
}
```

In `mesher_build`, after `compute_face_ao(...)`:

```c
                    uint8_t ao[4];
                    uint8_t light[4];
                    compute_face_ao   (chunk, neighbors, x, y, z, face, ao);
                    compute_face_light(chunk, neighbors, x, y, z, face, light);
```

And update the `emit_quad` call to pass `light`:

```c
                    emit_quad(out, pos, uv, (uint8_t)face, ao, light);
```

- [ ] **Step 9: Build and run**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build --target test_mesher && cd build && ctest -R mesher --output-on-failure"`
Expected: all 5 mesher tests pass.

Run the full test suite:

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 10: Commit**

```bash
git add src/vertex.h src/mesher.h src/mesher.c src/pipeline.c src/renderer.c tests/test_mesher.c
git commit -m "feat(lighting): smooth per-vertex skylight + light-boundary slice extractor"
```

---

## Task 9: Shaders — read per-vertex light, drop N·L

**Files:**
- Modify: `shaders/block.vert`
- Modify: `shaders/block.frag`

This task has no unit test (Vulkan-bound). Verification is manual: build the project, run the game on the host, and check the visual.

- [ ] **Step 1: Replace `shaders/block.vert`**

```glsl
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_normal;
layout(location = 3) in uint in_ao;
layout(location = 4) in uint in_light;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 sun_direction;
    vec4 sun_color;
    float ambient;
} ubo;

layout(push_constant) uniform PushConstants {
    vec4 chunk_offset;
} pc;

layout(location = 0) out vec2  frag_uv;
layout(location = 1) out float frag_light;
layout(location = 2) out float frag_ao;

void main() {
    vec3 world_pos = in_pos + pc.chunk_offset.xyz;
    gl_Position = ubo.proj * ubo.view * vec4(world_pos, 1.0);

    frag_uv    = in_uv;
    frag_light = float(in_light) / 15.0;
    frag_ao    = float(in_ao) / 3.0;
}
```

- [ ] **Step 2: Replace `shaders/block.frag`**

```glsl
#version 450

layout(location = 0) in vec2  frag_uv;
layout(location = 1) in float frag_light;
layout(location = 2) in float frag_ao;

layout(set = 0, binding = 1) uniform sampler2D tex_atlas;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 sun_direction;
    vec4 sun_color;
    float ambient;
} ubo;

layout(location = 0) out vec4 out_color;

const float MIN_BRIGHT = 0.08;

void main() {
    vec4 tex_color = texture(tex_atlas, frag_uv);
    if (tex_color.a < 0.5) discard;

    float sky       = max(frag_light, MIN_BRIGHT);
    float ao_factor = 0.4 + 0.6 * frag_ao;
    vec3  lit       = tex_color.rgb * sky * ubo.sun_color.rgb * ao_factor;
    out_color       = vec4(lit, 1.0);
}
```

- [ ] **Step 3: Build to verify shaders compile and pipeline matches**

Run: `distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build -j"`
Expected: clean build. Note that the running game will currently show all faces near black (light=0 everywhere) because Task 10 has not yet wired the lighting worker — that is expected.

- [ ] **Step 4: Commit**

```bash
git add shaders/block.vert shaders/block.frag
git commit -m "feat(lighting): shaders read per-vertex light, drop N·L"
```

---

## Task 10: World integration — lighting state machine + worker

**Files:**
- Modify: `src/chunk.h` (state enum)
- Modify: `src/world.c` (worker + state machine)

This task is threading-heavy. There is no unit test for the state machine itself; we rely on the in-game smoke test in Task 11.

- [ ] **Step 1: Add `CHUNK_LIGHTING` and `CHUNK_LIT` to `ChunkState` in `src/chunk.h`**

```c
typedef enum ChunkState {
    CHUNK_UNLOADED = 0,
    CHUNK_GENERATING,
    CHUNK_GENERATED,
    CHUNK_LIGHTING,
    CHUNK_LIT,
    CHUNK_MESHING,
    CHUNK_READY,
} ChunkState;
```

- [ ] **Step 2: Extend `WorkType` and `WorkItem` in `src/world.c`**

Replace the `WorkType` enum and `WorkItem` struct:

```c
typedef enum WorkType {
    WORK_GENERATE,
    WORK_LIGHT,
    WORK_MESH,
} WorkType;

typedef struct WorkItem {
    WorkType         type;
    Chunk*           chunk;
    int              seed;

    /* For WORK_MESH: boundary block + light slices (malloc'd, freed by worker) */
    BlockID*         boundary_pos_x;
    BlockID*         boundary_neg_x;
    BlockID*         boundary_pos_z;
    BlockID*         boundary_neg_z;
    uint8_t*         light_pos_x;
    uint8_t*         light_neg_x;
    uint8_t*         light_pos_z;
    uint8_t*         light_neg_z;

    /* For WORK_LIGHT: pointers to neighbor chunks (chunks live as long
     * as the world; worker is allowed to read them). NULL means edge of
     * the loaded world or the neighbor isn't ready. */
    Chunk*           lighting_neg_x;
    Chunk*           lighting_pos_x;
    Chunk*           lighting_neg_z;
    Chunk*           lighting_pos_z;

    /* Snapshot of chunk->meta at submission time (calloc if meta==NULL) */
    uint8_t*         meta_snapshot;

    struct WorkItem* next;
} WorkItem;
```

- [ ] **Step 3: Add the WORK_LIGHT branch to `worker_func`**

After the `WORK_GENERATE` branch and before `WORK_MESH`, insert:

```c
        } else if (item->type == WORK_LIGHT) {
            LightingNeighbors lnb = {
                .neg_x = item->lighting_neg_x,
                .pos_x = item->lighting_pos_x,
                .neg_z = item->lighting_neg_z,
                .pos_z = item->lighting_pos_z,
            };
            lighting_initial_pass(item->chunk, &lnb);
            lighting_consume_pending(item->chunk, &lnb);

            ResultItem* result = malloc(sizeof(ResultItem));
            if (!result) {
                fprintf(stderr, "worker_func: out of memory for light ResultItem\n");
                free(item);
                continue;
            }
            result->chunk = item->chunk;
            result->mesh_data = NULL;
            result->next = NULL;

            pt_mutex_lock(&world->result_mutex);
            result->next = world->result_head;
            world->result_head = result;
            pt_mutex_unlock(&world->result_mutex);
```

Add at the top of `world.c`:

```c
#include "lighting.h"
```

- [ ] **Step 4: Update mesh-result handling and result dispatch in `world_update`**

In the result-processing block of `world_update`, currently the "Generate result" else-branch covers any non-mesh result. We now have two non-mesh result types: generate (state still `CHUNK_GENERATING`) and lighting (state `CHUNK_LIGHTING`). Distinguish them by current state at result-handling time.

Replace the `else { /* Generate result ... */ }` block with:

```c
            } else {
                int state = atomic_load(&chunk->state);
                if (state == CHUNK_LIGHTING) {
                    /* Lighting completed. */
                    atomic_store(&chunk->state, CHUNK_LIT);
                } else {
                    /* Generate completed (worldgen sets state to GENERATED
                     * before pushing the result, but we re-confirm here). */
                    if (bp) {
                        int base_x = chunk->cx * CHUNK_X;
                        int base_z = chunk->cz * CHUNK_Z;
                        for (int lx = 0; lx < CHUNK_X; lx++) {
                            for (int lz = 0; lz < CHUNK_Z; lz++) {
                                for (int ly = 0; ly < CHUNK_Y; ly++) {
                                    BlockID b = chunk_get_block(chunk, lx, ly, lz);
                                    if (b == BLOCK_WATER || block_is_gravity(b)) {
                                        block_physics_notify(bp,
                                            base_x + lx, ly, base_z + lz);
                                    }
                                }
                            }
                        }
                    }
                }
            }
```

- [ ] **Step 5: Insert a new submit step between Step 3 (load) and Step 4 (mesh)**

After Step 3 ("Load missing chunks") and before Step 4 ("Submit meshing"), add:

```c
    /* ---- Step 3b: Submit lighting for GENERATED chunks ---- */
    {
        uint32_t idx = 0;
        Chunk* chunk;
        int light_submits = 0;

        while ((chunk = chunk_map_iter(&world->map, &idx)) != NULL
               && light_submits < 32) {
            if (atomic_load(&chunk->state) != CHUNK_GENERATED) continue;

            Chunk* nx_pos = chunk_map_get(&world->map, chunk->cx + 1, chunk->cz);
            Chunk* nx_neg = chunk_map_get(&world->map, chunk->cx - 1, chunk->cz);
            Chunk* nz_pos = chunk_map_get(&world->map, chunk->cx, chunk->cz + 1);
            Chunk* nz_neg = chunk_map_get(&world->map, chunk->cx, chunk->cz - 1);

            if (nx_pos && atomic_load(&nx_pos->state) < CHUNK_GENERATED) continue;
            if (nx_neg && atomic_load(&nx_neg->state) < CHUNK_GENERATED) continue;
            if (nz_pos && atomic_load(&nz_pos->state) < CHUNK_GENERATED) continue;
            if (nz_neg && atomic_load(&nz_neg->state) < CHUNK_GENERATED) continue;

            atomic_store(&chunk->state, CHUNK_LIGHTING);

            WorkItem* wi = calloc(1, sizeof(WorkItem));
            wi->type           = WORK_LIGHT;
            wi->chunk          = chunk;
            wi->lighting_neg_x = nx_neg;
            wi->lighting_pos_x = nx_pos;
            wi->lighting_neg_z = nz_neg;
            wi->lighting_pos_z = nz_pos;
            submit_work(world, wi);
            light_submits++;
        }
    }
```

- [ ] **Step 6: Update Step 4 (mesh submit) to require `CHUNK_LIT` and to extract light slices**

Change the candidate-collection state check from `CHUNK_GENERATED` to `CHUNK_LIT`, and the neighbor-readiness check from `< CHUNK_GENERATED` to `< CHUNK_LIT`. Same for Step 4b (re-mesh). Also extract light slices alongside block slices.

Replace the relevant block in Step 4 with:

```c
                if (atomic_load(&chunk->state) != CHUNK_LIT) continue;
                Chunk* nx_pos = chunk_map_get(&world->map, chunk->cx + 1, chunk->cz);
                Chunk* nx_neg = chunk_map_get(&world->map, chunk->cx - 1, chunk->cz);
                Chunk* nz_pos = chunk_map_get(&world->map, chunk->cx, chunk->cz + 1);
                Chunk* nz_neg = chunk_map_get(&world->map, chunk->cx, chunk->cz - 1);
                if (nx_pos && atomic_load(&nx_pos->state) < CHUNK_LIT) continue;
                if (nx_neg && atomic_load(&nx_neg->state) < CHUNK_LIT) continue;
                if (nz_pos && atomic_load(&nz_pos->state) < CHUNK_LIT) continue;
                if (nz_neg && atomic_load(&nz_neg->state) < CHUNK_LIT) continue;
```

(Repeat the same change in the mesh-submit loop and in Step 4b.)

In the boundary-extraction code, after each `mesher_extract_boundary(...)` call, add a parallel light-slice extraction:

```c
            uint8_t* lb_pos_x = NULL, *lb_neg_x = NULL, *lb_pos_z = NULL, *lb_neg_z = NULL;
            size_t   light_slice_size = 16 * CHUNK_Y * sizeof(uint8_t);

            if (nx_pos) {
                b_pos_x  = malloc(slice_size);
                lb_pos_x = malloc(light_slice_size);
                mesher_extract_boundary       (nx_pos, 0, b_pos_x);
                mesher_extract_light_boundary (nx_pos, 0, lb_pos_x);
            }
            if (nx_neg) {
                b_neg_x  = malloc(slice_size);
                lb_neg_x = malloc(light_slice_size);
                mesher_extract_boundary       (nx_neg, 1, b_neg_x);
                mesher_extract_light_boundary (nx_neg, 1, lb_neg_x);
            }
            if (nz_pos) {
                b_pos_z  = malloc(slice_size);
                lb_pos_z = malloc(light_slice_size);
                mesher_extract_boundary       (nz_pos, 2, b_pos_z);
                mesher_extract_light_boundary (nz_pos, 2, lb_pos_z);
            }
            if (nz_neg) {
                b_neg_z  = malloc(slice_size);
                lb_neg_z = malloc(light_slice_size);
                mesher_extract_boundary       (nz_neg, 3, b_neg_z);
                mesher_extract_light_boundary (nz_neg, 3, lb_neg_z);
            }

            WorkItem* wi = calloc(1, sizeof(WorkItem));
            wi->type           = WORK_MESH;
            wi->chunk          = chunk;
            wi->boundary_pos_x = b_pos_x;
            wi->boundary_neg_x = b_neg_x;
            wi->boundary_pos_z = b_pos_z;
            wi->boundary_neg_z = b_neg_z;
            wi->light_pos_x    = lb_pos_x;
            wi->light_neg_x    = lb_neg_x;
            wi->light_pos_z    = lb_pos_z;
            wi->light_neg_z    = lb_neg_z;
            wi->meta_snapshot  = take_meta_snapshot(chunk);
            submit_work(world, wi);
```

(Apply the same pattern to the re-mesh loop in Step 4b.)

- [ ] **Step 7: Wire light slices into the worker's mesh path**

In `worker_func`, in the `WORK_MESH` branch, populate the light fields when constructing `ChunkNeighbors`:

```c
            ChunkNeighbors neighbors = {
                .pos_x        = item->boundary_pos_x,
                .neg_x        = item->boundary_neg_x,
                .pos_z        = item->boundary_pos_z,
                .neg_z        = item->boundary_neg_z,
                .pos_x_lights = item->light_pos_x,
                .neg_x_lights = item->light_neg_x,
                .pos_z_lights = item->light_pos_z,
                .neg_z_lights = item->light_neg_z,
            };
```

After `mesher_build`, free the light slices alongside the block slices:

```c
            free(item->boundary_pos_x);
            free(item->boundary_neg_x);
            free(item->boundary_pos_z);
            free(item->boundary_neg_z);
            free(item->light_pos_x);
            free(item->light_neg_x);
            free(item->light_pos_z);
            free(item->light_neg_z);
            free(item->meta_snapshot);
```

Apply the same free in `world_destroy`'s leftover-work-item cleanup (the `if (wi->type == WORK_MESH)` block):

```c
            if (wi->type == WORK_MESH) {
                free(wi->boundary_pos_x);
                free(wi->boundary_neg_x);
                free(wi->boundary_pos_z);
                free(wi->boundary_neg_z);
                free(wi->light_pos_x);
                free(wi->light_neg_x);
                free(wi->light_pos_z);
                free(wi->light_neg_z);
                free(wi->meta_snapshot);
            }
```

- [ ] **Step 8: Update unload safety check**

In Step 2 (unload distant chunks), the existing check skips chunks in `CHUNK_GENERATING` and `CHUNK_MESHING`. Extend to also skip `CHUNK_LIGHTING`:

```c
                if (state == CHUNK_GENERATING ||
                    state == CHUNK_LIGHTING   ||
                    state == CHUNK_MESHING)
                    continue;
```

- [ ] **Step 9: Build and smoke-test**

Build:

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && cmake --build build -j"
```

Then exit distrobox and run on host:

```bash
./build/minecraft
```

Expected: world loads. Caves and overhangs should now look dark. You should see soft AO where blocks meet at corners. The fixed sun-direction face shading is gone — instead, faces under overhangs are darker than open faces because of skylight.

If chunks appear pure black, the lighting worker isn't running or the mesher isn't reading the light slices — check the state-machine wiring.

- [ ] **Step 10: Commit**

```bash
git add src/chunk.h src/world.c
git commit -m "feat(lighting): worker-stage lighting before meshing, light boundary slices"
```

---

## Task 11: World integration — block-change inline relight + final smoke test

**Files:**
- Modify: `src/world.c`

- [ ] **Step 1: Inline relight in `world_set_block`**

Replace `world_set_block` with:

```c
bool world_set_block(World* world, int x, int y, int z, BlockID id) {
    if (y < 0 || y >= CHUNK_Y) return false;

    int cx, cz, lx, lz;
    world_to_chunk(x, z, &cx, &cz, &lx, &lz);

    Chunk* chunk = chunk_map_get(&world->map, cx, cz);
    if (!chunk) return false;

    int state = atomic_load(&chunk->state);
    if (state == CHUNK_MESHING) return false; /* deferred — safe write rule */
    if (state == CHUNK_LIGHTING) return false; /* deferred — same rule */
    if (state < CHUNK_GENERATED) return false;

    BlockID old_id = chunk_get_block(chunk, lx, y, lz);
    chunk_set_block(chunk, lx, y, lz, id);

    /* Run inline relight if the chunk has been lit at least once. The
     * BFS is bounded to 15 levels and runs fast for single-block changes. */
    if (state >= CHUNK_LIT) {
        Chunk* nx_pos = chunk_map_get(&world->map, cx + 1, cz);
        Chunk* nx_neg = chunk_map_get(&world->map, cx - 1, cz);
        Chunk* nz_pos = chunk_map_get(&world->map, cx, cz + 1);
        Chunk* nz_neg = chunk_map_get(&world->map, cx, cz - 1);
        LightingNeighbors lnb = {
            .neg_x = nx_neg, .pos_x = nx_pos,
            .neg_z = nz_neg, .pos_z = nz_pos,
        };
        lighting_on_block_changed(chunk, &lnb, lx, y, lz, old_id, id);
    }

    chunk->needs_remesh = true;
    return true;
}
```

- [ ] **Step 2: Build and smoke-test placement / breakage**

Build inside distrobox, run on host. Place a block (or break one — depends on existing controls) and verify:

1. Placing an opaque block on the ground in a sunlit area causes the cells *under* it to darken (visible if you place a wide column).
2. Breaking an opaque ceiling block causes the cells under it to brighten.
3. No threading crashes during the place/break loop.

If the visual update does not appear, confirm `needs_remesh = true` triggers a remesh in Step 4b (existing path).

- [ ] **Step 3: Run the full test suite once more**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest --output-on-failure"
```

Expected: all tests pass — lighting (8), mesher (5), block_physics, agent_json, net, ui, remote_player, client.

- [ ] **Step 4: Commit**

```bash
git add src/world.c
git commit -m "feat(lighting): inline relight on world_set_block"
```

- [ ] **Step 5: Final manual smoke test (host)**

Run `./build/minecraft` and verify:

- [ ] Sunlit terrain looks bright; no checkerboarding from per-vertex light.
- [ ] Caves are dark (floored at MIN_BRIGHT ≈ 8% of texture brightness — navigable but obviously cave-dark).
- [ ] Overhangs cast soft skylight falloff into shaded areas.
- [ ] AO darkens block-meeting corners (look at where blocks of terrain meet).
- [ ] Place/break updates lighting in real time (no need to reload chunks).
- [ ] Multiplayer (if configured): a remote-placed block also relights on the local client because chunk packets re-trigger the GENERATED → LIGHTING flow.

---

## Spec coverage

| Spec section | Tasks |
|------|-------|
| Storage and data model (`lights`, accessors, packed nibbles) | Task 2 |
| `BlockDef.light_absorb`, `light_emit` | Task 1 |
| `lighting_initial_pass` (sky pass + BFS + cross-chunk) | Tasks 3, 4, 5 |
| `lighting_on_block_changed` | Task 6 |
| `lighting_consume_pending` | Task 5 |
| `BoundaryDelta` queue on `Chunk` | Tasks 2, 5 |
| Mesher AO computation | Task 7 |
| Mesher smooth lighting + `light` byte | Task 8 |
| `ChunkNeighbors` extension + `mesher_extract_light_boundary` | Task 8 |
| Vertex format with `light` byte at location 4 | Task 8 |
| `block.vert` / `block.frag` | Task 9 |
| `CHUNK_LIGHTING` / `CHUNK_LIT` states + lighting worker | Task 10 |
| Mesh worker reads light slices | Task 10 |
| `world_set_block` inline relight | Task 11 |
| Tests for sky pass, BFS, cross-chunk, block-change | Tasks 3, 4, 5, 6 |
| Tests for AO and smooth light | Tasks 7, 8 |

## Out of scope (deferred to later specs)

- Block-light sources (torches, glowstone) — spec 2.
- Day/night sun color animation, sky color, fog — spec 3.
- Server-side lighting computation — client-only.
- Persistence of computed light to disk — chunks aren't persisted yet.
