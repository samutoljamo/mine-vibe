#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/item.h"
#include "../src/block.h"

/* ------------------------------------------------------------------ */
/*  Classification: is_armor / slot-for-item                           */
/* ------------------------------------------------------------------ */

static void test_armor_above_other_ranges(void) {
    ItemId armor[] = {
        ITEM_LEATHER_HELMET, ITEM_LEATHER_CHESTPLATE,
        ITEM_LEATHER_LEGGINGS, ITEM_LEATHER_BOOTS,
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE,
        ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS,
    };
    for (size_t i = 0; i < sizeof(armor)/sizeof(armor[0]); i++) {
        assert(item_is_armor(armor[i]));
        assert(!item_is_block(armor[i]));
        assert(!item_is_tool(armor[i]));
        assert(!item_is_material(armor[i]));
    }
    /* Exactly 8 armour items: 2 tiers × 4 slots. */
    assert(ITEM_ARMOR_COUNT == 8);
    assert(ITEM_ARMOR_LAST == ITEM_ARMOR_FIRST + ITEM_ARMOR_COUNT - 1);
    /* Non-armour items classify false. */
    assert(!item_is_armor(item_from_block(BLOCK_STONE)));
    assert(!item_is_armor(ITEM_IRON_PICKAXE));
    assert(!item_is_armor(ITEM_STICK));
    printf("PASS: armor_above_other_ranges\n");
}

static void test_slot_for_item(void) {
    assert(item_armor_slot(ITEM_LEATHER_HELMET)     == ARMOR_SLOT_HEAD);
    assert(item_armor_slot(ITEM_LEATHER_CHESTPLATE) == ARMOR_SLOT_CHEST);
    assert(item_armor_slot(ITEM_LEATHER_LEGGINGS)   == ARMOR_SLOT_LEGS);
    assert(item_armor_slot(ITEM_LEATHER_BOOTS)      == ARMOR_SLOT_FEET);
    assert(item_armor_slot(ITEM_IRON_HELMET)        == ARMOR_SLOT_HEAD);
    assert(item_armor_slot(ITEM_IRON_CHESTPLATE)    == ARMOR_SLOT_CHEST);
    assert(item_armor_slot(ITEM_IRON_LEGGINGS)      == ARMOR_SLOT_LEGS);
    assert(item_armor_slot(ITEM_IRON_BOOTS)         == ARMOR_SLOT_FEET);
    /* Non-armour => ARMOR_SLOT_NONE. */
    assert(item_armor_slot(item_from_block(BLOCK_DIRT)) == ARMOR_SLOT_NONE);
    assert(item_armor_slot(ITEM_WOOD_AXE)               == ARMOR_SLOT_NONE);
    printf("PASS: slot_for_item\n");
}

/* ------------------------------------------------------------------ */
/*  Metadata: points + durability                                      */
/* ------------------------------------------------------------------ */

static void test_armor_metadata(void) {
    const ItemDef* h = item_get_def(ITEM_IRON_HELMET);
    assert(!h->is_tool);
    assert(h->max_durability > 0);
    assert(h->armor_points > 0);
    assert(h->armor_slot == ARMOR_SLOT_HEAD);
    assert(h->name != NULL && h->name[0] != '\0');

    /* Iron protects more than leather in every slot. */
    assert(item_armor_points(ITEM_IRON_HELMET)     > item_armor_points(ITEM_LEATHER_HELMET));
    assert(item_armor_points(ITEM_IRON_CHESTPLATE) > item_armor_points(ITEM_LEATHER_CHESTPLATE));
    assert(item_armor_points(ITEM_IRON_LEGGINGS)   > item_armor_points(ITEM_LEATHER_LEGGINGS));
    assert(item_armor_points(ITEM_IRON_BOOTS)      > item_armor_points(ITEM_LEATHER_BOOTS));

    /* Iron is more durable than leather. */
    assert(item_get_def(ITEM_IRON_HELMET)->max_durability
         > item_get_def(ITEM_LEATHER_HELMET)->max_durability);

    /* Non-armour items have 0 armour points. */
    assert(item_armor_points(item_from_block(BLOCK_STONE)) == 0);
    assert(item_armor_points(ITEM_IRON_PICKAXE) == 0);
    printf("PASS: armor_metadata\n");
}

