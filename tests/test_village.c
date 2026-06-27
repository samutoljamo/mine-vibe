#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/village.h"
#include "../src/block.h"
#include "../src/chunk.h"
#include "../src/worldgen.h"

#define SEED 1337

/* ── 1. Determinism ─────────────────────────────────────────────────────── */

static void test_cell_deterministic(void) {
    for (int i = 0; i < 2000; i++) {
        int cgx = (i * 13) - 1000;
        int cgz = (i * 7)  - 500;
        VillageCell a = village_cell_at(cgx, cgz, SEED);
        VillageCell b = village_cell_at(cgx, cgz, SEED);
        assert(a.present == b.present);
        assert(a.wx == b.wx && a.wz == b.wz);
        assert(a.seed == b.seed);
    }
    /* Layout helpers must be deterministic too. */
    for (int vs = 1; vs < 500; vs++) {
        assert(village_house_count(vs) == village_house_count(vs));
        for (int i = 0; i < VILLAGE_MAX_HOUSES; i++) {
            VillageHouse h1 = village_house_at(vs, 1000, 2000, i);
            VillageHouse h2 = village_house_at(vs, 1000, 2000, i);
            assert(h1.cx == h2.cx && h1.cz == h2.cz);
            assert(h1.w == h2.w && h1.d == h2.d);
            assert(h1.peaked_roof == h2.peaked_roof);
            assert(h1.door_face == h2.door_face);
        }
    }
    printf("PASS: cell_deterministic\n");
}

/* ── 2. Density within tolerance of VILLAGE_SPAWN_PCT ───────────────────── */

static void test_density(void) {
    long present = 0, total = 0;
    for (int cgx = -50; cgx < 50; cgx++)
        for (int cgz = -50; cgz < 50; cgz++) {
            total++;
            if (village_cell_at(cgx, cgz, SEED).present) present++;
        }
    double frac = (double)present / (double)total;
    printf("  present=%ld total=%ld frac=%.3f\n", present, total, frac);
    /* Wide tolerance around the 25% target. */
    assert(frac > 0.15 && frac < 0.35);
    printf("PASS: density\n");
}

/* ── 3. Centers in-bounds so adjacent cells cannot overlap ──────────────── */

static void test_center_in_bounds(void) {
    for (int cgx = -20; cgx < 20; cgx++)
        for (int cgz = -20; cgz < 20; cgz++) {
            VillageCell vc = village_cell_at(cgx, cgz, SEED);
            if (!vc.present) continue;
            int cell_x0 = cgx * VILLAGE_CELL_BLOCKS;
            int cell_z0 = cgz * VILLAGE_CELL_BLOCKS;
            /* Center must sit at least MARGIN inside the cell on all sides. */
            assert(vc.wx >= cell_x0 + VILLAGE_MARGIN);
            assert(vc.wx <  cell_x0 + VILLAGE_CELL_BLOCKS - VILLAGE_MARGIN);
            assert(vc.wz >= cell_z0 + VILLAGE_MARGIN);
            assert(vc.wz <  cell_z0 + VILLAGE_CELL_BLOCKS - VILLAGE_MARGIN);
        }
    /* Margin guarantees neighbour centers are > 2*MAX_RADIUS apart. Since
     * MARGIN(48) > MAX_RADIUS(40), two villages can never overlap. */
    assert(VILLAGE_MARGIN > VILLAGE_MAX_RADIUS);
    printf("PASS: center_in_bounds\n");
}

/* ── 4. Suitability gating ──────────────────────────────────────────────── */

static void test_suitability(void) {
    int good_h = VILLAGE_SEA_LEVEL + 5;

    /* All conditions hold → suitable. */
    assert(village_is_suitable(good_h, BLOCK_GRASS, good_h - 1, good_h + 2));

    /* At/below sea level → not suitable. */
    assert(!village_is_suitable(VILLAGE_SEA_LEVEL, BLOCK_GRASS,
                                VILLAGE_SEA_LEVEL, VILLAGE_SEA_LEVEL));
    assert(!village_is_suitable(VILLAGE_SEA_LEVEL - 3, BLOCK_GRASS,
                                VILLAGE_SEA_LEVEL - 3, VILLAGE_SEA_LEVEL - 3));

    /* Wrong surface block → not suitable. */
    assert(!village_is_suitable(good_h, BLOCK_SAND, good_h, good_h));
    assert(!village_is_suitable(good_h, BLOCK_WATER, good_h, good_h));
    assert(!village_is_suitable(good_h, BLOCK_STONE, good_h, good_h));

    /* Too steep → not suitable. */
    assert(!village_is_suitable(good_h, BLOCK_GRASS,
                                good_h, good_h + VILLAGE_MAX_SLOPE + 1));
    /* Exactly at the slope limit → still suitable. */
    assert(village_is_suitable(good_h, BLOCK_GRASS,
                               good_h, good_h + VILLAGE_MAX_SLOPE));
    printf("PASS: suitability\n");
}

