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

void worldgen_generate(Chunk* chunk, int seed)
{
    TerrainNoise tn;
    terrain_noise_init(&tn, seed);

    /* Tree placement noise (unchanged) */
    fnl_state tree_noise = fnlCreateState();
    tree_noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    tree_noise.frequency = 0.5f;
    tree_noise.seed = seed + 12345;

    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    /* Compute height map using 3-layer noise */
    int height_map[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            float wx = (float)(base_x + x);
            float wz = (float)(base_z + z);
            height_map[x][z] = compute_height(&tn, wx, wz);
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

    /* Place trees: ~2% chance on grass blocks, constrained to [2..13] local X/Z */
    for (int x = 2; x <= 13; x++) {
        for (int z = 2; z <= 13; z++) {
            int h = height_map[x][z];
            if (chunk_get_block(chunk, x, h, z) != BLOCK_GRASS) continue;

            float tn = fnlGetNoise2D(&tree_noise,
                                     (float)(base_x + x),
                                     (float)(base_z + z));
            int r = hash_pos(base_x + x, base_z + z, seed + 9999);
            float threshold = (tn + 1.0f) * 0.5f; /* 0..1 */
            if ((r % 100) >= 2) continue;          /* ~2% base chance */
            (void)threshold; /* noise used for variety but base chance dominates */

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
