#define FNL_IMPL
#include "FastNoiseLite.h"

#include "worldgen.h"
#include "chunk.h"
#include <stdlib.h>
#include <math.h>

#define SEA_LEVEL 62

static int hash_pos(int x, int z, int seed)
{
    /* Simple spatial hash for tree placement decisions */
    uint32_t h = (uint32_t)(x * 374761393 + z * 668265263 + seed * 1274126177);
    h = (h ^ (h >> 13)) * 1274126177;
    h = h ^ (h >> 16);
    return (int)(h & 0x7FFFFFFF);
}

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

                /* Never carve bedrock */
                if (block == BLOCK_BEDROCK) continue;
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

void worldgen_generate(Chunk* chunk, int seed)
{
    /* Cache noise states per worker thread — seed is constant after startup */
    static _Thread_local TerrainNoise terrain;
    static _Thread_local CaveNoise cn;
    static _Thread_local int cached_seed = -1;

    if (cached_seed != seed) {
        terrain_noise_init(&terrain, seed);
        cave_noise_init(&cn, seed);
        cached_seed = seed;
    }

    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    /* Compute height map using 3-layer noise */
    int height_map[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            float wx = (float)(base_x + x);
            float wz = (float)(base_z + z);
            height_map[x][z] = compute_height(&terrain, wx, wz);
        }
    }

    /* Fill terrain layers */
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int h = height_map[x][z];
            bool is_beach = (h <= SEA_LEVEL + 1 && h >= SEA_LEVEL - 2);

            for (int y = 0; y < CHUNK_Y; y++) {
                BlockID block = BLOCK_AIR;

                if (y == 0) {
                    block = BLOCK_BEDROCK;
                } else if (y < 10) {
                    /* Mix bedrock and stone */
                    int r = hash_pos(base_x + x, y * 7919 + base_z + z, seed);
                    block = (r % 3 == 0) ? BLOCK_BEDROCK : BLOCK_STONE;
                } else if (y < h - 3) {
                    block = BLOCK_STONE;
                } else if (y < h) {
                    block = is_beach ? BLOCK_SAND : BLOCK_DIRT;
                } else if (y == h) {
                    block = is_beach ? BLOCK_SAND : BLOCK_GRASS;
                } else if (y <= SEA_LEVEL && y > h) {
                    block = BLOCK_WATER;
                }

                chunk_set_block(chunk, x, y, z, block);
            }
        }
    }

    /* Carve caves */
    carve_caves(chunk, &cn, height_map);

    /* Place trees: ~2% chance on grass blocks, constrained to [2..13] local X/Z */
    for (int x = 2; x <= 13; x++) {
        for (int z = 2; z <= 13; z++) {
            int h = height_map[x][z];
            if (h < SEA_LEVEL) continue;
            if (chunk_get_block(chunk, x, h, z) != BLOCK_GRASS) continue;

            int r = hash_pos(base_x + x, base_z + z, seed + 9999);
            if ((r % 100) >= 2) continue; /* ~2% chance */

            /* Tree trunk height 4-6 */
            int trunk_h = 4 + (hash_pos(base_x + x, base_z + z, seed + 77) % 3);
            int top_y = h + trunk_h;
            if (top_y + 2 >= CHUNK_Y) continue;

            /* Place trunk */
            for (int ty = h + 1; ty <= top_y; ty++) {
                chunk_set_block(chunk, x, ty, z, BLOCK_WOOD);
            }

            /* Place leaf sphere */
            for (int ly = top_y - 1; ly <= top_y + 2; ly++) {
                int radius = (ly <= top_y) ? 2 : 1;
                for (int lx = -radius; lx <= radius; lx++) {
                    for (int lz = -radius; lz <= radius; lz++) {
                        if (lx == 0 && lz == 0 && ly <= top_y) continue; /* trunk */
                        int bx = x + lx;
                        int bz = z + lz;
                        if (bx < 0 || bx >= CHUNK_X || bz < 0 || bz >= CHUNK_Z) continue;
                        if (ly < 0 || ly >= CHUNK_Y) continue;
                        if (chunk_get_block(chunk, bx, ly, bz) == BLOCK_AIR) {
                            chunk_set_block(chunk, bx, ly, bz, BLOCK_LEAVES);
                        }
                    }
                }
            }
        }
    }

    atomic_store(&chunk->state, CHUNK_GENERATED);
}

int worldgen_get_height(int x, int z, int seed)
{
    static _Thread_local TerrainNoise terrain;
    static _Thread_local int cached_seed = -1;

    if (cached_seed != seed) {
        terrain_noise_init(&terrain, seed);
        cached_seed = seed;
    }
    return compute_height(&terrain, (float)x, (float)z);
}
