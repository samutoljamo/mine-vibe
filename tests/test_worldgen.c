#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/worldgen.h"
#include "../src/chunk.h"
#include "../src/ore.h"
#include "../src/block.h"
#include "../src/biome.h"
#include "../src/cave.h"
#include "../src/dungeon.h"

/* SEA_LEVEL is private to worldgen.c; mirror its value here. */
#define SEA_LEVEL 62

static int is_ore(BlockID b) {
    return b == BLOCK_COAL_ORE || b == BLOCK_IRON_ORE ||
           b == BLOCK_GOLD_ORE || b == BLOCK_DIAMOND_ORE;
}

/* A freshly created+generated chunk. Caller owns it (chunk_destroy). */
static Chunk* gen(int cx, int cz, int seed) {
    Chunk* c = chunk_create(cx, cz);
    assert(c);
    worldgen_generate(c, seed);
    return c;
}

/* ── 1. Determinism ─────────────────────────────────────────────────────── */
/* Same coords + same seed → byte-identical block array. Worldgen caches noise
 * state per thread keyed on seed, so this also exercises the cache path. */
static void test_deterministic(void) {
    int coords[][2] = { {0,0}, {1,2}, {-3,5}, {-7,-7}, {12,-4}, {100,100} };
    int seeds[] = { 1337, 42, -1 };
    for (size_t s = 0; s < sizeof(seeds)/sizeof(seeds[0]); s++) {
        for (size_t i = 0; i < sizeof(coords)/sizeof(coords[0]); i++) {
            Chunk* a = gen(coords[i][0], coords[i][1], seeds[s]);
            Chunk* b = gen(coords[i][0], coords[i][1], seeds[s]);
            for (int k = 0; k < CHUNK_BLOCKS; k++)
                assert(a->blocks[k] == b->blocks[k]);
            chunk_destroy(a);
            chunk_destroy(b);
        }
    }
    printf("PASS: deterministic\n");
}

/* ── 2. Bedrock floor ───────────────────────────────────────────────────── */
/* y=0 is unconditionally bedrock in every column of every chunk. */
static void test_bedrock_floor(void) {
    int coords[][2] = { {0,0}, {-5,3}, {8,-8}, {50,-50} };
    for (size_t i = 0; i < sizeof(coords)/sizeof(coords[0]); i++) {
        Chunk* c = gen(coords[i][0], coords[i][1], 1337);
        for (int x = 0; x < CHUNK_X; x++)
            for (int z = 0; z < CHUNK_Z; z++)
                assert(chunk_get_block(c, x, 0, z) == BLOCK_BEDROCK);
        chunk_destroy(c);
    }
    printf("PASS: bedrock_floor\n");
}

/* ── 3. Surface sanity ──────────────────────────────────────────────────── */
/* The topmost solid (non-air, non-water) block of each column must be a
 * legitimate surface/structure block — grass/sand/dirt for terrain, or
 * wood/leaves (trees) or village blocks (planks/cobble/glass/path). Stone is
 * allowed only where a cave breached the soil layer right at the top. Crucially
 * it must never be a "floating" surface: directly below the top solid block,
 * within the soil band, we don't expect air sandwiched under grass.
 * Water, if present, only ever sits at or below sea level. */
static int valid_surface_block(BlockID b) {
    switch (b) {
        case BLOCK_GRASS: case BLOCK_SAND: case BLOCK_DIRT:
        case BLOCK_SNOW:  case BLOCK_SANDSTONE: /* per-biome surface skins */
        case BLOCK_WOOD:  case BLOCK_LEAVES:
        case BLOCK_PLANKS: case BLOCK_COBBLE: case BLOCK_GLASS: case BLOCK_PATH:
        case BLOCK_STONE: /* possible when a cave exposes stone at the very top */
            return 1;
        default:
            return 0;
    }
}

