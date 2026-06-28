#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include "../src/mob_ai.h"

/* ---- Passive spawn-type classification ---- */

static void test_passive_spawn_types(void) {
    assert(mob_ai_is_passive_spawn_type(MOB_PIG)     == true);
    assert(mob_ai_is_passive_spawn_type(MOB_COW)     == true);
    assert(mob_ai_is_passive_spawn_type(MOB_CHICKEN) == true);
    assert(mob_ai_is_passive_spawn_type(MOB_ZOMBIE)   == false);
    assert(mob_ai_is_passive_spawn_type(MOB_SKELETON) == false);
    assert(mob_ai_is_passive_spawn_type(MOB_CREEPER)  == false);

    /* The advertised passive-type list is exactly the farm animals. */
    assert(PASSIVE_TYPE_COUNT == 3);
    for (int i = 0; i < PASSIVE_TYPE_COUNT; i++)
        assert(mob_ai_is_passive_spawn_type(MOB_PASSIVE_TYPES[i]));
    printf("PASS: passive_spawn_types\n");
}

/* ---- Eligibility gating ---- */

static void test_spawn_ok_all_conditions(void) {
    /* Ideal: day, grass, empty world, no nearby players. */
    assert(mob_ai_passive_spawn_ok(true, true, 0, FLT_MAX) == true);
    assert(mob_ai_passive_spawn_ok(true, true, 0,
                                   PASSIVE_MIN_PLAYER_DIST + 1.0f) == true);
    printf("PASS: spawn_ok_all_conditions\n");
}

static void test_spawn_blocked_at_night(void) {
    assert(mob_ai_passive_spawn_ok(false, true, 0, FLT_MAX) == false);
    printf("PASS: spawn_blocked_at_night\n");
}

static void test_spawn_blocked_off_grass(void) {
    assert(mob_ai_passive_spawn_ok(true, false, 0, FLT_MAX) == false);
    printf("PASS: spawn_blocked_off_grass\n");
}

static void test_spawn_blocked_at_cap(void) {
    /* Room for a full minimum herd is fine. */
    assert(mob_ai_passive_spawn_ok(true, true,
                                   PASSIVE_CAP - PASSIVE_HERD_MIN, FLT_MAX) == true);
    /* One more and the minimum herd would overflow the cap. */
    assert(mob_ai_passive_spawn_ok(true, true,
                                   PASSIVE_CAP - PASSIVE_HERD_MIN + 1, FLT_MAX) == false);
    assert(mob_ai_passive_spawn_ok(true, true, PASSIVE_CAP, FLT_MAX) == false);
    printf("PASS: spawn_blocked_at_cap\n");
}

static void test_spawn_blocked_near_player(void) {
    assert(mob_ai_passive_spawn_ok(true, true, 0,
                                   PASSIVE_MIN_PLAYER_DIST - 0.5f) == false);
    /* Exactly at the threshold counts as far enough. */
    assert(mob_ai_passive_spawn_ok(true, true, 0,
                                   PASSIVE_MIN_PLAYER_DIST) == true);
    printf("PASS: spawn_blocked_near_player\n");
}

/* ---- Herd sizing ---- */

static void test_herd_size_bounds(void) {
    for (uint32_t r = 0; r < 64; r++) {
        int n = mob_ai_herd_size(0, r);
        assert(n >= PASSIVE_HERD_MIN && n <= PASSIVE_HERD_MAX);
    }
    /* Limited room clamps the herd to what fits. */
    int room1 = mob_ai_herd_size(PASSIVE_CAP - 1, 12345);
    assert(room1 == 1);
    /* No room at all -> 0. */
    assert(mob_ai_herd_size(PASSIVE_CAP, 999) == 0);
    assert(mob_ai_herd_size(PASSIVE_CAP + 5, 1) == 0);
    /* Determinism. */
    assert(mob_ai_herd_size(0, 7) == mob_ai_herd_size(0, 7));
    printf("PASS: herd_size_bounds\n");
}

static void test_herd_type_is_passive(void) {
    for (uint32_t r = 0; r < 64; r++)
        assert(mob_ai_is_passive_spawn_type(mob_ai_herd_type(r)));
    /* Determinism + coverage: at least two distinct species appear. */
    MobType a = mob_ai_herd_type(0);
    MobType b = mob_ai_herd_type(1);
    MobType c = mob_ai_herd_type(2);
    assert(a == mob_ai_herd_type(0));
    assert(a != b || a != c || b != c);
    printf("PASS: herd_type_is_passive\n");
}

