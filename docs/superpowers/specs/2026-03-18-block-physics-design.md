# Block Physics System

## Overview

A simulation system for gravity-affected blocks and cellular automata water. Sand falls when unsupported; water fills containers, equalizes pressure, and dissipates over distance. Simulation runs within a 64-block radius of the player using a dirty-set scheduler that idles cheaply in a settled world.

---

## Data Model

### Block metadata

Water levels require per-block metadata that does not exist in the current storage model. A parallel `uint8_t* meta` array is added to `Chunk`, lazily allocated only for chunks that contain blocks needing metadata (currently only water). Most chunks will have `meta = NULL`.

```c
// chunk.h additions
typedef struct Chunk {
    // ... existing fields ...
    uint8_t* meta;          // Lazily allocated; NULL if no block in chunk needs metadata.
                            // Never freed until chunk_destroy() — see Threading section.
    bool     needs_remesh;  // Set when blocks change; cleared when remesh job submitted.
} Chunk;
```

Water level is stored in `meta` at the same flat index as the block (`x + z*16 + y*256`). The value 255 marks a permanent source block. Values 1–254 are flowing water that dissipates over time. Value 0 means no water — this state should not coexist with `BLOCK_WATER`; when level reaches 0 the block must be set to `BLOCK_AIR` in the same operation.

**`meta` ownership rule:** `meta` is allocated on first write and freed only in `chunk_destroy()`. It is never freed or reallocated at runtime. Worker threads may read `meta` concurrently with the main thread as long as no physics write is in progress on that chunk (see Threading section). This means up to 65 KB of metadata memory may be retained per chunk even after all water is removed; given the memory budget this is acceptable.

**Worldgen initialization:** Worldgen-placed `BLOCK_WATER` blocks must have their `meta` initialized to 255 (source level) by `worldgen_generate()`. The physics system never infers source status from block type alone — it always reads `meta`. Chunks with `meta == NULL` that somehow contain a `BLOCK_WATER` block are a bug; this cannot occur if worldgen initializes correctly.

### Block properties

A new `is_gravity` field is added to `BlockDef`. Only sand has `is_gravity = true` initially.

```c
// block.h addition
typedef struct BlockDef {
    const char* name;
    bool        is_solid;
    bool        is_transparent;
    bool        is_gravity;      // NEW: falls when unsupported
    uint8_t     tex_top;
    uint8_t     tex_side;
    uint8_t     tex_bottom;
} BlockDef;
```

---

## Dirty-Set Scheduler

The scheduler lives in `block_physics.{h,c}`. It maintains two hash sets of world positions and fires independent ticks for each physics system.

### PosSet data structure

`PosSet` is an open-addressing hash set with linear probing, keyed on packed 64-bit world positions (`(int64_t)x << 40 | (int64_t)(y & 0xFFFF) << 20 | (int64_t)(z & 0xFFFFF)`). Initial capacity: 4096 entries. Rehash threshold: 70% load. Maximum realistic population: ~50,000 entries for a fully active 64-block radius (~200 KB at 4 bytes/entry). Operations: O(1) amortized insert, remove, membership test. Iteration is over the backing array, skipping empty slots.

```c
// block_physics.h
typedef struct BlockPhysics {
    PosSet  gravity_active;   // Positions needing gravity evaluation
    PosSet  water_active;     // Positions needing water evaluation
    float   gravity_accum;    // Time accumulator for gravity ticks
    float   water_accum;      // Time accumulator for water ticks
} BlockPhysics;

void block_physics_init(BlockPhysics* bp);
void block_physics_destroy(BlockPhysics* bp);
void block_physics_update(BlockPhysics* bp, World* world, vec3 player_pos, float dt);
void block_physics_notify(BlockPhysics* bp, int x, int y, int z); // Call on any block change
```

**Tick rates:**
- Gravity: **10 Hz** (100 ms per tick)
- Water: **5 Hz** (200 ms per tick)

**Activation:** When any block changes (placed, removed, or moved by physics), `block_physics_notify()` adds that position and its 6 face-neighbors to both active sets. The caller decides which set is relevant, but over-notifying is safe — the tick logic checks block type before acting.

