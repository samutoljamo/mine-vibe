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
 * per cell, jittered toward the cell interior so adjacent cells never overlap. */
#define VILLAGE_CELL_CHUNKS  16                              /* 16x16 chunks   */
#define VILLAGE_CELL_BLOCKS  (VILLAGE_CELL_CHUNKS * 16)      /* 256x256 blocks */
#define VILLAGE_SPAWN_PCT    25      /* ~25% of cells host a candidate village */
#define VILLAGE_MAX_RADIUS   40      /* blocks; structures stay within this    */
#define VILLAGE_MARGIN       48      /* keep center this far from cell edges    */
#define VILLAGE_MAX_SLOPE    4       /* max surface height spread for flatness  */
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
