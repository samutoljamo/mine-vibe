#include <assert.h>
#include <stdio.h>
#include "../src/block.h"
#include "../src/chunk.h"
#include "../src/lighting.h"

static void test_block_light_absorb_values(void)
{
    /* Air transmits fully. */
    assert(block_get_def(BLOCK_AIR)->light_absorb == 0);
    assert(block_get_def(BLOCK_AIR)->light_emit   == 0);

    /* Leaves and water dim slightly. */
    assert(block_get_def(BLOCK_LEAVES)->light_absorb == 2);
    assert(block_get_def(BLOCK_WATER)->light_absorb  == 2);

    /* Solid opaque blocks fully absorb. */
    assert(block_get_def(BLOCK_STONE)->light_absorb   == 15);
    assert(block_get_def(BLOCK_DIRT)->light_absorb    == 15);
    assert(block_get_def(BLOCK_GRASS)->light_absorb   == 15);
    assert(block_get_def(BLOCK_SAND)->light_absorb    == 15);
    assert(block_get_def(BLOCK_WOOD)->light_absorb    == 15);
    assert(block_get_def(BLOCK_BEDROCK)->light_absorb == 15);

    /* Spec 1: no block emits light. */
    assert(block_get_def(BLOCK_AIR)->light_emit     == 0);
    assert(block_get_def(BLOCK_STONE)->light_emit   == 0);
    assert(block_get_def(BLOCK_DIRT)->light_emit    == 0);
    assert(block_get_def(BLOCK_GRASS)->light_emit   == 0);
    assert(block_get_def(BLOCK_SAND)->light_emit    == 0);
    assert(block_get_def(BLOCK_WOOD)->light_emit    == 0);
    assert(block_get_def(BLOCK_LEAVES)->light_emit  == 0);
    assert(block_get_def(BLOCK_WATER)->light_emit   == 0);
    assert(block_get_def(BLOCK_BEDROCK)->light_emit == 0);

    printf("PASS: test_block_light_absorb_values\n");
}

static void test_chunk_light_lazy_alloc_and_pack(void)
{
    Chunk* c = chunk_create(0, 0);

    /* Before any write, lights is NULL but reads return 0. */
    assert(c->lights == NULL);
    assert(chunk_get_skylight(c, 0, 0, 0)   == 0);
    assert(chunk_get_blocklight(c, 0, 0, 0) == 0);

    /* First write allocates lights. */
    chunk_set_skylight(c, 1, 2, 3, 15);
    assert(c->lights != NULL);
    assert(chunk_get_skylight(c, 1, 2, 3)   == 15);
    assert(chunk_get_blocklight(c, 1, 2, 3) == 0);

    /* Block-light nibble does not stomp sky nibble. */
    chunk_set_blocklight(c, 1, 2, 3, 9);
    assert(chunk_get_skylight(c, 1, 2, 3)   == 15);
    assert(chunk_get_blocklight(c, 1, 2, 3) == 9);

    /* Setting sky again does not stomp block. */
    chunk_set_skylight(c, 1, 2, 3, 4);
    assert(chunk_get_skylight(c, 1, 2, 3)   == 4);
    assert(chunk_get_blocklight(c, 1, 2, 3) == 9);

    /* Out-of-range writes are no-ops. */
    chunk_set_skylight(c, -1, 0, 0, 15);
    chunk_set_skylight(c,  0, -1, 0, 15);
    chunk_set_skylight(c,  0, 0, -1, 15);
    assert(chunk_get_skylight(c, 0, 0, 0) == 0);

    /* Upper-bound OOB writes are also no-ops. */
    chunk_set_skylight(c, CHUNK_X, 0, 0, 15);
    chunk_set_skylight(c, 0, CHUNK_Y, 0, 15);
    chunk_set_skylight(c, 0, 0, CHUNK_Z, 15);
    /* Max valid corner round-trips. */
    chunk_set_skylight(c, CHUNK_X - 1, CHUNK_Y - 1, CHUNK_Z - 1, 7);
    assert(chunk_get_skylight(c, CHUNK_X - 1, CHUNK_Y - 1, CHUNK_Z - 1) == 7);

    /* Setters truncate to nibble: only low 4 bits stored. */
    chunk_set_skylight(c, 5, 5, 5, 16);  /* 16 == 0b10000 → low nibble = 0 */
    assert(chunk_get_skylight(c, 5, 5, 5) == 0);
    chunk_set_blocklight(c, 5, 5, 5, 17); /* 17 == 0b10001 → low nibble = 1 */
    assert(chunk_get_blocklight(c, 5, 5, 5) == 1);

    chunk_destroy(c);
    printf("PASS: test_chunk_light_lazy_alloc_and_pack\n");
}

