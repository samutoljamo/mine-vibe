#include <assert.h>
#include <stdio.h>
#include "../src/block.h"

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

int main(void)
{
    test_block_light_absorb_values();
    return 0;
}
