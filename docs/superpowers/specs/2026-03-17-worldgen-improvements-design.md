# World Generation Improvements Design

## Overview

Improve world generation with multi-octave ridged noise terrain for mountains, 3D noise cave systems (spaghetti + cheese), and fix two bugs (trees on water, camera spawning inside terrain).

## Goals

- Varied terrain with flat plains transitioning into sharp mountain ridges
- Underground cave systems with both winding tunnels and open caverns
- Caves mostly buried with occasional surface openings on steep faces
- Fix tree placement on underwater grass blocks
- Fix player spawn to always be above terrain surface

## Non-Goals

- Biome system (future work)
- Ore generation (future work)
- Cave water/lava flooding (kept dry for now)
- New block types

## Terrain Noise System

Replace the single Perlin FBM heightmap with a three-layer system using FastNoiseLite (already in project):

### Layer 1: Base Continentalness

- Type: Perlin FBM
- Frequency: ~0.002
- Octaves: 2
- Purpose: Smooth base height variation (plains vs elevated regions)

### Layer 2: Mountain Ridged Noise

- Base noise type: `FNL_NOISE_PERLIN`
- Fractal type: `FNL_FRACTAL_RIDGED` (native FastNoiseLite support)
- Frequency: ~0.005
- Octaves: 4
- Output range: [-1, 1]. Clamp negative values: `ridged = fmaxf(raw_ridged, 0.0f)` to preserve sharp ridge character where noise crosses zero.
- Purpose: Sharp ridges and valleys for mountain terrain

### Layer 3: Mountain Mask

- Type: Perlin FBM
- Frequency: ~0.003
- Octaves: 2
- Output range: [-1, 1]. Remap to [0, 1]: `mask = (raw_mask + 1.0f) * 0.5f`
- Purpose: Controls where mountains appear (0 = flat plains, 1 = full mountains)

### Seed Assignments

Each noise field uses a distinct seed to prevent correlation:

- Continentalness: `seed`
- Ridged: `seed + 1`
- Mask: `seed + 2`
- Spaghetti A: `seed + 100`
- Spaghetti B: `seed + 200`
- Cheese: `seed + 300`
- Trees: `seed + 12345` (existing)

### Height Formula

After clamping/remapping:

```
base_height = 64 + continentalness * 16           // range ~48–80
mountain_contribution = ridged * mask * 80         // range 0–80
final_height = clamp(base_height + mountain_contribution, 1, CHUNK_Y - 1)
```

The height computation is factored into a shared static helper `compute_height()` called by both `worldgen_generate()` and `worldgen_get_height()` to prevent formula divergence.

Sea level remains at 62. Layer fill structure unchanged (bedrock → stone → dirt → grass/sand).

## Cave Generation

Two overlapping 3D noise systems carved as a second pass after terrain fill:

### Spaghetti Caves

- Two independent 3D Perlin noise fields (different seeds)
- Frequency: ~0.03, 3 octaves each
- Carving condition: `|noise_a| < 0.04 && |noise_b| < 0.04`
- Produces long, winding tunnels at the intersection of two noise zero-surfaces

### Cheese Caves

- Single 3D Perlin noise
- Frequency: ~0.015, 2 octaves
- Carving condition: `noise > 0.6`
- Produces larger open caverns

### Carving Order

Generation order within a chunk: (1) compute heightmap, (2) fill terrain layers, (3) carve caves, (4) place trees. Trees check the post-carving surface so they don't root over cave voids.

### Carving Rules

A block is carved (set to `BLOCK_AIR`) if either spaghetti OR cheese conditions are met, subject to:

- **Never carve bedrock blocks**: check block type (`== BLOCK_BEDROCK`), not y-level. This covers both the y=0 pure bedrock and the y=1..9 mixed bedrock/stone layer.
- **Hard-skip surface**: if `depth <= 1` (surface block and 1 below), skip entirely — no noise evaluation.
- **Surface proximity bias**: for `depth` in [2, 7], scale carving to make it harder near the surface:
  - Spaghetti: multiply thresholds (0.04) by `depth / 8.0` (smaller threshold = harder to carve)
  - Cheese: raise threshold: `noise > 0.6 + (1.0 - depth / 8.0) * 0.4` (higher threshold = harder to carve)
  - "Surface" means the terrain solid height (`h` from the heightmap), not the water surface.
- **Skip non-carving blocks**: only evaluate noise for stone and dirt blocks (skip air, water, bedrock)

**Known limitation**: Cave carving may leave floating dirt blocks 2-3 blocks above a carved void. Acceptable for now; a gravity pass for sand/dirt could be added later.

## Bug Fixes

### Trees on Water

**Root cause**: When surface height `h < SEA_LEVEL - 2`, `is_beach` is false, so `BLOCK_GRASS` is placed at y=h even though it's underwater. Tree placement checks for grass but not water above.

**Fix**: In tree placement loop, add `if (h < SEA_LEVEL) continue;` before spawning trees. (Strictly less-than: h=SEA_LEVEL is at the water line with no water above, but such blocks are sand due to beach logic and won't have grass anyway.)

### Camera Inside Terrain

**Root cause**: Camera Y is hardcoded to 80 in `main.c`. With taller mountains, this can be below the surface.

**Fix**: Add `worldgen_get_height(int x, int z, int seed)` to `worldgen.h/.c` that evaluates the terrain noise at a world position and returns the surface height. `main.c` calls this to set initial camera Y to `surface_height + 2`.

## Files Changed

| File | Changes |
|------|---------|
| `src/worldgen.c` | Rewrite terrain noise (3-layer system), add cave carving pass, add `worldgen_get_height()`, fix tree-on-water |
| `src/worldgen.h` | Add `worldgen_get_height()` declaration |
| `src/main.c` | Use `worldgen_get_height()` for spawn position |

## Files Unchanged

- Block types (`block.h/c`) — no new blocks needed
- Chunk structure (`chunk.h/c`) — unchanged
- Mesher, world management, renderer — all untouched

## Performance Considerations

- Terrain heightmap: 3 noise evaluations per column (was 1). Negligible cost for 256 columns per chunk.
- Cave carving: 3 noise evaluations per underground solid block. Mitigated by skipping air/bedrock/water blocks. Most of the 256-high column is air above surface.
- All generation runs on existing worker threads — no main thread impact.
- FastNoiseLite is well-optimized for these operations.
