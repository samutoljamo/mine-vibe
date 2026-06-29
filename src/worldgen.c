#define FNL_IMPL
#include "FastNoiseLite.h"

#include "worldgen.h"
#include "chunk.h"
#include "ore.h"
#include "village.h"
#include "biome.h"
#include "cave.h"
#include "dungeon.h"
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

/* Biome climate sampling (for border blending).
 *
 * biome.c exposes the pure blend helpers (biome_blend_*), but they take the
 * *normalized* climate triple (temp, humidity, elevation) — the same values the
 * hard classifier samples internally inside biome_at. biome.h does not expose a
 * climate sampler, so to feed the blend helpers we reproduce biome.c's three
 * climate noise fields here EXACTLY (identical seed offsets, frequencies,
 * octaves and fractal type). Because the noise is a pure function of
 * (wx, wz, seed) with those parameters, the triple computed here is bit-for-bit
 * the same one biome_at/biome_classify see — so deep inside a biome the blended
 * params equal the old per-biome values and only borders change.
 *
 * These constants MUST stay in sync with biome.c (BIOME_SEED_* and the field
 * setups in biome_noise_init). They are duplicated rather than shared because
 * this ticket is scoped to worldgen.c only and biome.h's signatures are final. */
#define BIOME_SEED_ELEVATION   4001
#define BIOME_SEED_TEMPERATURE 4002
#define BIOME_SEED_HUMIDITY    4003

typedef struct {
    fnl_state elevation;
    fnl_state temperature;
    fnl_state humidity;
} ClimateNoise;

static void climate_noise_init(ClimateNoise* cn, int seed)
{
    cn->elevation = fnlCreateState();
    cn->elevation.noise_type = FNL_NOISE_PERLIN;
    cn->elevation.fractal_type = FNL_FRACTAL_FBM;
    cn->elevation.octaves = 2;
    cn->elevation.frequency = 0.0012f;
    cn->elevation.seed = seed + BIOME_SEED_ELEVATION;

    cn->temperature = fnlCreateState();
    cn->temperature.noise_type = FNL_NOISE_PERLIN;
    cn->temperature.fractal_type = FNL_FRACTAL_FBM;
    cn->temperature.octaves = 2;
    cn->temperature.frequency = 0.0010f;
    cn->temperature.seed = seed + BIOME_SEED_TEMPERATURE;

    cn->humidity = fnlCreateState();
    cn->humidity.noise_type = FNL_NOISE_PERLIN;
    cn->humidity.fractal_type = FNL_FRACTAL_FBM;
    cn->humidity.octaves = 2;
    cn->humidity.frequency = 0.0009f;
    cn->humidity.seed = seed + BIOME_SEED_HUMIDITY;
}

/* Sample the normalized climate triple at a world column (matches biome.c). */
static void climate_sample(const ClimateNoise* cn, float wx, float wz,
                           float* temp, float* humidity, float* elevation)
{
    *elevation = fnlGetNoise2D((fnl_state*)&cn->elevation,   wx, wz);
    *temp      = fnlGetNoise2D((fnl_state*)&cn->temperature, wx, wz);
    *humidity  = fnlGetNoise2D((fnl_state*)&cn->humidity,    wx, wz);
}

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

/* Emit the slice of any underground dungeon room overlapping this chunk.
 *
 * Placement is the pure model in src/dungeon.c (a function of cell + seed only),
 * so a room straddling a chunk boundary materializes bit-identically from either
 * side: each chunk simply writes the voxels of the room that fall within its own
 * x/z extent. We consult this chunk's placement cell plus its 8 neighbours,
 * because a jittered room can sit anywhere inside a cell and a cell is larger
 * than a chunk — a neighbouring cell's room can therefore reach into this chunk.
 *
 * Voxels written:
 *   - shell (walls/floor/ceiling): BLOCK_MOSSY_COBBLESTONE
 *   - interior: BLOCK_AIR (hollow room)
 *   - one BLOCK_CHEST on the interior floor against a corner wall
 *
 * The shell never overwrites bedrock, keeping the y=0 floor and the bedrock mix
 * band intact (the placement band sits at y>=DUNGEON_MIN_Y, well above it). The
 * chest is placed as a bare block; populating it with loot requires a
 * server-side block-entity at generation time and is a follow-up. */
