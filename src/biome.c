#include "biome.h"

#include "FastNoiseLite.h"

/* Three independent low-frequency noise fields select the biome:
 *
 *   "elevation"   — when high, the column is mountainous regardless of climate.
 *   "temperature" — cold -> snow, hot -> desert (if dry), middle -> temperate.
 *   "humidity"    — moisture; separates dry desert from humid forest and keeps
 *                   moderate-humidity temperate land as plains.
 *
 * Frequencies are deliberately much lower than the terrain-height noise in
 * worldgen.c so biomes form large coherent regions rather than per-block
 * speckle. Distinct seed offsets keep these fields independent of the terrain
 * and cave noise (which use seed, seed+1, seed+2, seed+100..300).
 */
#define BIOME_SEED_ELEVATION   4001
#define BIOME_SEED_TEMPERATURE 4002
#define BIOME_SEED_HUMIDITY    4003

/* Climate thresholds for biome_classify (all in normalized noise space). */
#define BIOME_ELEV_MOUNTAIN  0.35f  /* elevation above this => mountains      */
#define BIOME_TEMP_COLD     (-0.40f) /* temperature below this => snow/tundra  */
#define BIOME_TEMP_HOT       0.30f  /* hot enough to form desert (if dry)     */
#define BIOME_HUMID_DRY      0.20f  /* humidity below this is "dry" (desert)  */
#define BIOME_HUMID_WET      0.15f  /* humidity above this is "wet" (forest)  */

typedef struct {
    fnl_state elevation;
    fnl_state temperature;
    fnl_state humidity;
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

    bn->humidity = fnlCreateState();
    bn->humidity.noise_type = FNL_NOISE_PERLIN;
    bn->humidity.fractal_type = FNL_FRACTAL_FBM;
    bn->humidity.octaves = 2;
    bn->humidity.frequency = 0.0009f;
    bn->humidity.seed = seed + BIOME_SEED_HUMIDITY;

    bn->seed = seed;
    bn->valid = 1;
}

Biome biome_classify(float temp, float humidity, float elevation)
{
    /* 1. High elevation is mountainous regardless of climate. */
    if (elevation > BIOME_ELEV_MOUNTAIN)
        return BIOME_MOUNTAINS;

    /* 2. Cold lowlands become snow / tundra. */
    if (temp < BIOME_TEMP_COLD)
        return BIOME_SNOW;

    /* 3. Hot AND dry forms desert. */
    if (temp > BIOME_TEMP_HOT && humidity < BIOME_HUMID_DRY)
        return BIOME_DESERT;

    /* 4. Sufficiently humid temperate/warm land grows forest. */
    if (humidity > BIOME_HUMID_WET)
        return BIOME_FOREST;

    /* 5. Everything else is open plains. */
    return BIOME_PLAINS;
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

    float elev  = fnlGetNoise2D(&bn.elevation,   fx, fz); /* [-1, 1] */
    float temp  = fnlGetNoise2D(&bn.temperature, fx, fz); /* [-1, 1] */
    float humid = fnlGetNoise2D(&bn.humidity,    fx, fz); /* [-1, 1] */
    return biome_classify(temp, humid, elev);
}

BlockID biome_surface_block(Biome b, int surface_h)
{
    switch (b) {
        case BIOME_DESERT:
            return BLOCK_SAND;
        case BIOME_MOUNTAINS:
            /* Bare rock below the snow line; snow caps high peaks. */
            return (surface_h >= BIOME_SNOW_LINE) ? BLOCK_SNOW : BLOCK_STONE;
        case BIOME_SNOW:
            /* Tundra snowfield: snow blanket over the ground. */
            return BLOCK_SNOW;
        case BIOME_PLAINS:
        case BIOME_FOREST:
        default:
            return BLOCK_GRASS;
    }
}

BlockID biome_subsurface_block(Biome b, int surface_h)
{
    switch (b) {
        case BIOME_DESERT:
            /* Sandstone under the sand skin instead of dirt/stone. */
            return BLOCK_SANDSTONE;
        case BIOME_SNOW:
            /* Snow blanket sits on ordinary dirt. */
            return BLOCK_DIRT;
        case BIOME_MOUNTAINS:
            /* Stone immediately under the snow cap or bare rock. */
            (void)surface_h;
            return BLOCK_STONE;
        case BIOME_PLAINS:
        case BIOME_FOREST:
        default:
            return BLOCK_DIRT;
    }
}

float biome_tree_density(Biome b)
{
    switch (b) {
        case BIOME_FOREST:    return 0.10f;  /* dense woods            */
        case BIOME_PLAINS:    return 0.02f;  /* sparse, matches legacy */
        case BIOME_DESERT:    return 0.0f;   /* no trees on sand       */
        case BIOME_MOUNTAINS: return 0.0f;   /* bare rock              */
        case BIOME_SNOW:      return 0.01f;  /* sparse snowy stragglers */
        default:              return 0.0f;
    }
}

int biome_height_bias(Biome b)
{
    switch (b) {
        case BIOME_MOUNTAINS: return 24;  /* push peaks higher */
        case BIOME_DESERT:    return -2;  /* gentle low dunes  */
        case BIOME_SNOW:      return 1;   /* slightly raised tundra */
        case BIOME_PLAINS:
        case BIOME_FOREST:
        default:              return 0;
    }
}
