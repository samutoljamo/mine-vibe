#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "block.h"
#include "item.h"

#define INVENTORY_SLOTS     6
#define INVENTORY_STACK_MAX 64

typedef struct {
    ItemId   item;       /* item id (block id in the low range); BLOCK_AIR when empty */
    uint8_t  count;      /* 0..INVENTORY_STACK_MAX */
    uint16_t durability; /* remaining uses for tools; 0/ignored for blocks */
} InventorySlot;

typedef struct Inventory {
    InventorySlot slots[INVENTORY_SLOTS];
    int           selected;                 /* 0..INVENTORY_SLOTS-1 */
} Inventory;

void    inventory_init(Inventory* inv);

/* Add `count` of block `block`. Strategy: top up matching non-full stacks
 * first, then fill the first empty slot. Returns leftover count (0 = all
 * picked up). Convenience wrapper around inventory_add_item for block drops. */
uint8_t inventory_add(Inventory* inv, BlockID block, uint8_t count);

/* Add `count` of an arbitrary item. Tools are unstackable (stack max 1) and
 * are placed at full durability in the first empty slot. Returns leftover. */
uint8_t inventory_add_item(Inventory* inv, ItemId item, uint8_t count);

/* Decrement slots[slot] by 1. Returns true if a unit was consumed.
 * If count reaches 0, sets the slot back to empty (BLOCK_AIR). Intended for
 * consuming a placeable block / stackable; not for tool durability. */
bool    inventory_consume(Inventory* inv, int slot);

/* Apply one point of tool wear to slots[slot] if it holds a tool. Returns true
 * if the tool broke (durability reached 0) and the slot was emptied. Non-tool
 * slots and empty slots are left untouched and return false. */
bool    inventory_damage_tool(Inventory* inv, int slot);

#endif