/* ── 5. House layout bounds & non-overlap ───────────────────────────────── */

static int boxes_overlap(VillageHouse a, VillageHouse b) {
    int ax0 = a.cx - a.w / 2, ax1 = ax0 + a.w;
    int az0 = a.cz - a.d / 2, az1 = az0 + a.d;
    int bx0 = b.cx - b.w / 2, bx1 = bx0 + b.w;
    int bz0 = b.cz - b.d / 2, bz1 = bz0 + b.d;
    /* Treat touching as non-overlap; require a 1-block gap. */
    return !(ax1 + 1 <= bx0 || bx1 + 1 <= ax0 ||
             az1 + 1 <= bz0 || bz1 + 1 <= az0);
}

static void test_house_layout(void) {
    for (int vs = 1; vs < 2000; vs++) {
        int cx = 5000, cz = -3000;
        int n = village_house_count(vs);
        assert(n >= VILLAGE_MIN_HOUSES && n <= VILLAGE_MAX_HOUSES);

        VillageHouse houses[VILLAGE_MAX_HOUSES];
        for (int i = 0; i < n; i++) {
            houses[i] = village_house_at(vs, cx, cz, i);
            VillageHouse h = houses[i];
            /* Footprint sane. */
            assert(h.w >= VILLAGE_HOUSE_MIN && h.w <= VILLAGE_HOUSE_MAX);
            assert(h.d >= VILLAGE_HOUSE_MIN && h.d <= VILLAGE_HOUSE_MAX);
            assert(h.door_face >= 0 && h.door_face <= 3);
            /* Whole footprint within MAX_RADIUS of the village center. */
            int dx = h.cx - cx, dz = h.cz - cz;
            int reach_x = (dx < 0 ? -dx : dx) + h.w / 2 + 1;
            int reach_z = (dz < 0 ? -dz : dz) + h.d / 2 + 1;
            assert(reach_x <= VILLAGE_MAX_RADIUS);
            assert(reach_z <= VILLAGE_MAX_RADIUS);
        }
        /* No two houses overlap. */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                assert(!boxes_overlap(houses[i], houses[j]));
    }
    printf("PASS: house_layout\n");
}

/* ── 6. Cross-chunk consistency & bounds safety (uses a real Chunk) ──────── */

/* Re-derive worldgen's center suitability the same way village.c does so the
 * test can locate a village that actually materializes. */
static bool center_suitable(int wx, int wz, int seed) {
    int ch = worldgen_get_height(wx, wz, seed);
    int mn = ch, mx = ch;
    int off[4][2] = { {VILLAGE_MAX_RADIUS,0},{-VILLAGE_MAX_RADIUS,0},
                      {0,VILLAGE_MAX_RADIUS},{0,-VILLAGE_MAX_RADIUS} };
    for (int i = 0; i < 4; i++) {
        int h = worldgen_get_height(wx+off[i][0], wz+off[i][1], seed);
        if (h < mn) mn = h;
        if (h > mx) mx = h;
    }
    BlockID surf = (ch >= VILLAGE_SEA_LEVEL + 2) ? BLOCK_GRASS : BLOCK_SAND;
    return village_is_suitable(ch, surf, mn, mx);
}

static int count_nonair(Chunk* c) {
    int n = 0;
    for (int i = 0; i < CHUNK_BLOCKS; i++)
        if (c->blocks[i] != BLOCK_AIR) n++;
    return n;
}

/* World-coord block lookup across an array of generated chunks. Returns
 * BLOCK_AIR if no chunk owns the column. */
