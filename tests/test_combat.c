#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/combat.h"
#include "../src/item.h"
#include "../src/block.h"

/* ------------------------------------------------------------------ */
/*  weapon_damage — sword > tool > fist, higher tier = more damage     */
/* ------------------------------------------------------------------ */

static void test_fist_is_baseline(void) {
    /* Empty hand / non-weapon items deal the bare-hand baseline. */
    assert(weapon_damage(BLOCK_AIR) == COMBAT_FIST_DAMAGE);
    assert(weapon_damage(item_from_block(BLOCK_STONE)) == COMBAT_FIST_DAMAGE);
    assert(weapon_damage(item_from_block(BLOCK_DIRT))  == COMBAT_FIST_DAMAGE);
    assert(weapon_damage(ITEM_STICK)      == COMBAT_FIST_DAMAGE);
    assert(weapon_damage(ITEM_IRON_INGOT) == COMBAT_FIST_DAMAGE);
    assert(weapon_damage(ITEM_RAW_BEEF)   == COMBAT_FIST_DAMAGE);
    /* Armour is not a weapon. */
    assert(weapon_damage(ITEM_IRON_CHESTPLATE) == COMBAT_FIST_DAMAGE);
    printf("PASS: fist_is_baseline\n");
}

static void test_tools_beat_fist(void) {
    /* Any tool used as a weapon beats bare fists. */
    assert(weapon_damage(ITEM_WOOD_PICKAXE) > COMBAT_FIST_DAMAGE);
    assert(weapon_damage(ITEM_WOOD_AXE)     > COMBAT_FIST_DAMAGE);
    assert(weapon_damage(ITEM_WOOD_SHOVEL)  > COMBAT_FIST_DAMAGE);
    assert(weapon_damage(ITEM_IRON_AXE)     > COMBAT_FIST_DAMAGE);
    printf("PASS: tools_beat_fist\n");
}

static void test_axe_beats_pickaxe_and_shovel(void) {
    /* Within a tier, the axe is the better improvised weapon than the
     * pickaxe/shovel (mirrors vanilla's tool-as-weapon ordering). */
    assert(weapon_damage(ITEM_IRON_AXE) > weapon_damage(ITEM_IRON_PICKAXE));
    assert(weapon_damage(ITEM_IRON_AXE) > weapon_damage(ITEM_IRON_SHOVEL));
    assert(weapon_damage(ITEM_STONE_AXE) > weapon_damage(ITEM_STONE_SHOVEL));
    /* Pickaxe is no weaker than a shovel of the same tier. */
    assert(weapon_damage(ITEM_IRON_PICKAXE) >= weapon_damage(ITEM_IRON_SHOVEL));
    printf("PASS: axe_beats_pickaxe_and_shovel\n");
}

static void test_higher_tier_more_damage(void) {
    /* Same kind, higher material tier => more damage. */
    assert(weapon_damage(ITEM_IRON_AXE)  > weapon_damage(ITEM_STONE_AXE));
    assert(weapon_damage(ITEM_STONE_AXE) > weapon_damage(ITEM_WOOD_AXE));

    assert(weapon_damage(ITEM_IRON_PICKAXE)  > weapon_damage(ITEM_STONE_PICKAXE));
    assert(weapon_damage(ITEM_STONE_PICKAXE) > weapon_damage(ITEM_WOOD_PICKAXE));

    assert(weapon_damage(ITEM_IRON_SHOVEL)  > weapon_damage(ITEM_STONE_SHOVEL));
    assert(weapon_damage(ITEM_STONE_SHOVEL) > weapon_damage(ITEM_WOOD_SHOVEL));
    printf("PASS: higher_tier_more_damage\n");
}

/* ------------------------------------------------------------------ */
/*  attack_cooldown — ordering, always positive                        */
/* ------------------------------------------------------------------ */

