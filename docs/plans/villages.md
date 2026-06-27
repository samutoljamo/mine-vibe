# Procedural Villages — Implementation Plan

Parent epic: **mine-vibe-07n** (Villages: procedural village structures in worldgen)

## 1. Goals & scope (v1)

Generate deterministic, seed-reproducible villages in the C/Vulkan voxel world:

- 3–6 simple houses: floor, walls, flat or peaked roof, a door gap, a window gap.
- A central well.
- Dirt/gravel paths connecting buildings to the well.
- Terrain flattened to a common platform height under each building.
- Placement is **rare** and gated to *flatish grassland above sea level*, using a
  hash like the existing tree code.
- Uses existing blocks plus a few **new decorative blocks** (planks, cobblestone,
  glass, path/gravel).

Out of scope (filed as follow-ups): villager mobs, trading.

## 2. Grounding in the existing code

Key facts confirmed by reading the source:

- `src/chunk.h`: `CHUNK_X=16`, `CHUNK_Y=256`, `CHUNK_Z=16`. Block access via
  `chunk_get_block(c,x,y,z)` / `chunk_set_block(c,x,y,z,id)` — both bounds-checked
  and **silently no-op / return AIR for out-of-range coords**. This is the single
  most important property for the per-chunk strategy (see §5): we can write a
  structure in *world coordinates* and let writes that fall outside `[0,16)` simply
  drop, so each chunk only materializes the part of a building that intersects it.
- `src/worldgen.c`:
  - `hash_pos(x,z,seed)` — the deterministic spatial hash used for tree placement
    (lines ~12–19). Villages reuse this mixing style.
  - `worldgen_generate(chunk, seed)` computes `height_map[CHUNK_X][CHUNK_Z]`, fills
    terrain, carves caves, sprinkles ores, then places trees (lines ~250–286).
    Village generation is appended **after** trees, before
    `atomic_store(&chunk->state, CHUNK_GENERATED)`.
  - `SEA_LEVEL = 62`.
- `src/block.h` / `src/block.c`: block ids are a packed enum ending in `BLOCK_COUNT`;
  `BlockDef` carries `tex_top/tex_side/tex_bottom` atlas tile indices.
  `BlockID` is `uint8_t` (safe to 255).
- `tools/gen_assets.py`: each tile is a `draw_*` generator registered in
  `TILE_GENERATORS` + `TILE_NAMES` by atlas index. Atlas is 256px / 16px tiles.
  Used indices so far: 0–11, 16, 17. Free indices 12,13,14,15 (and 18+) are
  available for new blocks.
- `src/ore.c` / `src/ore.h` + `tests/test_ore.c` + `CMakeLists.txt:336` are the
  template for an extracted **pure, unit-tested** module. Villages copy this shape.

### Architecture constraint (concurrency / merge safety)

Another agent is concurrently editing `worldgen.c` for world persistence. Therefore
**all village logic lives in a new module `src/village.c` / `src/village.h`**, and the
`worldgen.c` footprint is a **single 1–2 line call site**:

```c
#include "village.h"            /* one added include */
...
/* after the tree loop, before atomic_store(state) */
village_generate(chunk, seed, height_map);
```

`village_generate` takes the already-computed `height_map` so it never recomputes
terrain and adds no new state to `worldgen.c`.

## 3. New decorative blocks

Recommended new blocks (append to the `block.h` enum **before** `BLOCK_COUNT`):

| Block            | Purpose            | Texture approach (new `draw_*` in gen_assets.py)                          |
|------------------|--------------------|--------------------------------------------------------------------------|
| `BLOCK_PLANKS`   | walls / floor      | warm-brown plank stripes (horizontal bands + vertical seams, like `draw_wood_side` but lighter/oak) |
| `BLOCK_COBBLE`   | well rim, foundations, roof option | grey cobble: `draw_stone` base with darker mortar grid + lumps |
| `BLOCK_GLASS`    | windows            | near-transparent pale tile; `is_transparent=true`, low `light_absorb` (like leaves=2) so light passes |
| `BLOCK_PATH`     | gravel/dirt paths  | desaturated brown/grey gravel speckle (dirt base + scattered grey pebbles) |

Optional / deferred: `BLOCK_DOOR` (needs a non-cube mesh + meta state) — v1 uses a
**door gap** (air) instead, so no door block is required. Note this in the blocks task.

For each new block follow the stored recipe (`bd memories adding-a-block`):

