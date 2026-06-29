#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/item.h"
#include "../src/block.h"

/* ------------------------------------------------------------------ */
/*  Item / block id mapping                                            */
/* ------------------------------------------------------------------ */

static void test_block_ids_map_directly(void) {
    /* Low item ids ARE block ids: placeable blocks live in [0, BLOCK_COUNT). */
    for (BlockID b = 0; b < BLOCK_COUNT; b++) {
        ItemId id = item_from_block(b);
        assert((BlockID)id == b);
        assert(item_is_block(id));
        assert(!item_is_tool(id));
        assert(item_as_block(id) == b);
    }
    printf("PASS: block_ids_map_directly\n");
}

static void test_tools_are_above_block_range(void) {
    ItemId tools[] = {
        ITEM_WOOD_PICKAXE,    ITEM_STONE_PICKAXE,  ITEM_IRON_PICKAXE,  ITEM_DIAMOND_PICKAXE,
        ITEM_WOOD_AXE,        ITEM_STONE_AXE,      ITEM_IRON_AXE,      ITEM_DIAMOND_AXE,
        ITEM_WOOD_SHOVEL,     ITEM_STONE_SHOVEL,   ITEM_IRON_SHOVEL,   ITEM_DIAMOND_SHOVEL,
        ITEM_WOOD_SWORD,      ITEM_STONE_SWORD,    ITEM_IRON_SWORD,    ITEM_DIAMOND_SWORD,
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        assert(tools[i] >= BLOCK_COUNT);
        assert(item_is_tool(tools[i]));
        assert(!item_is_block(tools[i]));
    }
    /* 3 mining kinds × 4 tiers + 4 swords = 16 tools, contiguous range. */
    assert(ITEM_TOOL_FIRST == BLOCK_COUNT);
    assert(ITEM_TOOL_COUNT == 16);
    assert(ITEM_TOOL_LAST == ITEM_TOOL_FIRST + ITEM_TOOL_COUNT - 1);
    printf("PASS: tools_are_above_block_range\n");
}

static void test_is_tool_classification(void) {
    assert(!item_is_tool(item_from_block(BLOCK_STONE)));
    assert(!item_is_tool(item_from_block(BLOCK_DIRT)));
    assert(item_is_tool(ITEM_IRON_PICKAXE));
    assert(item_is_tool(ITEM_WOOD_SHOVEL));
    /* Out-of-range ids classify as neither tool nor block. */
    assert(!item_is_tool((ItemId)(ITEM_TOOL_LAST + 1)));
    assert(!item_is_block((ItemId)(ITEM_TOOL_LAST + 1)));
    printf("PASS: is_tool_classification\n");
}

static void test_tool_metadata(void) {
    const ItemDef* p = item_get_def(ITEM_IRON_PICKAXE);
    assert(p->is_tool);
    assert(p->tool_kind == TOOL_PICKAXE);
    assert(p->material == MATERIAL_IRON);
    assert(p->max_durability > 0);
    assert(p->name != NULL);

    const ItemDef* a = item_get_def(ITEM_WOOD_AXE);
    assert(a->tool_kind == TOOL_AXE);
    assert(a->material == MATERIAL_WOOD);

    const ItemDef* s = item_get_def(ITEM_STONE_SHOVEL);
    assert(s->tool_kind == TOOL_SHOVEL);
    assert(s->material == MATERIAL_STONE);

    /* Block items are not tools. */
    const ItemDef* blk = item_get_def(item_from_block(BLOCK_STONE));
    assert(!blk->is_tool);
    printf("PASS: tool_metadata\n");
}

/* ------------------------------------------------------------------ */
/*  Durability                                                         */
/* ------------------------------------------------------------------ */

static void test_durability_values_increase_with_tier(void) {
    uint16_t wood  = item_get_def(ITEM_WOOD_PICKAXE)->max_durability;
    uint16_t stone = item_get_def(ITEM_STONE_PICKAXE)->max_durability;
    uint16_t iron  = item_get_def(ITEM_IRON_PICKAXE)->max_durability;
    assert(wood < stone);
    assert(stone < iron);
    printf("PASS: durability_values_increase_with_tier\n");
}

/* ------------------------------------------------------------------ */
/*  tool_break_time — pure speed math                                  */
/* ------------------------------------------------------------------ */

static void test_matching_tool_faster_than_wrong_than_hand(void) {
    /* Stone is a pickaxe-category block. */
    float hand    = block_break_time(BLOCK_STONE);
    float pick    = tool_break_time(ITEM_IRON_PICKAXE, BLOCK_STONE);   /* matching */
    float shovel  = tool_break_time(ITEM_IRON_SHOVEL,  BLOCK_STONE);   /* wrong    */

    /* Matching tool is strictly faster than a wrong tool, which is no
     * faster than (== ) the bare hand. */
    assert(pick < shovel);
    assert(shovel <= hand + 1e-6f);
    /* And the matching tool beats the hand outright. */
    assert(pick < hand);
    printf("PASS: matching_tool_faster_than_wrong_than_hand\n");
}

static void test_tier_ordering_iron_faster_than_stone_than_wood(void) {
    float wood  = tool_break_time(ITEM_WOOD_PICKAXE,  BLOCK_STONE);
    float stone = tool_break_time(ITEM_STONE_PICKAXE, BLOCK_STONE);
    float iron  = tool_break_time(ITEM_IRON_PICKAXE,  BLOCK_STONE);
    /* Higher tier => less time. iron < stone < wood. */
    assert(iron < stone);
    assert(stone < wood);
    /* All matching-tool times still beat the bare hand. */
    assert(wood < block_break_time(BLOCK_STONE));
    printf("PASS: tier_ordering_iron_faster_than_stone_than_wood\n");
}