/* Helper: empty (all-air) chunk with optional pillar. */
static Chunk* make_chunk_with_pillar(int px, int pz, int top_y, BlockID b)
{
    Chunk* c = chunk_create(0, 0);
    /* Already calloc'd to BLOCK_AIR (0). */
    if (top_y >= 0) {
        for (int y = 0; y <= top_y; y++) {
            chunk_set_block(c, px, y, pz, b);
        }
    }
    return c;
}

static void test_sky_column_empty_chunk(void)
{
    Chunk* c = make_chunk_with_pillar(0, 0, -1, BLOCK_AIR); /* no pillar */
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };

    lighting_initial_pass(c, &nb);

    /* Every cell sees full sky. */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                assert(chunk_get_skylight(c, x, y, z) == 15);

    chunk_destroy(c);
    printf("PASS: test_sky_column_empty_chunk\n");
}

/* Pillar cell itself is opaque -> sky=0; cell above pillar sees sky=15.
 * Both assertions are BFS-stable:
 *   - sky_column_pass never writes into a cell whose absorb=15 except as 0.
 *   - horizontal_bfs (Task 4) computes new_sky = step_light(neighbor, 15) = 0,
 *     so it never raises stone cells. The cell above is in an open column
 *     and stays 15. */
static void test_sky_column_at_pillar(void)
{
    /* Stone pillar at (5, *, 7) reaching y=64. */
    Chunk* c = make_chunk_with_pillar(5, 7, 64, BLOCK_STONE);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };

    lighting_initial_pass(c, &nb);

    /* Cells above the pillar see sky. */
    for (int y = 65; y < CHUNK_Y; y++)
        assert(chunk_get_skylight(c, 5, y, 7) == 15);

    /* The pillar cells themselves are opaque (absorb=15) -> sky=0. */
    for (int y = 0; y <= 64; y++)
        assert(chunk_get_skylight(c, 5, y, 7) == 0);

    chunk_destroy(c);
    printf("PASS: test_sky_column_at_pillar\n");
}

/* Leaves cell itself absorbs 2; the cell above sees full sky.
 * Both assertions are BFS-stable:
 *   - The cell above is in an open column at sky=15; horizontal BFS sees
 *     no neighbor brighter than 15, so it stays.
 *   - The leaves cell's sky after column pass is 13. After BFS, neighbors
 *     at the same y in open air are sky=15; BFS computes
 *     step_light(15, 2) = 13 into the leaves cell, which is not greater
 *     than the existing 13, so no change. */
static void test_sky_column_through_leaves(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 3, 100, 3, BLOCK_LEAVES);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };

    lighting_initial_pass(c, &nb);

    /* Above leaves: 15. */
    assert(chunk_get_skylight(c, 3, 101, 3) == 15);
    /* Leaves cell: 15 - absorb(2) = 13. */
    assert(chunk_get_skylight(c, 3, 100, 3) == 13);

    chunk_destroy(c);
    printf("PASS: test_sky_column_through_leaves\n");
}

