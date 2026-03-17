#include "physics.h"
#include "world.h"
#include "block.h"
#include <math.h>

/*
 * Sweep one axis: move pos[axis] by delta, then check the resulting
 * AABB against all solid blocks. If overlap is found, push position to the
 * nearest block face on that axis and zero the velocity component.
 *
 * For Y-axis falling, sets *on_ground = true.
 */
static void sweep_axis(vec3 pos, vec3 vel, float half_w, float height,
                       int axis, float delta, World* world, bool* on_ground)
{
    if (delta == 0.0f) return;
    pos[axis] += delta;

    float min_x = pos[0] - half_w, max_x = pos[0] + half_w;
    float min_y = pos[1],          max_y = pos[1] + height;
    float min_z = pos[2] - half_w, max_z = pos[2] + half_w;

    int bx0 = (int)floorf(min_x), bx1 = (int)floorf(max_x);
    int by0 = (int)floorf(min_y), by1 = (int)floorf(max_y);
    int bz0 = (int)floorf(min_z), bz1 = (int)floorf(max_z);

    for (int by = by0; by <= by1; by++)
    for (int bx = bx0; bx <= bx1; bx++)
    for (int bz = bz0; bz <= bz1; bz++) {
        if (!block_is_solid(world_get_block(world, bx, by, bz)))
            continue;

        /* Verify real overlap (handles exact-boundary cases) */
        float bf[3] = { (float)bx, (float)by, (float)bz };
        if (max_x <= bf[0] || min_x >= bf[0] + 1.0f) continue;
        if (max_y <= bf[1] || min_y >= bf[1] + 1.0f) continue;
        if (max_z <= bf[2] || min_z >= bf[2] + 1.0f) continue;

        /* Push position to nearest block face on this axis */
        if (axis == 1) { /* Y */
            if (delta < 0.0f) {
                pos[1] = bf[1] + 1.0f;
                *on_ground = true;
            } else {
                pos[1] = bf[1] - height;
            }
        } else { /* X or Z */
            if (delta < 0.0f)
                pos[axis] = bf[axis] + 1.0f + half_w;
            else
                pos[axis] = bf[axis] - half_w;
        }
        vel[axis] = 0.0f;

        /* Recompute bounds for remaining block checks */
        min_x = pos[0] - half_w; max_x = pos[0] + half_w;
        min_y = pos[1];          max_y = pos[1] + height;
        min_z = pos[2] - half_w; max_z = pos[2] + half_w;
    }
}

PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, World* world)
{
    PhysicsResult result = { false, false };

    /* Y first (ground detection), then X, then Z */
    sweep_axis(pos, vel, half_w, height, 1, vel[1] * dt, world, &result.on_ground);
    sweep_axis(pos, vel, half_w, height, 0, vel[0] * dt, world, &result.on_ground);
    sweep_axis(pos, vel, half_w, height, 2, vel[2] * dt, world, &result.on_ground);

    result.in_water = physics_check_water(pos, half_w, height, world);
    return result;
}

bool physics_check_water(vec3 pos, float half_w, float height, World* world)
{
    int bx0 = (int)floorf(pos[0] - half_w), bx1 = (int)floorf(pos[0] + half_w);
    int by0 = (int)floorf(pos[1]),           by1 = (int)floorf(pos[1] + height);
    int bz0 = (int)floorf(pos[2] - half_w), bz1 = (int)floorf(pos[2] + half_w);

    for (int by = by0; by <= by1; by++)
    for (int bx = bx0; bx <= bx1; bx++)
    for (int bz = bz0; bz <= bz1; bz++) {
        if (world_get_block(world, bx, by, bz) == BLOCK_WATER)
            return true;
    }
    return false;
}
