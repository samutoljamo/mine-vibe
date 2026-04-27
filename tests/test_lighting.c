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
    /* Floor of stone at y=10. Wall of stone at z=8 from y=10..15.
     * One opening at (5, 11..15, 8) so light leaks south. */
    Chunk* c = chunk_create(0, 0);
    for (int x = 0; x < CHUNK_X; x++)
        for (int z = 0; z < CHUNK_Z; z++)
            chunk_set_block(c, x, 10, z, BLOCK_STONE);
    for (int x = 0; x < CHUNK_X; x++)
        for (int y = 10; y <= 15; y++)
            chunk_set_block(c, x, y, 8, BLOCK_STONE);
    /* Knock out a 1x5 doorway. */
    for (int y = 11; y <= 15; y++)
        chunk_set_block(c, 5, y, 8, BLOCK_AIR);

    LightingNeighbors nb = { NULL, NULL, NULL, NULL };
    lighting_initial_pass(c, &nb);

    /* Inside the doorway, sky-15 enters from above. */
    assert(chunk_get_skylight(c, 5, 15, 8) == 15);

    /* One step away from the doorway opening (still under the wall but in
     * open space at z=9, y=11): light is 14 (one step of cost 1). */
    assert(chunk_get_skylight(c, 5, 11, 9) == 14);

    /* Light falls off as we move further south under the overhang. */
    assert(chunk_get_skylight(c, 5, 11, 10) == 13);
    assert(chunk_get_skylight(c, 5, 11, 11) == 12);

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

int main(void)
{
    test_block_light_absorb_values();
    test_chunk_light_lazy_alloc_and_pack();
    test_sky_column_empty_chunk();
    test_sky_column_at_pillar();
    test_sky_column_through_leaves();
    test_bfs_through_doorway();
    test_bfs_blocked_by_solid();
    return 0;
}
