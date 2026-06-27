#include "inventory.h"
#include <string.h>

_Static_assert(BLOCK_AIR == 0,
    "inventory_init relies on BLOCK_AIR == 0 (memset zero = empty inventory)");
_Static_assert(INVENTORY_STACK_MAX > 0 && INVENTORY_STACK_MAX <= UINT8_MAX,
    "INVENTORY_STACK_MAX must fit in a uint8_t to avoid silent overflow in inventory_add");

void inventory_init(Inventory* inv) {
    memset(inv, 0, sizeof(*inv));   /* item == BLOCK_AIR == 0, count == 0 */
    inv->selected = 0;
}

uint8_t inventory_add(Inventory* inv, BlockID block, uint8_t count) {
    return inventory_add_item(inv, item_from_block(block), count);
}

uint8_t inventory_add_item(Inventory* inv, ItemId item, uint8_t count) {
    /* Load-bearing guard, NOT just a fast path: removing this would let
     * pass 1 below "top up" empty slots whose item field is BLOCK_AIR,
     * breaking the (item == BLOCK_AIR) <=> (count == 0) invariant. */
    if (item == (ItemId)BLOCK_AIR || count == 0) return count;

    uint8_t stack_max = item_stack_max(item);
    if (stack_max > INVENTORY_STACK_MAX) stack_max = INVENTORY_STACK_MAX;

    /* Pass 1: top up matching non-full stacks (no-op for unstackable tools). */
    if (stack_max > 1) {
        for (int i = 0; i < INVENTORY_SLOTS && count > 0; i++) {
            if (inv->slots[i].item != item || inv->slots[i].count == 0) continue;
            uint8_t room = (uint8_t)(stack_max - inv->slots[i].count);
            uint8_t take = count < room ? count : room;
            inv->slots[i].count = (uint8_t)(inv->slots[i].count + take);
            count = (uint8_t)(count - take);
        }
    }

    /* Pass 2: fill empty slots. */
    for (int i = 0; i < INVENTORY_SLOTS && count > 0; i++) {
        if (inv->slots[i].count != 0) continue;
        uint8_t take = count < stack_max ? count : stack_max;
        inv->slots[i].item       = item;
        inv->slots[i].count      = take;
        inv->slots[i].durability = item_is_tool(item)
                                  ? item_get_def(item)->max_durability : 0;
        count = (uint8_t)(count - take);
    }
    return count;
}

bool inventory_consume(Inventory* inv, int slot) {
    if (slot < 0 || slot >= INVENTORY_SLOTS) return false;
    if (inv->slots[slot].count == 0) return false;
    inv->slots[slot].count--;
    if (inv->slots[slot].count == 0) {
        inv->slots[slot].item       = (ItemId)BLOCK_AIR;
        inv->slots[slot].durability = 0;
    }
    return true;
}

bool inventory_damage_tool(Inventory* inv, int slot) {
    if (slot < 0 || slot >= INVENTORY_SLOTS) return false;
    InventorySlot* s = &inv->slots[slot];
    if (s->count == 0 || !item_is_tool(s->item)) return false;
    if (s->durability > 0) s->durability--;
    if (s->durability == 0) {
        /* Tool broke: remove it (tools are unstackable, so clear the slot). */
        s->item  = (ItemId)BLOCK_AIR;
        s->count = 0;
        return true;
    }
    return false;
}
