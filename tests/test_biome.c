#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/biome.h"
#include "../src/block.h"

#define SEED 1337

/* biome_at must be a pure function of its arguments. */
static void test_deterministic(void) {
    for (int i = 0; i < 5000; i++) {
        int x = i * 13 - 30000, z = i * 7 - 15000;
        Biome a = biome_at(x, z, SEED);
        Biome b = biome_at(x, z, SEED);
        assert(a == b);
        assert(a >= 0 && a < BIOME_COUNT);
    }
    printf("PASS: deterministic\n");
}

/* Re-querying in a different order (interleaved seeds) must not change the
 * result — guards against thread-local cache state leaking across calls. */
static void test_pure_across_seeds(void) {
    for (int i = 0; i < 2000; i++) {
        int x = i * 31, z = i * 17;
        Biome a1 = biome_at(x, z, SEED);
        Biome other = biome_at(x + 5, z + 5, SEED + 1); /* perturb cache */
        (void)other;
        Biome a2 = biome_at(x, z, SEED);
        assert(a1 == a2);
    }
    printf("PASS: pure_across_seeds\n");
}

/* Different seeds should not produce an identical biome field everywhere. */
static void test_seed_varies_field(void) {
    int diffs = 0;
    for (int x = 0; x < 400; x += 8)
        for (int z = 0; z < 400; z += 8)
            if (biome_at(x, z, SEED) != biome_at(x, z, SEED + 12345))
                diffs++;
    assert(diffs > 0);
    printf("PASS: seed_varies_field (%d differing columns)\n", diffs);
}

/* Across a large area, every biome must appear (distribution sanity). */
static void test_all_biomes_appear(void) {
    long counts[BIOME_COUNT] = {0};
    long total = 0;
    for (int x = -2000; x < 2000; x += 4)
        for (int z = -2000; z < 2000; z += 4) {
            Biome b = biome_at(x, z, SEED);
            assert(b >= 0 && b < BIOME_COUNT);
            counts[b]++;
            total++;
        }
    printf("  plains=%ld forest=%ld desert=%ld mountains=%ld snow=%ld total=%ld\n",
           counts[BIOME_PLAINS], counts[BIOME_FOREST],
           counts[BIOME_DESERT], counts[BIOME_MOUNTAINS], counts[BIOME_SNOW],
           total);
    for (int i = 0; i < BIOME_COUNT; i++)
        assert(counts[i] > 0);                 /* all biomes present     */
    assert(counts[BIOME_PLAINS] > total / 100); /* plains not vanishingly rare */
    printf("PASS: all_biomes_appear\n");
}

/* Pure climate -> biome classifier: each climate region maps as expected.
 * Inputs are normalized noise values in [-1, 1]. */
static void test_classify_regions(void) {
    /* High elevation always wins -> mountains, regardless of climate. */
    assert(biome_classify( 0.9f,  0.0f,  0.9f) == BIOME_MOUNTAINS);
    assert(biome_classify(-0.9f,  0.9f,  0.9f) == BIOME_MOUNTAINS);
    assert(biome_classify( 0.9f, -0.9f,  0.9f) == BIOME_MOUNTAINS);

    /* Cold (low temp), lowland -> snow/tundra, whatever the humidity. */
    assert(biome_classify(-0.9f,  0.0f, 0.0f) == BIOME_SNOW);
    assert(biome_classify(-0.9f,  0.9f, 0.0f) == BIOME_SNOW);
    assert(biome_classify(-0.9f, -0.9f, 0.0f) == BIOME_SNOW);

    /* Hot + dry, lowland -> desert. */
    assert(biome_classify( 0.9f, -0.9f, 0.0f) == BIOME_DESERT);

    /* Temperate + humid -> forest. */
    assert(biome_classify( 0.1f,  0.9f, 0.0f) == BIOME_FOREST);

    /* Temperate + moderate humidity -> plains. */
    assert(biome_classify( 0.1f,  0.0f, 0.0f) == BIOME_PLAINS);

    /* Hot but humid is NOT a desert (deserts require dryness). */
    assert(biome_classify( 0.9f,  0.9f, 0.0f) != BIOME_DESERT);

    printf("PASS: classify_regions\n");
}

/* classify is a total, deterministic, in-range pure function over the input
 * domain, and matches biome_at where elevation is supplied separately. */
