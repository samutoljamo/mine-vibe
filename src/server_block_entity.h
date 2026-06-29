#ifndef SERVER_BLOCK_ENTITY_H
#define SERVER_BLOCK_ENTITY_H

/* ------------------------------------------------------------------ */
/*  Server-side container block-entity store (furnace + chest)         */
/*                                                                     */
/*  This is a SEPARATE translation unit from server.c on purpose: it    */
/*  is the only place that includes the smelting + container models,    */
/*  whose `ItemStack` typedef (container.h) collides with the           */
/*  identically-named one in crafting.h. server.c needs crafting.h (for */
/*  PKT_CRAFT) but never includes container.h — it drives containers    */
/*  exclusively through this interface, which speaks only primitives,    */
/*  Inventory, and the net.h ContainerStatePacket (none of which clash).*/
/* ------------------------------------------------------------------ */

#include <stdint.h>
#include <stdbool.h>
#include "inventory.h"   /* Inventory — no ItemStack clash */
#include "net.h"         /* ContainerStatePacket            */
#include "server.h"      /* Server + the opaque BlockEntity forward-decl */

/* Container kind, mirrored from the placed block. */
typedef enum { SBE_FURNACE, SBE_CHEST } SbeType;

/* Find / create / destroy block-entities by block position. be_create returns
 * the existing entity if one is already present at that cell (idempotent). */
BlockEntity* sbe_find(Server* s, int x, int y, int z);
BlockEntity* sbe_create(Server* s, int x, int y, int z, SbeType type);

/* Tear down the block-entity at (x,y,z), pouring its contents into `inv`
 * (anything that doesn't fit is lost). No-op if none is tracked there. */
void sbe_destroy_return(Server* s, int x, int y, int z, Inventory* inv);

/* Free the whole store (teardown). */
void sbe_free_all(Server* s);

/* True if block id `b` should own a container block-entity; writes its type. */
bool sbe_block_is_container(uint8_t b, SbeType* out_type);

/* Viewer bookkeeping (bit `client_index` in the entity's viewer mask). */
void sbe_add_viewer(BlockEntity* be, int client_index);
void sbe_remove_viewer(BlockEntity* be, int client_index);
void sbe_clear_viewer_all(Server* s, int client_index);

/* Number of logical slots in the container (chest: 27, furnace: 3). */
int  sbe_slot_count(const BlockEntity* be);

/* Fill a CONTAINER_STATE snapshot from the entity. */
void sbe_fill_state(const BlockEntity* be, ContainerStatePacket* out);

/* Move up to `count` items between a container slot and the player inventory.
 * dir is a ContainerNetDir (CONTAINER_DIR_TO_INV / CONTAINER_DIR_FROM_INV).
 * For TO_INV `slot` indexes the container; for FROM_INV it indexes `inv`.
 * Caller is responsible for validating `slot` range via sbe_slot_count /
 * INVENTORY_SLOTS first. Applies in place. */
void sbe_transfer(BlockEntity* be, uint8_t slot, uint8_t dir, uint8_t count,
                  Inventory* inv);

/* Advance every furnace by `dt_ticks`; for each whose state changed AND has at
 * least one viewer, invoke `on_change(user, be)` so the caller can push a fresh
 * CONTAINER_STATE to that entity's viewers. */
void sbe_tick_furnaces(Server* s, int dt_ticks,
                       void (*on_change)(void* user, const BlockEntity* be),
                       void* user);

/* Iterate viewers: returns the viewer bitmask so the caller can fan out. */
uint32_t sbe_viewers(const BlockEntity* be);

/* ------------------------------------------------------------------ */
/*  Dungeon chest loot population (63k)                                 */
/*                                                                     */
/*  These live here (not server.c) on purpose: rolling dungeon loot     */
/*  needs dungeon.h, which pulls in container.h, whose `ItemStack`      */
/*  typedef clashes with crafting.h's (which server.c includes). This   */
/*  TU already isolates container.h, so server.c drives population      */
/*  through these primitives-only entry points and never sees the clash.*/
/* ------------------------------------------------------------------ */

/* Max dungeon chests that can have their chest cell inside one chunk. A chunk
 * consults its own placement cell plus the 8 neighbours, and each cell yields at
 * most one room/chest, so 9 is the hard upper bound. */
#define SBE_DUNGEON_CHESTS_PER_CHUNK 9

/* Pure: enumerate every dungeon chest whose chest cell falls inside chunk
 * (cx,cz) for `seed`, mirroring worldgen's placement query exactly (depends only
 * on world coords + seed, so it matches the generated BLOCK_CHEST). Writes the
 * chest world position and per-chest loot seed into the parallel arrays (each at
 * least `max` long) and returns the count written (0..SBE_DUNGEON_CHESTS_PER_CHUNK,
 * clamped to `max`). No I/O, no globals — unit-tested directly. */
int sbe_dungeon_chests_in_chunk(int cx, int cz, uint32_t seed,
                                int xs[], int ys[], int zs[],
                                uint32_t chest_seeds[], int max);

/* Fill a SBE_CHEST block-entity's container with the deterministic loot for
 * `chest_seed` (via dungeon_roll_chest). Clears the chest first so the result is
 * exactly the rolled stacks. No-op for a non-chest entity or NULL. */
void sbe_fill_chest_loot(BlockEntity* be, uint32_t chest_seed);

/* Create (idempotently) a pre-filled dungeon chest block-entity at (x,y,z) and
 * roll its loot from `chest_seed`. Does NOT overwrite an already-tracked entity
 * at that cell (so a chest a player has opened/looted this session, or one
 * already populated, is left untouched). Returns the entity (new or existing),
 * or NULL on allocation failure. */
BlockEntity* sbe_populate_dungeon_chest(Server* s, int x, int y, int z,
                                        uint32_t chest_seed);

#endif /* SERVER_BLOCK_ENTITY_H */