**Initial seeding:** Active positions are seeded incrementally, not via a bulk scan. When `world_update()` polls a `WORK_GENERATE` result and transitions a chunk to `CHUNK_GENERATED`, it scans the chunk's block array and calls `block_physics_notify()` for every gravity or water block found. This distributes the cost across many frames and avoids a startup spike.

**Radius enforcement:** Before processing any position, distance from `player_pos` is checked. Positions beyond 64 blocks are skipped but not removed — they re-enter processing when the player moves within range.

**Settling:** A position is removed from its active set when processed with no resulting change. In a fully settled world both sets drain to empty and the simulation costs nothing per frame.

---

## Threading & Chunk Safety

The physics system runs on the main thread. Worker threads run `WORK_MESH` jobs that read `chunk->blocks[]` and `chunk->meta`. To prevent data races, physics must never write to a chunk that a worker may currently be reading.

**Safe write rule:** `world_set_block()` and any `meta` write must check `atomic_load(&chunk->state)`. If the state is `CHUNK_MESHING`, the write is deferred: the target position is re-inserted into the relevant active set and skipped for this tick. The position will be retried on the next tick, by which point the mesh job will have completed and the state will have advanced.

**`needs_remesh` and re-meshing state transition:**
1. Physics writes a block; chunk state must be `CHUNK_READY` (enforced by the safe write rule above).
2. `needs_remesh = true` is set on the chunk.
3. In `world_update()`, CHUNK_READY chunks with `needs_remesh = true` are picked up. Before submitting `WORK_MESH`, the state is atomically set to `CHUNK_MESHING` and `needs_remesh` is cleared. This prevents double-submission: the next physics tick will see `CHUNK_MESHING` and defer any writes to that chunk.
4. When the mesh result is collected, the chunk transitions to `CHUNK_MESHED` then `CHUNK_READY` via the existing upload path.

**Cross-chunk writes:** `world_set_block(World* world, int x, int y, int z, BlockID id)` resolves world coordinates to chunk coordinates (floor division by 16 for x/z, y is global 0–255). It locates the target chunk in the chunk map and applies the safe write rule before modifying it. If the target chunk is absent from the chunk map (not loaded), the write is silently dropped and the position is re-queued in the physics set. The same apply-and-check logic applies for `world_set_meta()`.

---

## Gravity Physics (10 Hz)

Gravity-affected blocks (currently sand) are processed from the gravity active set each tick.

**Per-block logic:**

1. Read the block at (x, y-1).
2. **Below is air** → move sand down: set (x,y) to `BLOCK_AIR`, set (x,y-1) to `BLOCK_SAND`. Add (x,y-1) to gravity set. Call `block_physics_notify()` on (x,y) so neighboring water can fill the vacated space.
3. **Below is water** → displace: swap block IDs at (x,y) and (x,y-1), and copy `meta[(x,y-1)]` to a temporary before overwriting — the water's level must move with the water block to `(x,y)`. Add (x,y-1) to gravity set (sand continues falling). Add (x,y) to water set (displaced water may flow).
4. **Below is solid** → sand is stable. Remove from gravity set.
5. **Below is unloaded, out of map, or in a CHUNK_MESHING chunk** → skip, leave in set.

**Fall speed:** One block per gravity tick at 10 Hz = 10 blocks/second descent.

---

## Water Cellular Automata (5 Hz)

Water levels are stored as `uint8_t` in the chunk metadata array. Level 255 = permanent source; levels 1–254 = flowing water; level 0 = air (must not coexist with `BLOCK_WATER`).

**Per-block logic each tick (in this order):**

1. **Source refresh:** If `meta[pos] == 255`, reset to 255 and skip dissipation and step 3. Sources never dissipate.

2. **Downward flow:**
   - If below is air: set below to `BLOCK_WATER` with `meta = own_level`. Add below to water set.
   - If below is water with level < own_level: transfer `(own_level - below_level) / 2` units to below. Update both levels. Add below to water set. *(Fast vertical fill: a block above an empty-ish block equalizes quickly. A block above air fills instantly at full level. This asymmetry is intentional — gravity pulls water down faster than horizontal spread.)*

3. **Horizontal equalization:** For each of the 4 horizontal neighbors:
   - If neighbor is air and own_level > 1: set neighbor to `BLOCK_WATER` with level 1. Add to water set.
   - If neighbor is water with level < own_level - 1: transfer `(own_level - neighbor_level) / 2` units. Update both. Add neighbor to water set.

