#include "cave.h"

#include <math.h>
#include "FastNoiseLite.h"

/* Pure 3D-noise cave carving. Mirrors the noise setup in worldgen.c's
 * CaveNoise (FastNoiseLite Perlin/FBM, same frequencies/octaves and per-field
 * seed offsets) so this model and the existing inline carving stay consistent
 * and wa8.1.2 can wire it in without changing behaviour.
 *
 * Note: this file does NOT define FNL_IMPL. The implementation is provided by
 * worldgen.c in the main build (avoids duplicate symbols); the standalone test
 * target defines FNL_IMPL itself, exactly as test_biome does for biome.c. */

/* Lazily build the FastNoiseLite states once per (seed). They are pure
 * functions of the seed, so caching is just memoization -- it never makes the
 * result depend on call order. */
typedef struct {
    fnl_state spaghetti_a;
    fnl_state spaghetti_b;
    fnl_state cheese;
} CaveNoise;

static CaveNoise make_cave_noise(uint32_t seed)
{
    CaveNoise cn;

    cn.spaghetti_a = fnlCreateState();
    cn.spaghetti_a.noise_type = FNL_NOISE_PERLIN;
    cn.spaghetti_a.fractal_type = FNL_FRACTAL_FBM;
    cn.spaghetti_a.octaves = CAVE_SPAGHETTI_OCTAVES;
    cn.spaghetti_a.frequency = CAVE_SPAGHETTI_FREQUENCY;
    cn.spaghetti_a.seed = (int)seed + CAVE_SALT_SPAGHETTI_A;

    cn.spaghetti_b = fnlCreateState();
    cn.spaghetti_b.noise_type = FNL_NOISE_PERLIN;
    cn.spaghetti_b.fractal_type = FNL_FRACTAL_FBM;
    cn.spaghetti_b.octaves = CAVE_SPAGHETTI_OCTAVES;
    cn.spaghetti_b.frequency = CAVE_SPAGHETTI_FREQUENCY;
    cn.spaghetti_b.seed = (int)seed + CAVE_SALT_SPAGHETTI_B;

    cn.cheese = fnlCreateState();
    cn.cheese.noise_type = FNL_NOISE_PERLIN;
    cn.cheese.fractal_type = FNL_FRACTAL_FBM;
    cn.cheese.octaves = CAVE_CHEESE_OCTAVES;
    cn.cheese.frequency = CAVE_CHEESE_FREQUENCY;
    cn.cheese.seed = (int)seed + CAVE_SALT_CHEESE;

    return cn;
}

bool cave_is_carved(int x, int y, int z, uint32_t seed)
{
    /* Vertical bounds: keep the bedrock floor and bound the carved band. */
    if (y <= CAVE_MIN_Y || y >= CAVE_MAX_Y)
        return false;

    /* Cache the noise states per thread keyed on seed. Recomputed only when the
     * seed changes (effectively never after startup). Pure: identical inputs ->
     * identical outputs, independent of call order or threading. */
    static _Thread_local CaveNoise cn;
    static _Thread_local uint32_t cached_seed = 0;
    static _Thread_local bool have_cache = false;
    if (!have_cache || cached_seed != seed) {
        cn = make_cave_noise(seed);
        cached_seed = seed;
        have_cache = true;
    }

    float wx = (float)x;
    float wy = (float)y;
    float wz = (float)z;

    /* Spaghetti tunnels: intersection of two abs-thresholded iso-surfaces. */
    float sa = fnlGetNoise3D(&cn.spaghetti_a, wx, wy, wz);
    float sb = fnlGetNoise3D(&cn.spaghetti_b, wx, wy, wz);
    if (fabsf(sa) < CAVE_SPAGHETTI_THRESHOLD &&
        fabsf(sb) < CAVE_SPAGHETTI_THRESHOLD)
        return true;

    /* Cheese caverns: occasional larger pockets. */
    float ch = fnlGetNoise3D(&cn.cheese, wx, wy, wz);
    if (ch > CAVE_CHEESE_THRESHOLD)
        return true;

    return false;
}
