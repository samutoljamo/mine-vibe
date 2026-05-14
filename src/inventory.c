#include "inventory.h"
#include <string.h>

_Static_assert(BLOCK_AIR == 0,
    "inventory_init relies on BLOCK_AIR == 0 (memset zero = empty inventory)");
_Static_assert(INVENTORY_STACK_MAX > 0 && INVENTORY_STACK_MAX <= UINT8_MAX,
    "INVENTORY_STACK_MAX must fit in a uint8_t to avoid silent overflow in inventory_add");

void inventory_init(Inventory* inv) {
    memset(inv, 0, sizeof(*inv));   /* BLOCK_AIR == 0, count == 0 */
    inv->selected = 0;
}

uint8_t inventory_add(Inventory* inv, BlockID block, uint8_t count) {
    /* Load-bearing guard, NOT just a fast path: removing this would let
     * pass 1 below "top up" empty slots whose block field is BLOCK_AIR,
     * breaking the (block == BLOCK_AIR) <=> (count == 0) invariant. */
    if (block == BLOCK_AIR || count == 0) return count;

    /* Pass 1: top up matching non-full stacks. */
    for (int i = 0; i < INVENTORY_SLOTS && count > 0; i++) {
        if (inv->slots[i].block != block) continue;
        uint8_t room = (uint8_t)(INVENTORY_STACK_MAX - inv->slots[i].count);
        uint8_t take = count < room ? count : room;
        inv->slots[i].count = (uint8_t)(inv->slots[i].count + take);
        count = (uint8_t)(count - take);
    }

    /* Pass 2: fill empty slots. */
    for (int i = 0; i < INVENTORY_SLOTS && count > 0; i++) {
        if (inv->slots[i].count != 0) continue;
        uint8_t take = count < INVENTORY_STACK_MAX ? count : INVENTORY_STACK_MAX;
        inv->slots[i].block = block;
        inv->slots[i].count = take;
        count = (uint8_t)(count - take);
    }
    return count;
}

bool inventory_consume(Inventory* inv, int slot) {
    if (slot < 0 || slot >= INVENTORY_SLOTS) return false;
    if (inv->slots[slot].count == 0) return false;
    inv->slots[slot].count--;
    if (inv->slots[slot].count == 0) inv->slots[slot].block = BLOCK_AIR;
    return true;
}
