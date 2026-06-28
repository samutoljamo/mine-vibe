#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/mining.h"
#include "../src/item.h"
#include "../src/block.h"

/* ------------------------------------------------------------------ */
/*  mining_speed_multiplier — correct-tool-for-block speedup           */
/* ------------------------------------------------------------------ */

static void test_no_tool_is_baseline(void) {
    /* A bare hand / block item gives the 1.0 baseline (no speedup). */
    assert(mining_speed_multiplier(item_from_block(BLOCK_DIRT), BLOCK_STONE) == 1.0f);
    assert(mining_speed_multiplier(BLOCK_AIR, BLOCK_STONE) == 1.0f);
    printf("PASS: no_tool_is_baseline\n");
}

static void test_wrong_tool_is_baseline(void) {
    /* Wrong-category tool: no speedup, exactly 1.0. */
    assert(mining_speed_multiplier(ITEM_IRON_SHOVEL, BLOCK_STONE) == 1.0f);  /* shovel on stone */
    assert(mining_speed_multiplier(ITEM_IRON_PICKAXE, BLOCK_WOOD) == 1.0f);  /* pickaxe on wood */
    assert(mining_speed_multiplier(ITEM_IRON_AXE, BLOCK_DIRT) == 1.0f);      /* axe on dirt */
    printf("PASS: wrong_tool_is_baseline\n");
}

static void test_correct_tool_faster_than_hand(void) {
    /* Right tool for the block beats the 1.0 baseline. */
    assert(mining_speed_multiplier(ITEM_WOOD_PICKAXE, BLOCK_STONE) > 1.0f);
    assert(mining_speed_multiplier(ITEM_WOOD_AXE,     BLOCK_WOOD)  > 1.0f);
    assert(mining_speed_multiplier(ITEM_WOOD_SHOVEL,  BLOCK_DIRT)  > 1.0f);
    /* Pickaxe is correct for ores too. */
    assert(mining_speed_multiplier(ITEM_WOOD_PICKAXE, BLOCK_COAL_ORE)    > 1.0f);
    assert(mining_speed_multiplier(ITEM_WOOD_PICKAXE, BLOCK_IRON_ORE)    > 1.0f);
    assert(mining_speed_multiplier(ITEM_WOOD_PICKAXE, BLOCK_DIAMOND_ORE) > 1.0f);
    printf("PASS: correct_tool_faster_than_hand\n");
}

static void test_higher_material_higher_multiplier(void) {
    /* Same kind on the same block: iron > stone > wood. */
    float wood  = mining_speed_multiplier(ITEM_WOOD_PICKAXE,  BLOCK_STONE);
    float stone = mining_speed_multiplier(ITEM_STONE_PICKAXE, BLOCK_STONE);
    float iron  = mining_speed_multiplier(ITEM_IRON_PICKAXE,  BLOCK_STONE);
    assert(wood < stone);
    assert(stone < iron);

    /* Holds for the other tool categories too. */
    assert(mining_speed_multiplier(ITEM_WOOD_AXE, BLOCK_WOOD)
         < mining_speed_multiplier(ITEM_IRON_AXE, BLOCK_WOOD));
    assert(mining_speed_multiplier(ITEM_WOOD_SHOVEL, BLOCK_SAND)
         < mining_speed_multiplier(ITEM_IRON_SHOVEL, BLOCK_SAND));
    printf("PASS: higher_material_higher_multiplier\n");
}

static void test_multiplier_drives_break_time(void) {
    /* Effective break time = block_break_time / multiplier. */
    float base = block_break_time(BLOCK_STONE);
    float mult = mining_speed_multiplier(ITEM_IRON_PICKAXE, BLOCK_STONE);
    float eff  = base / mult;
    assert(eff < base);
    assert(mult > 1.0f);
    /* And it matches the existing tool_break_time helper for matching tools. */
    assert(fabsf(eff - tool_break_time(ITEM_IRON_PICKAXE, BLOCK_STONE)) < 1e-4f);
    printf("PASS: multiplier_drives_break_time\n");
}

/* ------------------------------------------------------------------ */
/*  Harvest-level gating                                               */
/* ------------------------------------------------------------------ */

