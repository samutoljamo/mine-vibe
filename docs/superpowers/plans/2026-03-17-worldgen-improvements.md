# World Generation Improvements Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace flat terrain with ridged-noise mountains and add spaghetti+cheese cave systems, plus fix tree-on-water and camera spawn bugs.

**Architecture:** Rewrite `worldgen_generate()` with a 3-layer noise heightmap (continentalness + ridged + mask), add a cave carving pass using 3 noise fields, and extract a shared `compute_height()` helper exposed via `worldgen_get_height()`. No changes to chunk structure, mesher, or renderer.

**Tech Stack:** C, FastNoiseLite (already in project), CMake build

**Spec:** `docs/superpowers/specs/2026-03-17-worldgen-improvements-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/worldgen.c` | Modify | Terrain noise, cave carving, tree fix, `compute_height()` helper |
| `src/worldgen.h` | Modify | Add `worldgen_get_height()` declaration |
| `src/main.c` | Modify | Use `worldgen_get_height()` for spawn Y |

No new files. No changes to block.h/c, chunk.h/c, mesher, world, or renderer.

---

## Task 1: Replace terrain noise with 3-layer system

**Files:**
- Modify: `src/worldgen.c:20-48` (noise setup + heightmap loop)

This task replaces the single Perlin FBM noise with the 3-layer continentalness + ridged + mask system, and extracts a shared `compute_height()` helper.

- [ ] **Step 1: Add `compute_height()` static helper and new noise init**

Replace the terrain noise setup and heightmap computation in `worldgen_generate()`. Add a struct to hold the three noise states and a static helper that computes height for any world coordinate.

In `src/worldgen.c`, replace lines 20-48 (from the start of `worldgen_generate` through the heightmap loop) with:

```c
/* Noise state bundle for terrain height computation */
typedef struct {
    fnl_state continental;
    fnl_state ridged;
    fnl_state mask;
} TerrainNoise;

static void terrain_noise_init(TerrainNoise* tn, int seed)
{
    /* Layer 1: Base continentalness — smooth, large-scale height variation */
    tn->continental = fnlCreateState();
    tn->continental.noise_type = FNL_NOISE_PERLIN;
    tn->continental.fractal_type = FNL_FRACTAL_FBM;
    tn->continental.octaves = 2;
    tn->continental.frequency = 0.002f;
    tn->continental.seed = seed;

    /* Layer 2: Mountain ridged noise — sharp ridges where noise crosses zero */
    tn->ridged = fnlCreateState();
    tn->ridged.noise_type = FNL_NOISE_PERLIN;
    tn->ridged.fractal_type = FNL_FRACTAL_RIDGED;
    tn->ridged.octaves = 4;
    tn->ridged.frequency = 0.005f;
    tn->ridged.seed = seed + 1;

    /* Layer 3: Mountain mask — controls where mountains appear */
    tn->mask = fnlCreateState();
    tn->mask.noise_type = FNL_NOISE_PERLIN;
    tn->mask.fractal_type = FNL_FRACTAL_FBM;
    tn->mask.octaves = 2;
    tn->mask.frequency = 0.003f;
    tn->mask.seed = seed + 2;
}

static int compute_height(TerrainNoise* tn, float wx, float wz)
{
    float c = fnlGetNoise2D(&tn->continental, wx, wz);
    float r = fmaxf(fnlGetNoise2D(&tn->ridged, wx, wz), 0.0f);
    float m = (fnlGetNoise2D(&tn->mask, wx, wz) + 1.0f) * 0.5f;

    float base = 64.0f + c * 16.0f;
    float mountain = r * m * 80.0f;
    int h = (int)(base + mountain);

    if (h < 1) h = 1;
    if (h >= CHUNK_Y) h = CHUNK_Y - 1;
    return h;
}
```

Then update `worldgen_generate()` to use these:

```c
void worldgen_generate(Chunk* chunk, int seed)
{
    TerrainNoise tn;
    terrain_noise_init(&tn, seed);

    /* Tree placement noise (unchanged) */
    fnl_state tree_noise = fnlCreateState();
    tree_noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    tree_noise.frequency = 0.5f;
    tree_noise.seed = seed + 12345;

    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    /* Compute height map using 3-layer noise */
    int height_map[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            float wx = (float)(base_x + x);
            float wz = (float)(base_z + z);
            height_map[x][z] = compute_height(&tn, wx, wz);
        }
    }

    /* Fill terrain layers (unchanged from here) */
```

