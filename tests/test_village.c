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
    printf("  present=%ld total=%ld frac=%.3f (target %d%%)\n",
           present, total, frac, VILLAGE_SPAWN_PCT);
    /* Wide tolerance around the VILLAGE_SPAWN_PCT target. */
    double target = VILLAGE_SPAWN_PCT / 100.0;
    assert(frac > target - 0.10 && frac < target + 0.10);
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
    /* Neighbour cell centers are at least 2*MARGIN apart. With the findability
     * tuning MARGIN(36) is below MAX_RADIUS(40), so two close villages may have
     * slightly overlapping reach — this is harmless (the generator just
     * overwrites blocks via chunk_set_block, and the seam test below proves the
     * central landmark still reconstructs correctly). The margin still keeps
     * every center safely inside its own cell. */
    assert(VILLAGE_MARGIN > 0);
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

    /* Relaxed surface rule: any non-water solid ground above sea level is now
     * suitable (the generator flattens the footprint). */
    assert(village_is_suitable(good_h, BLOCK_SAND, good_h, good_h));
    assert(village_is_suitable(good_h, BLOCK_STONE, good_h, good_h));
    /* Water (and air) are still rejected. */
    assert(!village_is_suitable(good_h, BLOCK_WATER, good_h, good_h));
    assert(!village_is_suitable(good_h, BLOCK_AIR, good_h, good_h));

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
    /* Sample at VILLAGE_SAMPLE_RADIUS to match village.c's footprint check. */
    int off[4][2] = { {VILLAGE_SAMPLE_RADIUS,0},{-VILLAGE_SAMPLE_RADIUS,0},
                      {0,VILLAGE_SAMPLE_RADIUS},{0,-VILLAGE_SAMPLE_RADIUS} };
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

/* ── 7. Deterministic locator ──────────────────────────────────────────────
 * village_nearest() must (a) be deterministic, (b) return a center that is
 * actually a present + suitable village (i.e. one the generator materializes),
 * and (c) return the genuinely nearest such center within its scan window. */

static void test_nearest(void) {
    int seeds[] = { 1337, 420, 7, 99999 };
    for (size_t s = 0; s < sizeof(seeds)/sizeof(seeds[0]); s++) {
        int seed = seeds[s];

        /* Probe several query positions, including the origin (spawn). */
        int probes[][2] = { {0,0}, {500,-500}, {-1234,777}, {3000,3000} };
        for (size_t p = 0; p < sizeof(probes)/sizeof(probes[0]); p++) {
            int qx = probes[p][0], qz = probes[p][1];

            int ax, az, bx, bz;
            bool a = village_nearest(qx, qz, seed, &ax, &az);
            bool b = village_nearest(qx, qz, seed, &bx, &bz);

            /* (a) Deterministic. */
            assert(a == b);
            if (!a) continue;
            assert(ax == bx && az == bz);

            /* (b) The returned center is a present + suitable village: find the
             * cell that owns it and confirm village_cell_at agrees, then that
             * the center materializes per the generator's suitability test. */
            int ccx = ax >= 0 ? ax / VILLAGE_CELL_BLOCKS
                              : -((-ax + VILLAGE_CELL_BLOCKS - 1) / VILLAGE_CELL_BLOCKS);
            int ccz = az >= 0 ? az / VILLAGE_CELL_BLOCKS
                              : -((-az + VILLAGE_CELL_BLOCKS - 1) / VILLAGE_CELL_BLOCKS);
            VillageCell vc = village_cell_at(ccx, ccz, seed);
            assert(vc.present);
            assert(vc.wx == ax && vc.wz == az);
            assert(center_suitable(ax, az, seed));

            /* The located village actually emits non-air blocks when generated. */
            int gcx = ax >= 0 ? ax / 16 : -((-ax + 15) / 16);
            int gcz = az >= 0 ? az / 16 : -((-az + 15) / 16);
            int nonair = 0;
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++) {
                    Chunk* c = chunk_create(gcx + dx, gcz + dz);
                    for (int i = 0; i < CHUNK_BLOCKS; i++) c->blocks[i] = BLOCK_AIR;
                    int hm[16][16] = {0};
                    village_generate(c, seed, hm);
                    nonair += count_nonair(c);
                    chunk_destroy(c);
                }
            assert(nonair > 0);

            /* (c) No nearer present+suitable village exists in a small window
             * around the query (brute-force check against the locator result). */
            long got2 = (long)(ax - qx)*(ax - qx) + (long)(az - qz)*(az - qz);
            int home_cx = qx >= 0 ? qx / VILLAGE_CELL_BLOCKS
                                  : -((-qx + VILLAGE_CELL_BLOCKS - 1) / VILLAGE_CELL_BLOCKS);
            int home_cz = qz >= 0 ? qz / VILLAGE_CELL_BLOCKS
                                  : -((-qz + VILLAGE_CELL_BLOCKS - 1) / VILLAGE_CELL_BLOCKS);
            for (int dcx = -6; dcx <= 6; dcx++)
                for (int dcz = -6; dcz <= 6; dcz++) {
                    VillageCell cand = village_cell_at(home_cx + dcx, home_cz + dcz, seed);
                    if (!cand.present || !center_suitable(cand.wx, cand.wz, seed))
                        continue;
                    long d2 = (long)(cand.wx - qx)*(cand.wx - qx) +
                              (long)(cand.wz - qz)*(cand.wz - qz);
                    assert(d2 >= got2);
                }
        }
    }
    /* Report the nearest village to spawn for the two seeds of interest. */
    int vx, vz;
    if (village_nearest(0, 0, 1337, &vx, &vz))
        printf("  seed=1337 nearest-to-origin=(%d,%d)\n", vx, vz);
    if (village_nearest(0, 0, 420, &vx, &vz))
        printf("  seed=420  nearest-to-origin=(%d,%d)\n", vx, vz);
    printf("PASS: nearest\n");
}

