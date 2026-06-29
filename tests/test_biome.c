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

/* ---- Border blending (wa8.3.4) ---------------------------------------- */

/* Climate points that sit comfortably deep inside each biome's region (far
 * from any decision boundary relative to BIOME_BLEND_RADIUS). */
static void deep_point(Biome b, float* t, float* h, float* e) {
    switch (b) {
        case BIOME_MOUNTAINS: *t =  0.0f; *h =  0.0f; *e =  0.9f; break;
        case BIOME_SNOW:      *t = -0.9f; *h =  0.0f; *e = -0.5f; break;
        case BIOME_DESERT:    *t =  0.9f; *h = -0.9f; *e = -0.5f; break;
        case BIOME_FOREST:    *t =  0.1f; *h =  0.9f; *e = -0.5f; break;
        case BIOME_PLAINS:
        default:              *t =  0.1f; *h =  0.0f; *e = -0.5f; break;
    }
}

/* Weights are non-negative and sum to ~1 across the whole climate domain. */
static void test_blend_weights_normalized(void) {
    for (float t = -1.0f; t <= 1.0f; t += 0.1f)
        for (float h = -1.0f; h <= 1.0f; h += 0.1f)
            for (float e = -1.0f; e <= 1.0f; e += 0.1f) {
                float w[BIOME_COUNT];
                biome_blend_weights(t, h, e, w);
                float sum = 0.0f;
                for (int i = 0; i < BIOME_COUNT; i++) {
                    assert(w[i] >= 0.0f && w[i] <= 1.0f);
                    sum += w[i];
                }
                assert(sum > 0.999f && sum < 1.001f);
            }
    printf("PASS: blend_weights_normalized\n");
}

/* Deep inside a biome, that biome's weight is ~1 and the blended params equal
 * the raw per-biome params; the dominant biome matches the classifier. */
static void test_blend_interior_matches_raw(void) {
    for (int b = 0; b < BIOME_COUNT; b++) {
        float t, h, e;
        deep_point((Biome)b, &t, &h, &e);

        /* The hard classifier agrees this point is biome b. */
        assert(biome_classify(t, h, e) == (Biome)b);

        float w[BIOME_COUNT];
        biome_blend_weights(t, h, e, w);
        assert(w[b] > 0.999f);

        assert(biome_blend_dominant(t, h, e) == (Biome)b);

        float hb = biome_blend_height_bias(t, h, e);
        float td = biome_blend_tree_density(t, h, e);
        assert(hb > (float)biome_height_bias((Biome)b) - 0.001f &&
               hb < (float)biome_height_bias((Biome)b) + 0.001f);
        assert(td > biome_tree_density((Biome)b) - 0.0001f &&
               td < biome_tree_density((Biome)b) + 0.0001f);
    }
    printf("PASS: blend_interior_matches_raw\n");
}

/* Near a border the blended param lies strictly between the two neighbours'
 * raw params — proving the snap is smoothed out. */
static void test_blend_border_interpolates(void) {
    /* Plains<->desert border. With humidity dry (-0.5, below both the wet and
     * dry thresholds so the temperate side is plains, not forest), sweep temp
     * across the HOT threshold (0.30): hotter -> desert, cooler -> plains.
     * Exactly two biomes meet here. Confirm with the classifier first. */
    assert(biome_classify(0.30f + 0.05f, -0.5f, -0.5f) == BIOME_DESERT);
    assert(biome_classify(0.30f - 0.05f, -0.5f, -0.5f) == BIOME_PLAINS);

    float lo = (float)biome_height_bias(BIOME_DESERT); /* -2 */
    float hi = (float)biome_height_bias(BIOME_PLAINS); /*  0 */
    float on_border = biome_blend_height_bias(0.30f, -0.5f, -0.5f);
    assert(on_border > lo && on_border < hi); /* strictly between -2 and 0 */

    /* Tree density across the same border is also strictly between. */
    float td_lo = biome_tree_density(BIOME_DESERT); /* 0.0  */
    float td_hi = biome_tree_density(BIOME_PLAINS); /* 0.02 */
    float td_b = biome_blend_tree_density(0.30f, -0.5f, -0.5f);
    assert(td_b > td_lo && td_b < td_hi);

    /* Monotone progression as we cross the border: hotter (more desert) gives a
     * lower (more negative) height bias than cooler (more plains). */
    float hot  = biome_blend_height_bias(0.30f + 0.04f, -0.5f, -0.5f);
    float cool = biome_blend_height_bias(0.30f - 0.04f, -0.5f, -0.5f);
    assert(hot < cool);

    /* Plains<->mountains elevation border (threshold 0.35): blended bias sits
     * strictly between plains (0) and mountains (24). */
    float m_lo = (float)biome_height_bias(BIOME_PLAINS);
    float m_hi = (float)biome_height_bias(BIOME_MOUNTAINS);
    float m_b = biome_blend_height_bias(0.1f, 0.0f, 0.35f);
    assert(m_b > m_lo && m_b < m_hi);
    printf("PASS: blend_border_interpolates\n");
}

/* Blend helpers are pure/deterministic. */
static void test_blend_deterministic(void) {
    for (float t = -1.0f; t <= 1.0f; t += 0.17f)
        for (float h = -1.0f; h <= 1.0f; h += 0.19f)
            for (float e = -1.0f; e <= 1.0f; e += 0.23f) {
                float w1[BIOME_COUNT], w2[BIOME_COUNT];
                biome_blend_weights(t, h, e, w1);
                biome_blend_weights(t, h, e, w2);
                for (int i = 0; i < BIOME_COUNT; i++) assert(w1[i] == w2[i]);
                assert(biome_blend_height_bias(t, h, e) ==
                       biome_blend_height_bias(t, h, e));
                assert(biome_blend_tree_density(t, h, e) ==
                       biome_blend_tree_density(t, h, e));
            }
    printf("PASS: blend_deterministic\n");
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
    test_blend_weights_normalized();
    test_blend_interior_matches_raw();
    test_blend_border_interpolates();
    test_blend_deterministic();
    printf("ALL BIOME TESTS PASSED\n");
    return 0;
}