- [ ] **Step 2: Build and verify**

Run:
```bash
cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20
```
Expected: compiles without errors.

- [ ] **Step 3: Commit**

```bash
git add src/worldgen.c
git commit -m "feat: replace terrain noise with 3-layer mountain system"
```

---

## Task 2: Add cave carving pass

**Files:**
- Modify: `src/worldgen.c` (add cave carving function, call it after terrain fill)

- [ ] **Step 1: Add cave noise init struct and carving function**

Add after the `compute_height()` function, before `worldgen_generate()`:

```c
/* Noise state bundle for cave generation */
typedef struct {
    fnl_state spaghetti_a;
    fnl_state spaghetti_b;
    fnl_state cheese;
} CaveNoise;

static void cave_noise_init(CaveNoise* cn, int seed)
{
    /* Spaghetti cave A */
    cn->spaghetti_a = fnlCreateState();
    cn->spaghetti_a.noise_type = FNL_NOISE_PERLIN;
    cn->spaghetti_a.fractal_type = FNL_FRACTAL_FBM;
    cn->spaghetti_a.octaves = 3;
    cn->spaghetti_a.frequency = 0.03f;
    cn->spaghetti_a.seed = seed + 100;

    /* Spaghetti cave B */
    cn->spaghetti_b = fnlCreateState();
    cn->spaghetti_b.noise_type = FNL_NOISE_PERLIN;
    cn->spaghetti_b.fractal_type = FNL_FRACTAL_FBM;
    cn->spaghetti_b.octaves = 3;
    cn->spaghetti_b.frequency = 0.03f;
    cn->spaghetti_b.seed = seed + 200;

    /* Cheese caves */
    cn->cheese = fnlCreateState();
    cn->cheese.noise_type = FNL_NOISE_PERLIN;
    cn->cheese.fractal_type = FNL_FRACTAL_FBM;
    cn->cheese.octaves = 2;
    cn->cheese.frequency = 0.015f;
    cn->cheese.seed = seed + 300;
}

static void carve_caves(Chunk* chunk, CaveNoise* cn,
                        int height_map[CHUNK_X][CHUNK_Z])
{
    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int surface_h = height_map[x][z];
            float wx = (float)(base_x + x);
            float wz = (float)(base_z + z);

            for (int y = 0; y < surface_h; y++) {
                BlockID block = chunk_get_block(chunk, x, y, z);

                /* Only carve stone and dirt */
                if (block != BLOCK_STONE && block != BLOCK_DIRT) continue;

                int depth = surface_h - y;

                /* Hard-skip surface and 1 below */
                if (depth <= 1) continue;

                float wy = (float)y;
                bool carve = false;

                if (depth < 8) {
                    /* Surface proximity — reduced carving */
                    float scale = (float)depth / 8.0f;

                    float sa = fnlGetNoise3D(&cn->spaghetti_a, wx, wy, wz);
                    float sb = fnlGetNoise3D(&cn->spaghetti_b, wx, wy, wz);
                    float spaghetti_thresh = 0.04f * scale;
                    if (fabsf(sa) < spaghetti_thresh && fabsf(sb) < spaghetti_thresh)
                        carve = true;

                    if (!carve) {
                        float ch = fnlGetNoise3D(&cn->cheese, wx, wy, wz);
                        float cheese_thresh = 0.6f + (1.0f - scale) * 0.4f;
                        if (ch > cheese_thresh)
                            carve = true;
                    }
                } else {
                    /* Deep underground — full carving */
                    float sa = fnlGetNoise3D(&cn->spaghetti_a, wx, wy, wz);
                    float sb = fnlGetNoise3D(&cn->spaghetti_b, wx, wy, wz);
                    if (fabsf(sa) < 0.04f && fabsf(sb) < 0.04f)
                        carve = true;

                    if (!carve) {
                        float ch = fnlGetNoise3D(&cn->cheese, wx, wy, wz);
                        if (ch > 0.6f)
                            carve = true;
                    }
                }

                if (carve) {
                    chunk_set_block(chunk, x, y, z, BLOCK_AIR);
                }
            }
        }
    }
}
```