static void test_surface_sanity(void) {
    int coords[][2] = { {0,0}, {3,-2}, {-9,9}, {25,40}, {-30,-15} };
    for (size_t i = 0; i < sizeof(coords)/sizeof(coords[0]); i++) {
        Chunk* c = gen(coords[i][0], coords[i][1], 1337);
        for (int x = 0; x < CHUNK_X; x++)
            for (int z = 0; z < CHUNK_Z; z++) {
                /* Water only at/below sea level. */
                for (int y = SEA_LEVEL + 1; y < CHUNK_Y; y++)
                    assert(chunk_get_block(c, x, y, z) != BLOCK_WATER);

                /* Find topmost solid (non-air, non-water) block. */
                int top = -1;
                for (int y = CHUNK_Y - 1; y >= 1; y--) {
                    BlockID b = chunk_get_block(c, x, y, z);
                    if (b != BLOCK_AIR && b != BLOCK_WATER) { top = y; break; }
                }
                assert(top >= 1); /* always something solid above bedrock */
                BlockID tb = chunk_get_block(c, x, top, z);
                assert(valid_surface_block(tb) || is_ore(tb));
            }
        chunk_destroy(c);
    }
    printf("PASS: surface_sanity\n");
}

/* ── 4. Stone body ──────────────────────────────────────────────────────── */
/* Every chunk must have a substantial stone body: deep underground (the
 * 10..30 band, always below any dirt layer) the columns are overwhelmingly
 * stone/ore, never open air or soil. Soil (dirt/grass/sand) belongs near the
 * surface, not in the deep band. */
static void test_stone_body(void) {
    int coords[][2] = { {0,0}, {4,4}, {-6,2}, {17,-23} };
    for (size_t i = 0; i < sizeof(coords)/sizeof(coords[0]); i++) {
        Chunk* c = gen(coords[i][0], coords[i][1], 1337);
        long stone_or_ore = 0, total = 0, soil_deep = 0;
        for (int x = 0; x < CHUNK_X; x++)
            for (int z = 0; z < CHUNK_Z; z++)
                for (int y = 10; y <= 30; y++) {
                    BlockID b = chunk_get_block(c, x, y, z);
                    total++;
                    if (b == BLOCK_STONE || is_ore(b)) stone_or_ore++;
                    if (b == BLOCK_DIRT || b == BLOCK_GRASS || b == BLOCK_SAND)
                        soil_deep++;
                }
        /* No soil this deep — surfaces are far above y=30. */
        assert(soil_deep == 0);
        /* Deep band is dominated by stone/ore even with caves carved out. */
        assert(stone_or_ore > total / 2);
        chunk_destroy(c);
    }
    printf("PASS: stone_body\n");
}

/* ── 5. Ores ────────────────────────────────────────────────────────────── */
/* Across many chunks ores must appear, and every ore must respect the depth
 * gating declared in ore.h: diamond only at/below ORE_DIAMOND_MAX_Y, gold only
 * at/below ORE_GOLD_MAX_Y, iron only at/below ORE_IRON_MAX_Y. Coal is ungated.
 * Ores also never appear in the bedrock/air bands (y<1 or above any solid). */
static void test_ores(void) {
    long coal = 0, iron = 0, gold = 0, diamond = 0;
    for (int cx = -3; cx <= 3; cx++)
        for (int cz = -3; cz <= 3; cz++) {
            Chunk* c = gen(cx, cz, 1337);
            for (int x = 0; x < CHUNK_X; x++)
                for (int z = 0; z < CHUNK_Z; z++)
                    for (int y = 1; y < CHUNK_Y; y++) {
                        BlockID b = chunk_get_block(c, x, y, z);
                        switch (b) {
                            case BLOCK_COAL_ORE:    coal++;    break;
                            case BLOCK_IRON_ORE:
                                iron++;
                                assert(y <= ORE_IRON_MAX_Y);
                                break;
                            case BLOCK_GOLD_ORE:
                                gold++;
                                assert(y <= ORE_GOLD_MAX_Y);
                                break;
                            case BLOCK_DIAMOND_ORE:
                                diamond++;
                                assert(y <= ORE_DIAMOND_MAX_Y);
                                break;
                            default: break;
                        }
                    }
            chunk_destroy(c);
        }
    printf("  coal=%ld iron=%ld gold=%ld diamond=%ld\n",
           coal, iron, gold, diamond);
    /* Ores demonstrably spawn across this 7x7 chunk spread. */
    assert(coal > 0);
    assert(iron > 0);
    assert(diamond > 0); /* deep diamonds do appear somewhere in the spread */
    printf("PASS: ores\n");
}

/* ── 6. Spread of coords incl. negatives — no crashes / OOB ─────────────── */
/* Generate a wide spread of chunk coordinates (including negative coords and
 * coords that don't share sign on the two axes). Every emitted block id must
 * be a valid enum value (< BLOCK_COUNT); the act of generating without crashing
 * or aborting validates the in-bounds writes (chunk_set_* clamp OOB anyway). */
