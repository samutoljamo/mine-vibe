#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/container.h"
#include "../src/inventory.h"
#include "../src/item.h"
#include "../src/block.h"

/* container_init zeroes every slot to empty (BLOCK_AIR / count 0). */
static void test_init_empty(void) {
    Container c;
    container_init(&c);
    assert(container_slot_count(&c) == CHEST_SLOTS);
    for (int i = 0; i < CHEST_SLOTS; i++) {
        assert(c.slots[i].item == (ItemId)BLOCK_AIR);
        assert(c.slots[i].count == 0);
    }
    assert(container_count(&c, BLOCK_STONE) == 0);
    printf("PASS: init_empty\n");
}

/* Adding into an empty container drops the items into the first empty slot. */
static void test_add_basic(void) {
    Container c;
    container_init(&c);
    uint8_t left = container_add(&c, BLOCK_STONE, 10);
    assert(left == 0);
    assert(c.slots[0].item == BLOCK_STONE);
    assert(c.slots[0].count == 10);
    assert(container_count(&c, BLOCK_STONE) == 10);
    printf("PASS: add_basic\n");
}

/* Adding tops up an existing matching stack before opening a new slot. */
static void test_add_stacks_existing(void) {
    Container c;
    container_init(&c);
    container_add(&c, BLOCK_STONE, 10);
    uint8_t left = container_add(&c, BLOCK_STONE, 5);
    assert(left == 0);
    assert(c.slots[0].count == 15);
    assert(c.slots[1].count == 0);          /* still only one slot used */
    assert(container_count(&c, BLOCK_STONE) == 15);
    printf("PASS: add_stacks_existing\n");
}

/* Overflowing a stack spills the remainder into the next empty slot. */
static void test_add_overflow_to_new_slot(void) {
    Container c;
    container_init(&c);
    /* Fill slot 0 to the max, then add more so it must open slot 1. */
    uint8_t left = container_add(&c, BLOCK_DIRT, CONTAINER_STACK_MAX);
    assert(left == 0);
    assert(c.slots[0].count == CONTAINER_STACK_MAX);
    left = container_add(&c, BLOCK_DIRT, 3);
    assert(left == 0);
    assert(c.slots[0].count == CONTAINER_STACK_MAX);
    assert(c.slots[1].item == BLOCK_DIRT);
    assert(c.slots[1].count == 3);
    assert(container_count(&c, BLOCK_DIRT) == CONTAINER_STACK_MAX + 3);
    printf("PASS: add_overflow_to_new_slot\n");
}

/* Max stack is respected: a count over the max splits across slots. */
static void test_max_stack_respected(void) {
    Container c;
    container_init(&c);
    /* Add max+1: max in slot 0, 1 in slot 1. count is uint8 so use max value. */
    uint8_t left = container_add(&c, BLOCK_STONE, CONTAINER_STACK_MAX);
    assert(left == 0);
    left = container_add(&c, BLOCK_STONE, 1);
    assert(left == 0);
    assert(c.slots[0].count == CONTAINER_STACK_MAX);
    assert(c.slots[1].count == 1);
    printf("PASS: max_stack_respected\n");
}

/* When every slot is full of the same item, extra returns as leftover. */
static void test_full_container_leftover(void) {
    Container c;
    container_init(&c);
    /* Fill all slots with stone to the brim. */
    for (int i = 0; i < CHEST_SLOTS; i++) {
        uint8_t left = container_add(&c, BLOCK_STONE, CONTAINER_STACK_MAX);
        assert(left == 0);
    }
    /* Container is now completely full; further adds bounce back as leftover. */
    uint8_t left = container_add(&c, BLOCK_STONE, 5);
    assert(left == 5);
    /* A different item also has nowhere to go. */
    left = container_add(&c, BLOCK_DIRT, 7);
    assert(left == 7);
    printf("PASS: full_container_leftover\n");
}

/* A partially-full container accepts what it can and returns the rest. */
static void test_partial_full_leftover(void) {
    Container c;
    container_init(&c);
    /* Fill all but the last slot completely, last slot to max-2. */
    for (int i = 0; i < CHEST_SLOTS - 1; i++)
        container_add(&c, BLOCK_STONE, CONTAINER_STACK_MAX);
    /* Last slot: leave room for 2 of dirt's own stack. */
    /* Put max-2 dirt in the final slot manually via add (it goes to slot N-1). */
    container_add(&c, BLOCK_DIRT, CONTAINER_STACK_MAX - 2);
    /* Now only 2 units of room remain (in the dirt slot). Add 5 dirt: 2 fit. */
    uint8_t left = container_add(&c, BLOCK_DIRT, 5);
    assert(left == 3);
    assert(container_count(&c, BLOCK_DIRT) == CONTAINER_STACK_MAX);
    printf("PASS: partial_full_leftover\n");
}

