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

/* Sub-step sweep to prevent tunneling when |delta| > 1 block.
 * At terminal velocity (78.4 m/s) a single tick moves 1.3 blocks,
 * enough to skip through a 1-block-thick floor without sub-stepping. */
static void sweep_axis_substepped(vec3 pos, vec3 vel, float half_w, float height,
                                   int axis, float total_delta, World* world,
                                   bool* on_ground)
{
    float remaining = total_delta;
    while (fabsf(remaining) > 0.0001f) {
        float step = remaining;
        if (fabsf(step) > 0.999f)
            step = (remaining > 0.0f) ? 0.999f : -0.999f;
        sweep_axis(pos, vel, half_w, height, axis, step, world, on_ground);
        remaining -= step;
        if (vel[axis] == 0.0f) break; /* velocity zeroed by collision */
    }
}

/* Is there a solid block under the player's footprint? Probes the slice
 * one texel below feet across the AABB column; any solid block keeps the
 * player supported. Used by crouch edge protection. */
static bool aabb_supported(vec3 pos, float half_w, World* world)
{
    int by = (int)floorf(pos[1] - 0.001f);
    int bx0 = (int)floorf(pos[0] - half_w);
    int bx1 = (int)floorf(pos[0] + half_w);
    int bz0 = (int)floorf(pos[2] - half_w);
    int bz1 = (int)floorf(pos[2] + half_w);
    for (int bx = bx0; bx <= bx1; bx++)
    for (int bz = bz0; bz <= bz1; bz++) {
        if (block_is_solid(world_get_block(world, bx, by, bz)))
            return true;
    }
    return false;
}

PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, bool crouching, World* world)
{
    PhysicsResult result = { false, false };

    /* Whether the AABB was sitting on a supporting block at the START of
     * this tick. We can't use the Y-sweep's on_ground for this: when the
     * player is stably standing, vel[1] is 0, the Y-sweep is a no-op, and
     * result.on_ground stays false — even though the player is on the
     * ground. (Gravity is gated on player->on_ground, which is the prior
     * tick's result; on a flat surface vel[1] oscillates 0 / -gravity*dt
     * every other tick.) Probing the floor directly is reliable. */
    bool was_supported = aabb_supported(pos, half_w, world);

    /* Y first (sets on_ground from collisions), then X, then Z */
    sweep_axis_substepped(pos, vel, half_w, height, 1, vel[1] * dt, world, &result.on_ground);

    /* Edge protection: when crouching and started supported, refuse any
     * horizontal sweep whose result would leave the player unsupported.
     * Applied per-axis so the player can still slide along an edge as long
     * as one axis keeps them on the supporting block. */
    bool edge_protect = crouching && was_supported;

    float prev_x = pos[0];
    sweep_axis_substepped(pos, vel, half_w, height, 0, vel[0] * dt, world, &result.on_ground);
    if (edge_protect && !aabb_supported(pos, half_w, world)) {
        pos[0] = prev_x;
        vel[0] = 0.0f;
    }

    float prev_z = pos[2];
    sweep_axis_substepped(pos, vel, half_w, height, 2, vel[2] * dt, world, &result.on_ground);
    if (edge_protect && !aabb_supported(pos, half_w, world)) {
        pos[2] = prev_z;
        vel[2] = 0.0f;
    }

    /* Final on_ground: true if the AABB is currently sitting on a
     * supporting block. The Y-sweep's collision-snap already updates
     * result.on_ground when the player lands this tick; this final probe
     * fills in the "stably standing, vel[1]=0" case where Y-sweep was a
     * no-op so the prior-tick on_ground field doesn't oscillate. */
    if (!result.on_ground)
        result.on_ground = aabb_supported(pos, half_w, world);

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
