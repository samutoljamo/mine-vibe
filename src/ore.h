#ifndef ORE_H
#define ORE_H

#include "block.h"

/* Deterministic, pure ore selection for worldgen.
 *
 * Given a world-space stone position and the surface height of that column,
 * decides whether the stone should be replaced by an ore block. Returns one
 * of the *_ORE block ids, or BLOCK_STONE when no ore is placed.
 *
 * Properties (relied on by worldgen and verified by tests):
 *   - Pure & deterministic: identical args always yield the same result.
 *   - Ores never appear within ORE_MIN_DEPTH blocks of the surface, so they
 *     never poke through dirt/grass.
 *   - Depth gating by absolute Y: diamond only at/below ORE_DIAMOND_MAX_Y,
 *     gold only at/below ORE_GOLD_MAX_Y, iron only at/below ORE_IRON_MAX_Y.
 *   - Rarity ordering coal > iron > gold > diamond.
 */

#define ORE_MIN_DEPTH     3   /* blocks below surface before any ore appears  */
#define ORE_IRON_MAX_Y    64
#define ORE_GOLD_MAX_Y    30
#define ORE_DIAMOND_MAX_Y 14

BlockID ore_select(int wx, int wy, int wz, int surface_h, int seed);

#endif
