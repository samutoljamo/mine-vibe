#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/block.h"

/* block_break_time must be a pure function: same input, same output. */
static void test_deterministic(void) {
    for (BlockID b = 0; b < BLOCK_COUNT; b++) {
        float a = block_break_time(b);
        float c = block_break_time(b);
        assert(a == c);
    }
    /* Out-of-range ids fall back to AIR (instant), and stay deterministic. */
    assert(block_break_time(200) == block_break_time(200));
    printf("PASS: deterministic\n");
}

/* Non-mineable blocks take zero time; everything mineable is positive. */
static void test_instant_and_positive(void) {
    assert(block_break_time(BLOCK_AIR)   == 0.0f);
    assert(block_break_time(BLOCK_WATER) == 0.0f);

    assert(block_break_time(BLOCK_DIRT)   > 0.0f);
    assert(block_break_time(BLOCK_WOOD)   > 0.0f);
    assert(block_break_time(BLOCK_STONE)  > 0.0f);
    assert(block_break_time(BLOCK_GLASS)  > 0.0f);
    printf("PASS: instant_and_positive\n");
}

/* Hardness ordering: dirt < glass < wood < stone, and the four ores match
 * stone (all HARDNESS_HARD). */
static void test_ordering(void) {
    float dirt  = block_break_time(BLOCK_DIRT);
    float glass = block_break_time(BLOCK_GLASS);
    float wood  = block_break_time(BLOCK_WOOD);
    float stone = block_break_time(BLOCK_STONE);

    assert(dirt  < glass);
    assert(glass < wood);
    assert(wood  < stone);

    /* dirt < wood < stone < (obsidian-equivalent / hardest) sentinel chain. */
    assert(dirt < wood && wood < stone);
    assert(stone < BLOCK_BREAK_UNBREAKABLE);

    /* Soft blocks all share the dirt time. */
    assert(block_break_time(BLOCK_SAND)   == dirt);
    assert(block_break_time(BLOCK_GRASS)  == dirt);
    assert(block_break_time(BLOCK_LEAVES) == dirt);
    assert(block_break_time(BLOCK_PATH)   == dirt);

    /* Wood and planks are the same medium tier. */
    assert(block_break_time(BLOCK_PLANKS) == wood);

    /* Ores and cobble are as hard as stone. */
    assert(block_break_time(BLOCK_COBBLE)      == stone);
    assert(block_break_time(BLOCK_COAL_ORE)    == stone);
    assert(block_break_time(BLOCK_IRON_ORE)    == stone);
    assert(block_break_time(BLOCK_GOLD_ORE)    == stone);
    assert(block_break_time(BLOCK_DIAMOND_ORE) == stone);
    printf("PASS: ordering\n");
}

/* Bedrock is the unbreakable sentinel and is the largest of all times. */
static void test_bedrock_unbreakable(void) {
    float bed = block_break_time(BLOCK_BEDROCK);
    assert(bed == BLOCK_BREAK_UNBREAKABLE);
    for (BlockID b = 0; b < BLOCK_COUNT; b++) {
        if (b == BLOCK_BEDROCK) continue;
        assert(block_break_time(b) < bed);
    }
    printf("PASS: bedrock_unbreakable\n");
}

int main(void) {
    test_deterministic();
    test_instant_and_positive();
    test_ordering();
    test_bedrock_unbreakable();
    printf("ALL HARDNESS TESTS PASSED\n");
    return 0;
}
