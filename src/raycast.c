#include "raycast.h"
#include <assert.h>
#include <math.h>

void block_face_offset(BlockFace face, int* dx, int* dy, int* dz) {
    *dx = 0; *dy = 0; *dz = 0;
    switch (face) {
        case FACE_NX: *dx = -1; break;
        case FACE_PX: *dx =  1; break;
        case FACE_NY: *dy = -1; break;
        case FACE_PY: *dy =  1; break;
        case FACE_NZ: *dz = -1; break;
        case FACE_PZ: *dz =  1; break;
        default: assert(0 && "block_face_offset: invalid BlockFace");
    }
}

static int floor_int(float v) { return (int)floorf(v); }

RaycastHit raycast_voxel(World* world,
                         vec3 origin, vec3 dir, float max_dist) {
    RaycastHit miss = { .hit = false };
    float dx = dir[0], dy = dir[1], dz = dir[2];
    float dlen = sqrtf(dx*dx + dy*dy + dz*dz);
    /* Guard zero/near-zero/NaN direction; later math depends on a finite, non-tiny dlen. */
    if (!isfinite(dlen) || dlen < 1e-6f) return miss;
    dx /= dlen; dy /= dlen; dz /= dlen;

    int   x = floor_int(origin[0]);
    int   y = floor_int(origin[1]);
    int   z = floor_int(origin[2]);
    int   sx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
    int   sy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    int   sz = (dz > 0) ? 1 : (dz < 0 ? -1 : 0);

    float tDeltaX = (sx != 0) ? fabsf(1.0f / dx) : INFINITY;
    float tDeltaY = (sy != 0) ? fabsf(1.0f / dy) : INFINITY;
    float tDeltaZ = (sz != 0) ? fabsf(1.0f / dz) : INFINITY;

    float nextX = (sx > 0) ? (x + 1.0f) : (float)x;
    float nextY = (sy > 0) ? (y + 1.0f) : (float)y;
    float nextZ = (sz > 0) ? (z + 1.0f) : (float)z;
    float tMaxX = (sx != 0) ? (nextX - origin[0]) / dx : INFINITY;
    float tMaxY = (sy != 0) ? (nextY - origin[1]) / dy : INFINITY;
    float tMaxZ = (sz != 0) ? (nextZ - origin[2]) / dz : INFINITY;

    BlockFace last_face = FACE_NX;
    float t = 0.0f;

    /* First check the starting cell — handles ray-starts-inside-block. */
    BlockID b0 = world_get_block(world, x, y, z);
    if (b0 != BLOCK_AIR && b0 != BLOCK_WATER) {
        return (RaycastHit){ .hit = true, .x = x, .y = y, .z = z, .face = FACE_PY };
    }

    while (t <= max_dist) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += sx; t = tMaxX; tMaxX += tDeltaX;
            last_face = (sx > 0) ? FACE_NX : FACE_PX;
        } else if (tMaxY < tMaxZ) {
            y += sy; t = tMaxY; tMaxY += tDeltaY;
            last_face = (sy > 0) ? FACE_NY : FACE_PY;
        } else {
            z += sz; t = tMaxZ; tMaxZ += tDeltaZ;
            last_face = (sz > 0) ? FACE_NZ : FACE_PZ;
        }
        if (t > max_dist) break;
        BlockID b = world_get_block(world, x, y, z);
        if (b != BLOCK_AIR && b != BLOCK_WATER) {
            return (RaycastHit){ .hit = true, .x = x, .y = y, .z = z, .face = last_face };
        }
    }
    return miss;
}