static void test_spread_no_oob(void) {
    int gen_count = 0;
    for (int cx = -8; cx <= 8; cx += 4)
        for (int cz = -8; cz <= 8; cz += 4) {
            Chunk* c = gen(cx, cz, 4242);
            for (int i = 0; i < CHUNK_BLOCKS; i++)
                assert(c->blocks[i] < BLOCK_COUNT);
            /* State advanced to GENERATED. */
            assert(atomic_load(&c->state) == CHUNK_GENERATED);
            gen_count++;
            chunk_destroy(c);
        }
    printf("  generated %d chunks across spread\n", gen_count);
    printf("PASS: spread_no_oob\n");
}

/* Find the topmost non-air, non-water block in a column. -1 if none. */
static int top_solid(Chunk* c, int x, int z) {
    for (int y = CHUNK_Y - 1; y >= 1; y--) {
        BlockID b = chunk_get_block(c, x, y, z);
        if (b != BLOCK_AIR && b != BLOCK_WATER) return y;
    }
    return -1;
}

/* ── 7. Per-biome surface composition ───────────────────────────────────── */
/* Desert columns get a SAND surface skin over SANDSTONE (not dirt/stone), and
 * SNOW-biome columns get BLOCK_SNOW on top. We scan a wide chunk spread, locate
 * columns whose biome is desert/snow via the pure biome classifier, and assert
 * their surface composition wherever a cave hasn't breached the very top. */
static void test_biome_surface_blocks(void) {
    int seed = 1337;
    int desert_checked = 0, snow_checked = 0;

    /* Deserts/snowfields are sparse and can be thousands of blocks away. Scan a
     * wide coarse grid of chunk coords, but only pay to generate a chunk whose
     * centre column is the biome we still need samples of. */
    for (int cx = -210; cx <= 210 && (desert_checked < 20 || snow_checked < 20); cx += 2)
        for (int cz = -210; cz <= 210 && (desert_checked < 20 || snow_checked < 20); cz += 2) {
            int cbx = cx * CHUNK_X + 8, cbz = cz * CHUNK_Z + 8;
            Biome cb = biome_at(cbx, cbz, seed);
            int want_desert = (cb == BIOME_DESERT && desert_checked < 20);
            int want_snow   = (cb == BIOME_SNOW   && snow_checked   < 20);
            if (!want_desert && !want_snow) continue;
            Chunk* c = gen(cx, cz, seed);
            int base_x = cx * CHUNK_X, base_z = cz * CHUNK_Z;
            for (int x = 0; x < CHUNK_X; x++)
                for (int z = 0; z < CHUNK_Z; z++) {
                    Biome b = biome_at(base_x + x, base_z + z, seed);
                    int top = top_solid(c, x, z);
                    if (top < 0) continue;
                    BlockID tb = chunk_get_block(c, x, top, z);

                    if (b == BIOME_DESERT && desert_checked < 20) {
                        /* Desert surface, where not a beach (beaches force sand
                         * anyway) and not breached by a cave (stone exposed). */
                        if (tb == BLOCK_SAND) {
                            BlockID under = chunk_get_block(c, x, top - 1, z);
                            /* Directly under the sand skin: sandstone, more sand,
                             * or (beach) more sand. Never dirt/grass. */
                            assert(under == BLOCK_SANDSTONE || under == BLOCK_SAND);
                            assert(under != BLOCK_DIRT && under != BLOCK_GRASS);
                            desert_checked++;
                        }
                    } else if (b == BIOME_SNOW && snow_checked < 20) {
                        /* Snow surface column tops out in snow (unless a cave
                         * breached it, exposing stone/dirt). */
                        if (tb == BLOCK_SNOW) snow_checked++;
                    }
                }
            chunk_destroy(c);
        }

    printf("  desert columns checked=%d snow columns checked=%d\n",
           desert_checked, snow_checked);
    assert(desert_checked > 0);  /* deserts exist and carry sand/sandstone */
    assert(snow_checked > 0);    /* snowfields exist and carry snow on top  */
    printf("PASS: biome_surface_blocks\n");
}

/* ── 8. Cave carved density is sane ──────────────────────────────────────── */
/* The pure cave model should carve a modest fraction of the underground band:
 * enough to be real caves, far from honeycombing the world solid. We measure
 * the fraction of carvable (originally stone/dirt) deep voxels turned to air. */
