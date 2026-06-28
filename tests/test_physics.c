#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/physics.h"
#include "../src/block.h"
#include "../src/gameplay.h"

/* -------------------------------------------------------------------------
 * In-memory voxel grid as the block source for the pure collision core.
 * Coordinates outside the grid read as BLOCK_AIR (matching world_get_block's
 * "unloaded / out of range = air" contract). The grid is small and centred so
 * tests can place floors/walls at concrete integer cells.
 * ------------------------------------------------------------------------- */
#define GW 64   /* width  (x, z) */
#define GH 64   /* height (y)    */
#define ORIGIN 32  /* world coord that maps to grid index 0 on x/z */

typedef struct Grid {
    BlockID cells[GW][GH][GW]; /* [x][y][z] */
} Grid;

static Grid g_grid;

static void grid_clear(void) { memset(&g_grid, 0, sizeof(g_grid)); /* all AIR */ }

static void grid_set(int x, int y, int z, BlockID b) {
    int gx = x + ORIGIN, gz = z + ORIGIN;
    if (gx < 0 || gx >= GW || y < 0 || y >= GH || gz < 0 || gz >= GW) return;
    g_grid.cells[gx][y][gz] = b;
}

static BlockID grid_query(int x, int y, int z, void* ctx) {
    (void)ctx;
    int gx = x + ORIGIN, gz = z + ORIGIN;
    if (gx < 0 || gx >= GW || y < 0 || y >= GH || gz < 0 || gz >= GW)
        return BLOCK_AIR;
    return g_grid.cells[gx][y][gz];
}

/* Lay a solid floor (stone) over the whole grid at world-y = floor_y. */
static void grid_floor(int floor_y) {
    for (int gx = 0; gx < GW; gx++)
        for (int gz = 0; gz < GW; gz++)
            g_grid.cells[gx][floor_y][gz] = BLOCK_STONE;
}

#define HW PLAYER_HALF_W
#define HT PLAYER_HEIGHT

/* ------------------------------------------------------------------ */

/* Falling onto a floor: the feet snap to the top face of the floor block
 * and on_ground becomes true, velocity zeroed. */
static void test_land_on_floor(void) {
    grid_clear();
    grid_floor(10);                 /* solid block occupies y in [10,11) */
    vec3 pos = { 0.0f, 12.0f, 0.0f };
    vec3 vel = { 0.0f, -5.0f, 0.0f };
    PhysicsResult r = physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false,
                                     grid_query, NULL);
    /* keep stepping until it settles */
    for (int i = 0; i < 200; i++)
        r = physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
    assert(fabsf(pos[1] - 11.0f) < 1e-4f);   /* feet rest on top of floor */
    assert(r.on_ground);
    assert(fabsf(vel[1]) < 1e-4f);           /* vertical velocity killed */
    printf("PASS: land_on_floor\n");
}

/* A stationary AABB resting on a floor reports on_ground even with zero
 * vertical velocity (the no-op Y-sweep "stably standing" case). */
static void test_grounded_detection_stationary(void) {
    grid_clear();
    grid_floor(10);
    vec3 pos = { 0.0f, 11.0f, 0.0f };  /* already resting on floor */
    vec3 vel = { 0.0f, 0.0f, 0.0f };
    PhysicsResult r = physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false,
                                     grid_query, NULL);
    assert(r.on_ground);
    assert(fabsf(pos[1] - 11.0f) < 1e-4f);
    printf("PASS: grounded_detection_stationary\n");
}

/* Hovering one block above the floor is NOT grounded. */
static void test_not_grounded_in_air(void) {
    grid_clear();
    grid_floor(10);
    vec3 pos = { 0.0f, 13.0f, 0.0f };
    vec3 vel = { 0.0f, 0.0f, 0.0f };
    PhysicsResult r = physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false,
                                     grid_query, NULL);
    assert(!r.on_ground);
    printf("PASS: not_grounded_in_air\n");
}

/* Horizontal sweep into a wall stops at the wall face; the player does not
 * penetrate the solid block. */
static void test_wall_stops_x(void) {
    grid_clear();
    grid_floor(10);
    /* Wall column at x=2 from y=11..14 */
    for (int y = 11; y <= 14; y++) grid_set(2, y, 0, BLOCK_STONE);
    vec3 pos = { 0.0f, 11.0f, 0.0f };
    vec3 vel = { 10.0f, 0.0f, 0.0f };   /* move +x toward wall */
    for (int i = 0; i < 120; i++)
        physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
    /* AABB max edge (pos+half_w) must not pass the wall's near face (x=2). */
    assert(pos[0] + HW <= 2.0f + 1e-4f);
    assert(fabsf(vel[0]) < 1e-4f);      /* x velocity zeroed by collision */
    printf("PASS: wall_stops_x  (final x=%.4f)\n", (double)pos[0]);
}

/* Same on the Z axis, moving in the negative direction toward a wall. */
static void test_wall_stops_z_negative(void) {
    grid_clear();
    grid_floor(10);
    for (int y = 11; y <= 14; y++) grid_set(0, y, -3, BLOCK_STONE);
    vec3 pos = { 0.0f, 11.0f, 0.0f };
    vec3 vel = { 0.0f, 0.0f, -10.0f };
    for (int i = 0; i < 120; i++)
        physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
    /* Near face of the wall block (x covers world z in [-3,-2)) is z=-2.
     * AABB min edge (pos-half_w) must stay >= -2. */
    assert(pos[2] - HW >= -2.0f - 1e-4f);
    assert(fabsf(vel[2]) < 1e-4f);
    printf("PASS: wall_stops_z_negative  (final z=%.4f)\n", (double)pos[2]);
}