static void test_bfs_through_doorway(void)
{
    /* Stone roof at y=15 covering everything EXCEPT one cell at (5, 15, 8).
     * Below the roof (y < 15) is open air. Without BFS, sky_column_pass
     * floods column (5, *, 8) with sky=15 (because the roof has a hole),
     * but every other column under the roof is sky=0. With BFS, light
     * propagates horizontally from (5, *, 8) through the open space below
     * the roof, decrementing by 1 per step. */
    Chunk* c = chunk_create(0, 0);
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 15, z, BLOCK_STONE);
    /* Knock out one roof cell. */
    chunk_set_block(c, 5, 15, 8, BLOCK_AIR);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Directly under the hole at y=14: sky=15. */
    assert(chunk_get_skylight(c, 5, 14, 8) == 15);

    /* One BFS step away (still below the roof, so initially sky=0). */
    assert(chunk_get_skylight(c, 5, 14, 9) == 14);
    assert(chunk_get_skylight(c, 5, 14, 7) == 14);
    assert(chunk_get_skylight(c, 4, 14, 8) == 14);
    assert(chunk_get_skylight(c, 6, 14, 8) == 14);

    /* Two and three steps away. */
    assert(chunk_get_skylight(c, 5, 14, 10) == 13);
    assert(chunk_get_skylight(c, 5, 14, 11) == 12);

    chunk_destroy(c);
    printf("PASS: test_bfs_through_doorway\n");
}

static void test_bfs_blocked_by_solid(void)
{
    /* Solid roof at y=15 covers everything; sky cannot penetrate. */
    Chunk* c = chunk_create(0, 0);
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 15, z, BLOCK_STONE);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Above the roof: 15. */
    assert(chunk_get_skylight(c, 8, 16, 8) == 15);

    /* Under the roof: 0 (no doorway). */
    for (int y = 0; y < 15; y++)
        assert(chunk_get_skylight(c, 8, y, 8) == 0);

    chunk_destroy(c);
    printf("PASS: test_bfs_blocked_by_solid\n");
}

/* Two chunks side by side. Chunk A (left) has a doorway opening into
 * the BFS field; Chunk B (right) is fully open with sky=15 already.
 * After A's pass, A's eastern boundary cells should match the propagated
 * values; B should have a needs_relight flag set so it picks up A's
 * boundary contribution next. */
static void test_cross_chunk_boundary_delta(void)
{
    Chunk* a = chunk_create(0, 0);
    Chunk* b = chunk_create(1, 0); /* +X neighbor */

    /* Seal A under a stone roof at y=15 except a doorway at +X edge. */
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(a, x, 15, z, BLOCK_STONE);
    chunk_set_block(a, CHUNK_X - 1, 15, 8, BLOCK_AIR); /* doorway */

    /* B is fully open (all air), pre-lit with sky=15 by its own pass. */
    LightingNeighbors nb_b = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(b, &nb_b);

    /* Now run A's pass with B as +X neighbor. */
    LightingNeighbors nb_a = { NULL, b, NULL, NULL };
    lighting_initial_pass(a, &nb_a);

    /* Inside A under the doorway: sky=15 directly under, falls off west. */
    assert(chunk_get_skylight(a, CHUNK_X - 1, 15, 8) == 15);
    /* One step west of doorway under roof: 14. */
    assert(chunk_get_skylight(a, CHUNK_X - 2, 14, 8) == 14);

    /* B's western boundary (x=0) should not yet be re-lit. The BFS only
     * RECORDS deltas onto B's pending queue. After consume_pending runs,
     * B's boundary cells are unchanged because they were already 15. */
    assert(chunk_get_skylight(b, 0, 15, 8) == 15);

    /* Reverse case: B has a column of solid blocks at x=0. After A lights
     * via the doorway, B's x=0 column under the column should rise from 0
     * to whatever propagates from A. */
    chunk_destroy(a);
    chunk_destroy(b);

    /* Reset for second sub-case. */
    a = chunk_create(0, 0);
    b = chunk_create(1, 0);

    /* B has solid pillar at x=0 from y=10..14 — fully shaded under it. */
    for (int y = 10; y <= 14; y++)
        chunk_set_block(b, 0, y, 8, BLOCK_STONE);

    /* A is fully open. */
    LightingNeighbors nb_a2 = { NULL, b, NULL, NULL };
    LightingNeighbors nb_b2 = { a, NULL, NULL, NULL };
    lighting_initial_pass(a, &nb_a2);
    lighting_initial_pass(b, &nb_b2);

    /* After both passes, B should be flagged needs_relight (A's bright
     * boundary at x=15 wants to push light into B at x=0). */
    assert(b->needs_relight == true);

    /* Consume pending on B and re-run BFS. */
    lighting_consume_pending(b, &nb_b2);

    /* B's cell at (0, 12, 8) is solid stone — light=0. But (0, 12, 7)
     * under the pillar's shade now sees A's bright neighbor and gets
     * sky=14 (one step from A's x=15 edge). */
    assert(chunk_get_skylight(b, 0, 12, 7) >= 13);

    chunk_destroy(a);
    chunk_destroy(b);
    printf("PASS: test_cross_chunk_boundary_delta\n");
}

