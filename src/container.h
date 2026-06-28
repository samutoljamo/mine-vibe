#ifndef CONTAINER_H
#define CONTAINER_H

#include <stdint.h>
#include <stdbool.h>
#include "block.h"
#include "item.h"
#include "inventory.h"

/* ------------------------------------------------------------------ */
/*  Chest container model (pure)                                        */
/*                                                                     */
/*  A Container is a fixed-size grid of stackable item slots, mirroring */
/*  the Inventory stacking semantics (top up matching stacks first,     */
/*  then fill empty slots, never exceeding the per-item max stack).     */
/*  All operations here are pure: no Vulkan/GLFW, no networking, no I/O.*/
/* ------------------------------------------------------------------ */

#define CHEST_SLOTS          27   /* a single chest: 3 rows of 9 */
#define CONTAINER_STACK_MAX  64   /* upper bound on a slot's count (matches blocks) */

/* A single stack held in a container slot. `item` is an ItemId (a block id in
 * the low range); BLOCK_AIR with count 0 means empty. Durability is not tracked
 * here — chests store stackable goods; tools land at full durability via the
 * inventory model and are treated as count-1 stacks. */
typedef struct {
    ItemId  item;
    uint8_t count;   /* 0..CONTAINER_STACK_MAX */
} ItemStack;

typedef struct Container {
    ItemStack slots[CHEST_SLOTS];
} Container;

/* Reset every slot to empty (BLOCK_AIR / count 0). */
void container_init(Container* c);

/* Alias for container_init: empties the whole container. */
void container_clear(Container* c);

/* Number of slots in the container (always CHEST_SLOTS). */
int container_slot_count(const Container* c);

/* Add `count` of `item`. Strategy mirrors inventory_add: top up matching
 * non-full stacks first, then fill empty slots, never exceeding the per-item
 * max stack (clamped to CONTAINER_STACK_MAX). Returns leftover count that did
 * not fit (0 = everything stored). */
uint8_t container_add(Container* c, ItemId item, uint8_t count);

/* Total count of `item` across all slots (saturates at UINT16_MAX). */
uint16_t container_count(const Container* c, ItemId item);

/* Take up to `count` units out of slot `slot`. Returns the stack actually
 * removed (item + count); an out-of-range or empty slot yields an empty stack
 * (BLOCK_AIR / 0). The slot is emptied when fully drained. */
ItemStack container_take_slot(Container* c, int slot, uint8_t count);

/* Remove exactly `count` of `item`, draining matching slots. Returns true and
 * applies the removal only if at least `count` are present; otherwise returns
 * false and leaves the container untouched (atomic). */
bool container_remove(Container* c, ItemId item, uint8_t count);

/* Move up to `count` units from container slot `slot` into `inv`, reusing the
 * inventory stacking rules. Only what the inventory can hold is moved; the rest
 * stays in the container slot. Returns the number of units actually moved. */
uint8_t container_transfer_to_inventory(Container* c, int slot,
                                        Inventory* inv, uint8_t count);

/* Move up to `count` units from inventory slot `slot` into the container,
 * reusing the container stacking rules. Only what the container can hold is
 * moved; the rest stays in the inventory slot. Returns the number of units
 * actually moved. */
uint8_t container_transfer_from_inventory(Inventory* inv, int slot,
                                          Container* c, uint8_t count);

#endif /* CONTAINER_H */
