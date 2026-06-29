#ifndef CAVE_H
#define CAVE_H

#include <stdbool.h>
#include <stdint.h>

/* Deterministic, pure 3D-noise cave carving for worldgen.
 *
 * Given a world-space voxel position and the world seed, decides whether that
 * voxel should be carved to air by the cave system. This is the pure model
 * only; placing entrances and wiring it into worldgen is a separate concern.
 *
 * Noise approach (mirrors src/worldgen.c's existing cave noise so the two stay
 * consistent and integration is trivial): FastNoiseLite Perlin/FBM 3D fields.
 *
 *   - Two "spaghetti" Perlin-FBM fields. A voxel is on a tunnel when BOTH
 *     |n_a| < CAVE_SPAGHETTI_THRESHOLD AND |n_b| < CAVE_SPAGHETTI_THRESHOLD.
 *     Two abs-thresholded fields ANDed together carve the intersection of two
 *     iso-surfaces, i.e. winding connected tubes (Perlin-worm-ish) rather than
 *     isolated blobs.
 *   - One "cheese" Perlin-FBM field for occasional larger caverns when
 *     n_cheese > CAVE_CHEESE_THRESHOLD.
 *
 * Properties (relied on by integration and verified by tests):
 *   - Pure & deterministic: identical (x, y, z, seed) always yield the same
 *     result. No globals, no rand/time. This is what makes caves continuous
 *     across chunk boundaries: the predicate depends ONLY on absolute world
 *     coordinates + seed, never on chunk-local state.
 *   - Seed-sensitive: different seeds produce different carving.
 *   - Vertically bounded: nothing is carved at/below CAVE_MIN_Y (protects the
 *     bedrock floor) or at/above CAVE_MAX_Y.
 *
 * Tunable threshold constants are exposed here so the integration ticket can
 * adjust cave density / extent without touching cave.c.
 */

/* Frequencies/octaves of the noise fields. Matched to worldgen.c's CaveNoise. */
#define CAVE_SPAGHETTI_FREQUENCY 0.03f
#define CAVE_SPAGHETTI_OCTAVES   3
#define CAVE_CHEESE_FREQUENCY    0.015f
#define CAVE_CHEESE_OCTAVES      2

/* Carve thresholds. Wider spaghetti band => thicker/denser tunnels; higher
 * cheese threshold => fewer/smaller caverns. */
#define CAVE_SPAGHETTI_THRESHOLD 0.04f
#define CAVE_CHEESE_THRESHOLD    0.60f

/* Salts added to the seed so each field rolls independently. Matched to the
 * offsets worldgen.c uses (+100/+200/+300) so carving is bit-identical when
 * this model is wired in. */
#define CAVE_SALT_SPAGHETTI_A 100
#define CAVE_SALT_SPAGHETTI_B 200
#define CAVE_SALT_CHEESE      300

/* Vertical bounds (inclusive interior). Y outside (CAVE_MIN_Y, CAVE_MAX_Y) is
 * never carved, keeping the bedrock floor intact and bounding the work. */
#define CAVE_MIN_Y 2
#define CAVE_MAX_Y 128

/* True iff the voxel at world (x, y, z) should be carved to air. Pure. */
bool cave_is_carved(int x, int y, int z, uint32_t seed);

#endif
