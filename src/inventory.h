#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "block.h"
#include "ui/hud.h"   /* HUD_SLOT_COUNT */

#define INVENTORY_SLOTS     HUD_SLOT_COUNT
#define INVENTORY_STACK_MAX 64

typedef struct {
    BlockID block;     /* BLOCK_AIR when slot is empty */
    uint8_t count;     /* 0..INVENTORY_STACK_MAX */
} InventorySlot;

typedef struct {
    InventorySlot slots[INVENTORY_SLOTS];
    int           selected;                 /* 0..INVENTORY_SLOTS-1 */
} Inventory;

void    inventory_init(Inventory* inv);

/* Add `count` of `block`. Strategy: top up matching non-full stacks first,
 * then fill the first empty slot. Returns leftover count (0 = all picked up). */
uint8_t inventory_add(Inventory* inv, BlockID block, uint8_t count);

/* Decrement slots[slot] by 1. Returns true if a unit was consumed.
 * If count reaches 0, sets block back to BLOCK_AIR. */
bool    inventory_consume(Inventory* inv, int slot);

#endif
