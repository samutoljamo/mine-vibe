#define FNL_IMPL
#include "FastNoiseLite.h"

#include "worldgen.h"
#include "chunk.h"
#include "ore.h"
#include "village.h"
#include "biome.h"
#include "cave.h"
#include <stdlib.h>
#include <math.h>

#define SEA_LEVEL 62

static int hash_pos(int x, int z, int seed)
{
    /* Simple spatial hash for tree placement decisions. All multiplies are
     * done in uint32_t: signed-int multiplication by these large constants
     * overflows for ordinary world coordinates, which is undefined behavior.
     * Unsigned wraparound is well-defined and gives a stable hash. */
    uint32_t h = (uint32_t)x * 374761393u
               + (uint32_t)z * 668265263u
               + (uint32_t)seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
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

/* Cave carving is delegated to the pure, world-coordinate cave model in
 * src/cave.c (cave_is_carved). Because that predicate depends only on absolute
 * world (x,y,z) + seed, carving is automatically continuous across chunk seams
 * and deterministic regardless of generation order.
 *
 * Two things layer on top of the raw predicate here:
 *   - Bedrock protection: never carve bedrock (cave.c already refuses y at/below
 *     CAVE_MIN_Y, but we also guard against the bedrock/stone mix band).
 *   - Surface entrances: a cave column that reaches up near the surface is
 *     allowed to break through the soil/surface skin so the cavern opens to the
 *     sky and is discoverable. Without this, caves stop a few blocks under the
 *     surface and stay sealed. To keep the world from turning into swiss cheese
 *     up top, we only punch through when the block immediately below is itself
 *     carved cave air (i.e. an actual cave is pushing up to meet the surface). */

/* True if the cave model would carve this voxel AND it is a carvable material
 * (never bedrock). Surface-proximity tapering is handled by the caller. */
static bool cave_carves_solid(BlockID block, int wx, int y, int wz, int seed)
{
    if (block == BLOCK_BEDROCK) return false;
    if (block != BLOCK_STONE && block != BLOCK_DIRT) return false;
    return cave_is_carved(wx, y, wz, (uint32_t)seed);
}

static void carve_caves(Chunk* chunk, int seed,
                        int height_map[CHUNK_X][CHUNK_Z])
{
    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int surface_h = height_map[x][z];
            int wx = base_x + x;
            int wz = base_z + z;

            /* Carve the body of the column with the pure cave model. We stop one
             * block below the surface skin here; the surface skin / soil is
             * handled by the entrance pass below so we control how holey the top
             * becomes. */
            for (int y = 1; y < surface_h - 1; y++) {
                BlockID block = chunk_get_block(chunk, x, y, z);
                if (cave_carves_solid(block, wx, y, wz, seed))
                    chunk_set_block(chunk, x, y, z, BLOCK_AIR);
            }

            /* Surface entrances: if the topmost soil layer (surface skin and the
             * block just beneath it) sits directly on cave air, open it to the
             * sky so the cave is reachable. Only punch through when a real cave
             * has risen to meet the surface — we never carve into a column that
             * is solid all the way down, so the surface stays mostly intact. */
            for (int y = surface_h; y >= surface_h - 1 && y >= 1; y--) {
                BlockID block = chunk_get_block(chunk, x, y, z);
                /* Only open soil/sand/grass/snow/sandstone surface skins — leave
                 * stone peaks and water alone. */
                if (block != BLOCK_GRASS && block != BLOCK_DIRT &&
                    block != BLOCK_SAND && block != BLOCK_SNOW &&
                    block != BLOCK_SANDSTONE)
                    continue;
                BlockID below = (y >= 1) ? chunk_get_block(chunk, x, y - 1, z)
                                         : BLOCK_BEDROCK;
                if (below == BLOCK_AIR)
                    chunk_set_block(chunk, x, y, z, BLOCK_AIR);
            }
        }
    }
}