static void test_cooldown_positive(void) {
    /* Every weapon and the bare hand have a positive, finite cooldown. */
    ItemId all[] = {
        BLOCK_AIR, item_from_block(BLOCK_STONE), ITEM_STICK,
        ITEM_WOOD_PICKAXE, ITEM_WOOD_AXE, ITEM_WOOD_SHOVEL,
        ITEM_STONE_PICKAXE, ITEM_STONE_AXE, ITEM_STONE_SHOVEL,
        ITEM_IRON_PICKAXE, ITEM_IRON_AXE, ITEM_IRON_SHOVEL,
    };
    for (size_t i = 0; i < sizeof(all)/sizeof(all[0]); i++) {
        assert(attack_cooldown(all[i]) > 0.0f);
        assert(isfinite(attack_cooldown(all[i])));
    }
    printf("PASS: cooldown_positive\n");
}

static void test_cooldown_ordering(void) {
    /* The bare hand / non-weapon item recovers fastest. */
    assert(attack_cooldown(BLOCK_AIR) == COMBAT_FIST_COOLDOWN);
    assert(attack_cooldown(item_from_block(BLOCK_STONE)) == COMBAT_FIST_COOLDOWN);

    /* Weapons swing slower than the bare hand. */
    assert(attack_cooldown(ITEM_IRON_AXE)     > COMBAT_FIST_COOLDOWN);
    assert(attack_cooldown(ITEM_IRON_PICKAXE) > COMBAT_FIST_COOLDOWN);

    /* The heavy axe recovers slower than the lighter pickaxe/shovel. */
    assert(attack_cooldown(ITEM_IRON_AXE) > attack_cooldown(ITEM_IRON_PICKAXE));
    assert(attack_cooldown(ITEM_IRON_AXE) > attack_cooldown(ITEM_IRON_SHOVEL));
    printf("PASS: cooldown_ordering\n");
}

/* ------------------------------------------------------------------ */
/*  armor_damage_reduction — wrapper over the armour math              */
/* ------------------------------------------------------------------ */

static void test_no_armor_full_damage(void) {
    ItemId none[ARMOR_SLOT_COUNT] = { BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR };
    assert(armor_damage_reduction(none, 10) == 10);
    assert(armor_damage_reduction(none, 1)  == 1);
    printf("PASS: no_armor_full_damage\n");
}

static void test_armor_reduces_damage(void) {
    ItemId none[ARMOR_SLOT_COUNT]    = { BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR };
    ItemId leather[ARMOR_SLOT_COUNT] = {
        ITEM_LEATHER_HELMET, ITEM_LEATHER_CHESTPLATE,
        ITEM_LEATHER_LEGGINGS, ITEM_LEATHER_BOOTS };
    ItemId iron[ARMOR_SLOT_COUNT]    = {
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE,
        ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS };

    int raw   = 20;
    int dn    = armor_damage_reduction(none, raw);
    int dlt   = armor_damage_reduction(leather, raw);
    int dit   = armor_damage_reduction(iron, raw);

    /* Any armour reduces damage; more/better armour reduces more. */
    assert(dlt < dn);
    assert(dit < dlt);
    printf("PASS: armor_reduces_damage\n");
}

static void test_more_pieces_reduce_more(void) {
    ItemId one[ARMOR_SLOT_COUNT]  = {
        ITEM_IRON_HELMET, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR };
    ItemId two[ARMOR_SLOT_COUNT]  = {
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE, BLOCK_AIR, BLOCK_AIR };
    int raw = 30;
    assert(armor_damage_reduction(two, raw) < armor_damage_reduction(one, raw));
    printf("PASS: more_pieces_reduce_more\n");
}

static void test_full_set_caps_reduction(void) {
    /* A full set hits (but cannot exceed) the 80% cap: 100 -> 20 at worst. */
    ItemId iron[ARMOR_SLOT_COUNT] = {
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE,
        ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS };
    int out = armor_damage_reduction(iron, 100);
    assert(out >= 20);   /* never below the 80%-cap floor */
    /* Reduction matches the direct armour math (no extra fudge). */
    assert(out == damage_after_armor(100, armor_points_total(iron)));
    printf("PASS: full_set_caps_reduction\n");
}

static void test_damage_never_negative(void) {
    ItemId iron[ARMOR_SLOT_COUNT] = {
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE,
        ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS };
    /* Zero in => zero out; a real blow never drops below 1. */
    assert(armor_damage_reduction(iron, 0) == 0);
    assert(armor_damage_reduction(iron, 1) >= 1);
    /* Negative raw is clamped to non-negative. */
    assert(armor_damage_reduction(iron, -5) >= 0);
    printf("PASS: damage_never_negative\n");
}