/* container_take_slot pulls up to `count` from a slot and empties it when drained. */
static void test_take_slot(void) {
    Container c;
    container_init(&c);
    container_add(&c, BLOCK_STONE, 20);
    ItemStack got = container_take_slot(&c, 0, 5);
    assert(got.item == BLOCK_STONE);
    assert(got.count == 5);
    assert(c.slots[0].count == 15);

    /* Asking for more than present takes only what is there and empties slot. */
    got = container_take_slot(&c, 0, 100);
    assert(got.item == BLOCK_STONE);
    assert(got.count == 15);
    assert(c.slots[0].item == (ItemId)BLOCK_AIR);
    assert(c.slots[0].count == 0);

    /* Taking from an empty slot yields an empty stack. */
    got = container_take_slot(&c, 0, 5);
    assert(got.item == (ItemId)BLOCK_AIR);
    assert(got.count == 0);

    /* Out-of-range slot is safe and yields empty. */
    got = container_take_slot(&c, -1, 5);
    assert(got.count == 0);
    got = container_take_slot(&c, CHEST_SLOTS, 5);
    assert(got.count == 0);
    printf("PASS: take_slot\n");
}

/* container_remove drains `count` across matching slots atomically. */
static void test_remove(void) {
    Container c;
    container_init(&c);
    container_add(&c, BLOCK_STONE, CONTAINER_STACK_MAX);
    container_add(&c, BLOCK_STONE, 10);       /* spills into slot 1 */
    assert(container_count(&c, BLOCK_STONE) == CONTAINER_STACK_MAX + 10);

    /* Remove more than one slot's worth: drains across slots. */
    bool ok = container_remove(&c, BLOCK_STONE, CONTAINER_STACK_MAX + 5);
    assert(ok);
    assert(container_count(&c, BLOCK_STONE) == 5);

    /* Removing more than present fails and leaves the container untouched. */
    ok = container_remove(&c, BLOCK_STONE, 100);
    assert(!ok);
    assert(container_count(&c, BLOCK_STONE) == 5);
    printf("PASS: remove\n");
}

/* Move a slot from a container into an inventory, reusing inventory stacking. */
static void test_transfer_to_inventory(void) {
    Container c;
    Inventory inv;
    container_init(&c);
    inventory_init(&inv);

    container_add(&c, BLOCK_STONE, 30);
    uint8_t moved = container_transfer_to_inventory(&c, 0, &inv, 30);
    assert(moved == 30);
    assert(inventory_count(&inv, BLOCK_STONE) == 30);
    assert(c.slots[0].count == 0);
    assert(c.slots[0].item == (ItemId)BLOCK_AIR);

    /* If inventory is full, only what fits moves; the rest stays in container. */
    Inventory full;
    inventory_init(&full);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        inventory_add(&full, BLOCK_DIRT, INVENTORY_STACK_MAX);
    container_add(&c, BLOCK_STONE, 10);       /* slot 0 has 10 stone */
    uint8_t before = container_count(&c, BLOCK_STONE);
    moved = container_transfer_to_inventory(&c, 0, &full, 10);
    assert(moved == 0);                       /* nothing fits */
    assert(container_count(&c, BLOCK_STONE) == before);
    printf("PASS: transfer_to_inventory\n");
}

/* Move from inventory slot into the container, reusing container stacking. */
static void test_transfer_from_inventory(void) {
    Container c;
    Inventory inv;
    container_init(&c);
    inventory_init(&inv);

    inventory_add(&inv, BLOCK_STONE, 40);     /* lands in inventory slot 0 */
    uint8_t moved = container_transfer_from_inventory(&inv, 0, &c, 40);
    assert(moved == 40);
    assert(container_count(&c, BLOCK_STONE) == 40);
    assert(inventory_count(&inv, BLOCK_STONE) == 0);

    /* Fill the container completely, then a transfer in moves nothing. */
    for (int i = 0; i < CHEST_SLOTS; i++)
        container_add(&c, BLOCK_DIRT, CONTAINER_STACK_MAX);
    inventory_add(&inv, BLOCK_COBBLE, 5);
    uint8_t cobble_before = inventory_count(&inv, BLOCK_COBBLE);
    /* find the cobble slot */
    int cs = -1;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (inv.slots[i].item == BLOCK_COBBLE) { cs = i; break; }
    assert(cs >= 0);
    moved = container_transfer_from_inventory(&inv, cs, &c, 5);
    assert(moved == 0);
    assert(inventory_count(&inv, BLOCK_COBBLE) == cobble_before);
    printf("PASS: transfer_from_inventory\n");
}

/* container_clear resets to empty. */
static void test_clear(void) {
    Container c;
    container_init(&c);
    container_add(&c, BLOCK_STONE, 50);
    container_clear(&c);
    assert(container_count(&c, BLOCK_STONE) == 0);
    for (int i = 0; i < CHEST_SLOTS; i++)
        assert(c.slots[i].count == 0);
    printf("PASS: clear\n");
}

int main(void) {
    test_init_empty();
    test_add_basic();
    test_add_stacks_existing();
    test_add_overflow_to_new_slot();
    test_max_stack_respected();
    test_full_container_leftover();
    test_partial_full_leftover();
    test_take_slot();
    test_remove();
    test_transfer_to_inventory();
    test_transfer_from_inventory();
    test_clear();
    printf("ALL CONTAINER TESTS PASSED\n");
    return 0;
}
