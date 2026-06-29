#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/loot.h"
#include "../src/item.h"

/* A small fixed table with known weights for exact boundary testing.
 * weights: 10, 1, 5  -> total 16; cumulative bounds: [0,10) [10,11) [11,16) */
static const LootEntry TEST_ENTRIES[] = {
    { ITEM_BONE,     1, 1, 10 },
    { ITEM_ARROW,    2, 4,  1 },
    { ITEM_GUNPOWDER,1, 3,  5 },
};
static const LootTable TEST_TABLE = { TEST_ENTRIES, 3 };

static void test_total_weight(void) {
    assert(loot_total_weight(&TEST_TABLE) == 16);
    LootTable empty = { TEST_ENTRIES, 0 };
    assert(loot_total_weight(&empty) == 0);
    assert(loot_total_weight(NULL) == 0);
    printf("PASS: total_weight\n");
}

/* roll 0 -> entry 0; cumulative boundaries pick the right entry. */
static void test_select_index_boundaries(void) {
    assert(loot_select_index(&TEST_TABLE, 0) == 0);   /* start of entry 0 */
    assert(loot_select_index(&TEST_TABLE, 9) == 0);   /* last of entry 0  */
    assert(loot_select_index(&TEST_TABLE, 10) == 1);  /* start of entry 1 */
    assert(loot_select_index(&TEST_TABLE, 11) == 2);  /* start of entry 2 */
    assert(loot_select_index(&TEST_TABLE, 15) == 2);  /* last of entry 2  */
    /* rolls >= total wrap modulo total */
    assert(loot_select_index(&TEST_TABLE, 16) == 0);
    assert(loot_select_index(&TEST_TABLE, 26) == 1);
    printf("PASS: select_index_boundaries\n");
}

static void test_select_index_empty(void) {
    LootTable empty = { TEST_ENTRIES, 0 };
    assert(loot_select_index(&empty, 0) == -1);
    assert(loot_select_index(NULL, 0) == -1);
    printf("PASS: select_index_empty\n");
}

/* Every entry must be selectable as the cumulative weights are swept. */
static void test_all_entries_reachable(void) {
    int seen[3] = {0,0,0};
    for (uint32_t r = 0; r < 16; r++) {
        int idx = loot_select_index(&TEST_TABLE, r);
        assert(idx >= 0 && idx < 3);
        seen[idx] = 1;
    }
    assert(seen[0] && seen[1] && seen[2]);
    printf("PASS: all_entries_reachable\n");
}

/* loot_roll: returned item is from the table and count within [min,max]. */
static void test_roll_within_bounds(void) {
    for (uint32_t seed = 1; seed <= 5000; seed++) {
        uint32_t st = seed;
        uint32_t rng = loot_rng_next(&st);
        LootDrop d = loot_roll(&TEST_TABLE, rng);
        int found = 0;
        for (int i = 0; i < TEST_TABLE.count; i++) {
            if (TEST_TABLE.entries[i].item == d.item) {
                assert(d.count >= TEST_TABLE.entries[i].min_count);
                assert(d.count <= TEST_TABLE.entries[i].max_count);
                found = 1;
                break;
            }
        }
        assert(found);
    }
    printf("PASS: roll_within_bounds\n");
}

/* loot_roll must be a pure function of (table, rng). */
static void test_roll_deterministic(void) {
    for (uint32_t rng = 0; rng < 2000; rng++) {
        LootDrop a = loot_roll(&TEST_TABLE, rng);
        LootDrop b = loot_roll(&TEST_TABLE, rng);
        assert(a.item == b.item);
        assert(a.count == b.count);
    }
    printf("PASS: roll_deterministic\n");
}

static void test_roll_empty(void) {
    LootTable empty = { TEST_ENTRIES, 0 };
    LootDrop d = loot_roll(&empty, 12345);
    assert(d.count == 0);
    printf("PASS: roll_empty\n");
}

/* The PRNG must be deterministic and advance its state. */
static void test_rng_deterministic(void) {
    uint32_t a = 42, b = 42;
    for (int i = 0; i < 100; i++)
        assert(loot_rng_next(&a) == loot_rng_next(&b));
    uint32_t s = 7;
    uint32_t first = loot_rng_next(&s);
    assert(s != 7);          /* state advanced */
    (void)first;
    printf("PASS: rng_deterministic\n");
}

/* Weighted distribution: heavier entries are drawn more often. With weights
 * 10:1:5, bone should dominate and arrow be rarest over many seeded rolls. */
static void test_roll_distribution(void) {
    long bone = 0, arrow = 0, gunp = 0;
    uint32_t st = 0xC0FFEEu;
    for (int i = 0; i < 100000; i++) {
        LootDrop d = loot_roll(&TEST_TABLE, loot_rng_next(&st));
        if (d.item == ITEM_BONE) bone++;
        else if (d.item == ITEM_ARROW) arrow++;
        else if (d.item == ITEM_GUNPOWDER) gunp++;
    }
    printf("  bone=%ld gunpowder=%ld arrow=%ld\n", bone, gunp, arrow);
    assert(bone > gunp);
    assert(gunp > arrow);
    assert(arrow > 0);
    printf("PASS: roll_distribution\n");
}

static void test_dungeon_table_valid(void) {
    assert(LOOT_DUNGEON_CHEST.count > 0);
    assert(loot_total_weight(&LOOT_DUNGEON_CHEST) > 0);
    for (int i = 0; i < LOOT_DUNGEON_CHEST.count; i++) {
        const LootEntry *e = &LOOT_DUNGEON_CHEST.entries[i];
        assert(e->item < ITEM_COUNT);
        assert(e->min_count >= 1);
        assert(e->max_count >= e->min_count);
        assert(e->weight > 0);
    }
    printf("PASS: dungeon_table_valid\n");
}

int main(void) {
    test_total_weight();
    test_select_index_boundaries();
    test_select_index_empty();
    test_all_entries_reachable();
    test_roll_within_bounds();
    test_roll_deterministic();
    test_roll_empty();
    test_rng_deterministic();
    test_roll_distribution();
    test_dungeon_table_valid();
    printf("ALL LOOT TESTS PASSED\n");
    return 0;
}
