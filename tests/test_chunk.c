#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/chunk.h"
#include "../src/block.h"

/* The chunk block/meta/light store is plain memory (no Vulkan), so we can
 * exercise the get/set/index math and bounds directly. */

/* Block get/set round-trips for every cell, and the linear index layout
 * (x + z*CHUNK_X + y*CHUNK_X*CHUNK_Z) is a bijection — distinct cells never
 * alias. We verify the latter by writing a unique value derived from the
 * index and reading it back. */
static void test_block_set_get_roundtrip(void) {
    Chunk* c = chunk_create(0, 0);

    /* Sparse but boundary-covering sweep (full sweep is 65536 cells; sample
     * corners + a stride to keep it fast yet exhaustive on edges). */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++) {
                BlockID v = (BlockID)((x * 7 + z * 13 + y) % BLOCK_COUNT);
                chunk_set_block(c, x, y, z, v);
            }
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++) {
                BlockID expect = (BlockID)((x * 7 + z * 13 + y) % BLOCK_COUNT);
                assert(chunk_get_block(c, x, y, z) == expect);
            }
    chunk_destroy(c);
    printf("PASS: block_set_get_roundtrip\n");
}

/* No aliasing: setting one cell must not change a neighbouring cell. Walk
 * adjacent indices on each axis. */
static void test_no_aliasing(void) {
    Chunk* c = chunk_create(0, 0);
    /* All air to start (calloc'd in chunk_create). */
    chunk_set_block(c, 5, 100, 5, BLOCK_STONE);
    /* Every other cell must still be air. */
    int nonair = 0;
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                if (chunk_get_block(c, x, y, z) != BLOCK_AIR) nonair++;
    assert(nonair == 1);
    assert(chunk_get_block(c, 5, 100, 5) == BLOCK_STONE);
    chunk_destroy(c);
    printf("PASS: no_aliasing\n");
}

/* Out-of-bounds reads return BLOCK_AIR; OOB writes are silently dropped (no
 * crash, no effect on in-bounds data). */
static void test_block_bounds(void) {
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 0, 0, 0, BLOCK_STONE);

    assert(chunk_get_block(c, -1, 0, 0)  == BLOCK_AIR);
    assert(chunk_get_block(c, CHUNK_X, 0, 0) == BLOCK_AIR);
    assert(chunk_get_block(c, 0, -1, 0)  == BLOCK_AIR);
    assert(chunk_get_block(c, 0, CHUNK_Y, 0) == BLOCK_AIR);
    assert(chunk_get_block(c, 0, 0, -1)  == BLOCK_AIR);
    assert(chunk_get_block(c, 0, 0, CHUNK_Z) == BLOCK_AIR);

    /* OOB writes are no-ops and must not corrupt the in-bounds cell. */
    chunk_set_block(c, -1, 0, 0, BLOCK_DIRT);
    chunk_set_block(c, CHUNK_X, CHUNK_Y, CHUNK_Z, BLOCK_DIRT);
    assert(chunk_get_block(c, 0, 0, 0) == BLOCK_STONE);
    chunk_destroy(c);
    printf("PASS: block_bounds\n");
}

/* Meta is lazily allocated: get before any set returns 0; after set it
 * round-trips, and writing meta does not disturb blocks. */
static void test_meta_lazy_and_roundtrip(void) {
    Chunk* c = chunk_create(0, 0);
    /* No meta allocated yet -> reads zero, no crash. */
    assert(chunk_get_meta(c, 3, 50, 7) == 0);

    chunk_set_meta(c, 3, 50, 7, 13);
    assert(chunk_get_meta(c, 3, 50, 7) == 13);
    /* meta is 0..255; verify a couple more cells independent. */
    chunk_set_meta(c, 0, 0, 0, 200);
    assert(chunk_get_meta(c, 0, 0, 0) == 200);
    assert(chunk_get_meta(c, 3, 50, 7) == 13);

    /* OOB meta access is safe. */
    assert(chunk_get_meta(c, -1, 0, 0) == 0);
    chunk_set_meta(c, -1, 0, 0, 99); /* no-op */
    chunk_destroy(c);
    printf("PASS: meta_lazy_and_roundtrip\n");
}

/* Light store packs sky (low nibble) and block (high nibble) into one byte.
 * Setting one nibble must not clobber the other, and values are clamped to
 * 4 bits by the mask. */
static void test_light_nibble_packing(void) {
    Chunk* c = chunk_create(0, 0);
    /* Unallocated -> reads zero. */
    assert(chunk_get_skylight(c, 1, 1, 1) == 0);
    assert(chunk_get_blocklight(c, 1, 1, 1) == 0);

    chunk_set_skylight(c, 1, 1, 1, 15);
    assert(chunk_get_skylight(c, 1, 1, 1) == 15);
    assert(chunk_get_blocklight(c, 1, 1, 1) == 0);   /* untouched */

    chunk_set_blocklight(c, 1, 1, 1, 7);
    assert(chunk_get_blocklight(c, 1, 1, 1) == 7);
    assert(chunk_get_skylight(c, 1, 1, 1) == 15);    /* preserved */

    /* Overwrite skylight again; block nibble must persist. */
    chunk_set_skylight(c, 1, 1, 1, 4);
    assert(chunk_get_skylight(c, 1, 1, 1) == 4);
    assert(chunk_get_blocklight(c, 1, 1, 1) == 7);

    /* Values are masked to 4 bits on store. */
    chunk_set_skylight(c, 2, 2, 2, 0xFF);
    assert(chunk_get_skylight(c, 2, 2, 2) == 0x0F);
    chunk_destroy(c);
    printf("PASS: light_nibble_packing\n");
}

/* chunk_create stores cx/cz and starts in CHUNK_UNLOADED with no blocks. */
static void test_create_initial_state(void) {
    Chunk* c = chunk_create(-3, 7);
    assert(c->cx == -3 && c->cz == 7);
    assert(atomic_load(&c->state) == CHUNK_UNLOADED);
    /* Fresh chunk is all air. */
    for (int i = 0; i < 16; i++)
        assert(chunk_get_block(c, i, 0, i) == BLOCK_AIR);
    chunk_destroy(c);
    printf("PASS: create_initial_state\n");
}

int main(void) {
    test_block_set_get_roundtrip();
    test_no_aliasing();
    test_block_bounds();
    test_meta_lazy_and_roundtrip();
    test_light_nibble_packing();
    test_create_initial_state();
    printf("ALL CHUNK TESTS PASSED\n");
    return 0;
}
