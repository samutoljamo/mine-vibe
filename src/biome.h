#ifndef BIOME_H
#define BIOME_H

#include "block.h"

/* Deterministic, pure biome classification for worldgen.
 *
 * A low-frequency noise field partitions the world into biomes. Every helper
 * here is a pure function of its arguments — identical (wx, wz, seed) always
 * yields the same biome — so chunk boundaries line up seamlessly regardless of
 * generation order or threading.
 *
 * Biomes vary three things:
 *   - the surface skin block (grass / sand / stone),
 *   - tree density (chance in [0,1] that a column grows a tree),
 *   - a height bias added to the base terrain height.
 *
 * No new block types are introduced: deserts use SAND, mountains expose STONE,
 * snowy peaks reuse the lightest surface available (SAND stand-in handled by
 * caller height/tree logic). Plains and forest keep GRASS.
 */

typedef enum {
    BIOME_PLAINS = 0,
    BIOME_FOREST,
    BIOME_DESERT,
    BIOME_MOUNTAINS,
    BIOME_SNOW,        /* cold tundra / snowfields (appended — keep last) */
    BIOME_COUNT,
} Biome;

/* Classify the biome at a world column. Pure function of (wx, wz, seed). */
Biome biome_at(int wx, int wz, int seed);

/* Pure climate classifier underlying biome_at. Maps a normalized climate
 * sample to a biome. All three inputs are in [-1, 1]:
 *   temp      — temperature (low = cold, high = hot)
 *   humidity  — moisture    (low = dry,  high = wet)
 *   elevation — terrain height field (high = mountainous)
 *
 * Decision order (first match wins):
 *   1. high elevation                -> MOUNTAINS  (climate-independent)
 *   2. cold                          -> SNOW
 *   3. hot & dry                     -> DESERT
 *   4. temperate/warm & humid        -> FOREST
 *   5. otherwise                     -> PLAINS
 *
 * Deterministic, no global state. Exposed for unit testing the thresholds. */
Biome biome_classify(float temp, float humidity, float elevation);

/* Surface skin block for a biome, given the column's surface height so high
 * mountain peaks can switch to a snow stand-in. Returns GRASS / SAND / STONE. */
BlockID biome_surface_block(Biome b, int surface_h);

/* Tree-spawn probability for a biome, in [0, 1]. Deserts/mountains are bare;
 * forest is dense; plains is sparse. */
float biome_tree_density(Biome b);

/* Per-biome additive height bias (blocks) layered onto the base terrain. */
int biome_height_bias(Biome b);

/* Snow line: at or above this surface height, mountain skin becomes the snow
 * stand-in. Exposed for tests. */
#define BIOME_SNOW_LINE 120

#endif
