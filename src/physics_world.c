#include "physics.h"
#include "world.h"

/*
 * World*-bound adapters over the pure collision core in physics.c. Kept in a
 * separate TU so physics.c (the testable math) links with nothing but block.c —
 * world_get_block lives here, behind the BlockQueryFn callback.
 */

static BlockID world_block_query(int x, int y, int z, void* ctx)
{
    return world_get_block((World*)ctx, x, y, z);
}

PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, bool crouching, World* world)
{
    return physics_move_q(pos, vel, half_w, height, dt, crouching,
                          world_block_query, world);
}

bool physics_check_water(vec3 pos, float half_w, float height, World* world)
{
    return physics_check_water_q(pos, half_w, height, world_block_query, world);
}