- [ ] **Step 2: Call cave carving in `worldgen_generate()`**

In `worldgen_generate()`, after the terrain fill loop (the `for (int x = 0; ...) { for (int z = 0; ...) { for (int y = 0; ...) }}}` block) and before the tree placement loop, add:

```c
    /* Carve caves */
    CaveNoise cn;
    cave_noise_init(&cn, seed);
    carve_caves(chunk, &cn, height_map);
```

- [ ] **Step 3: Build and verify**

Run:
```bash
cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20
```
Expected: compiles without errors.

- [ ] **Step 4: Commit**

```bash
git add src/worldgen.c
git commit -m "feat: add spaghetti + cheese cave generation"
```

---

## Task 3: Fix tree-on-water bug

**Files:**
- Modify: `src/worldgen.c:80-84` (tree placement loop)

- [ ] **Step 1: Add sea level check to tree placement**

In the tree placement loop, after `int h = height_map[x][z];` and before the `chunk_get_block` grass check, add:

```c
            if (h < SEA_LEVEL) continue;
```

So the tree loop beginning becomes:

```c
    for (int x = 2; x <= 13; x++) {
        for (int z = 2; z <= 13; z++) {
            int h = height_map[x][z];
            if (h < SEA_LEVEL) continue;
            if (chunk_get_block(chunk, x, h, z) != BLOCK_GRASS) continue;
```

- [ ] **Step 2: Build and verify**

Run:
```bash
cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20
```
Expected: compiles without errors.

- [ ] **Step 3: Commit**

```bash
git add src/worldgen.c
git commit -m "fix: prevent trees from spawning on underwater grass"
```

---

## Task 4: Add `worldgen_get_height()` and fix camera spawn

**Files:**
- Modify: `src/worldgen.h:6` (add declaration)
- Modify: `src/worldgen.c` (add public function)
- Modify: `src/main.c:47-48` (use for spawn position)

- [ ] **Step 1: Add `worldgen_get_height()` to header**

In `src/worldgen.h`, add after the `worldgen_generate` declaration:

```c
int worldgen_get_height(int x, int z, int seed);
```

- [ ] **Step 2: Add `worldgen_get_height()` implementation**

At the end of `src/worldgen.c` (before the closing of the file, after `worldgen_generate`), add:

```c
int worldgen_get_height(int x, int z, int seed)
{
    TerrainNoise tn;
    terrain_noise_init(&tn, seed);
    return compute_height(&tn, (float)x, (float)z);
}
```

- [ ] **Step 3: Update camera spawn in `main.c`**

In `src/main.c`, add `#include "worldgen.h"` to the includes, then replace:

```c
    camera_init(&g_camera, (vec3){0, 80, 0});
    World* world = world_create(&renderer, 42, 32);
```

with:

```c
    int spawn_seed = 42;
    int spawn_y = worldgen_get_height(0, 0, spawn_seed) + 2;
    camera_init(&g_camera, (vec3){0, (float)spawn_y, 0});
    World* world = world_create(&renderer, spawn_seed, 32);
```

This also extracts the seed literal to avoid duplication.

- [ ] **Step 4: Build and verify**

Run:
```bash
cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20
```
Expected: compiles without errors.

- [ ] **Step 5: Commit**

```bash
git add src/worldgen.c src/worldgen.h src/main.c
git commit -m "fix: spawn camera above terrain using worldgen_get_height()"
```

---

## Task 5: Visual verification and run

- [ ] **Step 1: Build full project**

Run:
```bash
cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20
```
Expected: clean build, no warnings.

- [ ] **Step 2: Run the game**

Run:
```bash
cd /var/home/samu/minecraft/build && ./minecraft
```

Verify visually:
- Mountains are visible with sharp ridges and flat plains elsewhere
- Camera spawns above the terrain surface
- No trees visible on water
- Caves visible when flying into mountainsides (occasional surface openings on steep faces)
- No holes to void at world bottom (bedrock intact)

- [ ] **Step 3: Final commit if any tweaks needed**

Adjust noise parameters if terrain looks wrong (too flat, too spiky, caves too common/rare). Commit any parameter tweaks.
