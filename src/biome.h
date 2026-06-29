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
 * Deserts use SAND over SANDSTONE, mountains expose STONE (snow-capped above
 * the snow line), snowfields and high peaks use SNOW. Plains and forest keep
 * GRASS over DIRT.
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
 * mountain peaks can switch to snow. Returns GRASS / SAND / STONE / SNOW. */
BlockID biome_surface_block(Biome b, int surface_h);

/* Sub-surface block (the few layers directly under the surface skin) for a
 * biome: dirt under grass/snow, sandstone under desert sand, stone under
 * mountains. Returns DIRT / SANDSTONE / STONE. */
BlockID biome_subsurface_block(Biome b, int surface_h);

/* Tree-spawn probability for a biome, in [0, 1]. Deserts/mountains are bare;
 * forest is dense; plains is sparse. */
float biome_tree_density(Biome b);

/* Per-biome additive height bias (blocks) layered onto the base terrain. */
int biome_height_bias(Biome b);

/* Snow line: at or above this surface height, mountain skin becomes snow.
 * Exposed for tests. */
#define BIOME_SNOW_LINE 120

/* ---------------------------------------------------------------------------
 * Biome border blending (wa8.3.4)
 *
 * biome_classify is a hard step function of (temp, humidity, elevation), so a
 * column's continuous terrain params (height bias, tree density) snap at biome
 * borders. The helpers below compute SMOOTH transitions by treating each biome
 * as having a soft "membership" weight near borders rather than a single
 * winner-takes-all classification.
 *
 * Weights are derived by sampling the existing classifier on a small fixed
 * grid of offsets in (temp, humidity, elevation) space around the query point
 * and counting how many samples land in each biome — points deep inside a
 * biome see only that biome (weight 1), points near a border see a mix that
 * varies smoothly as the point crosses. Everything is a pure, deterministic
 * function of its arguments (no global/thread state, no noise sampling).
 *
 * Continuous params (height bias, tree density) are returned as the weighted
 * average over biomes. Discrete choices (surface block) cannot be averaged —
 * use biome_blend_dominant() to pick the highest-weight biome for those.
 * ------------------------------------------------------------------------- */

/* Radius (in normalized climate units) of the blend sampling kernel. Borders
 * smooth over roughly +/- this distance in (temp, humidity, elevation).
 * Exposed for tests. */
#define BIOME_BLEND_RADIUS 0.06f

/* Fill out_w[BIOME_COUNT] with soft membership weights for the given climate
 * point. Weights are non-negative and sum to 1. Deep inside a biome the
 * dominant weight is ~1; near a border weight is shared between neighbours.
 * Pure/deterministic. */
void biome_blend_weights(float temp, float humidity, float elevation,
                         float out_w[BIOME_COUNT]);

/* Dominant (highest-weight) biome at a climate point. Ties break toward the
 * lower enum value. Matches biome_classify deep inside a biome; near a border
 * it tracks whichever side holds the majority of the kernel. */
Biome biome_blend_dominant(float temp, float humidity, float elevation);

/* Weight-blended additive height bias (blocks) at a climate point. Equals the
 * raw biome_height_bias deep inside a biome and transitions smoothly across
 * borders. */
float biome_blend_height_bias(float temp, float humidity, float elevation);

/* Weight-blended tree-spawn probability ([0,1]) at a climate point. Equals the
 * raw biome_tree_density deep inside a biome and transitions smoothly across
 * borders. */
float biome_blend_tree_density(float temp, float humidity, float elevation);

#endif
