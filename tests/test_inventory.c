#include "../src/inventory.h"
#include "../src/item.h"
#include <assert.h>
#include <stdio.h>

static void test_init_is_empty(void) {
    Inventory inv;
    inventory_init(&inv);
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        assert(inv.slots[i].item == BLOCK_AIR);
        assert(inv.slots[i].count == 0);
    }
    assert(inv.selected == 0);
}

static void test_add_into_empty(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].item == BLOCK_STONE);
    assert(inv.slots[0].count == 10);
    assert(inv.slots[1].count == 0);
}

static void test_add_overflows_into_next_slot(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 100);
    assert(leftover == 0);
    assert(inv.slots[0].count == 64);
    assert(inv.slots[1].count == 36);
    assert(inv.slots[1].item == BLOCK_STONE);
}

static void test_add_tops_up_matching_stack(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 30);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].count == 40);
    assert(inv.slots[1].count == 0);
}

static void test_add_skips_full_matching_stack(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 64);          /* slot 0 full */
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].count == 64);
    assert(inv.slots[1].count == 10);
    assert(inv.slots[1].item == BLOCK_STONE);
}

static void test_add_returns_leftover_when_full(void) {
    Inventory inv; inventory_init(&inv);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        inventory_add(&inv, BLOCK_DIRT, 64);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 5);
    assert(leftover == 5);
    /* No slot mutated by the failed add */
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        assert(inv.slots[i].item == BLOCK_DIRT);
        assert(inv.slots[i].count == 64);
    }
}

static void test_consume_decrements(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 5);
    assert(inventory_consume(&inv, 0));
    assert(inv.slots[0].count == 4);
}

static void test_consume_empties_slot_at_zero(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 1);
    assert(inventory_consume(&inv, 0));
    assert(inv.slots[0].item == BLOCK_AIR);
    assert(inv.slots[0].count == 0);
}

static void test_consume_empty_returns_false(void) {
    Inventory inv; inventory_init(&inv);
    assert(!inventory_consume(&inv, 0));
}

static void test_consume_out_of_range_returns_false(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 5);   /* slot 0 non-empty so we know it's the bounds check, not the empty check */
    assert(!inventory_consume(&inv, -1));
    assert(!inventory_consume(&inv, INVENTORY_SLOTS));
    /* Slot 0 must not have been mutated. */
    assert(inv.slots[0].count == 5);
}

static void test_add_air_is_noop(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_AIR, 5);
    assert(leftover == 5);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        assert(inv.slots[i].count == 0 && inv.slots[i].item == BLOCK_AIR);
}

static void test_add_zero_count_is_noop(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 0);
    assert(leftover == 0);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        assert(inv.slots[i].count == 0);
}

static void test_add_only_tops_up_matching_block(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 30);    /* slot 0 = stone/30 */
    inventory_add(&inv, BLOCK_DIRT,  30);    /* slot 1 = dirt/30  */
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].item == BLOCK_STONE && inv.slots[0].count == 40);
    assert(inv.slots[1].item == BLOCK_DIRT  && inv.slots[1].count == 30);
}

static void test_add_top_up_then_overflow_into_empty(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 60);    /* slot 0 = stone/60 */
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);  /* fills 4 into slot 0, 6 into slot 1 */
    assert(leftover == 0);
    assert(inv.slots[0].count == 64);
    assert(inv.slots[1].item == BLOCK_STONE && inv.slots[1].count == 6);
}

/* --- Tool items: unstackable, durability tracked + worn down --- */

static void test_tool_lands_at_full_durability(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add_item(&inv, ITEM_IRON_PICKAXE, 1);
    assert(leftover == 0);
    assert(inv.slots[0].item == ITEM_IRON_PICKAXE);
    assert(inv.slots[0].count == 1);
    assert(inv.slots[0].durability == item_get_def(ITEM_IRON_PICKAXE)->max_durability);
}

static void test_tools_do_not_stack(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add_item(&inv, ITEM_WOOD_AXE, 1);
    inventory_add_item(&inv, ITEM_WOOD_AXE, 1);
    /* Each tool occupies its own slot, count stays 1. */
    assert(inv.slots[0].item == ITEM_WOOD_AXE && inv.slots[0].count == 1);
    assert(inv.slots[1].item == ITEM_WOOD_AXE && inv.slots[1].count == 1);
}

static void test_damage_tool_decrements_then_breaks(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add_item(&inv, ITEM_WOOD_PICKAXE, 1);
    uint16_t start = inv.slots[0].durability;
    bool broke = inventory_damage_tool(&inv, 0);
    assert(!broke);
    assert(inv.slots[0].durability == start - 1);
    /* Wear it to 0 — slot empties and reports broken. */
    while (inv.slots[0].count != 0) {
        broke = inventory_damage_tool(&inv, 0);
    }
    assert(broke);
    assert(inv.slots[0].item == BLOCK_AIR && inv.slots[0].count == 0);
}

static void test_damage_block_slot_is_noop(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 5);
    assert(!inventory_damage_tool(&inv, 0));
    assert(inv.slots[0].count == 5);   /* untouched */
}

int main(void) {
    test_init_is_empty();
    test_add_into_empty();
    test_add_overflows_into_next_slot();
    test_add_tops_up_matching_stack();
    test_add_skips_full_matching_stack();
    test_add_returns_leftover_when_full();
    test_consume_decrements();
    test_consume_empties_slot_at_zero();
    test_consume_empty_returns_false();
    test_consume_out_of_range_returns_false();
    test_add_air_is_noop();
    test_add_zero_count_is_noop();
    test_add_only_tops_up_matching_block();
    test_add_top_up_then_overflow_into_empty();
    test_tool_lands_at_full_durability();
    test_tools_do_not_stack();
    test_damage_tool_decrements_then_breaks();
    test_damage_block_slot_is_noop();
    printf("test_inventory: all passed\n");
    return 0;
}