/* Place an opaque block at a sky-exposed cell. Column under it should go dark. */
static void test_relight_place_opaque_at_sky(void)
{
    Chunk* c = chunk_create(0, 0);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);
    /* All cells are 15. */
    assert(chunk_get_skylight(c, 8, 64, 8) == 15);

    /* Place stone at y=128 column (8,8). */
    chunk_set_block(c, 8, 128, 8, BLOCK_STONE);
    lighting_on_block_changed(c, &nb, 8, 128, 8, BLOCK_AIR, BLOCK_STONE);

    /* Cells directly below should now be 0 (sky no longer reaches). */
    assert(chunk_get_skylight(c, 8, 127, 8) == 0);
    assert(chunk_get_skylight(c, 8, 64, 8)  == 0);
    /* Side cells refilled by horizontal BFS from neighbors. */
    assert(chunk_get_skylight(c, 7, 64, 8) == 15);
    assert(chunk_get_skylight(c, 8, 64, 7) == 15);

    chunk_destroy(c);
    printf("PASS: test_relight_place_opaque_at_sky\n");
}

/* Break an opaque block in a roof. Column below should re-light. */
static void test_relight_break_opaque_roof(void)
{
    Chunk* c = chunk_create(0, 0);

    /* Stone roof at y=20 over the whole chunk. */
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 20, z, BLOCK_STONE);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);
    assert(chunk_get_skylight(c, 5, 10, 5) == 0);

    /* Break the roof above (5, *, 5). */
    chunk_set_block(c, 5, 20, 5, BLOCK_AIR);
    lighting_on_block_changed(c, &nb, 5, 20, 5, BLOCK_STONE, BLOCK_AIR);

    /* Cells directly under the new hole get sky=15. */
    assert(chunk_get_skylight(c, 5, 19, 5) == 15);
    assert(chunk_get_skylight(c, 5, 10, 5) == 15);
    /* Adjacent cells under the rest of the roof get less than 15. */
    assert(chunk_get_skylight(c, 4, 19, 5) == 14);
    assert(chunk_get_skylight(c, 5, 19, 4) == 14);

    chunk_destroy(c);
    printf("PASS: test_relight_break_opaque_roof\n");
}

/* Place an opaque block where light comes laterally (not from above).
 * Cells lit by independent paths must NOT be zeroed by removal_bfs. */
