#ifndef PHYSICS_H
#define PHYSICS_H

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>
#include "block.h"

typedef struct World World;

typedef struct PhysicsResult {
    bool on_ground;
    bool in_water;
} PhysicsResult;

/* Block-query callback used by the pure collision core. Returns the BlockID at
 * an integer world cell; out-of-range cells should return BLOCK_AIR. ctx is an
 * opaque pointer passed straight through (the world, or a test grid). */
typedef BlockID (*BlockQueryFn)(int x, int y, int z, void* ctx);

/*
 * Move player AABB through world, resolving collisions axis-by-axis.
 *   pos:       feet position (modified in place)
 *   vel:       velocity (components zeroed on collision)
 *   half_w:    half-width of hitbox (0.3)
 *   height:    hitbox height (1.8)
 *   dt:        time delta for this tick
 *   crouching: enable edge protection — if true and the player would step
 *              off the edge of a supporting block, the horizontal axis is
 *              reverted instead. Only applies when on_ground before the
 *              horizontal sweep.
 *   world:     world pointer for block queries
 */
PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, bool crouching, World* world);

bool physics_check_water(vec3 pos, float half_w, float height, World* world);

/* Pure collision core: identical resolution to physics_move but driven by a
 * BlockQueryFn instead of a World*, so it carries no world/threading
 * dependency and is directly unit-testable against an in-memory grid. */
PhysicsResult physics_move_q(vec3 pos, vec3 vel, float half_w, float height,
                             float dt, bool crouching,
                             BlockQueryFn query, void* ctx);

bool physics_check_water_q(vec3 pos, float half_w, float height,
                           BlockQueryFn query, void* ctx);

/* Column-query callback for spawn-safety: returns the BlockID at integer
 * world-y in a single (fixed x,z) column. ctx is passed straight through. */
typedef BlockID (*ColumnQueryFn)(int y, void* ctx);

/* Pure spawn-safety helper. Scans DOWN from top_y to base_y looking for the
 * first solid surface that has at least two air cells of headroom directly
 * above it, and returns the feet-Y at which the player stands ON that surface
 * (i.e. solid_y + 1). This guarantees the player spawns resting on ground with
 * room for the 1.8-tall hitbox rather than mid-air (where they'd fall) or
 * embedded in terrain.
 *
 * Returns base_y + 1 as a safe fallback if no qualifying surface is found
 * (caller never gets a Y that drops below the scanned column base). */
int physics_safe_spawn_y(int top_y, int base_y,
                         ColumnQueryFn query, void* ctx);

#endif