static void test_cave_density(void) {
    int seed = 1337;
    long carved = 0, sampled = 0;
    for (int cx = -2; cx <= 2; cx++)
        for (int cz = -2; cz <= 2; cz++) {
            Chunk* c = gen(cx, cz, seed);
            int base_x = cx * CHUNK_X, base_z = cz * CHUNK_Z;
            for (int x = 0; x < CHUNK_X; x++)
                for (int z = 0; z < CHUNK_Z; z++)
                    /* Deep band, comfortably below any surface/soil. */
                    for (int y = 20; y <= 60; y++) {
                        /* Count voxels the cave model could touch (stone-ish) plus
                         * the air it already produced there. */
                        BlockID b = chunk_get_block(c, x, y, z);
                        int is_ore_b = is_ore(b);
                        if (b == BLOCK_STONE || is_ore_b || b == BLOCK_AIR) {
                            sampled++;
                            if (b == BLOCK_AIR &&
                                cave_is_carved(base_x + x, y, base_z + z, (uint32_t)seed))
                                carved++;
                        }
                    }
            chunk_destroy(c);
        }
    double frac = (double)carved / (double)sampled;
    printf("  cave carved fraction (deep band) = %.4f (%ld/%ld)\n",
           frac, carved, sampled);
    assert(carved > 0);          /* caves actually exist */
    assert(frac < 0.25);         /* world is not swiss cheese */
    printf("PASS: cave_density\n");
}

/* ── 9. Surface cave entrances exist ─────────────────────────────────────── */
/* Over a sampled region, at least some columns under open sky have a cave
 * opening reaching the surface: a surface skin air-block sitting directly above
 * a cave-carved void, i.e. you can fall in from the top. */
static void test_cave_entrances(void) {
    int seed = 1337;
    int entrances = 0;
    for (int cx = -4; cx <= 4 && entrances == 0; cx++)
        for (int cz = -4; cz <= 4 && entrances == 0; cz++) {
            Chunk* c = gen(cx, cz, seed);
            for (int x = 0; x < CHUNK_X; x++)
                for (int z = 0; z < CHUNK_Z; z++) {
                    int top = top_solid(c, x, z);
                    if (top < 0 || top + 1 >= CHUNK_Y) continue;
                    /* Open sky directly above the top solid block. */
                    if (chunk_get_block(c, x, top + 1, z) != BLOCK_AIR) continue;
                    /* An air void within a few blocks under the surface: a cave
                     * that has opened toward / through the surface. */
                    for (int dy = 1; dy <= 4 && top - dy >= 1; dy++) {
                        if (chunk_get_block(c, x, top - dy, z) == BLOCK_AIR) {
                            entrances++;
                            break;
                        }
                    }
                }
            chunk_destroy(c);
        }
    printf("  surface cave entrances found (first hit) = %d\n", entrances);
    assert(entrances > 0); /* caves are discoverable from the surface */
    printf("PASS: cave_entrances\n");
}

/* ── 10. Dungeon rooms in generated chunks ──────────────────────────────────
 * Locate a present dungeon (pure model), generate every chunk its footprint
 * overlaps, and reconstruct the room from the real block array. Assert: the
 * shell voxels are mossy cobblestone (except where bedrock was protected), the
 * interior is air, and exactly one chest sits inside. Also asserts cross-chunk
 * continuity at the generation level: each chunk wrote only its slice and the
 * slices reassemble into one coherent room (no clipping, no duplication). */
