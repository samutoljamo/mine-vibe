#ifndef RAYCAST_H
#define RAYCAST_H

#include <stdbool.h>
#include <cglm/cglm.h>
#include "world.h"

typedef enum {
    FACE_NX = 0, FACE_PX,
    FACE_NY,     FACE_PY,
    FACE_NZ,     FACE_PZ
} BlockFace;

typedef struct RaycastHit {
    bool      hit;
    int       x, y, z;     /* hit cell, valid only if hit */
    BlockFace face;        /* face of the hit cell that the ray crossed */
    BlockID   block;       /* block at the hit cell; BLOCK_AIR (0) if !hit */
} RaycastHit;

/* Voxel DDA (Amanatides & Woo) against the world. `max_dist` in blocks.
 * Returns the first non-air, non-water cell hit, or {hit=false} otherwise. */
RaycastHit raycast_voxel(World* world,
                         vec3 origin, vec3 dir, float max_dist);

/* Fills (dx,dy,dz) ∈ {-1,0,1}^3 with the unit offset from a block to the
 * neighbour on the given face. */
void block_face_offset(BlockFace face, int* dx, int* dy, int* dz);

#endif