static void generate_dungeons(Chunk* chunk, int seed)
{
    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;
    int chunk_x0 = base_x, chunk_x1 = base_x + CHUNK_X - 1;
    int chunk_z0 = base_z, chunk_z1 = base_z + CHUNK_Z - 1;

    int ccx = dungeon_cell_index(base_x);
    int ccz = dungeon_cell_index(base_z);

    for (int dcx = -1; dcx <= 1; dcx++)
        for (int dcz = -1; dcz <= 1; dcz++) {
            DungeonRoom r = dungeon_cell_at(ccx + dcx, ccz + dcz, (uint32_t)seed);
            if (!r.present) continue;

            int rx1 = r.x0 + r.w - 1;
            int rz1 = r.z0 + r.d - 1;
            /* AABB reject: room footprint vs this chunk. */
            if (rx1 < chunk_x0 || r.x0 > chunk_x1 ||
                rz1 < chunk_z0 || r.z0 > chunk_z1)
                continue;

            /* Clamp the room's world-x/z range to this chunk and write only the
             * overlapping voxels. */
            int x_lo = r.x0 > chunk_x0 ? r.x0 : chunk_x0;
            int x_hi = rx1  < chunk_x1 ? rx1  : chunk_x1;
            int z_lo = r.z0 > chunk_z0 ? r.z0 : chunk_z0;
            int z_hi = rz1  < chunk_z1 ? rz1  : chunk_z1;

            for (int wx = x_lo; wx <= x_hi; wx++)
                for (int wz = z_lo; wz <= z_hi; wz++)
                    for (int wy = r.y0; wy < r.y0 + r.h; wy++) {
                        if (wy <= 0 || wy >= CHUNK_Y) continue;
                        int lx = wx - base_x, lz = wz - base_z;
                        int role = dungeon_voxel_role(&r, wx, wy, wz);
                        if (role == 1) {
                            /* Shell — never replace bedrock. */
                            if (chunk_get_block(chunk, lx, wy, lz) != BLOCK_BEDROCK)
                                chunk_set_block(chunk, lx, wy, lz,
                                                BLOCK_MOSSY_COBBLESTONE);
                        } else if (role == 2) {
                            chunk_set_block(chunk, lx, wy, lz, BLOCK_AIR);
                        }
                    }

            /* Chest: only if it lands in this chunk. */
            if (r.chest_x >= chunk_x0 && r.chest_x <= chunk_x1 &&
                r.chest_z >= chunk_z0 && r.chest_z <= chunk_z1 &&
                r.chest_y > 0 && r.chest_y < CHUNK_Y) {
                chunk_set_block(chunk, r.chest_x - base_x, r.chest_y,
                                r.chest_z - base_z, BLOCK_CHEST);
            }
        }
}

void worldgen_generate(Chunk* chunk, int seed)
{
    /* Cache noise states per worker thread — seed is constant after startup.
     * Cave noise is owned by cave.c (cached internally), so we only cache the
     * terrain noise here. */
    static _Thread_local TerrainNoise terrain;
    static _Thread_local ClimateNoise climate;
    static _Thread_local int cached_seed = -1;

    if (cached_seed != seed) {
        terrain_noise_init(&terrain, seed);
        climate_noise_init(&climate, seed);
        cached_seed = seed;
    }

    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    /* Compute height map using 3-layer noise, then nudge each column by its
     * biome height bias. The bias is the *border-blended* weighted average
     * (biome_blend_height_bias) of the climate triple at this column rather than
     * the single hard biome's bias, so terrain elevation transitions smoothly
     * across biome borders (no cliffs) while remaining the exact per-biome value
     * deep inside a biome (blend weight ~= 1 there). The discrete surface biome
     * (for the surface/sub-surface skin) is the *dominant* blended biome, which
     * also equals the hard classification deep inside a biome. Everything is a
     * pure function of (wx, wz, seed) so it stays stable across chunk seams. */
    int height_map[CHUNK_X][CHUNK_Z];
    Biome biome_map[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            float wx = (float)(base_x + x);
            float wz = (float)(base_z + z);
            float temp, humid, elev;
            climate_sample(&climate, wx, wz, &temp, &humid, &elev);

            /* Discrete surface choice: dominant blended biome. */
            biome_map[x][z] = biome_blend_dominant(temp, humid, elev);

            /* Continuous height bias: weight-blended across biomes. */
            float bias = biome_blend_height_bias(temp, humid, elev);
            int h = compute_height(&terrain, wx, wz) + (int)lroundf(bias);
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

    /* Carve underground dungeon rooms (rare, mossy-cobble shell + chest). Done
     * after caves/ores so the room shell reclaims any cave-air or ore that the
     * earlier passes left inside its footprint, giving a clean enclosed room. */
    generate_dungeons(chunk, seed);

    /* Place trees: per-biome density on grass blocks, constrained to [2..13]
     * local X/Z. Desert/mountain biomes report density 0 (and lack grass), so
     * they stay bare; forest is dense, plains sparse. */
    for (int x = 2; x <= 13; x++) {
        for (int z = 2; z <= 13; z++) {
            int h = height_map[x][z];
            if (h < SEA_LEVEL) continue;
            if (chunk_get_block(chunk, x, h, z) != BLOCK_GRASS) continue;

            /* Decoration density is the border-blended tree density (weighted
             * average across biomes), so tree cover fades smoothly across biome
             * borders instead of snapping. Deep inside a biome the blend weight
             * is ~1, so this equals the old per-biome density (no regression). */
            float temp, humid, elev;
            climate_sample(&climate, (float)(base_x + x), (float)(base_z + z),
                           &temp, &humid, &elev);
            int tree_chance = (int)(biome_blend_tree_density(temp, humid, elev)
                                    * 100.0f);
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
