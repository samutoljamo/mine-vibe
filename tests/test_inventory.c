#include "../src/inventory.h"
#include <assert.h>
#include <stdio.h>

static void test_init_is_empty(void) {
    Inventory inv;
    inventory_init(&inv);
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        assert(inv.slots[i].block == BLOCK_AIR);
        assert(inv.slots[i].count == 0);
    }
    assert(inv.selected == 0);
}

static void test_add_into_empty(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].block == BLOCK_STONE);
    assert(inv.slots[0].count == 10);
    assert(inv.slots[1].count == 0);
}

static void test_add_overflows_into_next_slot(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 100);
    assert(leftover == 0);
    assert(inv.slots[0].count == 64);
    assert(inv.slots[1].count == 36);
    assert(inv.slots[1].block == BLOCK_STONE);
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
    assert(inv.slots[1].block == BLOCK_STONE);
}

static void test_add_returns_leftover_when_full(void) {
    Inventory inv; inventory_init(&inv);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        inventory_add(&inv, BLOCK_DIRT, 64);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 5);
    assert(leftover == 5);
    /* No slot mutated by the failed add */
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        assert(inv.slots[i].block == BLOCK_DIRT);
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
    assert(inv.slots[0].block == BLOCK_AIR);
    assert(inv.slots[0].count == 0);
}

static void test_consume_empty_returns_false(void) {
    Inventory inv; inventory_init(&inv);
    assert(!inventory_consume(&inv, 0));
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
    printf("test_inventory: all passed\n");
    return 0;
}
