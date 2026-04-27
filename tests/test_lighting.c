#include <assert.h>
#include <stdio.h>
#include "../src/block.h"
#include "../src/chunk.h"

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

    chunk_destroy(c);
    printf("PASS: test_chunk_light_lazy_alloc_and_pack\n");
}

int main(void)
{
    test_block_light_absorb_values();
    test_chunk_light_lazy_alloc_and_pack();
    return 0;
}