static void test_relight_place_does_not_clear_lateral_light(void)
{
    Chunk* c = chunk_create(0, 0);
    /* Stone roof at y=20 covering x=4..11 only (open at x<4 and x>11).
     * Light reaches under the roof from both sides. */
    for (int x = 4; x <= 11; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 20, z, BLOCK_STONE);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Trace expected values pre-relight: cells under the roof get light
     * from BFS through the open sides. (5, 19, 8) is under the roof, two
     * steps from open at x=3, so sky should be ~13. (8, 19, 8) is the
     * middle cell, ~10. We don't assert the exact pre-values, just that
     * they're nonzero. */
    uint8_t sky_before_5 = chunk_get_skylight(c, 5, 19, 8);
    uint8_t sky_before_10 = chunk_get_skylight(c, 10, 19, 8);
    assert(sky_before_5 > 0);
    assert(sky_before_10 > 0);

    /* Place stone at (5, 19, 8) — under the roof, between two paths.
     * Lateral light through the doorway at x=3 still feeds (4, 19, 8),
     * and through x=12 still feeds (10, 19, 8). The placed block must
     * not zero those independent paths. */
    chunk_set_block(c, 5, 19, 8, BLOCK_STONE);
    lighting_on_block_changed(c, &nb, 5, 19, 8, BLOCK_AIR, BLOCK_STONE);

    /* (4, 19, 8) is independently lit from x<4 open. Must remain bright. */
    assert(chunk_get_skylight(c, 4, 19, 8) > 0);
    /* (10, 19, 8) is independently lit from x>11 open. Must remain bright. */
    assert(chunk_get_skylight(c, 10, 19, 8) > 0);
    /* The placed block itself is opaque → 0. */
    assert(chunk_get_skylight(c, 5, 19, 8) == 0);

    chunk_destroy(c);
    printf("PASS: test_relight_place_does_not_clear_lateral_light\n");
}

/* ── Block light (emissive torches) ─────────────────────────────────────── */

/* The torch block exists, is opaque/solid, and emits ~14. */
static void test_torch_block_def(void)
{
    const BlockDef* t = block_get_def(BLOCK_TORCH);
    assert(t->light_emit == 14);
    assert(t->is_solid == true);
    assert(t->is_transparent == false);
    /* No non-torch block emits. */
    assert(block_get_def(BLOCK_STONE)->light_emit == 0);
    assert(block_get_def(BLOCK_PATH)->light_emit  == 0);
    printf("PASS: test_torch_block_def\n");
}

/* A torch placed in open air lights its neighbourhood with linear falloff. */
static void test_torch_lights_neighbourhood(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 100, 8, BLOCK_TORCH);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Torch cell itself holds the emit value. */
    assert(chunk_get_blocklight(c, 8, 100, 8) == 14);
    /* One step away: 13, two steps: 12, three: 11. */
    assert(chunk_get_blocklight(c, 9, 100, 8)  == 13);
    assert(chunk_get_blocklight(c, 8, 100, 9)  == 13);
    assert(chunk_get_blocklight(c, 8, 101, 8)  == 13);
    assert(chunk_get_blocklight(c, 10, 100, 8) == 12);
    assert(chunk_get_blocklight(c, 11, 100, 8) == 11);
    /* Manhattan distance 14 reaches 0 (15 steps away gone). */
    assert(chunk_get_blocklight(c, 8 + 14, 100, 8) == 0);

    /* Block light is in the high nibble — sky nibble at these cells is its
     * own value and unaffected by the torch. */
    assert(chunk_get_skylight(c, 9, 100, 8) == 15);

    chunk_destroy(c);
    printf("PASS: test_torch_lights_neighbourhood\n");
}

/* Block light stops at an opaque wall. */
static void test_torch_blocked_by_wall(void)
{
    Chunk* c = chunk_create(0, 0);
    /* Torch at x=8, stone wall at x=10 spanning the y/z plane locally. */
    chunk_set_block(c, 8, 100, 8, BLOCK_TORCH);
    for (int y = 96; y <= 104; y++)
        for (int z = 4; z <= 12; z++)
            chunk_set_block(c, 10, y, z, BLOCK_STONE);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* In front of the wall (x=9): lit. */
    assert(chunk_get_blocklight(c, 9, 100, 8) == 13);
    /* The wall cell is opaque: no block light entered it. */
    assert(chunk_get_blocklight(c, 10, 100, 8) == 0);
    /* Directly behind the wall (x=11), with the wall sealing the straight
     * path, light must be strictly less than the open-air value (12) — it can
     * only arrive by detouring around the 9x9 wall patch, if at all. */
    assert(chunk_get_blocklight(c, 11, 100, 8) < 12);

    chunk_destroy(c);
    printf("PASS: test_torch_blocked_by_wall\n");
}

