#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/ore.h"
#include "../src/block.h"

#define SEED 1337

static int is_ore(BlockID b) {
    return b == BLOCK_COAL_ORE || b == BLOCK_IRON_ORE ||
           b == BLOCK_GOLD_ORE || b == BLOCK_DIAMOND_ORE;
}

/* ore_select must be a pure function of its arguments. */
static void test_deterministic(void) {
    for (int i = 0; i < 1000; i++) {
        int x = i * 13, y = i % 64, z = i * 7;
        BlockID a = ore_select(x, y, z, 80, SEED);
        BlockID b = ore_select(x, y, z, 80, SEED);
        assert(a == b);
    }
    printf("PASS: deterministic\n");
}

/* Only ever returns stone or one of the four ores. */
static void test_returns_only_stone_or_ore(void) {
    for (int x = 0; x < 64; x++)
        for (int y = 1; y < 128; y++)
            for (int z = 0; z < 16; z++) {
                BlockID b = ore_select(x, y, z, 130, SEED);
                assert(b == BLOCK_STONE || is_ore(b));
            }
    printf("PASS: returns_only_stone_or_ore\n");
}

/* No ore within ORE_MIN_DEPTH of the surface (would poke through dirt). */
static void test_no_ore_near_surface(void) {
    for (int x = 0; x < 128; x++)
        for (int z = 0; z < 128; z++) {
            int surface = 90;
            for (int d = 0; d < ORE_MIN_DEPTH; d++) {
                BlockID b = ore_select(x, surface - d, z, surface, SEED);
                assert(b == BLOCK_STONE);
            }
        }
    printf("PASS: no_ore_near_surface\n");
}

/* Depth gating: diamond and gold never appear above their max Y. */
static void test_depth_gating(void) {
    for (int x = 0; x < 200; x++)
        for (int z = 0; z < 200; z++)
            for (int y = 1; y < 200; y++) {
                BlockID b = ore_select(x, y, z, 220, SEED);
                if (y > ORE_DIAMOND_MAX_Y) assert(b != BLOCK_DIAMOND_ORE);
                if (y > ORE_GOLD_MAX_Y)    assert(b != BLOCK_GOLD_ORE);
                if (y > ORE_IRON_MAX_Y)    assert(b != BLOCK_IRON_ORE);
            }
    printf("PASS: depth_gating\n");
}

/* Rarity ordering coal > iron > gold > diamond, sampled deep where all
 * four are eligible. Also: ores stay rare (stone dominates). */
static void test_rarity_ordering(void) {
    long coal = 0, iron = 0, gold = 0, diamond = 0, stone = 0, total = 0;
    for (int x = 0; x < 300; x++)
        for (int z = 0; z < 300; z++)
            for (int y = 4; y <= ORE_DIAMOND_MAX_Y; y++) {
                BlockID b = ore_select(x, y, z, 200, SEED);
                total++;
                switch (b) {
                    case BLOCK_COAL_ORE:    coal++;    break;
                    case BLOCK_IRON_ORE:    iron++;    break;
                    case BLOCK_GOLD_ORE:    gold++;    break;
                    case BLOCK_DIAMOND_ORE: diamond++; break;
                    default:                stone++;   break;
                }
            }
    printf("  coal=%ld iron=%ld gold=%ld diamond=%ld stone=%ld total=%ld\n",
           coal, iron, gold, diamond, stone, total);
    assert(coal > iron);
    assert(iron > gold);
    assert(gold > diamond);
    assert(diamond > 0);                 /* diamonds do spawn deep        */
    assert(stone > total * 9 / 10);      /* ores rare: >90% stays stone   */
    printf("PASS: rarity_ordering\n");
}

int main(void) {
    test_deterministic();
    test_returns_only_stone_or_ore();
    test_no_ore_near_surface();
    test_depth_gating();
    test_rarity_ordering();
    printf("ALL ORE TESTS PASSED\n");
    return 0;
}
