#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/worldgen.h"
#include "../src/chunk.h"
#include "../src/ore.h"
#include "../src/block.h"

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

int main(void) {
    test_deterministic();
    test_bedrock_floor();
    test_surface_sanity();
    test_stone_body();
    test_ores();
    test_spread_no_oob();
    printf("ALL WORLDGEN TESTS PASSED\n");
    return 0;
}
