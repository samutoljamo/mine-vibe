#ifndef PHYSICS_H
#define PHYSICS_H

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct World World;

typedef struct PhysicsResult {
    bool on_ground;
    bool in_water;
} PhysicsResult;

/*
 * Move player AABB through world, resolving collisions axis-by-axis.
 *   pos:    feet position (modified in place)
 *   vel:    velocity (components zeroed on collision)
 *   half_w: half-width of hitbox (0.3)
 *   height: hitbox height (1.8)
 *   dt:     time delta for this tick
 *   world:  world pointer for block queries
 */
PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, World* world);

bool physics_check_water(vec3 pos, float half_w, float height, World* world);

#endif