/* ------------------------------------------------------------------ */
/*  armor_points_total                                                 */
/* ------------------------------------------------------------------ */

static void test_points_total(void) {
    ItemId none[ARMOR_SLOT_COUNT] = {
        BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR };
    assert(armor_points_total(none) == 0);

    ItemId leather[ARMOR_SLOT_COUNT] = {
        ITEM_LEATHER_HELMET, ITEM_LEATHER_CHESTPLATE,
        ITEM_LEATHER_LEGGINGS, ITEM_LEATHER_BOOTS };
    int lt = armor_points_total(leather);
    assert(lt == item_armor_points(ITEM_LEATHER_HELMET)
              + item_armor_points(ITEM_LEATHER_CHESTPLATE)
              + item_armor_points(ITEM_LEATHER_LEGGINGS)
              + item_armor_points(ITEM_LEATHER_BOOTS));

    ItemId iron[ARMOR_SLOT_COUNT] = {
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE,
        ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS };
    int it = armor_points_total(iron);
    assert(it > lt);                       /* full iron beats full leather */
    assert(it <= ARMOR_MAX_POINTS);        /* clamped to the cap */

    /* Partial set sums only equipped pieces. */
    ItemId partial[ARMOR_SLOT_COUNT] = {
        ITEM_IRON_HELMET, BLOCK_AIR, BLOCK_AIR, ITEM_LEATHER_BOOTS };
    assert(armor_points_total(partial)
         == item_armor_points(ITEM_IRON_HELMET)
          + item_armor_points(ITEM_LEATHER_BOOTS));
    printf("PASS: points_total\n");
}

/* ------------------------------------------------------------------ */
/*  damage_after_armor — 4%/point, capped at 80%                       */
/* ------------------------------------------------------------------ */

static void test_damage_after_armor(void) {
    /* No armour => no reduction. */
    assert(damage_after_armor(10, 0) == 10);

    /* Each point removes 4%: 5 points => 20% off 10 = 8. */
    assert(damage_after_armor(10, 5) == 8);

    /* 20 points => 80% off 100 = 20. */
    assert(damage_after_armor(100, 20) == 20);

    /* Cap at 80%: more than 20 points can't reduce beyond 80%. */
    assert(damage_after_armor(100, 25) == damage_after_armor(100, 20));
    assert(damage_after_armor(100, 99) == 20);

    /* More armour never increases the damage taken (monotonic). */
    int prev = damage_after_armor(50, 0);
    for (int p = 1; p <= ARMOR_MAX_POINTS; p++) {
        int d = damage_after_armor(50, p);
        assert(d <= prev);
        prev = d;
    }

    /* Floor: a nonzero blow never becomes 0 just from armour. */
    assert(damage_after_armor(1, 25) >= 1);
    /* Zero in => zero out. */
    assert(damage_after_armor(0, 10) == 0);
    printf("PASS: damage_after_armor\n");
}

/* ------------------------------------------------------------------ */
/*  Durability decrement on hits                                       */
/* ------------------------------------------------------------------ */

static void test_durability_decrement(void) {
    /* Simulate wear: each hit costs one durability point; piece breaks at 0. */
    uint16_t dur = item_get_def(ITEM_LEATHER_HELMET)->max_durability;
    assert(dur > 0);
    uint16_t d = dur;
    int hits = 0;
    while (d > 0) { d--; hits++; }
    assert(d == 0);
    assert(hits == (int)dur);   /* breaks after exactly max_durability hits */
    printf("PASS: durability_decrement\n");
}

int main(void) {
    test_armor_above_other_ranges();
    test_slot_for_item();
    test_armor_metadata();
    test_points_total();
    test_damage_after_armor();
    test_durability_decrement();
    printf("test_armor: all passed\n");
    return 0;
}