static void test_category_matching(void) {
    /* Axe speeds up wood/planks; pickaxe does not. */
    assert(tool_break_time(ITEM_IRON_AXE,     BLOCK_WOOD)
         < tool_break_time(ITEM_IRON_PICKAXE, BLOCK_WOOD));
    /* Shovel speeds up dirt/sand/grass/path; axe does not. */
    assert(tool_break_time(ITEM_IRON_SHOVEL,  BLOCK_DIRT)
         < tool_break_time(ITEM_IRON_AXE,     BLOCK_DIRT));
    /* Pickaxe speeds up ores. */
    assert(tool_break_time(ITEM_IRON_PICKAXE, BLOCK_IRON_ORE)
         < block_break_time(BLOCK_IRON_ORE));
    printf("PASS: category_matching\n");
}

static void test_break_time_passing_a_block_item_is_base(void) {
    /* A non-tool item (a block) gives no mining bonus: base hand time. */
    float t = tool_break_time(item_from_block(BLOCK_DIRT), BLOCK_STONE);
    assert(fabsf(t - block_break_time(BLOCK_STONE)) < 1e-6f);
    printf("PASS: break_time_passing_a_block_item_is_base\n");
}

static void test_unbreakable_stays_unbreakable(void) {
    /* No tool can break bedrock. */
    float t = tool_break_time(ITEM_IRON_PICKAXE, BLOCK_BEDROCK);
    assert(t >= BLOCK_BREAK_UNBREAKABLE);
    printf("PASS: unbreakable_stays_unbreakable\n");
}

/* ------------------------------------------------------------------ */
/*  Food + new loot materials                                          */
/* ------------------------------------------------------------------ */

static void test_food_ids_distinct_and_in_range(void) {
    ItemId foods[] = {
        ITEM_RAW_PORK, ITEM_COOKED_PORK,
        ITEM_RAW_BEEF, ITEM_COOKED_BEEF,
        ITEM_RAW_CHICKEN, ITEM_COOKED_CHICKEN,
        ITEM_ROTTEN_FLESH,
    };
    size_t n = sizeof(foods) / sizeof(foods[0]);
    for (size_t i = 0; i < n; i++) {
        /* In range and classified as food (not block/tool/material/armour). */
        assert(foods[i] < ITEM_COUNT);
        assert(item_is_food(foods[i]));
        assert(!item_is_block(foods[i]));
        assert(!item_is_tool(foods[i]));
        assert(!item_is_material(foods[i]));
        assert(!item_is_armor(foods[i]));
        /* Pairwise distinct ids. */
        for (size_t j = i + 1; j < n; j++)
            assert(foods[i] != foods[j]);
        /* Named def + edible. */
        const ItemDef* d = item_get_def(foods[i]);
        assert(d->name != NULL);
        assert(d->hunger_restore > 0);
        assert(item_hunger_restore(foods[i]) == d->hunger_restore);
    }
    printf("PASS: food_ids_distinct_and_in_range\n");
}

static void test_cooked_food_restores_more_than_raw(void) {
    assert(item_hunger_restore(ITEM_COOKED_PORK)    > item_hunger_restore(ITEM_RAW_PORK));
    assert(item_hunger_restore(ITEM_COOKED_BEEF)    > item_hunger_restore(ITEM_RAW_BEEF));
    assert(item_hunger_restore(ITEM_COOKED_CHICKEN) > item_hunger_restore(ITEM_RAW_CHICKEN));
    /* Rotten flesh is edible but a poor food. */
    assert(item_hunger_restore(ITEM_ROTTEN_FLESH) > 0);
    assert(item_hunger_restore(ITEM_ROTTEN_FLESH) <= item_hunger_restore(ITEM_RAW_BEEF));
    printf("PASS: cooked_food_restores_more_than_raw\n");
}

static void test_non_food_has_no_hunger(void) {
    assert(item_hunger_restore(ITEM_IRON_PICKAXE) == 0);
    assert(item_hunger_restore(item_from_block(BLOCK_STONE)) == 0);
    assert(item_hunger_restore(ITEM_BONE) == 0);
    assert(item_hunger_restore(ITEM_FEATHER) == 0);
    assert(item_hunger_restore(ITEM_ARROW) == 0);
    assert(item_hunger_restore(ITEM_GUNPOWDER) == 0);
    assert(!item_is_food(ITEM_BONE));
    printf("PASS: non_food_has_no_hunger\n");
}

static void test_new_materials_distinct_and_named(void) {
    ItemId mats[] = { ITEM_FEATHER, ITEM_BONE, ITEM_ARROW, ITEM_GUNPOWDER };
    size_t n = sizeof(mats) / sizeof(mats[0]);
    for (size_t i = 0; i < n; i++) {
        assert(mats[i] < ITEM_COUNT);
        assert(item_is_material(mats[i]));
        assert(!item_is_food(mats[i]));
        assert(item_get_def(mats[i])->name != NULL);
        for (size_t j = i + 1; j < n; j++)
            assert(mats[i] != mats[j]);
    }
    printf("PASS: new_materials_distinct_and_named\n");
}

int main(void) {
    test_block_ids_map_directly();
    test_tools_are_above_block_range();
    test_is_tool_classification();
    test_tool_metadata();
    test_durability_values_increase_with_tier();
    test_matching_tool_faster_than_wrong_than_hand();
    test_tier_ordering_iron_faster_than_stone_than_wood();
    test_category_matching();
    test_break_time_passing_a_block_item_is_base();
    test_unbreakable_stays_unbreakable();
    test_food_ids_distinct_and_in_range();
    test_cooked_food_restores_more_than_raw();
    test_non_food_has_no_hunger();
    test_new_materials_distinct_and_named();
    printf("test_item: all passed\n");
    return 0;
}