/* Placing a torch at runtime lights neighbours via the relight-on-edit path. */
static void test_torch_place_relights(void)
{
    Chunk* c = chunk_create(0, 0);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* No block light anywhere yet. */
    assert(chunk_get_blocklight(c, 8, 50, 8) == 0);
    assert(chunk_get_blocklight(c, 9, 50, 8) == 0);

    chunk_set_block(c, 8, 50, 8, BLOCK_TORCH);
    lighting_on_block_changed(c, &nb, 8, 50, 8, BLOCK_AIR, BLOCK_TORCH);

    assert(chunk_get_blocklight(c, 8, 50, 8) == 14);
    assert(chunk_get_blocklight(c, 9, 50, 8) == 13);
    assert(chunk_get_blocklight(c, 10, 50, 8) == 12);

    chunk_destroy(c);
    printf("PASS: test_torch_place_relights\n");
}

/* Removing a torch darkens its neighbourhood back to zero. */
static void test_torch_remove_darkens(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 50, 8, BLOCK_TORCH);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);
    assert(chunk_get_blocklight(c, 9, 50, 8) == 13);

    /* Break the torch. */
    chunk_set_block(c, 8, 50, 8, BLOCK_AIR);
    lighting_on_block_changed(c, &nb, 8, 50, 8, BLOCK_TORCH, BLOCK_AIR);

    /* Whole neighbourhood goes dark. */
    assert(chunk_get_blocklight(c, 8, 50, 8)  == 0);
    assert(chunk_get_blocklight(c, 9, 50, 8)  == 0);
    assert(chunk_get_blocklight(c, 10, 50, 8) == 0);
    assert(chunk_get_blocklight(c, 8, 51, 8)  == 0);

    chunk_destroy(c);
    printf("PASS: test_torch_remove_darkens\n");
}

/* Two torches: removing one leaves cells fed by the other still lit. */
static void test_torch_remove_keeps_independent_source(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 4, 50, 8, BLOCK_TORCH);
    chunk_set_block(c, 12, 50, 8, BLOCK_TORCH);
    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Remove the first torch. */
    chunk_set_block(c, 4, 50, 8, BLOCK_AIR);
    lighting_on_block_changed(c, &nb, 4, 50, 8, BLOCK_TORCH, BLOCK_AIR);

    /* Second torch still lights its own neighbourhood. */
    assert(chunk_get_blocklight(c, 12, 50, 8) == 14);
    assert(chunk_get_blocklight(c, 11, 50, 8) == 13);
    /* The removed torch's cell is dark. */
    assert(chunk_get_blocklight(c, 4, 50, 8) == 0);

    chunk_destroy(c);
    printf("PASS: test_torch_remove_keeps_independent_source\n");
}

int main(void)
{
    test_torch_block_def();
    test_torch_lights_neighbourhood();
    test_torch_blocked_by_wall();
    test_torch_place_relights();
    test_torch_remove_darkens();
    test_torch_remove_keeps_independent_source();

    test_block_light_absorb_values();
    test_chunk_light_lazy_alloc_and_pack();
    test_sky_column_empty_chunk();
    test_sky_column_at_pillar();
    test_sky_column_through_leaves();
    test_bfs_through_doorway();
    test_bfs_blocked_by_solid();
    test_cross_chunk_boundary_delta();
    test_relight_place_opaque_at_sky();
    test_relight_break_opaque_roof();
    test_relight_place_does_not_clear_lateral_light();
    return 0;
}