void worldgen_generate(Chunk* chunk, int seed)
{
    /* Cache noise states per worker thread — seed is constant after startup.
     * Cave noise is owned by cave.c (cached internally), so we only cache the
     * terrain noise here. */
    static _Thread_local TerrainNoise terrain;
    static _Thread_local int cached_seed = -1;

    if (cached_seed != seed) {
        terrain_noise_init(&terrain, seed);
        cached_seed = seed;
    }

    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    /* Compute height map using 3-layer noise, then nudge each column by its
     * biome's height bias (mountains rise, deserts dip). Biome is a pure
     * function of (wx, wz, seed) so the result is stable across chunk seams. */
    int height_map[CHUNK_X][CHUNK_Z];
    Biome biome_map[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            float wx = (float)(base_x + x);
            float wz = (float)(base_z + z);
            Biome b = biome_at(base_x + x, base_z + z, seed);
            biome_map[x][z] = b;
            int h = compute_height(&terrain, wx, wz) + biome_height_bias(b);
            if (h < 1) h = 1;
            if (h >= CHUNK_Y) h = CHUNK_Y - 1;
            height_map[x][z] = h;
        }
    }

    /* Fill terrain layers */
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int h = height_map[x][z];
            bool is_beach = (h <= SEA_LEVEL + 1 && h >= SEA_LEVEL - 2);
            /* Biome surface skin (grass / sand / stone / snow) and the matching
             * sub-surface band (dirt / sandstone / stone). Beaches keep their
             * sand shoreline regardless of biome. */
            BlockID surf = biome_surface_block(biome_map[x][z], h);
            BlockID subsurf = biome_subsurface_block(biome_map[x][z], h);

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
                    block = is_beach ? BLOCK_SAND : subsurf;
                } else if (y == h) {
                    block = is_beach ? BLOCK_SAND : surf;
                } else if (y <= SEA_LEVEL && y > h) {
                    block = BLOCK_WATER;
                }

                chunk_set_block(chunk, x, y, z, block);

                /* Water blocks placed by worldgen are permanent sources */
                if (block == BLOCK_WATER) {
                    chunk_set_meta(chunk, x, y, z, 255); /* source level */
                }
            }
        }
    }

    /* Carve caves (pure world-coord model + surface entrances) */
    carve_caves(chunk, seed, height_map);

    /* Sprinkle ores into the remaining (uncarved) stone. Done after carving so
     * ores only replace solid stone — never left floating in cave air. Ores
     * exposed in cave walls are intentional and harvestable. */
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int surface_h = height_map[x][z];
            for (int y = 1; y < surface_h; y++) {
                if (chunk_get_block(chunk, x, y, z) != BLOCK_STONE) continue;
                BlockID ore = ore_select(base_x + x, y, base_z + z, surface_h, seed);
                if (ore != BLOCK_STONE)
                    chunk_set_block(chunk, x, y, z, ore);
            }
        }
    }

    /* Place trees: per-biome density on grass blocks, constrained to [2..13]
     * local X/Z. Desert/mountain biomes report density 0 (and lack grass), so
     * they stay bare; forest is dense, plains sparse. */
    for (int x = 2; x <= 13; x++) {
        for (int z = 2; z <= 13; z++) {
            int h = height_map[x][z];
            if (h < SEA_LEVEL) continue;
            if (chunk_get_block(chunk, x, h, z) != BLOCK_GRASS) continue;

            int tree_chance = (int)(biome_tree_density(biome_map[x][z]) * 100.0f);
            if (tree_chance <= 0) continue;
            int r = hash_pos(base_x + x, base_z + z, seed + 9999);
            if ((r % 100) >= tree_chance) continue;

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

    /* Per-biome surface decoration. Pure of (world coord, seed) so it lines up
     * across chunk seams; constrained to [2..13] local X/Z so a single feature
     * never needs neighbouring-chunk writes.
     *
     *   - DESERT: sparse cactus. No cactus block exists, so a cactus is a short
     *     1x1 column of LEAVES (a green stand-in) on the sand surface. Skipped
     *     as a true cactus until a dedicated block is added.
     *   - SNOW:   the snow blanket is already the surface skin (see fill loop);
     *     additionally drop the odd snow "drift" one block higher for relief. */
    for (int x = 2; x <= 13; x++) {
        for (int z = 2; z <= 13; z++) {
            int h = height_map[x][z];
            if (h < SEA_LEVEL) continue;
            if (h + 4 >= CHUNK_Y) continue;
            Biome b = biome_map[x][z];

            if (b == BIOME_DESERT) {
                /* Only on undisturbed sand surface (not carved by a cave). */
                if (chunk_get_block(chunk, x, h, z) != BLOCK_SAND) continue;
                int r = hash_pos(base_x + x, base_z + z, seed + 5151);
                if ((r % 100) >= 3) continue; /* ~3% of desert columns */
                int cactus_h = 2 + (hash_pos(base_x + x, base_z + z, seed + 88) % 2);
                for (int cy = 1; cy <= cactus_h; cy++) {
                    if (chunk_get_block(chunk, x, h + cy, z) != BLOCK_AIR) break;
                    chunk_set_block(chunk, x, h + cy, z, BLOCK_LEAVES);
                }
            } else if (b == BIOME_SNOW) {
                /* Occasional raised snow drift on the snowfield surface. */
                if (chunk_get_block(chunk, x, h, z) != BLOCK_SNOW) continue;
                int r = hash_pos(base_x + x, base_z + z, seed + 6262);
                if ((r % 100) >= 8) continue; /* ~8% of snow columns */
                if (chunk_get_block(chunk, x, h + 1, z) == BLOCK_AIR)
                    chunk_set_block(chunk, x, h + 1, z, BLOCK_SNOW);
            }
        }
    }

    /* Procedural villages: emit the slice of any village intersecting this
     * chunk (pure of seed, so cross-chunk seams line up). See src/village.c. */
    village_generate(chunk, seed, height_map);

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
