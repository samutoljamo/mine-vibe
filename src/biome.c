#include "biome.h"

#include "FastNoiseLite.h"

/* Two independent low-frequency noise fields select the biome:
 *
 *   "elevation"   — when high, the column is mountainous regardless of climate.
 *   "temperature" — splits the remaining lowlands into desert (hot), plains
 *                   (temperate) and forest (cool/wet).
 *
 * Frequencies are deliberately much lower than the terrain-height noise in
 * worldgen.c so biomes form large coherent regions rather than per-block
 * speckle. Distinct seed offsets keep these fields independent of the terrain
 * and cave noise (which use seed, seed+1, seed+2, seed+100..300).
 */
#define BIOME_SEED_ELEVATION   4001
#define BIOME_SEED_TEMPERATURE 4002

typedef struct {
    fnl_state elevation;
    fnl_state temperature;
    int       seed;
    int       valid;
} BiomeNoise;

static void biome_noise_init(BiomeNoise* bn, int seed)
{
    bn->elevation = fnlCreateState();
    bn->elevation.noise_type = FNL_NOISE_PERLIN;
    bn->elevation.fractal_type = FNL_FRACTAL_FBM;
    bn->elevation.octaves = 2;
    bn->elevation.frequency = 0.0012f;
    bn->elevation.seed = seed + BIOME_SEED_ELEVATION;

    bn->temperature = fnlCreateState();
    bn->temperature.noise_type = FNL_NOISE_PERLIN;
    bn->temperature.fractal_type = FNL_FRACTAL_FBM;
    bn->temperature.octaves = 2;
    bn->temperature.frequency = 0.0010f;
    bn->temperature.seed = seed + BIOME_SEED_TEMPERATURE;

    bn->seed = seed;
    bn->valid = 1;
}

Biome biome_at(int wx, int wz, int seed)
{
    /* Cache noise states per worker thread keyed on seed (constant after
     * startup). Pure: the cache only memoizes fnl_state construction; the
     * returned biome depends solely on (wx, wz, seed). */
    static _Thread_local BiomeNoise bn;
    if (!bn.valid || bn.seed != seed)
        biome_noise_init(&bn, seed);

    float fx = (float)wx;
    float fz = (float)wz;

    float elev = fnlGetNoise2D(&bn.elevation, fx, fz);   /* [-1, 1] */
    if (elev > 0.35f)
        return BIOME_MOUNTAINS;

    float temp = fnlGetNoise2D(&bn.temperature, fx, fz); /* [-1, 1] */
    if (temp > 0.30f)
        return BIOME_DESERT;
    if (temp < -0.15f)
        return BIOME_FOREST;
    return BIOME_PLAINS;
}

BlockID biome_surface_block(Biome b, int surface_h)
{
    switch (b) {
        case BIOME_DESERT:
            return BLOCK_SAND;
        case BIOME_MOUNTAINS:
            /* Bare rock; snowy stand-in (SAND, the lightest existing block)
             * caps high peaks since no dedicated snow block exists. */
            return (surface_h >= BIOME_SNOW_LINE) ? BLOCK_SAND : BLOCK_STONE;
        case BIOME_PLAINS:
        case BIOME_FOREST:
        default:
            return BLOCK_GRASS;
    }
}

float biome_tree_density(Biome b)
{
    switch (b) {
        case BIOME_FOREST:    return 0.10f;  /* dense woods            */
        case BIOME_PLAINS:    return 0.02f;  /* sparse, matches legacy */
        case BIOME_DESERT:    return 0.0f;   /* no trees on sand       */
        case BIOME_MOUNTAINS: return 0.0f;   /* bare rock              */
        default:              return 0.0f;
    }
}

int biome_height_bias(Biome b)
{
    switch (b) {
        case BIOME_MOUNTAINS: return 24;  /* push peaks higher */
        case BIOME_DESERT:    return -2;  /* gentle low dunes  */
        case BIOME_PLAINS:
        case BIOME_FOREST:
        default:              return 0;
    }
}
