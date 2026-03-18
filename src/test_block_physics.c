#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "block_physics.h"   /* brings in block.h -> BlockID */

/* ---- Minimal world stubs ---- */
typedef struct World World;  /* already forward-declared in block_physics.h, but re-declaring is fine in C11 */

BlockID world_get_block(World* w, int x, int y, int z)  { (void)w;(void)x;(void)y;(void)z; return BLOCK_AIR; }
bool    world_set_block(World* w, int x, int y, int z, BlockID id) { (void)w;(void)x;(void)y;(void)z;(void)id; return true; }
uint8_t world_get_meta(World* w, int x, int y, int z) { (void)w;(void)x;(void)y;(void)z; return 0; }
bool    world_set_meta(World* w, int x, int y, int z, uint8_t v) { (void)w;(void)x;(void)y;(void)z;(void)v; return true; }

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

int main(void) {
    test_posset_insert_contains();
    test_posset_remove();
    test_posset_iterate();
    test_posset_duplicate_insert();
    test_posset_rehash();
    printf("All PosSet tests passed.\n");
    return 0;
}