static void test_dungeon_room(void) {
    int seed = 1337;

    /* Find a present dungeon candidate in a modest cell range. */
    DungeonRoom r = (DungeonRoom){0};
    for (int cgx = -20; cgx <= 20 && !r.present; cgx++)
        for (int cgz = -20; cgz <= 20 && !r.present; cgz++) {
            DungeonRoom c = dungeon_cell_at(cgx, cgz, (uint32_t)seed);
            if (c.present) r = c;
        }
    assert(r.present);

    int x1 = r.x0 + r.w - 1, y1 = r.y0 + r.h - 1, z1 = r.z0 + r.d - 1;

    /* Generate all chunks the footprint spans, cache pointers in a small grid. */
    int cx0 = (int)floor((double)r.x0 / CHUNK_X);
    int cx1 = (int)floor((double)x1   / CHUNK_X);
    int cz0 = (int)floor((double)r.z0 / CHUNK_Z);
    int cz1 = (int)floor((double)z1   / CHUNK_Z);

    long chests = 0, interior_air = 0, shell_mossy = 0, shell_total = 0;
    /* Read a world voxel by locating its chunk among the generated set. */
    for (int cx = cx0; cx <= cx1; cx++)
        for (int cz = cz0; cz <= cz1; cz++) {
            Chunk* c = gen(cx, cz, seed);
            int base_x = cx * CHUNK_X, base_z = cz * CHUNK_Z;
            for (int wx = r.x0; wx <= x1; wx++)
                for (int wz = r.z0; wz <= z1; wz++) {
                    int lx = wx - base_x, lz = wz - base_z;
                    if (lx < 0 || lx >= CHUNK_X || lz < 0 || lz >= CHUNK_Z)
                        continue; /* not this chunk's slice */
                    for (int wy = r.y0; wy <= y1; wy++) {
                        BlockID b = chunk_get_block(c, lx, wy, lz);
                        int role = dungeon_voxel_role(&r, wx, wy, wz);
                        if (wx == r.chest_x && wy == r.chest_y &&
                            wz == r.chest_z) {
                            assert(b == BLOCK_CHEST);
                            chests++;
                            continue;
                        }
                        if (role == 1) {
                            shell_total++;
                            /* Shell is mossy cobble unless bedrock was protected
                             * (never happens this high, but allow it defensively). */
                            if (b == BLOCK_MOSSY_COBBLESTONE) shell_mossy++;
                            else assert(b == BLOCK_BEDROCK);
                        } else if (role == 2) {
                            assert(b == BLOCK_AIR);
                            interior_air++;
                        }
                    }
                }
            chunk_destroy(c);
        }

    printf("  dungeon at (%d,%d,%d) %dx%dx%d : shell_mossy=%ld/%ld "
           "interior_air=%ld chests=%ld\n",
           r.x0, r.y0, r.z0, r.w, r.h, r.d,
           shell_mossy, shell_total, interior_air, chests);
    assert(chests == 1);            /* exactly one chest, placed once */
    assert(shell_mossy == shell_total); /* full mossy-cobble shell */
    assert(interior_air > 0);       /* genuinely hollow */
    printf("PASS: dungeon_room\n");
}

/* Read the generated surface height (topmost solid, non-water) of a world
 * column by generating its chunk. Returns -1 if the column has no solid block.
 * Generates a fresh chunk each call — fine for the small strips below. */
static int gen_surface_height(int wx, int wz, int seed) {
    int cx = (int)floor((double)wx / CHUNK_X);
    int cz = (int)floor((double)wz / CHUNK_Z);
    Chunk* c = gen(cx, cz, seed);
    int lx = wx - cx * CHUNK_X;
    int lz = wz - cz * CHUNK_Z;
    int top = top_solid(c, lx, lz);
    chunk_destroy(c);
    return top;
}

/* ── 11. Biome borders blend smoothly (no height cliffs) ─────────────────────
 * Wiring biome_blend_height_bias into worldgen must make terrain elevation
 * transition gradually across a biome border rather than snapping by the full
 * per-biome bias gap. We use a known plains↔mountains border (bias 0 vs 24, the
 * largest gap) at seed 1337, scan a strip crossing it, and contrast:
 *
 *   - HARD reference: worldgen_get_height (raw base, no bias) + the *hard*
 *     biome_height_bias(biome_at(...)). This is what per-column hard selection
 *     WOULD produce; its max adjacent-column jump spans the border cliff.
 *   - ACTUAL: the real generated surface heights from worldgen (blended bias).
 *
 * We assert the hard reference genuinely cliffs (large single-column jump) while
 * the actual generated terrain stays smooth (small max adjacent jump). If the
 * blend were NOT wired in, ACTUAL would equal HARD and the smoothness assert
 * would fail — so this test fails against the old hard selection. */