static BlockID world_block(Chunk** chunks, int nchunks, int wx, int wy, int wz) {
    for (int i = 0; i < nchunks; i++) {
        Chunk* c = chunks[i];
        int lx = wx - c->cx * CHUNK_X;
        int lz = wz - c->cz * CHUNK_Z;
        if (lx >= 0 && lx < CHUNK_X && lz >= 0 && lz < CHUNK_Z)
            return chunk_get_block(c, lx, wy, lz);
    }
    return BLOCK_AIR;
}

static void test_cross_chunk_and_bounds(void) {
    int generated_villages = 0;
    int seam_villages = 0; /* villages that genuinely straddle a chunk edge */

    for (int test_seed = 1; test_seed <= 6000 && seam_villages < 3; test_seed++) {
        for (int cgx = -2; cgx <= 2 && seam_villages < 3; cgx++)
        for (int cgz = -2; cgz <= 2 && seam_villages < 3; cgz++) {
            VillageCell vc = village_cell_at(cgx, cgz, test_seed);
            if (!vc.present) continue;
            if (!center_suitable(vc.wx, vc.wz, test_seed)) continue;

            /* Floor-div the center to its chunk, then generate a 5x5 block of
             * chunks covering the whole village (radius 40 < 3 chunks). */
            int ccx = vc.wx >= 0 ? vc.wx / 16 : -((-vc.wx + 15) / 16);
            int ccz = vc.wz >= 0 ? vc.wz / 16 : -((-vc.wz + 15) / 16);

            Chunk* chunks[25];
            int n = 0;
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++) {
                    Chunk* c = chunk_create(ccx + dx, ccz + dz);
                    for (int i = 0; i < CHUNK_BLOCKS; i++) c->blocks[i] = BLOCK_AIR;
                    int hm[16][16] = {0};
                    village_generate(c, test_seed, hm);
                    chunks[n++] = c;
                }

            int total_nonair = 0;
            for (int i = 0; i < n; i++) total_nonair += count_nonair(chunks[i]);
            if (total_nonair > 0) generated_villages++;

            /* Bounds safety: only valid block ids emitted, no out-of-range. */
            for (int i = 0; i < n; i++)
                for (int b = 0; b < CHUNK_BLOCKS; b++)
                    assert(chunks[i]->blocks[b] < BLOCK_COUNT);

            /* Determinism: regenerating any chunk yields identical blocks. */
            Chunk* dup = chunk_create(ccx, ccz);
            for (int i = 0; i < CHUNK_BLOCKS; i++) dup->blocks[i] = BLOCK_AIR;
            int hm2[16][16] = {0};
            village_generate(dup, test_seed, hm2);
            for (int i = 0; i < CHUNK_BLOCKS; i++)
                assert(dup->blocks[i] == chunks[12]->blocks[i]); /* center chunk */
            chunk_destroy(dup);

            /* Seam test: the well canopy is a 3x3 cobble slab at py+4 that no
             * path or house touches. It is emitted in world coords, so each
             * chunk supplies only its slice; the union across all chunks must
             * reconstruct the COMPLETE 3x3 slab with no gaps at any chunk
             * boundary it crosses. This proves structures tile seamlessly. */
            int py = worldgen_get_height(vc.wx, vc.wz, test_seed);
            int canopy_ok = 1;
            for (int dx = -1; dx <= 1; dx++)
                for (int dz = -1; dz <= 1; dz++) {
                    BlockID cb = world_block(chunks, n, vc.wx+dx, py+4, vc.wz+dz);
                    if (cb != BLOCK_COBBLE) canopy_ok = 0;
                }
            assert(canopy_ok); /* complete canopy regardless of seams */

            /* Does this well actually straddle a chunk edge? */
            int edge = (vc.wx % 16 == 0 || vc.wx % 16 == 15 ||
                        ((vc.wx-1) % 16 + 16) % 16 == 15 ||
                        vc.wz % 16 == 0 || vc.wz % 16 == 15);
            if (edge && total_nonair > 0) seam_villages++;

            for (int i = 0; i < n; i++) chunk_destroy(chunks[i]);
        }
    }
    printf("  generated_villages=%d seam_villages=%d\n",
           generated_villages, seam_villages);
    assert(generated_villages > 0); /* villages do materialize */
    printf("PASS: cross_chunk_and_bounds\n");
}

int main(void) {
    test_cell_deterministic();
    test_density();
    test_center_in_bounds();
    test_suitability();
    test_house_layout();
    test_cross_chunk_and_bounds();
    printf("ALL VILLAGE TESTS PASSED\n");
    return 0;
}