/* ---- Loot tables ---- */

static void test_loot_cow_drops_leather(void) {
    MobLootDrop out[MOB_LOOT_MAX];
    int n = mob_loot(MOB_COW, out);
    /* Cow must drop leather (the one loot item that exists today). */
    int found_leather = 0;
    for (int i = 0; i < n; i++) {
        assert(out[i].count >= 1);
        if (out[i].item == ITEM_LEATHER) found_leather = 1;
    }
    assert(found_leather == 1);
    printf("PASS: loot_cow_drops_leather\n");
}

static void test_loot_only_existing_items(void) {
    /* Every emitted item id must be a real, in-range item. No drop may name a
     * not-yet-modelled food/material id. */
    MobType all[MOB_TYPE_COUNT] = {
        MOB_ZOMBIE, MOB_SKELETON, MOB_CREEPER, MOB_PIG, MOB_COW, MOB_CHICKEN
    };
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        MobLootDrop out[MOB_LOOT_MAX];
        int n = mob_loot(all[t], out);
        assert(n >= 0 && n <= MOB_LOOT_MAX);
        for (int i = 0; i < n; i++) {
            assert(out[i].item < ITEM_COUNT);
            assert(out[i].count >= 1);
        }
    }
    printf("PASS: loot_only_existing_items\n");
}

/* Helper: does this mob's loot contain `item`? */
static int loot_has(MobType type, ItemId item) {
    MobLootDrop out[MOB_LOOT_MAX];
    int n = mob_loot(type, out);
    for (int i = 0; i < n; i++)
        if (out[i].item == item) return 1;
    return 0;
}

static void test_loot_per_mob_type(void) {
    /* Pig -> raw pork. */
    assert(loot_has(MOB_PIG, ITEM_RAW_PORK));
    /* Cow -> raw beef + leather. */
    assert(loot_has(MOB_COW, ITEM_RAW_BEEF));
    assert(loot_has(MOB_COW, ITEM_LEATHER));
    /* Chicken -> raw chicken + feather. */
    assert(loot_has(MOB_CHICKEN, ITEM_RAW_CHICKEN));
    assert(loot_has(MOB_CHICKEN, ITEM_FEATHER));
    /* Zombie -> rotten flesh. */
    assert(loot_has(MOB_ZOMBIE, ITEM_ROTTEN_FLESH));
    /* Skeleton -> bone + arrow. */
    assert(loot_has(MOB_SKELETON, ITEM_BONE));
    assert(loot_has(MOB_SKELETON, ITEM_ARROW));
    /* Creeper -> gunpowder. */
    assert(loot_has(MOB_CREEPER, ITEM_GUNPOWDER));
    printf("PASS: loot_per_mob_type\n");
}

static void test_every_mob_drops_something(void) {
    MobType all[MOB_TYPE_COUNT] = {
        MOB_ZOMBIE, MOB_SKELETON, MOB_CREEPER, MOB_PIG, MOB_COW, MOB_CHICKEN
    };
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        MobLootDrop out[MOB_LOOT_MAX];
        int n = mob_loot(all[t], out);
        assert(n >= 1 && n <= MOB_LOOT_MAX);
    }
    printf("PASS: every_mob_drops_something\n");
}

static void test_loot_determinism(void) {
    MobLootDrop a[MOB_LOOT_MAX], b[MOB_LOOT_MAX];
    int na = mob_loot(MOB_COW, a);
    int nb = mob_loot(MOB_COW, b);
    assert(na == nb);
    for (int i = 0; i < na; i++) {
        assert(a[i].item == b[i].item);
        assert(a[i].count == b[i].count);
    }
    printf("PASS: loot_determinism\n");
}

int main(void) {
    test_passive_spawn_types();
    test_spawn_ok_all_conditions();
    test_spawn_blocked_at_night();
    test_spawn_blocked_off_grass();
    test_spawn_blocked_at_cap();
    test_spawn_blocked_near_player();
    test_herd_size_bounds();
    test_herd_type_is_passive();
    test_loot_cow_drops_leather();
    test_loot_only_existing_items();
    test_loot_per_mob_type();
    test_every_mob_drops_something();
    test_loot_determinism();
    printf("All mob_ai tests passed.\n");
    return 0;
}
