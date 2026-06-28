#include "container.h"
#include <string.h>

_Static_assert(BLOCK_AIR == 0,
    "container_init relies on BLOCK_AIR == 0 (memset zero = empty container)");
_Static_assert(CONTAINER_STACK_MAX > 0 && CONTAINER_STACK_MAX <= UINT8_MAX,
    "CONTAINER_STACK_MAX must fit in a uint8_t to avoid silent overflow");

/* Per-item stack cap clamped to the container's own ceiling. */
static uint8_t container_stack_max(ItemId item) {
    uint8_t m = item_stack_max(item);
    return m > CONTAINER_STACK_MAX ? CONTAINER_STACK_MAX : m;
}

void container_init(Container* c) {
    memset(c, 0, sizeof(*c));   /* item == BLOCK_AIR == 0, count == 0 */
}

void container_clear(Container* c) {
    container_init(c);
}

int container_slot_count(const Container* c) {
    (void)c;
    return CHEST_SLOTS;
}

uint8_t container_add(Container* c, ItemId item, uint8_t count) {
    /* Guard, not just a fast path: without it pass 1 would "top up" empty slots
     * whose item field is BLOCK_AIR, breaking the (item==AIR) <=> (count==0)
     * invariant. */
    if (item == (ItemId)BLOCK_AIR || count == 0) return count;

    uint8_t stack_max = container_stack_max(item);

    /* Pass 1: top up matching non-full stacks (skip for unstackable items). */
    if (stack_max > 1) {
        for (int i = 0; i < CHEST_SLOTS && count > 0; i++) {
            if (c->slots[i].item != item || c->slots[i].count == 0) continue;
            if (c->slots[i].count >= stack_max) continue;
            uint8_t room = (uint8_t)(stack_max - c->slots[i].count);
            uint8_t take = count < room ? count : room;
            c->slots[i].count = (uint8_t)(c->slots[i].count + take);
            count = (uint8_t)(count - take);
        }
    }

    /* Pass 2: fill empty slots. */
    for (int i = 0; i < CHEST_SLOTS && count > 0; i++) {
        if (c->slots[i].count != 0) continue;
        uint8_t take = count < stack_max ? count : stack_max;
        c->slots[i].item  = item;
        c->slots[i].count = take;
        count = (uint8_t)(count - take);
    }
    return count;
}

uint16_t container_count(const Container* c, ItemId item) {
    if (item == (ItemId)BLOCK_AIR) return 0;
    uint32_t total = 0;
    for (int i = 0; i < CHEST_SLOTS; i++) {
        if (c->slots[i].item == item && c->slots[i].count > 0)
            total += c->slots[i].count;
    }
    return total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
}

ItemStack container_take_slot(Container* c, int slot, uint8_t count) {
    ItemStack out = { (ItemId)BLOCK_AIR, 0 };
    if (slot < 0 || slot >= CHEST_SLOTS) return out;
    ItemStack* s = &c->slots[slot];
    if (s->count == 0 || count == 0) return out;

    uint8_t take = count < s->count ? count : s->count;
    out.item  = s->item;
    out.count = take;
    s->count  = (uint8_t)(s->count - take);
    if (s->count == 0) s->item = (ItemId)BLOCK_AIR;
    return out;
}

bool container_remove(Container* c, ItemId item, uint8_t count) {
    if (count == 0) return true;
    if (item == (ItemId)BLOCK_AIR) return false;
    if (container_count(c, item) < count) return false;   /* atomic: check first */

    uint8_t remaining = count;
    for (int i = 0; i < CHEST_SLOTS && remaining > 0; i++) {
        if (c->slots[i].item != item || c->slots[i].count == 0) continue;
        uint8_t take = c->slots[i].count < remaining ? c->slots[i].count : remaining;
        c->slots[i].count = (uint8_t)(c->slots[i].count - take);
        remaining = (uint8_t)(remaining - take);
        if (c->slots[i].count == 0) c->slots[i].item = (ItemId)BLOCK_AIR;
    }
    return true;
}

uint8_t container_transfer_to_inventory(Container* c, int slot,
                                        Inventory* inv, uint8_t count) {
    if (slot < 0 || slot >= CHEST_SLOTS || count == 0) return 0;
    ItemStack* s = &c->slots[slot];
    if (s->count == 0) return 0;

    uint8_t want = count < s->count ? count : s->count;
    ItemId  item = s->item;

    /* Hand the requested units to the inventory; it returns what didn't fit. */
    uint8_t leftover = inventory_add_item(inv, item, want);
    uint8_t moved = (uint8_t)(want - leftover);

    s->count = (uint8_t)(s->count - moved);
    if (s->count == 0) s->item = (ItemId)BLOCK_AIR;
    return moved;
}

uint8_t container_transfer_from_inventory(Inventory* inv, int slot,
                                          Container* c, uint8_t count) {
    if (slot < 0 || slot >= INVENTORY_SLOTS || count == 0) return 0;
    InventorySlot* s = &inv->slots[slot];
    if (s->count == 0) return 0;

    uint8_t want = count < s->count ? count : s->count;
    ItemId  item = s->item;

    /* Offer the requested units to the container; it returns what didn't fit. */
    uint8_t leftover = container_add(c, item, want);
    uint8_t moved = (uint8_t)(want - leftover);

    s->count = (uint8_t)(s->count - moved);
    if (s->count == 0) {
        s->item       = (ItemId)BLOCK_AIR;
        s->durability = 0;
    }
    return moved;
}