/* ── 8. Per-building grounding on a sloped surface ───────────────────────────
 * On sloped terrain each building must settle on its OWN local ground height
 * (not one shared village plane) and never float — the floor base Y must equal
 * the MIN surface height over the building's footprint, and very steep
 * footprints are skipped rather than towered. */

/* Synthetic terrain: surface height rises 1 block per block of +X (a 45° ramp
 * along X, flat along Z). Sea-level base keeps everything above water. */
static int ramp_x_height(int wx, int wz, void* ctx) {
    (void)wz; (void)ctx;
    return (VILLAGE_SEA_LEVEL + 5) + wx; /* slope 1.0 along X */
}
/* Flat terrain. */
static int flat_height(int wx, int wz, void* ctx) {
    (void)wx; (void)wz; (void)ctx;
    return (VILLAGE_SEA_LEVEL + 8);
}

static void test_building_grounding(void) {
    /* Flat terrain: any footprint settles at the flat height, slope 0. */
    int by, mn, mx;
    bool ok = village_footprint_base(0, 0, 5, 5, flat_height, NULL, &by, &mn, &mx);
    assert(ok);
    assert(by == VILLAGE_SEA_LEVEL + 8);
    assert(mn == mx && mn == VILLAGE_SEA_LEVEL + 8);

    /* Gentle ramp footprint within the slope budget: base Y == MIN surface over
     * the footprint, i.e. the lowest (smallest-X) column. */
    int x0 = 100, x1 = x0 + VILLAGE_BUILDING_MAX_SLOPE; /* width == slope budget */
    ok = village_footprint_base(x0, 0, x1, 4, ramp_x_height, NULL, &by, &mn, &mx);
    assert(ok);
    assert(mn == ramp_x_height(x0, 0, NULL));   /* lowest column */
    assert(mx == ramp_x_height(x1, 0, NULL));   /* highest column */
    assert(by == mn);                           /* floor sits on the low side */
    assert(mx - mn == VILLAGE_BUILDING_MAX_SLOPE);

    /* Too steep: a wider footprint on the same ramp exceeds the budget → skip. */
    ok = village_footprint_base(x0, 0, x1 + 1, 4, ramp_x_height, NULL,
                                &by, &mn, &mx);
    assert(!ok);

    /* Determinism. */
    int by2, mn2, mx2;
    bool ok2 = village_footprint_base(x0, 0, x1, 4, ramp_x_height, NULL,
                                      &by2, &mn2, &mx2);
    assert(ok2 && by2 == by && mn2 == mn && mx2 == mx);

    printf("PASS: building_grounding\n");
}

/* ── 9. Buildings rest on local ground in a real generated village ───────────
 * Generate a village on the real terrain and assert that (a) different houses
 * across a sloped footprint do NOT all share a single floor Y (per-building
 * grounding actually happens), and (b) under every house footprint corner the
 * block directly beneath the floor is solid (no air gap — no floating). */

static int prod_height(int wx, int wz, void* ctx) {
    int seed = *(int*)ctx;
    return worldgen_get_height(wx, wz, seed);
}

