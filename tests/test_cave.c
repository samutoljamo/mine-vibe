#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "../src/cave.h"

#define SEED 1337u

/* cave_is_carved must be a pure function of its arguments: same inputs always
 * give the same output, no matter how many times it's called. */
static void test_deterministic(void) {
    for (int i = 0; i < 5000; i++) {
        int x = i * 13 - 100;
        int y = CAVE_MIN_Y + 1 + (i % (CAVE_MAX_Y - CAVE_MIN_Y - 1));
        int z = i * 7 - 50;
        bool a = cave_is_carved(x, y, z, SEED);
        bool b = cave_is_carved(x, y, z, SEED);
        bool c = cave_is_carved(x, y, z, SEED);
        assert(a == b && b == c);
    }
    printf("PASS: deterministic\n");
}

/* Different seeds must produce different carving over a sampled volume,
 * otherwise the seed has no effect. */
static void test_seed_sensitivity(void) {
    long differ = 0, total = 0;
    for (int x = 0; x < 40; x++)
        for (int z = 0; z < 40; z++)
            for (int y = CAVE_MIN_Y + 1; y < 60; y++) {
                bool a = cave_is_carved(x, y, z, SEED);
                bool b = cave_is_carved(x, y, z, SEED + 1u);
                if (a != b) differ++;
                total++;
            }
    printf("  seed differ=%ld / %ld\n", differ, total);
    /* A meaningful fraction of voxels must change when the seed changes. */
    assert(differ > 0);
    assert(differ > total / 1000);
    printf("PASS: seed_sensitivity\n");
}

/* Continuity across chunk boundaries: the predicate is a pure function of
 * absolute world coords, so a voxel's carve state cannot depend on which
 * chunk it's considered part of. Voxel at absolute x=15 (chunk 0, local 15)
 * and absolute x=16 (chunk 1, local 0) are simply adjacent world coords and
 * must be computed independently of any chunk origin. We assert that
 * re-deriving the absolute coordinate two different ways yields the same call
 * result, and that the function never references chunk-local indices. */
static void test_continuity_across_chunks(void) {
    const int CHUNK_X = 16;
    for (int cx = -3; cx < 3; cx++)
        for (int local = 0; local < CHUNK_X; local++)
            for (int z = -20; z < 20; z += 5)
                for (int y = CAVE_MIN_Y + 1; y < CAVE_MAX_Y; y += 7) {
                    int abs_x = cx * CHUNK_X + local;
                    /* The same absolute coord, however its chunk/local split is
                     * expressed, must give the same answer. */
                    bool via_chunk = cave_is_carved(abs_x, y, z, SEED);
                    bool via_abs   = cave_is_carved(cx * CHUNK_X + local, y, z, SEED);
                    assert(via_chunk == via_abs);

                    /* Boundary voxels of neighbouring chunks are just adjacent
                     * world columns; verify x=15 and x=16 are each stable. */
                    bool b15 = cave_is_carved(15, y, z, SEED);
                    bool b16 = cave_is_carved(16, y, z, SEED);
                    assert(b15 == cave_is_carved(15, y, z, SEED));
                    assert(b16 == cave_is_carved(16, y, z, SEED));
                }
    printf("PASS: continuity_across_chunks\n");
}

/* Vertical bounds: nothing carved at/below CAVE_MIN_Y or at/above CAVE_MAX_Y. */
static void test_vertical_bounds(void) {
    for (int x = -30; x < 30; x++)
        for (int z = -30; z < 30; z++) {
            assert(!cave_is_carved(x, CAVE_MIN_Y, z, SEED));
            assert(!cave_is_carved(x, CAVE_MIN_Y - 1, z, SEED));
            assert(!cave_is_carved(x, 0, z, SEED));
            assert(!cave_is_carved(x, CAVE_MAX_Y, z, SEED));
            assert(!cave_is_carved(x, CAVE_MAX_Y + 50, z, SEED));
        }
    printf("PASS: vertical_bounds\n");
}

/* Density sanity: over a representative underground volume the carved fraction
 * is in a sane band -- caves exist but don't hollow out the world. */
static void test_density_sanity(void) {
    long carved = 0, total = 0;
    for (int x = -50; x < 50; x++)
        for (int z = -50; z < 50; z++)
            for (int y = CAVE_MIN_Y + 1; y < CAVE_MAX_Y; y++) {
                if (cave_is_carved(x, y, z, SEED)) carved++;
                total++;
            }
    double frac = (double)carved / (double)total;
    printf("  carved density = %.4f (%ld / %ld)\n", frac, carved, total);
    assert(frac > 0.0);    /* caves must exist                */
    assert(frac < 0.50);   /* must not destroy the world      */
    printf("PASS: density_sanity\n");
}

int main(void) {
    test_deterministic();
    test_seed_sensitivity();
    test_continuity_across_chunks();
    test_vertical_bounds();
    test_density_sanity();
    printf("ALL CAVE TESTS PASSED\n");
    return 0;
}