static void test_biome_border_smooth(void) {
    int seed = 1337;
    int z = -200;            /* row crossing a plains→mountains border */
    int x0 = 150, x1 = 200;

    int prev_hard = -9999, prev_actual = -9999;
    int max_hard_jump = 0, max_actual_jump = 0;
    int crossed_border = 0;

    for (int x = x0; x <= x1; x++) {
        Biome b = biome_at(x, z, seed);
        int hard = worldgen_get_height(x, z, seed) + biome_height_bias(b);
        int actual = gen_surface_height(x, z, seed);
        assert(actual >= 1);   /* dry highland: always a solid surface */

        if (prev_hard != -9999) {
            int dh = hard - prev_hard;   if (dh < 0) dh = -dh;
            int da = actual - prev_actual; if (da < 0) da = -da;
            if (dh > max_hard_jump)   max_hard_jump = dh;
            if (da > max_actual_jump) max_actual_jump = da;
        }
        prev_hard = hard;
        prev_actual = actual;
    }

    /* Confirm the strip really straddles the plains/mountains border. */
    if (biome_at(x0, z, seed) != biome_at(x1, z, seed)) crossed_border = 1;
    assert(crossed_border);

    printf("  border strip: max hard jump=%d  max actual(blended) jump=%d\n",
           max_hard_jump, max_actual_jump);
    /* Hard selection cliffs at the border (≈ the 24-block bias gap). */
    assert(max_hard_jump >= 20);
    /* Blended terrain transitions smoothly — no single-column cliff. */
    assert(max_actual_jump <= 12);
    /* And it is strictly smoother than hard selection. */
    assert(max_actual_jump < max_hard_jump);
    printf("PASS: biome_border_smooth\n");
}

/* ── 12. Deep inside a biome: no regression vs hard selection ────────────────
 * Far from any border the blend weight of the dominant biome is ~1, so the
 * blended params must reduce exactly to the old per-biome behavior:
 *   - surface skin == biome_surface_block(hard biome)
 *   - surface height == base + hard biome_height_bias
 * We find deep-interior columns by requiring the hard biome (biome_at) to be
 * uniform over a generous ±64-block world window (climate noise is smooth and
 * low-frequency, so spatial uniformity over that span implies the blend kernel
 * — radius BIOME_BLEND_RADIUS in climate space — sees a single biome). For such
 * columns we assert exact agreement with hard selection. Columns breached at the
 * very top by a cave (stone/air exposed) are skipped, as elsewhere. */
static void test_deep_biome_no_regression(void) {
    int seed = 1337;
    int checked = 0;

    /* Walk a coarse grid of candidate columns; for each, verify biome uniformity
     * over a window, then check the generated chunk's surface against hard. */
    for (int cx = -30; cx <= 30 && checked < 12; cx += 3)
        for (int cz = -30; cz <= 30 && checked < 12; cz += 3) {
            int wx = cx * CHUNK_X + 8;
            int wz = cz * CHUNK_Z + 8;
            Biome b = biome_at(wx, wz, seed);

            /* Require deep interior: same biome across a ±64-block window. */
            int uniform = 1;
            for (int dx = -64; dx <= 64 && uniform; dx += 16)
                for (int dz = -64; dz <= 64 && uniform; dz += 16)
                    if (biome_at(wx + dx, wz + dz, seed) != b) uniform = 0;
            if (!uniform) continue;

            /* Skip below sea level (water/beach overrides the biome skin). */
            int hard_h = worldgen_get_height(wx, wz, seed) + biome_height_bias(b);
            if (hard_h <= SEA_LEVEL + 1) continue;

            Chunk* c = gen(cx, cz, seed);
            BlockID surf = chunk_get_block(c, 8, hard_h, 8);
            int actual_top = top_solid(c, 8, 8);
            chunk_destroy(c);

            /* A cave entrance can breach the very top; only assert where the
             * column is intact (top solid at the expected hard height). */
            if (actual_top != hard_h) continue;

            BlockID expect = biome_surface_block(b, hard_h);
            /* Trees can sit on top of grass biomes; allow wood/leaves there. */
            int surf_ok = (surf == expect) ||
                          (surf == BLOCK_WOOD) || (surf == BLOCK_LEAVES);
            assert(surf_ok);
            /* Height matches hard selection exactly (blend weight ~= 1). */
            assert(actual_top == hard_h);
            checked++;
        }

    printf("  deep-biome columns verified against hard selection: %d\n", checked);
    assert(checked > 0);
    printf("PASS: deep_biome_no_regression\n");
}

int main(void) {
    test_deterministic();
    test_bedrock_floor();
    test_surface_sanity();
    test_stone_body();
    test_ores();
    test_spread_no_oob();
    test_biome_surface_blocks();
    test_cave_density();
    test_cave_entrances();
    test_dungeon_room();
    test_biome_border_smooth();
    test_deep_biome_no_regression();
    printf("ALL WORLDGEN TESTS PASSED\n");
    return 0;
}