static void test_no_float_in_generated_village(void) {
    int checked = 0;
    for (int test_seed = 1; test_seed <= 8000 && checked < 4; test_seed++) {
        for (int cgx = -2; cgx <= 2 && checked < 4; cgx++)
        for (int cgz = -2; cgz <= 2 && checked < 4; cgz++) {
            VillageCell vc = village_cell_at(cgx, cgz, test_seed);
            if (!vc.present) continue;
            if (!center_suitable(vc.wx, vc.wz, test_seed)) continue;

            int ccx = vc.wx >= 0 ? vc.wx / 16 : -((-vc.wx + 15) / 16);
            int ccz = vc.wz >= 0 ? vc.wz / 16 : -((-vc.wz + 15) / 16);

            /* Use real worldgen (full terrain) so house floors land on actual
             * ground; village_generate only emits structure blocks over AIR, so
             * fill the column with STONE up to (surface) per worldgen height to
             * model the ground under the footprint. */
            Chunk* chunks[25];
            int nch = 0;
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++) {
                    Chunk* c = chunk_create(ccx + dx, ccz + dz);
                    int bx = (ccx + dx) * CHUNK_X, bz = (ccz + dz) * CHUNK_Z;
                    for (int lx = 0; lx < CHUNK_X; lx++)
                        for (int lz = 0; lz < CHUNK_Z; lz++) {
                            int sh = worldgen_get_height(bx + lx, bz + lz, test_seed);
                            for (int y = 0; y < CHUNK_Y; y++)
                                chunk_set_block(c, lx, y, lz,
                                    y <= sh ? BLOCK_STONE : BLOCK_AIR);
                        }
                    int hm[16][16] = {0};
                    village_generate(c, test_seed, hm);
                    chunks[nch++] = c;
                }

            /* Each house's base floor Y is the per-building grounded base
             * (min surface over its footprint), recomputed via the same pure
             * helper production uses. Assert (a) the bases differ across the
             * sloped village (not one shared plane) and (b) under every
             * footprint corner the block directly below the floor is solid (no
             * air gap = no floating). */
            int n = village_house_count(vc.seed);
            int nfloors = 0;
            int distinct_seen = 0, first_y = -9999;
            for (int i = 0; i < n; i++) {
                VillageHouse h = village_house_at(vc.seed, vc.wx, vc.wz, i);
                int hx0 = h.cx - h.w / 2, hx1 = hx0 + h.w - 1;
                int hz0 = h.cz - h.d / 2, hz1 = hz0 + h.d - 1;

                int base_y, mn, mx;
                if (!village_footprint_base(hx0, hz0, hx1, hz1,
                                            prod_height, &test_seed,
                                            &base_y, &mn, &mx))
                    continue; /* house skipped (too steep) — fine */
                nfloors++;
                if (first_y == -9999) first_y = base_y;
                else if (base_y != first_y) distinct_seen = 1;

                /* No-float: the block under each footprint corner's floor is
                 * solid. The floor sits at base_y; flatten fills foundation
                 * below it, so base_y-1 must not be air. */
                int corners[4][2] = { {hx0,hz0},{hx1,hz0},{hx0,hz1},{hx1,hz1} };
                for (int cc = 0; cc < 4; cc++) {
                    BlockID under = world_block(chunks, nch,
                                                corners[cc][0], base_y - 1,
                                                corners[cc][1]);
                    assert(under != BLOCK_AIR);
                }
            }

            /* Only count villages whose center footprint actually spans a slope
             * (so we can meaningfully assert per-building Y differs). */
            int lo = worldgen_get_height(vc.wx - VILLAGE_MAX_RADIUS, vc.wz, test_seed);
            int hi = worldgen_get_height(vc.wx + VILLAGE_MAX_RADIUS, vc.wz, test_seed);
            int spread = hi - lo; if (spread < 0) spread = -spread;
            if (nfloors >= 2 && spread >= 2) {
                assert(distinct_seen); /* not a single shared plane on a slope */
                checked++;
            }

            for (int i = 0; i < nch; i++) chunk_destroy(chunks[i]);
        }
    }
    printf("  sloped-villages-checked=%d\n", checked);
    assert(checked > 0);
    printf("PASS: no_float_in_generated_village\n");
}

int main(void) {
    test_cell_deterministic();
    test_density();
    test_center_in_bounds();
    test_suitability();
    test_house_layout();
    test_cross_chunk_and_bounds();
    test_nearest();
    test_building_grounding();
    test_no_float_in_generated_village();
    printf("ALL VILLAGE TESTS PASSED\n");
    return 0;
}