/* ------------------------------------------------------------------ */
/*  armor_durability_loss — per-hit wear each worn piece takes          */
/* ------------------------------------------------------------------ */

static void test_durability_loss_min_one(void) {
    /* Any real blow costs at least one point of durability per worn piece,
     * even a tiny hit. */
    assert(armor_durability_loss(1) == 1);
    assert(armor_durability_loss(2) == 1);
    assert(armor_durability_loss(3) == 1);
    assert(armor_durability_loss(4) == 1);
    printf("PASS: durability_loss_min_one\n");
}

static void test_durability_loss_scales(void) {
    /* Vanilla rule: floor(damage/4), with a floor of 1 on a real blow. */
    assert(armor_durability_loss(8)  == 2);
    assert(armor_durability_loss(12) == 3);
    assert(armor_durability_loss(20) == 5);
    /* Monotonic non-decreasing in damage. */
    assert(armor_durability_loss(20) >= armor_durability_loss(8));
    printf("PASS: durability_loss_scales\n");
}

static void test_durability_loss_zero_damage(void) {
    /* Zero/negative (non-)damage events cost no durability — armour only
     * wears when a real blow lands. */
    assert(armor_durability_loss(0)  == 0);
    assert(armor_durability_loss(-5) == 0);
    printf("PASS: durability_loss_zero_damage\n");
}

/* ------------------------------------------------------------------ */
/*  knockback_impulse — direction away from attacker, magnitude         */
/* ------------------------------------------------------------------ */

static void test_knockback_direction(void) {
    /* Attacker at origin, target to the +X: shove points +X, no Z. */
    float dx, dz;
    knockback_impulse(0.0f, 0.0f, 5.0f, 0.0f, COMBAT_KNOCKBACK_STRENGTH, &dx, &dz);
    assert(dx > 0.0f);
    assert(fabsf(dz) < 1e-4f);

    /* Target to the -Z: shove points -Z, no X. */
    knockback_impulse(0.0f, 0.0f, 0.0f, -3.0f, COMBAT_KNOCKBACK_STRENGTH, &dx, &dz);
    assert(dz < 0.0f);
    assert(fabsf(dx) < 1e-4f);
    printf("PASS: knockback_direction\n");
}

static void test_knockback_magnitude(void) {
    /* Magnitude equals `strength` regardless of attacker/target distance. */
    float dx, dz;
    knockback_impulse(2.0f, 2.0f, 12.0f, 2.0f, COMBAT_KNOCKBACK_STRENGTH, &dx, &dz);
    float mag = sqrtf(dx * dx + dz * dz);
    assert(fabsf(mag - COMBAT_KNOCKBACK_STRENGTH) < 1e-3f);

    /* A different (diagonal, close) separation still normalises to strength. */
    knockback_impulse(0.0f, 0.0f, 0.3f, 0.4f, 10.0f, &dx, &dz);
    mag = sqrtf(dx * dx + dz * dz);
    assert(fabsf(mag - 10.0f) < 1e-3f);
    printf("PASS: knockback_magnitude\n");
}

static void test_knockback_degenerate(void) {
    /* Attacker and target at the same XZ point: zero impulse, no NaN. */
    float dx = 99.0f, dz = 99.0f;
    knockback_impulse(4.0f, 4.0f, 4.0f, 4.0f, COMBAT_KNOCKBACK_STRENGTH, &dx, &dz);
    assert(dx == 0.0f && dz == 0.0f);
    printf("PASS: knockback_degenerate\n");
}

int main(void) {
    test_fist_is_baseline();
    test_tools_beat_fist();
    test_axe_beats_pickaxe_and_shovel();
    test_higher_tier_more_damage();
    test_cooldown_positive();
    test_cooldown_ordering();
    test_no_armor_full_damage();
    test_armor_reduces_damage();
    test_more_pieces_reduce_more();
    test_full_set_caps_reduction();
    test_damage_never_negative();
    test_durability_loss_min_one();
    test_durability_loss_scales();
    test_durability_loss_zero_damage();
    test_knockback_direction();
    test_knockback_magnitude();
    test_knockback_degenerate();
    printf("test_combat: all passed\n");
    return 0;
}
