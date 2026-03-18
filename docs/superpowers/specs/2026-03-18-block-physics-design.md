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
    uint8_t* meta;          // Lazily allocated, NULL if no metadata needed
    bool     needs_remesh;  // Set when blocks change, cleared after re-mesh submitted
} Chunk;
```

Water level is stored in `meta` at the same index as the block. Level 0 means no water (should not occur — use BLOCK_AIR instead). Level 255 is a permanent source block. Values 1–254 are flowing water that dissipates over time.

### Block properties

A new `is_gravity` field is added to `BlockDef`. Only sand has `is_gravity = true` initially; future block types (gravel, etc.) opt in the same way.

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
void block_physics_notify(BlockPhysics* bp, int x, int y, int z); // Called on any block change
```

**Tick rates:**
- Gravity: **10 Hz** (100 ms per tick) — fast enough for responsive falling sand
- Water: **5 Hz** (200 ms per tick) — slower spread gives natural-looking flow

**Activation:** When any block changes (placed, removed, or moved by physics), `block_physics_notify()` adds that position and its 6 face-neighbors to the relevant active sets. On initial load, the 64-block radius is scanned to seed both sets with active block types.

**Radius enforcement:** Before processing any position, distance from player is checked. Positions beyond 64 blocks are skipped but not removed from the set — they re-enter processing when the player moves within range.

**Settling:** A position is removed from its active set when it is processed and produces no change. In a fully settled world, both sets drain to empty and the simulation costs nothing.

---

## Gravity Physics (10 Hz)

Gravity-affected blocks (currently sand) are processed in the gravity active set each tick.

**Per-block logic:**

1. Read the block directly below.
2. **Below is air** → move sand down: set current position to `BLOCK_AIR`, set below to `BLOCK_SAND`. Add the new position to the gravity set (continues falling next tick). Add old position and neighbors to the water set (water may fill the vacated space).
3. **Below is water** → displace water: swap sand and water positions. Add the displaced water position to the water set. Add the new sand position to the gravity set.
4. **Below is solid** → sand is stable. Remove from gravity set.
5. **Below is unloaded or out of radius** → skip, leave in set for later.

**Fall speed:** One block per tick at 10 Hz = 10 blocks/second. A 10-block column of unsupported sand fully settles in 1 second.

---

## Water Cellular Automata (5 Hz)

Water levels are stored as `uint8_t` in the chunk metadata array. Level 255 = permanent source; levels 1–254 = flowing water; level 0 = no water (air).

**Per-block logic each tick:**

1. **Downward flow:** If the block directly below is air, set it to `BLOCK_WATER` with level `min(255, own_level)`. If below is already water with a lower level, equalize: transfer half the difference downward. Add the below position to the water active set.

2. **Horizontal equalization:** For each of the 4 horizontal neighbors, if the neighbor's level is lower than `own_level - 1`, transfer `(own_level - neighbor_level) / 2` units to the neighbor (integer division). Add changed neighbors to the water active set.

3. **Dissipation:** Non-source blocks (level < 255) lose 2 units per tick. When level reaches 0, set block to `BLOCK_AIR` and remove from water set. This gives a finite reach from any source — a level-200 block can travel approximately 100 ticks × horizontal spread before dissipating.

4. **Source permanence:** Blocks with level 255 are reset to 255 before the flow step each tick. They never dissipate.

**Resulting behaviors:**
- Water poured into a pit fills bottom-up and equalizes to a flat surface.
- Water flowing over an edge thins as it travels — it has a natural reach proportional to source level.
- Sealed containers fill completely.
- No infinite spreading — dissipation rate bounds the total volume reachable from any source.

---

## Mesh Invalidation & Remeshing

When `chunk_set_block()` or `chunk_set_meta()` is called, the owning chunk's `needs_remesh` flag is set. `world_update()` is extended to pick up `CHUNK_READY` chunks with `needs_remesh = true` and re-submit them as `WORK_MESH` jobs through the existing worker pool. The upload path is unchanged.

**Partial-height water surface:** The mesher reads the water level from chunk metadata to compute the top face Y offset for water blocks. A full block (level 255) renders at full height; flowing water renders at `level / 255.0 * BLOCK_HEIGHT`. This is the only mesher change required.

---

## Integration

### Game loop order

```c
// main.c per-frame order
player_update(player, world, dt);               // player physics (spec 2026-03-17)
block_physics_update(physics, world, cam_pos, dt); // NEW: gravity + water ticks
world_update(world, cam_pos);                   // chunk load/unload/remesh
renderer_draw_frame(...);
```

### New files

| File | Purpose |
|------|---------|
| `src/block_physics.h` | `BlockPhysics` struct, public API |
| `src/block_physics.c` | Gravity tick, water tick, dirty-set management |

### Modified files

| File | Change |
|------|--------|
| `src/block.h` | Add `is_gravity` to `BlockDef` |
| `src/block.c` | Set `is_gravity = true` for sand |
| `src/chunk.h` | Add `uint8_t* meta`, `bool needs_remesh` to `Chunk` |
| `src/chunk.c` | Lazy-allocate/free `meta`; initialize `needs_remesh = false` |
| `src/world.h` | Expose `world_set_block()` for physics writes |
| `src/world.c` | Pick up `needs_remesh` chunks in `world_update()` |
| `src/mesher.c` | Read water metadata for partial-height top face |
| `src/main.c` | Call `block_physics_update()` in game loop |

---

## Constants Summary

| Constant | Value | Notes |
|----------|-------|-------|
| `GRAVITY_TICK_HZ` | 10 | Gravity ticks per second |
| `WATER_TICK_HZ` | 5 | Water ticks per second |
| `PHYSICS_RADIUS` | 64 | Simulation radius in blocks |
| `WATER_DISSIPATION` | 2 | Level units lost per water tick |
| `WATER_SOURCE_LEVEL` | 255 | Permanent source marker |