static void test_tool_harvest_levels_increase_with_material(void) {
    /* Higher material tier => higher harvest level. */
    assert(tool_harvest_level(ITEM_WOOD_PICKAXE)  < tool_harvest_level(ITEM_STONE_PICKAXE));
    assert(tool_harvest_level(ITEM_STONE_PICKAXE) < tool_harvest_level(ITEM_IRON_PICKAXE));
    /* Hand / block item has the lowest level. */
    assert(tool_harvest_level(item_from_block(BLOCK_DIRT)) < tool_harvest_level(ITEM_WOOD_PICKAXE));
    printf("PASS: tool_harvest_levels_increase_with_material\n");
}

static void test_block_required_levels(void) {
    /* Soft blocks need nothing (hand harvests them). */
    assert(block_required_harvest_level(BLOCK_DIRT)  <= tool_harvest_level(item_from_block(BLOCK_AIR)));
    assert(block_required_harvest_level(BLOCK_WOOD)  <= tool_harvest_level(item_from_block(BLOCK_AIR)));
    /* Ordered tiers: stone < iron-ore < diamond-ore requirement. */
    assert(block_required_harvest_level(BLOCK_STONE)      < block_required_harvest_level(BLOCK_IRON_ORE));
    assert(block_required_harvest_level(BLOCK_IRON_ORE)   <= block_required_harvest_level(BLOCK_DIAMOND_ORE));
    printf("PASS: block_required_levels\n");
}

static void test_drops_gating_stone(void) {
    /* Stone needs a wood pickaxe or better to drop. */
    assert(!block_drops_with(item_from_block(BLOCK_DIRT), BLOCK_STONE)); /* hand: no drop */
    assert(!block_drops_with(ITEM_IRON_SHOVEL, BLOCK_STONE));            /* wrong tool: no drop */
    assert(block_drops_with(ITEM_WOOD_PICKAXE,  BLOCK_STONE));           /* wood pick: drops */
    assert(block_drops_with(ITEM_STONE_PICKAXE, BLOCK_STONE));
    assert(block_drops_with(ITEM_IRON_PICKAXE,  BLOCK_STONE));
    printf("PASS: drops_gating_stone\n");
}

static void test_drops_gating_iron_ore(void) {
    /* Iron ore needs a stone pickaxe or better. */
    assert(!block_drops_with(ITEM_WOOD_PICKAXE,  BLOCK_IRON_ORE)); /* too low */
    assert(block_drops_with(ITEM_STONE_PICKAXE,  BLOCK_IRON_ORE)); /* exactly enough */
    assert(block_drops_with(ITEM_IRON_PICKAXE,   BLOCK_IRON_ORE));
    /* Wrong tool kind never drops even at high material. */
    assert(!block_drops_with(ITEM_IRON_AXE, BLOCK_IRON_ORE));
    printf("PASS: drops_gating_iron_ore\n");
}

static void test_drops_gating_diamond_ore(void) {
    /* Diamond ore needs an iron pickaxe (current top tier). */
    assert(!block_drops_with(ITEM_WOOD_PICKAXE,  BLOCK_DIAMOND_ORE));
    assert(!block_drops_with(ITEM_STONE_PICKAXE, BLOCK_DIAMOND_ORE));
    assert(block_drops_with(ITEM_IRON_PICKAXE,   BLOCK_DIAMOND_ORE));
    printf("PASS: drops_gating_diamond_ore\n");
}

static void test_soft_blocks_drop_with_anything(void) {
    /* Dirt/sand/wood drop by hand (no harvest gate). */
    assert(block_drops_with(item_from_block(BLOCK_AIR), BLOCK_DIRT));
    assert(block_drops_with(item_from_block(BLOCK_AIR), BLOCK_SAND));
    assert(block_drops_with(item_from_block(BLOCK_AIR), BLOCK_WOOD));
    assert(block_drops_with(item_from_block(BLOCK_AIR), BLOCK_GRASS));
    printf("PASS: soft_blocks_drop_with_anything\n");
}

int main(void) {
    test_no_tool_is_baseline();
    test_wrong_tool_is_baseline();
    test_correct_tool_faster_than_hand();
    test_higher_material_higher_multiplier();
    test_multiplier_drives_break_time();
    test_tool_harvest_levels_increase_with_material();
    test_block_required_levels();
    test_drops_gating_stone();
    test_drops_gating_iron_ore();
    test_drops_gating_diamond_ore();
    test_soft_blocks_drop_with_anything();
    printf("test_mining: all passed\n");
    return 0;
}
