#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "block_physics.h"   /* brings in block.h -> BlockID */

/* ---- Stateful world stub ---- */
typedef struct World World;

#define TWS 32  /* test world XZ size */
#define TWH 64  /* test world Y size  */

static BlockID g_blocks[TWS][TWH][TWS];
static uint8_t g_meta[TWS][TWH][TWS];

static void tworld_reset(void) {
    memset(g_blocks, 0, sizeof(g_blocks));
    memset(g_meta,   0, sizeof(g_meta));
}

BlockID world_get_block(World* w, int x, int y, int z) {
    (void)w;
    if (x < 0 || x >= TWS || y < 0 || y >= TWH || z < 0 || z >= TWS)
        return BLOCK_AIR;
    return g_blocks[x][y][z];
}
bool world_set_block(World* w, int x, int y, int z, BlockID id) {
    (void)w;
    if (x < 0 || x >= TWS || y < 0 || y >= TWH || z < 0 || z >= TWS)
        return false;
    g_blocks[x][y][z] = id;
    return true;
}
uint8_t world_get_meta(World* w, int x, int y, int z) {
    (void)w;
    if (x < 0 || x >= TWS || y < 0 || y >= TWH || z < 0 || z >= TWS)
        return 0;
    return g_meta[x][y][z];
}
bool world_set_meta(World* w, int x, int y, int z, uint8_t v) {
    (void)w;
    if (x < 0 || x >= TWS || y < 0 || y >= TWH || z < 0 || z >= TWS)
        return false;
    g_meta[x][y][z] = v;
    return true;
}

/* ---- PosSet tests ---- */

static void test_posset_insert_contains(void) {
    PosSet s;
    posset_init(&s);

    assert(!posset_contains(&s, 0, 0, 0));
    posset_insert(&s, 0, 0, 0);
    assert(posset_contains(&s, 0, 0, 0));

    posset_insert(&s, -100, 64, 200);
    assert(posset_contains(&s, -100, 64, 200));
    assert(!posset_contains(&s, -100, 64, 201));

    /* Negative coordinates */
    posset_insert(&s, -1, 0, -1);
    assert(posset_contains(&s, -1, 0, -1));
    assert(!posset_contains(&s, 1, 0, 1));

    assert(posset_count(&s) == 3);
    posset_destroy(&s);
    printf("PASS: test_posset_insert_contains\n");
}

static void test_posset_remove(void) {
    PosSet s;
    posset_init(&s);

    posset_insert(&s, 10, 5, 20);
    assert(posset_contains(&s, 10, 5, 20));
    posset_remove(&s, 10, 5, 20);
    assert(!posset_contains(&s, 10, 5, 20));
    assert(posset_count(&s) == 0);

    /* Remove non-existent — should be a no-op */
    posset_remove(&s, 99, 99, 99);
    assert(posset_count(&s) == 0);

    posset_destroy(&s);
    printf("PASS: test_posset_remove\n");
}

static void test_posset_iterate(void) {
    PosSet s;
    posset_init(&s);

    posset_insert(&s, 1, 2, 3);
    posset_insert(&s, 4, 5, 6);
    posset_insert(&s, 7, 8, 9);

    int found_count = 0;
    int idx = 0;
    int x, y, z;
    while (posset_iter_next(&s, &idx, &x, &y, &z)) {
        found_count++;
        idx++;
    }
    assert(found_count == 3);

    posset_destroy(&s);
    printf("PASS: test_posset_iterate\n");
}

static void test_posset_duplicate_insert(void) {
    PosSet s;
    posset_init(&s);

    posset_insert(&s, 5, 5, 5);
    posset_insert(&s, 5, 5, 5); /* duplicate */
    assert(posset_count(&s) == 1);

    posset_destroy(&s);
    printf("PASS: test_posset_duplicate_insert\n");
}

static void test_posset_rehash(void) {
    PosSet s;
    posset_init(&s);

    /* Insert enough to trigger rehash (> 70% of 4096 = 2868) */
    for (int i = 0; i < 3000; i++) {
        posset_insert(&s, i, i % 256, i * 2);
    }
    assert(posset_count(&s) == 3000);

    for (int i = 0; i < 3000; i++) {
        assert(posset_contains(&s, i, i % 256, i * 2));
    }

    posset_destroy(&s);
    printf("PASS: test_posset_rehash\n");
}

/* ---- Gravity tests ---- */

/* Helper: fire block_physics_update enough to trigger exactly N gravity ticks */
static void run_gravity_ticks(BlockPhysics* bp, int n) {
    float dt = (float)n / (float)GRAVITY_TICK_HZ + 0.001f;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    block_physics_update(bp, NULL, origin, dt);
}

static void test_gravity_falls_one_step(void) {
    tworld_reset();
    /* Sand at (5, 10, 5), air at (5, 9, 5) */
    g_blocks[5][10][5] = BLOCK_SAND;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_gravity_ticks(&bp, 1);

    assert(g_blocks[5][10][5] == BLOCK_AIR);
    assert(g_blocks[5][9][5]  == BLOCK_SAND);

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_falls_one_step\n");
}

static void test_gravity_stops_on_ground(void) {
    tworld_reset();
    /* Sand at (5, 10, 5), stone at (5, 9, 5) — sand should not move */
    g_blocks[5][10][5] = BLOCK_SAND;
    g_blocks[5][9][5]  = BLOCK_STONE;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_gravity_ticks(&bp, 1);

    assert(g_blocks[5][10][5] == BLOCK_SAND);  /* stayed */
    assert(g_blocks[5][9][5]  == BLOCK_STONE); /* unchanged */

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_stops_on_ground\n");
}