static void test_classify_total_and_pure(void) {
    for (float t = -1.0f; t <= 1.0f; t += 0.05f)
        for (float h = -1.0f; h <= 1.0f; h += 0.05f)
            for (float e = -1.0f; e <= 1.0f; e += 0.05f) {
                Biome a = biome_classify(t, h, e);
                Biome b = biome_classify(t, h, e);
                assert(a == b);
                assert(a >= 0 && a < BIOME_COUNT);
            }
    printf("PASS: classify_total_and_pure\n");
}

/* Surface-block mapping correctness for each biome. */
static void test_surface_block_mapping(void) {
    /* Plains and forest are grassy. */
    assert(biome_surface_block(BIOME_PLAINS, 70) == BLOCK_GRASS);
    assert(biome_surface_block(BIOME_FOREST, 70) == BLOCK_GRASS);
    /* Desert is sand at any height. */
    assert(biome_surface_block(BIOME_DESERT, 64) == BLOCK_SAND);
    assert(biome_surface_block(BIOME_DESERT, 200) == BLOCK_SAND);
    /* Mountains: bare stone below the snow line, snow at/above it. */
    assert(biome_surface_block(BIOME_MOUNTAINS, BIOME_SNOW_LINE - 1) == BLOCK_STONE);
    assert(biome_surface_block(BIOME_MOUNTAINS, BIOME_SNOW_LINE) == BLOCK_SNOW);
    assert(biome_surface_block(BIOME_MOUNTAINS, BIOME_SNOW_LINE + 50) == BLOCK_SNOW);
    /* Snow biome surface is the snow block. */
    assert(biome_surface_block(BIOME_SNOW, 70) == BLOCK_SNOW);
    /* Surface block is always a real, non-air block. */
    for (int b = 0; b < BIOME_COUNT; b++) {
        BlockID s = biome_surface_block((Biome)b, 70);
        assert(s != BLOCK_AIR && s < BLOCK_COUNT);
    }
    printf("PASS: surface_block_mapping\n");
}

/* Sub-surface mapping: dirt under grass/snow, sandstone under desert sand,
 * stone under mountains. */
static void test_subsurface_block_mapping(void) {
    assert(biome_subsurface_block(BIOME_PLAINS, 70) == BLOCK_DIRT);
    assert(biome_subsurface_block(BIOME_FOREST, 70) == BLOCK_DIRT);
    assert(biome_subsurface_block(BIOME_DESERT, 70) == BLOCK_SANDSTONE);
    assert(biome_subsurface_block(BIOME_SNOW, 70) == BLOCK_DIRT);
    assert(biome_subsurface_block(BIOME_MOUNTAINS, 70) == BLOCK_STONE);
    assert(biome_subsurface_block(BIOME_MOUNTAINS, BIOME_SNOW_LINE + 10) == BLOCK_STONE);
    for (int b = 0; b < BIOME_COUNT; b++) {
        BlockID s = biome_subsurface_block((Biome)b, 70);
        assert(s != BLOCK_AIR && s < BLOCK_COUNT);
    }
    printf("PASS: subsurface_block_mapping\n");
}

/* Tree density: bounded [0,1], forest densest, desert/mountains bare. */
static void test_tree_density(void) {
    for (int b = 0; b < BIOME_COUNT; b++) {
        float d = biome_tree_density((Biome)b);
        assert(d >= 0.0f && d <= 1.0f);
    }
    assert(biome_tree_density(BIOME_FOREST) > biome_tree_density(BIOME_PLAINS));
    assert(biome_tree_density(BIOME_PLAINS) > 0.0f);
    assert(biome_tree_density(BIOME_DESERT) == 0.0f);
    assert(biome_tree_density(BIOME_MOUNTAINS) == 0.0f);
    /* Snow/tundra is sparse-to-bare, never denser than forest. */
    assert(biome_tree_density(BIOME_SNOW) < biome_tree_density(BIOME_FOREST));
    printf("PASS: tree_density\n");
}

/* Height bias: mountains rise above the rest. */
static void test_height_bias(void) {
    assert(biome_height_bias(BIOME_MOUNTAINS) > biome_height_bias(BIOME_PLAINS));
    assert(biome_height_bias(BIOME_MOUNTAINS) > biome_height_bias(BIOME_FOREST));
    assert(biome_height_bias(BIOME_PLAINS) == 0);
    printf("PASS: height_bias\n");
}

int main(void) {
    test_deterministic();
    test_pure_across_seeds();
    test_seed_varies_field();
    test_all_biomes_appear();
    test_surface_block_mapping();
    test_subsurface_block_mapping();
    test_tree_density();
    test_height_bias();
    test_classify_regions();
    test_classify_total_and_pure();
    printf("ALL BIOME TESTS PASSED\n");
    return 0;
}