4. **Dissipation:** Subtract 2 from own_level. If own_level reaches 0: set block to `BLOCK_AIR`, clear `meta[pos]` to 0, call `block_physics_notify()` for neighbors, remove from water set.

*Ordering rationale: equalization and flow happen first using the level at the start of the tick, then dissipation reduces it. This prevents dissipation from starving horizontal spread before it runs.*

**Resulting behaviors:**
- Water poured into a pit fills bottom-up and equalizes to a flat surface.
- Water flowing over an edge thins as it travels — reach is proportional to source level divided by dissipation rate.
- Sealed containers fill completely.
- No infinite spreading — dissipation bounds total volume reachable from any source.

---

## Mesh Invalidation & Remeshing

When `world_set_block()` or `world_set_meta()` is called on a `CHUNK_READY` chunk, `needs_remesh = true` is set. `world_update()` picks up these chunks and re-submits them as `WORK_MESH` jobs via the existing worker pool, setting state back to `CHUNK_MESHING` first (see Threading section).

The mesher receives a snapshot copy of the `meta` array alongside the existing boundary block slices. This snapshot is allocated by the main thread before job submission and freed by the worker after the mesh is built — matching the existing boundary slice ownership pattern. The main thread's `chunk->meta` pointer remains valid and is never passed directly to worker threads.

**Partial-height water surface:** The mesher reads the water level from the `meta` snapshot to set the top face Y offset: `y_top = level / 255.0f * BLOCK_SIZE`. Full source blocks (255) render at full height. Flowing water renders shorter.

**Greedy merging of water tops:** Adjacent water blocks with different levels produce top faces at different heights and cannot be merged into a single quad. Water top faces are always emitted as individual quads (no greedy merge for the top face of water). Side and bottom faces of water blocks are still greedily merged. This increases mesh complexity in water-heavy areas but is unavoidable given per-block level variation.

---

## Integration

### Game loop order

```c
// main.c per-frame order
player_update(player, world, dt);                    // player physics (spec 2026-03-17)
block_physics_update(physics, world, cam_pos, dt);   // NEW: gravity + water ticks
world_update(world, cam_pos);                        // chunk load/unload/remesh
renderer_draw_frame(...);                            // render
```

### New files

| File | Purpose |
|------|---------|
| `src/block_physics.h` | `BlockPhysics` struct, `PosSet`, public API |
| `src/block_physics.c` | Gravity tick, water tick, dirty-set management |

### Modified files

| File | Change |
|------|--------|
| `src/block.h` | Add `is_gravity` to `BlockDef` |
| `src/block.c` | Set `is_gravity = true` for sand |
| `src/chunk.h` | Add `uint8_t* meta`, `bool needs_remesh` to `Chunk` |
| `src/chunk.c` | Allocate `meta` on first write; free in `chunk_destroy()`; init `needs_remesh = false` |
| `src/world.h` | Expose `world_set_block()` and `world_set_meta()` |
| `src/world.c` | Implement cross-chunk write with chunk-state safety check; pick up `needs_remesh` chunks; seed physics sets from `WORK_GENERATE` results |
| `src/worldgen.c` | Initialize `meta[i] = 255` for every `BLOCK_WATER` block placed |
| `src/mesher.c` | Accept `meta` snapshot parameter; partial-height water tops; skip greedy merge for water top faces |
| `src/main.c` | Call `block_physics_update()` in game loop |

---

## Constants Summary

| Constant | Value | Notes |
|----------|-------|-------|
| `GRAVITY_TICK_HZ` | 10 | Gravity ticks per second |
| `WATER_TICK_HZ` | 5 | Water ticks per second |
| `PHYSICS_RADIUS` | 64 | Simulation radius in blocks |
| `WATER_DISSIPATION` | 2 | Level units lost per water tick (non-source blocks) |
| `WATER_SOURCE_LEVEL` | 255 | Permanent source marker |
| `POSSET_INIT_CAPACITY` | 4096 | Initial hash set capacity |
| `POSSET_LOAD_THRESHOLD` | 0.7f | Rehash when load exceeds this fraction |