static void test_gravity_falls_multiple_steps(void) {
    tworld_reset();
    /* Sand at (5, 10, 5), air from y=0 to y=9 — should fall 3 steps */
    g_blocks[5][10][5] = BLOCK_SAND;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_gravity_ticks(&bp, 3);

    /* After 3 ticks the sand should have moved down 3 */
    assert(g_blocks[5][10][5] == BLOCK_AIR);
    assert(g_blocks[5][9][5]  == BLOCK_AIR);
    assert(g_blocks[5][8][5]  == BLOCK_AIR);
    assert(g_blocks[5][7][5]  == BLOCK_SAND);

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_falls_multiple_steps\n");
}

static void test_gravity_non_gravity_block_ignored(void) {
    tworld_reset();
    /* Stone at (5, 10, 5) — should not fall */
    g_blocks[5][10][5] = BLOCK_STONE;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_gravity_ticks(&bp, 1);

    assert(g_blocks[5][10][5] == BLOCK_STONE); /* stayed */
    assert(g_blocks[5][9][5]  == BLOCK_AIR);

    block_physics_destroy(&bp);
    printf("PASS: test_gravity_non_gravity_block_ignored\n");
}

/* ---- Water tests ---- */

static void run_water_ticks(BlockPhysics* bp, int n) {
    float dt = (float)n / (float)WATER_TICK_HZ + 0.001f;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    block_physics_update(bp, NULL, origin, dt);
}

static void test_water_source_flows_down(void) {
    tworld_reset();
    /* Source at (5, 10, 5), air at (5, 9, 5) */
    g_blocks[5][10][5] = BLOCK_WATER;
    g_meta[5][10][5]   = WATER_SOURCE_LEVEL;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_water_ticks(&bp, 1);

    /* Source unchanged */
    assert(g_blocks[5][10][5] == BLOCK_WATER);
    assert(g_meta[5][10][5]   == WATER_SOURCE_LEVEL);
    /* Water flowed down */
    assert(g_blocks[5][9][5] == BLOCK_WATER);
    assert(g_meta[5][9][5]   == WATER_SOURCE_LEVEL - 1);

    block_physics_destroy(&bp);
    printf("PASS: test_water_source_flows_down\n");
}

static void test_water_source_flows_horizontal(void) {
    tworld_reset();
    /* Source at (5, 10, 5), solid stone at y=9 to prevent downward flow */
    g_blocks[5][10][5] = BLOCK_WATER;
    g_meta[5][10][5]   = WATER_SOURCE_LEVEL;
    g_blocks[5][9][5]  = BLOCK_STONE; /* floor */

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_water_ticks(&bp, 1);

    /* Source unchanged */
    assert(g_blocks[5][10][5] == BLOCK_WATER);
    assert(g_meta[5][10][5]   == WATER_SOURCE_LEVEL);
    /* Water spread to at least one horizontal neighbor */
    bool spread = (g_blocks[6][10][5] == BLOCK_WATER) ||
                  (g_blocks[4][10][5] == BLOCK_WATER) ||
                  (g_blocks[5][10][6] == BLOCK_WATER) ||
                  (g_blocks[5][10][4] == BLOCK_WATER);
    assert(spread);

    block_physics_destroy(&bp);
    printf("PASS: test_water_source_flows_horizontal\n");
}

static void test_water_flowing_dissipates(void) {
    tworld_reset();
    /* Flowing water with a low level — should vanish after enough ticks */
    g_blocks[5][10][5] = BLOCK_WATER;
    g_meta[5][10][5]   = 4; /* small level: 4 -> 2 -> 0 (gone after 2 ticks) */
    /* Floor below so it can't flow down */
    g_blocks[5][9][5]  = BLOCK_STONE;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    /* After 2 water ticks the block should be gone */
    run_water_ticks(&bp, 2);

    assert(g_blocks[5][10][5] == BLOCK_AIR);

    block_physics_destroy(&bp);
    printf("PASS: test_water_flowing_dissipates\n");
}

static void test_water_source_never_dissipates(void) {
    tworld_reset();
    /* Source should still be present after many ticks */
    g_blocks[5][10][5] = BLOCK_WATER;
    g_meta[5][10][5]   = WATER_SOURCE_LEVEL;
    /* Contain it: floor and stone walls */
    g_blocks[5][9][5]  = BLOCK_STONE;
    g_blocks[6][10][5] = BLOCK_STONE;
    g_blocks[4][10][5] = BLOCK_STONE;
    g_blocks[5][10][6] = BLOCK_STONE;
    g_blocks[5][10][4] = BLOCK_STONE;

    BlockPhysics bp;
    block_physics_init(&bp);
    block_physics_notify(&bp, 5, 10, 5);

    run_water_ticks(&bp, 20);

    assert(g_blocks[5][10][5] == BLOCK_WATER);
    assert(g_meta[5][10][5]   == WATER_SOURCE_LEVEL);

    block_physics_destroy(&bp);
    printf("PASS: test_water_source_never_dissipates\n");
}

int main(void) {
    test_posset_insert_contains();
    test_posset_remove();
    test_posset_iterate();
    test_posset_duplicate_insert();
    test_posset_rehash();
    printf("All PosSet tests passed.\n");

    test_gravity_falls_one_step();
    test_gravity_stops_on_ground();
    test_gravity_falls_multiple_steps();
    test_gravity_non_gravity_block_ignored();
    printf("All gravity tests passed.\n");

    test_water_source_flows_down();
    test_water_source_flows_horizontal();
    test_water_flowing_dissipates();
    test_water_source_never_dissipates();
    printf("All water tests passed.\n");
    return 0;
}