1. Add `BLOCK_*` id to `src/block.h` enum before `BLOCK_COUNT`.
2. Add `BlockDef` row (solidity, transparency, `light_absorb`, tex indices) **and**
   a `block_representative_color` case in `src/block.c`.
3. Add a `draw_*` generator and register its atlas index in `TILE_GENERATORS` +
   `TILE_NAMES` in `tools/gen_assets.py` (use free indices 12–15).
4. Regenerate: `distrobox enter cyberismo -- python3 tools/gen_assets.py`
   (rewrites `src/assets_generated.c`).

`GLASS` is the only block needing special flags: `is_solid=true`,
`is_transparent=true`, `light_absorb` ~2, so the mesher renders neighbor faces and
light leaks through windows.

## 4. Village-cell placement math (pure, testable)

Villages are placed on a coarse **cell grid** so a village can span chunk
boundaries while every chunk independently agrees on the same villages.

Constants (in `village.h`):

```c
#define VILLAGE_CELL_CHUNKS  16          /* cell = 16x16 chunks = 256x256 blocks */
#define VILLAGE_CELL_BLOCKS  (VILLAGE_CELL_CHUNKS * CHUNK_X)
#define VILLAGE_SPAWN_PCT    25          /* ~25% of cells host a village (rare overall, since cells are huge) */
#define VILLAGE_MAX_RADIUS   40          /* blocks; buildings/paths stay within this of center */
```

Pure functions (the unit-testable core, mirroring `ore_select`):

```c
typedef struct {
    bool present;     /* does this cell host a candidate village?      */
    int  wx, wz;      /* world-space village center (block coords)      */
    int  seed;        /* per-village seed (= mix(cell, world seed))     */
} VillageCell;

/* Deterministic: which village (if any) is anchored to cell (cgx,cgz). */
VillageCell village_cell_at(int cgx, int cgz, int world_seed);

/* Suitability predicate evaluated at the village center column. Caller passes
 * the surface height and surface block at the center (from height_map / worldgen).
 * A village is built only if suitable: above sea level, on grass, and the local
 * terrain is flatish (caller supplies min/max sampled surface heights). */
bool village_is_suitable(int center_surface_h, BlockID center_surface_block,
                         int sampled_min_h, int sampled_max_h);
```

`village_cell_at`:
- `int cgx = floor_div(wx, VILLAGE_CELL_BLOCKS)` etc. (chunk → cell is computed by
  caller; the function takes cell indices directly so it is trivially testable).
- `h = hash_pos(cgx, cgz, world_seed + VILLAGE_SALT)`.
- `present = (h % 100) < VILLAGE_SPAWN_PCT`.
- center offset within the cell jittered by further hashes, clamped so the center
  plus `VILLAGE_MAX_RADIUS` stays comfortably inside the cell (avoids two adjacent
  cells overlapping): `wx = cgx*CELL + MARGIN + h2 % (CELL - 2*MARGIN)`.
- `seed = hash_pos(wx, wz, world_seed + VILLAGE_SALT2)`.

`village_is_suitable`:
- `center_surface_h >= SEA_LEVEL + 1`,
- `center_surface_block == BLOCK_GRASS`,
- flatness: `(sampled_max_h - sampled_min_h) <= VILLAGE_MAX_SLOPE` (e.g. 4).
  The caller samples a few columns around the center (using `worldgen_get_height`,
  which is pure and already exported) so suitability doesn't depend on whether the
  center chunk is loaded.

Because suitability is computed from `worldgen_get_height` (a pure function of
world seed) rather than from chunk contents, **every chunk that touches the village
reaches the same present/suitable/center/platform-height decision** without loading
neighbors. This is the crux of cross-chunk consistency.

## 5. Per-chunk intersection generation (the hard part)

`village_generate(chunk, seed, height_map)` runs per chunk and must render only the
slice of each structure that falls inside this chunk, with **no writes to neighbor
chunks**.

Algorithm:

1. Compute this chunk's world-block AABB:
   `[base_x, base_x+16) x [base_z, base_z+16)`.
2. Determine which village **cells** could reach into this chunk. A village center
   can be up to `VILLAGE_MAX_RADIUS` away, so check the cell containing the chunk
   plus the 8 neighbors (3x3 cell window). In practice, because
   `VILLAGE_MAX_RADIUS << VILLAGE_CELL_BLOCKS`, a chunk overlaps at most one
   village; iterating the 3x3 window is cheap and fully general.
3. For each candidate cell: `vc = village_cell_at(...)`. If `!vc.present`, skip.
4. Evaluate suitability using `worldgen_get_height` samples around `vc.wx,vc.wz`.
   If not suitable, skip. (Pure → same answer in every chunk.)
