#ifndef VILLAGE_H
#define VILLAGE_H

#include <stdbool.h>
#include "block.h"

/* Procedural villages for worldgen.
 *
 * All placement decisions are PURE functions of the world seed and cell/world
 * coordinates, so every chunk that a village touches independently agrees on
 * the same villages, centers, layout and platform height — no cross-chunk
 * writes or neighbour loads are needed. village_generate() emits only the
 * slice of each structure that falls inside the given chunk; out-of-range
 * writes are harmlessly dropped by chunk_set_block's bounds check.
 *
 * The decision core (cell placement, suitability, house layout) is extracted
 * as pure functions and unit-tested in tests/test_village.c, mirroring ore.c.
 */

/* A village cell is a coarse grid of chunks; at most one village is anchored
 * per cell, jittered toward the cell interior so adjacent cells never overlap.
 *
 * Findability tuning (see tests/test_village.c density check and the original
 * 0.3%-of-cells materialization rate): the cell grid was shrunk and the
 * suitability predicate relaxed so a player reliably finds a village within a
 * few hundred blocks. The dominant filter used to be flatness — sampling at
 * radius 40 with a 4-block slope tolerance rejected ~97% of candidate cells on
 * this terrain. We now sample a tighter footprint (VILLAGE_SAMPLE_RADIUS) with
 * a looser tolerance, and accept any non-water surface above sea level (the
 * generator flattens the footprint itself, so gentle slopes are fine). */
#define VILLAGE_CELL_CHUNKS  8                               /* 8x8 chunks     */
#define VILLAGE_CELL_BLOCKS  (VILLAGE_CELL_CHUNKS * 16)      /* 128x128 blocks */
#define VILLAGE_SPAWN_PCT    35      /* ~35% of cells host a candidate village */
#define VILLAGE_MAX_RADIUS   40      /* blocks; structures stay within this    */
#define VILLAGE_MARGIN       36      /* keep center this far from cell edges    */
#define VILLAGE_SAMPLE_RADIUS 24     /* footprint radius for the flatness check */
#define VILLAGE_MAX_SLOPE    12      /* max surface height spread for flatness  */
#define VILLAGE_SEA_LEVEL    62      /* matches worldgen SEA_LEVEL              */

/* Distinct salts so each decision rolls independently of the others. */
#define VILLAGE_SALT_PRESENT 0x5117
#define VILLAGE_SALT_JITTER  0x91A3
#define VILLAGE_SALT_SEED    0x2C6D

#define VILLAGE_MIN_HOUSES   3
#define VILLAGE_MAX_HOUSES   6
#define VILLAGE_HOUSE_MIN    5       /* min house footprint side (blocks)       */
#define VILLAGE_HOUSE_MAX    8       /* max house footprint side (blocks)       */
#define VILLAGE_WALL_H       3       /* wall height in blocks                    */

typedef struct {
    bool present;   /* does this cell host a candidate village?               */
    int  wx, wz;    /* world-space village center (block coords)              */
    int  seed;      /* per-village seed = mix(center, world seed)             */
} VillageCell;

typedef struct {
    int  cx, cz;        /* world-space house center (block coords)            */
    int  w, d;          /* footprint width (x) and depth (z) in blocks       */
    bool peaked_roof;   /* true = peaked planks roof, false = flat cobble     */
    int  door_face;     /* 0=+X 1=-X 2=+Z 3=-Z — wall holding the door gap   */
} VillageHouse;

/* Deterministic: which village (if any) is anchored to cell (cgx,cgz). */
VillageCell village_cell_at(int cgx, int cgz, int world_seed);

/* Suitability predicate evaluated at the village center. The caller supplies
 * the center column's surface height + block and the min/max sampled surface
 * heights around the center (for the flatness check). Pure. */
bool village_is_suitable(int center_surface_h, BlockID center_surface_block,
                         int sampled_min_h, int sampled_max_h);

/* Number of houses for a village with the given per-village seed. Pure,
 * in [VILLAGE_MIN_HOUSES, VILLAGE_MAX_HOUSES]. */
int village_house_count(int vseed);

/* The i-th house of a village centered at (center_wx, center_wz) with the
 * given per-village seed. Houses are scattered within VILLAGE_MAX_RADIUS of
 * the center and do not overlap each other. Pure & deterministic. */
VillageHouse village_house_at(int vseed, int center_wx, int center_wz, int i);

/* Deterministic locator: find the nearest village center to world position
 * (wx,wz). Scans the surrounding cell neighbourhood and applies the same
 * presence + suitability test the generator uses (via worldgen_get_height), so
 * the returned center is one that actually materializes. Returns true and
 * writes the center to (*out_x,*out_z) when one is found within the scan
 * radius, false otherwise. Pure of run state; identical for a given seed. */
bool village_nearest(int wx, int wz, int seed, int* out_x, int* out_z);

/* Emit a one-line log of the nearest village to (wx,wz) for the given seed to
 * stderr, so players know which way to walk. Safe to call once at world init.
 * No-op effect on world state. */
void village_log_nearest(int wx, int wz, int seed);

/* Forward declaration to avoid pulling chunk.h (and Vulkan headers) into the
 * pure unit test. */
struct Chunk;

/* Per-chunk entry point. Emits the slice of every village intersecting this
 * chunk via chunk_set_block (world coords; out-of-range writes dropped).
 * height_map is the worldgen-computed [16][16] surface heights for this chunk
 * (unused by placement decisions, which are pure of world seed, but accepted
 * to match the call-site contract and avoid recomputation). */
void village_generate(struct Chunk* chunk, int seed, int height_map[16][16]);

#endif
