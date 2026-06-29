#ifndef DUNGEON_H
#define DUNGEON_H

#include <stdbool.h>
#include <stdint.h>

/* ItemStack {ItemId item; uint8_t count;} comes from container.h — the same
 * 27-slot chest type the deferred server step will fill. We use container.h's
 * ItemStack (not crafting.h's identically-named typedef): dungeon never needs
 * crafting, so only one ItemStack definition is ever in scope and the two
 * typedefs can't collide here. */
#include "container.h"

/* Pure, deterministic placement model for underground dungeon rooms.
 *
 * A dungeon is a small hollow room (mossy-cobblestone shell, air interior, one
 * chest against a wall) buried in the stone layer. Like cave.c, every decision
 * here depends ONLY on absolute world coordinates + the world seed, never on
 * chunk-local state. That is what makes a room that straddles a chunk boundary
 * generate bit-identically no matter which overlapping chunk renders it.
 *
 * Placement uses a coarse cell grid (DUNGEON_CELL_BLOCKS on a side). At most one
 * dungeon candidate exists per cell; a low spawn percentage keeps dungeons rare
 * but findable. The candidate's origin is jittered within the cell interior so
 * its full footprint (plus a margin) always stays inside the cell — adjacent
 * cells can therefore never produce overlapping rooms, and a chunk only ever has
 * to consult its own cell plus the 8 neighbours.
 *
 * The room origin is the minimum-corner (x0, y0, z0) of the wall shell. Interior
 * is the hollow inside the shell; the chest sits on the interior floor against a
 * wall. All dimensions include the 1-block wall on each side.
 *
 * Properties (verified by tests/test_dungeon.c):
 *   - Pure & deterministic: identical (cell, seed) / (coords, seed) always yield
 *     the same result. No globals, no rand/time.
 *   - Cross-chunk continuity: a room is a function of its cell + seed only, so
 *     it is identical regardless of which chunk asks about it.
 *   - Vertically banded: rooms sit between DUNGEON_MIN_Y and DUNGEON_MAX_Y, well
 *     inside the stone layer and clear of the bedrock floor.
 *   - Rare: spawn probability is DUNGEON_SPAWN_PCT per cell.
 */

/* Coarse placement grid. One dungeon candidate per cell at most. */
#define DUNGEON_CELL_BLOCKS 96

/* Per-cell spawn probability (percent). Kept low so dungeons stay uncommon. */
#define DUNGEON_SPAWN_PCT 18

/* Room size bounds (full extent INCLUDING the 1-block wall on each side).
 * Interior is (dim - 2) on each axis, so width/depth 5..7 give 3..5 interior,
 * and height 5 gives 3 interior (floor-to-ceiling clearance). */
#define DUNGEON_MIN_W 5
#define DUNGEON_MAX_W 7
#define DUNGEON_MIN_D 5
#define DUNGEON_MAX_D 7
#define DUNGEON_ROOM_H 5   /* fixed total height (3 interior) */

/* Vertical band for the room origin (y0, the floor-shell layer). Kept above the
 * bedrock mix band (y<10) and below the typical surface so rooms stay buried. */
#define DUNGEON_MIN_Y 12
#define DUNGEON_MAX_Y 50

/* Independent hash salts so each roll decorrelates. */
#define DUNGEON_SALT_PRESENT 0x0D016E01
#define DUNGEON_SALT_JITTER  0x0D016E02
#define DUNGEON_SALT_DIMS    0x0D016E03
#define DUNGEON_SALT_SEED    0x0D016E04

/* One dungeon candidate for a placement cell. */
typedef struct {
    bool present;   /* does a room materialize in this cell?            */
    int  x0, y0, z0; /* min-corner of the wall shell (world coords)     */
    int  w, h, d;   /* full extent including walls (x, y, z)            */
    int  chest_x, chest_y, chest_z; /* chest world position (interior)  */
    int  seed;      /* per-dungeon derived seed (for later loot fills)  */
} DungeonRoom;

/* Pure decision for placement cell (cgx, cgz). Always fills geometry fields so
 * callers can AABB-test even an absent candidate cheaply; check .present to know
 * whether to emit it. */
DungeonRoom dungeon_cell_at(int cgx, int cgz, uint32_t seed);

/* Floor-divide a world coordinate to its placement-cell index. */
int dungeon_cell_index(int world_coord);

/* Classify a single voxel relative to a room shell:
 *   0 = outside the room's bounding box
 *   1 = wall/floor/ceiling shell (mossy cobblestone)
 *   2 = hollow interior (air)
 * The chest voxel is part of the interior (category 2); callers place the chest
 * block themselves at (chest_x,chest_y,chest_z). Pure. */
int dungeon_voxel_role(const DungeonRoom *room, int wx, int wy, int wz);

/* ------------------------------------------------------------------ */
/*  Deterministic dungeon-chest loot roller (ado core)                  */
/*                                                                     */
/*  Rolls the CONTENTS of a dungeon chest from a single seed. Pure and  */
/*  deterministic: the same chest_seed always produces byte-identical   */
/*  stacks, so the authoritative server can re-roll a chest's loot at    */
/*  chunk-load and any client/host agrees on what is inside without      */
/*  storing it. Draws come from LOOT_DUNGEON_CHEST via the splitmix32    */
/*  PRNG (loot_rng_next) seeded from chest_seed — no rand/time/globals.  */
/*                                                                     */
/*  DEFERRED FOLLOW-UP (do NOT do here): wiring this into real chest     */
/*  block-entities at worldgen — i.e. server.c constructing a Container  */
/*  at (chest_x,chest_y,chest_z) on chunk generation and filling it via  */
/*  this roller — is a separate ticket and touches server.c (owned by    */
/*  another agent). This module only computes the contents.             */
/* ------------------------------------------------------------------ */

/* Inclusive bounds on how many loot stacks a single chest yields. */
#define DUNGEON_CHEST_MIN_STACKS 3
#define DUNGEON_CHEST_MAX_STACKS 7

/* Roll the deterministic loot contents for one dungeon chest.
 *
 * Draws a seed-derived number of stacks in [DUNGEON_CHEST_MIN_STACKS,
 * DUNGEON_CHEST_MAX_STACKS] from LOOT_DUNGEON_CHEST and writes them into the
 * first distinct slots of `out` (one stack per slot, in order). Returns the
 * number of stacks produced (>= 0).
 *
 * The number written is clamped to `max_slots`; a non-positive `max_slots`
 * produces nothing and leaves `out` untouched. Never writes past
 * out[max_slots-1]. Each written stack's count lies within its loot entry's
 * [min_count, max_count]. Pure / deterministic in `chest_seed`. */
int dungeon_roll_chest(uint32_t chest_seed, ItemStack out[], int max_slots);

#endif /* DUNGEON_H */