/* No tunneling: a single very large downward delta (well past terminal
 * velocity) must NOT skip through a 1-block-thick floor. The sub-stepper
 * exists precisely to prevent this. */
static void test_no_tunneling_through_thin_floor(void) {
    grid_clear();
    /* Single thin floor at y=10, nothing below it. */
    grid_floor(10);
    vec3 pos = { 0.0f, 40.0f, 0.0f };
    vec3 vel = { 0.0f, -300.0f, 0.0f };   /* absurd speed, ~5 blocks/tick */
    PhysicsResult r = {0};
    for (int i = 0; i < 60; i++)
        r = physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
    /* Must have been caught by the floor, never fallen below it. */
    assert(pos[1] >= 11.0f - 1e-3f);
    assert(r.on_ground);
    printf("PASS: no_tunneling_through_thin_floor  (final y=%.4f)\n", (double)pos[1]);
}

/* Head-bump: moving up into a ceiling stops at the ceiling and zeroes upward
 * velocity (the delta>0 Y branch). */
static void test_ceiling_stops_upward(void) {
    grid_clear();
    grid_floor(10);
    /* Ceiling block at y=15 above the player column. */
    grid_set(0, 15, 0, BLOCK_STONE);
    vec3 pos = { 0.0f, 11.0f, 0.0f };
    vec3 vel = { 0.0f, 50.0f, 0.0f };
    for (int i = 0; i < 60; i++)
        physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
    /* Head (pos + height) must not pass the ceiling's bottom face (y=15). */
    assert(pos[1] + HT <= 15.0f + 1e-3f);
    printf("PASS: ceiling_stops_upward  (final y=%.4f)\n", (double)pos[1]);
}

/* Water detection: feet submerged in water sets in_water; air does not. */
static void test_water_detection(void) {
    grid_clear();
    /* Fill a 3x3x3 pocket of water around the origin feet. */
    for (int y = 10; y <= 12; y++)
        for (int x = -1; x <= 1; x++)
            for (int z = -1; z <= 1; z++)
                grid_set(x, y, z, BLOCK_WATER);
    vec3 pos = { 0.0f, 11.0f, 0.0f };
    assert(physics_check_water_q(pos, HW, HT, grid_query, NULL));

    grid_clear();
    assert(!physics_check_water_q(pos, HW, HT, grid_query, NULL));
    /* Water is not solid: an AABB inside water is not pushed out. */
    printf("PASS: water_detection\n");
}

/* Water blocks are non-solid: a downward sweep through water does not stop. */
static void test_water_is_not_collidable(void) {
    grid_clear();
    for (int y = 0; y <= 12; y++)
        for (int x = -1; x <= 1; x++)
            for (int z = -1; z <= 1; z++)
                grid_set(x, y, z, BLOCK_WATER);
    vec3 pos = { 0.0f, 12.0f, 0.0f };
    vec3 vel = { 0.0f, -2.0f, 0.0f };
    physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
    assert(pos[1] < 12.0f);  /* sank, not stopped */
    printf("PASS: water_is_not_collidable\n");
}

/* Crouch edge protection: a crouching player standing on the very edge of a
 * single supporting block is prevented from walking off into the void; a
 * non-crouching player is not. */
static void test_crouch_edge_protection(void) {
    grid_clear();
    /* One supporting block under origin (occupies world x,z in [0,1)). */
    grid_set(0, 10, 0, BLOCK_STONE);

    /* Start standing on top of that block, centred over it. */
    vec3 start = { 0.5f, 11.0f, 0.5f };

    /* Non-crouching: a strong +x push WILL carry the player off the block. */
    {
        vec3 pos; glm_vec3_copy(start, pos);
        vec3 vel = { 8.0f, 0.0f, 0.0f };
        for (int i = 0; i < 60; i++)
            physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, false, grid_query, NULL);
        assert(pos[0] > 1.0f);   /* moved off the block's +x edge */
    }

    /* Crouching: same push is refused on the axis that would unsupport. */
    {
        vec3 pos; glm_vec3_copy(start, pos);
        vec3 vel = { 8.0f, 0.0f, 0.0f };
        for (int i = 0; i < 60; i++)
            physics_move_q(pos, vel, HW, HT, 1.0f/60.0f, true, grid_query, NULL);
        /* Stayed supported: AABB still overlaps the block in x. */
        assert(pos[0] - HW < 1.0f && pos[0] + HW > 0.0f);
    }
    printf("PASS: crouch_edge_protection\n");
}

int main(void) {
    test_land_on_floor();
    test_grounded_detection_stationary();
    test_not_grounded_in_air();
    test_wall_stops_x();
    test_wall_stops_z_negative();
    test_no_tunneling_through_thin_floor();
    test_ceiling_stops_upward();
    test_water_detection();
    test_water_is_not_collidable();
    test_crouch_edge_protection();
    printf("ALL PHYSICS TESTS PASSED\n");
    return 0;
}
