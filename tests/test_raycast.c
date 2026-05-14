#include "../src/raycast.h"
#include "../src/world.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Minimal world stub matching the symbol signature used by raycast_voxel. */
static BlockID g_grid[16][16][16];   /* [x][y][z], BLOCK_AIR == 0 */

BlockID world_get_block(World* w, int x, int y, int z) {
    (void)w;
    if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 16)
        return BLOCK_AIR;
    return g_grid[x][y][z];
}

static void clear_grid(void) { memset(g_grid, 0, sizeof(g_grid)); }
static void set_block(int x, int y, int z, BlockID b) { g_grid[x][y][z] = b; }

static void test_face_offset(void) {
    int dx, dy, dz;
    block_face_offset(FACE_PX, &dx, &dy, &dz); assert(dx ==  1 && dy == 0 && dz == 0);
    block_face_offset(FACE_NX, &dx, &dy, &dz); assert(dx == -1 && dy == 0 && dz == 0);
    block_face_offset(FACE_PY, &dx, &dy, &dz); assert(dx ==  0 && dy == 1 && dz == 0);
    block_face_offset(FACE_NY, &dx, &dy, &dz); assert(dx ==  0 && dy ==-1 && dz == 0);
    block_face_offset(FACE_PZ, &dx, &dy, &dz); assert(dx ==  0 && dy == 0 && dz == 1);
    block_face_offset(FACE_NZ, &dx, &dy, &dz); assert(dx ==  0 && dy == 0 && dz ==-1);
}

static void test_hit_along_x(void) {
    clear_grid();
    set_block(5, 0, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 5 && h.y == 0 && h.z == 0);
    assert(h.face == FACE_NX);
}

static void test_hit_along_negative_y(void) {
    clear_grid();
    set_block(0, 3, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 8.5f, 0.5f};
    vec3 dir    = {0.0f, -1.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 0 && h.y == 3 && h.z == 0);
    assert(h.face == FACE_PY);
}

static void test_no_hit_through_air(void) {
    clear_grid();
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 4.0f);
    assert(!h.hit);
}

static void test_max_distance_terminates(void) {
    clear_grid();
    set_block(10, 0, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 5.0f);
    assert(!h.hit);
}

static void test_water_is_not_a_hit(void) {
    clear_grid();
    set_block(5, 0, 0, BLOCK_WATER);
    set_block(7, 0, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 7);
}

static void test_diagonal_ray_hits(void) {
    /* Diagonal ray from (0.5,0.5,0.5) toward (1,1,1).  Block placed at (3,3,3). */
    clear_grid();
    set_block(3, 3, 3, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 1.0f, 1.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 3 && h.y == 3 && h.z == 3);
    /* Face is whichever axis the DDA crossed last — we don't lock the choice
     * here, only that one of the three -axis entries is reported. */
    assert(h.face == FACE_NX || h.face == FACE_NY || h.face == FACE_NZ);
}

static void test_starts_inside_block(void) {
    clear_grid();
    set_block(2, 2, 2, BLOCK_STONE);
    vec3 origin = {2.5f, 2.5f, 2.5f};   /* inside the block */
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 2 && h.y == 2 && h.z == 2);
    assert(h.face == FACE_PY);   /* documented eject direction */
}

int main(void) {
    test_face_offset();
    test_hit_along_x();
    test_hit_along_negative_y();
    test_no_hit_through_air();
    test_max_distance_terminates();
    test_water_is_not_a_hit();
    test_diagonal_ray_hits();
    test_starts_inside_block();
    printf("test_raycast: all passed\n");
    return 0;
}
