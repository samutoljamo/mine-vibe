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

- Type: `FNL_FRACTAL_RIDGED` (native FastNoiseLite support)
- Frequency: ~0.005
- Octaves: 4
- Purpose: Sharp ridges and valleys for mountain terrain

### Layer 3: Mountain Mask

- Type: Perlin
- Frequency: ~0.003
- Octaves: 2
- Purpose: Controls where mountains appear (0 = flat plains, 1 = full mountains)

### Height Formula

```
base_height = 64 + continentalness * 16           // range ~48–80
mountain_contribution = ridged * mask * 80         // range 0–80
final_height = base_height + mountain_contribution // range ~48–160
```

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

### Carving Rules

A block is carved (set to `BLOCK_AIR`) if either spaghetti OR cheese conditions are met, subject to:

- **Never carve bedrock** (y < 5)
- **Never carve surface**: skip the surface block and 1 block below it
- **Surface proximity bias**: for depth below surface < 8, linearly reduce carving probability by multiplying thresholds by `depth / 8.0`. This keeps caves mostly buried but allows occasional openings on steep mountain faces.
- **Skip non-solid blocks**: only evaluate noise for stone/dirt blocks (air, water, bedrock are skipped)

## Bug Fixes

### Trees on Water

**Root cause**: When surface height `h < SEA_LEVEL - 2`, `is_beach` is false, so `BLOCK_GRASS` is placed at y=h even though it's underwater. Tree placement checks for grass but not water above.

**Fix**: In tree placement loop, add `if (h <= SEA_LEVEL) continue;` before spawning trees.

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