5. Quick reject: if the village bounding box
   `[vc.wx-R, vc.wx+R] x [vc.wz-R, vc.wz+R]` does not intersect the chunk AABB,
   skip.
6. **Generate the village layout deterministically from `vc.seed`** (see §6): this
   yields a list of structures (houses, well, path segments), each as a world-space
   box / span of `(wx, wy, wz, block)` placements. The layout is identical no
   matter which chunk is generating it — it is a pure function of `vc.seed`.
7. Emit every block of every structure by calling
   `chunk_set_block(chunk, wx - base_x, wy, wz - base_z, block)`. Coordinates
   outside `[0,16)` are silently dropped by the bounds check, so each chunk
   naturally keeps only its slice. **No neighbor writes, no cross-chunk state.**

Determinism guarantees that where two chunks share a building, both compute the
exact same blocks for the shared columns, so the seam is invisible.

### Platform height (shared across chunks)

Each building sits on a flattened platform. The platform Y for a building must be
identical in every chunk it spans, so it is derived from the **village center**, not
from local `height_map`:

```c
int platform_y = village_is_suitable ? worldgen_get_height(vc.wx, vc.wz, seed) : ...
```

i.e. the whole village shares one base height = surface height at the village center
(or each building's own center column via `worldgen_get_height`, which is pure).
Either choice is chunk-independent.

## 6. Building footprint / layout generation

From `vc.seed`, deterministically derive (all via `hash_pos`-style draws with
distinct salts):

- House count `N` in `[3,6]`.
- For each house `i`: footprint `w,d in [5,8]`, wall height `3`, roof style
  (flat cobble slab vs peaked planks), and a center position scattered around the
  village center within `VILLAGE_MAX_RADIUS`, avoiding overlap (simple rejection:
  reroll if AABB intersects an already-placed house; deterministic because the
  reroll sequence is seed-driven).
- Door gap on one wall (1 wide x 2 tall, facing the well), window gaps (1x1)
  centered on the other walls.

House emit (world coords, platform `py` from §5):
- Flatten: for the footprint, conceptually the terrain under the house is set to the
  platform; in practice the house floor at `py` is `PLANKS`, and we **carve air**
  for `py+1 .. py+wall_h+roof` and **fill DIRT/PLANKS** below `py` down to a few
  blocks (or rely on existing terrain). See §7.
- Walls: `PLANKS` perimeter, `py+1 .. py+wall_h`, minus door/window gaps; windows
  filled with `GLASS`.
- Roof: flat → `COBBLE` slab at `py+wall_h+1` over footprint; peaked → planks
  stepping up to a ridge.

Well (at village center):
- 3x3 `COBBLE` rim at platform, hollow 1x1 `WATER` core down 2–3 blocks, four corner
  `PLANKS` posts + a `PLANKS`/`COBBLE` canopy 3 blocks up.

Paths:
- For each house, a `BLOCK_PATH` line (Bresenham in world coords) from the house door
  to the well, laid at the platform surface (replace grass/dirt top with `PATH`).
  Bresenham is computed in world space, emitted per-block via `chunk_set_block`, so
  path pieces split across chunks automatically.

## 7. Terrain flattening

For each building footprint (world-space box), at platform height `py`:

- For `y` from `py+1` up to roof top: set to `AIR` (clear trees/terrain bumps inside
  the house volume) — except where walls/roof/glass are placed.
- For `y` at `py` and a few below (`py-1..py-3`): if currently `AIR`/`WATER` (i.e.
  the ground dips below platform), fill with `DIRT` to create a level base.
- Floor row at `py` = `PLANKS` over the interior, `COBBLE`/`PLANKS` under walls.

All of this is per-column and uses only `chunk_set_block` in world coords, so it
naturally clips to the chunk. Because `py` is village-center-derived (pure), a column
shared by two chunks flattens to the same Y in both.

Order vs trees: village generation runs **after** the tree loop. The flattening
clear-to-air step removes any trunk/leaf blocks that intruded into a house volume,
keeping interiors clean.

## 8. Testability

Extract the deterministic decision logic as **pure functions** in `village.c`
(declared in `village.h`), exactly like `ore_select`. Add `tests/test_village.c` and
wire it in `CMakeLists.txt` next to `test_ore` (link `src/village.c src/block.c`,
and `m` if math is used).

Pure functions to test:

- `village_cell_at(cgx, cgz, seed)`
- `village_is_suitable(center_h, center_block, min_h, max_h)`
- A layout helper, e.g. `village_house_count(vseed)` and
  `village_house_at(vseed, i) -> {wx,wz,w,d,roof,door_face}` so the layout can be
  asserted without a Chunk.

Test cases (mirroring `tests/test_ore.c`):

1. **Deterministic**: `village_cell_at` returns identical results across repeated
   calls for many `(cgx,cgz)`; same for layout helpers.
2. **Density sane**: over a large grid of cells, fraction with `present==true` is
   within tolerance of `VILLAGE_SPAWN_PCT` (e.g. 15–35%).
3. **Center in-bounds**: every present cell's `(wx,wz)` lies inside the cell minus
   margin, so adjacent cells can't overlap (assert `|center - neighbor center| >
   2*VILLAGE_MAX_RADIUS`).
4. **Suitability gating**: not suitable when `center_h <= SEA_LEVEL`, when
   `center_block != BLOCK_GRASS`, or when `max_h - min_h > VILLAGE_MAX_SLOPE`;
   suitable when all hold.
5. **Layout bounds**: every house from `village_house_at` lies within
   `VILLAGE_MAX_RADIUS` of center and houses don't overlap.
6. **Cross-chunk consistency (key test)**: pick a village straddling a chunk
   boundary; generate both chunks via `village_generate` and assert that for every
   world column in the overlap region both chunks wrote the same block stack (proves
   no seam). Use a real `Chunk` (cheap; `chunk_create`).

## 9. Task breakdown & commit sequence

Each step is a separate beads sub-issue (IDs in §11). Suggested order / commits:

1. **Decorative blocks** (`src/block.h`, `src/block.c`, `tools/gen_assets.py`,
   regen `src/assets_generated.c`). Commit: `feat(block): planks/cobble/glass/path`.
2. **Placement + suitability (pure + tested)** — `src/village.h` (constants, structs,
   pure decls), pure parts of `src/village.c`, `tests/test_village.c`, CMake wiring.
   Commit: `feat(village): deterministic cell placement + suitability (tested)`.
3. **Per-chunk building generation** — implement `village_generate` intersection loop
   + house layout/emit; add the 1–2 line call site in `worldgen.c`; add
   `src/village.c` to the `minecraft` target in `CMakeLists.txt`.
   Commit: `feat(village): per-chunk house generation`.
4. **Well + paths + terrain flattening** — well structure, Bresenham paths, footprint
   flattening; extend cross-chunk consistency test. Commit:
   `feat(village): well, paths, terrain flattening`.

Quality gates (run in cyberismo distrobox per the build skill): build, then
`ctest` (must include the new `village` test).

## 10. Risks & mitigations

- **Cross-chunk consistency**: every shared decision (present, suitable, center,
  platform Y, layout) must be a pure function of the world seed + cell indices, never
  of local chunk contents. Mitigation: route all such decisions through
  `village_cell_at` / `worldgen_get_height`; test #6 proves seams match.
- **Performance per chunk**: the 3x3 cell scan + suitability sampling runs for every
  chunk, including the vast majority with no village. Keep it cheap: a couple of
  `hash_pos` calls + early AABB reject before any layout work. Layout generation only
  runs when a village actually intersects. No allocations on the hot path (use
  fixed-size stack arrays for the structure list).
- **Interaction with caves/trees**: villages run after carving/ores/trees. Flattening
  fills holes left by caves under footprints and clears trees inside houses. A house
  placed over a cave mouth is filled to the platform, so floors are never floating.
- **Interaction with persistence (concurrent worldgen.c edit)**: villages are part of
  worldgen and are regenerated from seed; only **player edits** are persisted. Keep
  the call site to 1–2 lines so the persistence agent's diff doesn't conflict.
  Village generation must run during initial generation only (same place as trees),
  not on reload of persisted chunks — coordinate the exact call site with the
  persistence work if it changes the generate path.
- **Atlas index exhaustion / wrong UVs**: pick free indices 12–15; verify the mesher
  UV math (`tile%16`, `tile/16`) matches after regen by eyeballing
  `assets/atlas_preview.png`.
- **Door omission**: v1 ships door *gaps*; a real `BLOCK_DOOR` (custom mesh + open/
  close meta) is deferred to avoid mesher changes.

## 11. Beads issues

Sub-issues created under epic **mine-vibe-07n** (see `bd show mine-vibe-07n`).
IDs and dependencies are recorded in beads; summary in the PR / session handoff.
