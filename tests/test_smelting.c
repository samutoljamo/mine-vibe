#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/smelting.h"
#include "../src/item.h"
#include "../src/block.h"

/* ---- recipe lookups ------------------------------------------------ */

static void test_recipe_valid(void) {
    ItemId out = 0xFFFF;

    /* iron ore -> iron ingot */
    assert(smelting_result((ItemId)BLOCK_IRON_ORE, &out));
    assert(out == ITEM_IRON_INGOT);

    /* sand -> glass */
    out = 0xFFFF;
    assert(smelting_result((ItemId)BLOCK_SAND, &out));
    assert(out == (ItemId)BLOCK_GLASS);

    /* raw foods -> cooked foods */
    out = 0xFFFF;
    assert(smelting_result(ITEM_RAW_PORK, &out));
    assert(out == ITEM_COOKED_PORK);
    out = 0xFFFF;
    assert(smelting_result(ITEM_RAW_BEEF, &out));
    assert(out == ITEM_COOKED_BEEF);
    out = 0xFFFF;
    assert(smelting_result(ITEM_RAW_CHICKEN, &out));
    assert(out == ITEM_COOKED_CHICKEN);

    /* NULL out is allowed (smeltability query only). */
    assert(smelting_result((ItemId)BLOCK_IRON_ORE, NULL));
    printf("PASS: recipe_valid\n");
}

static void test_recipe_invalid(void) {
    ItemId out = 0xFFFF;
    /* dirt is not smeltable */
    assert(!smelting_result((ItemId)BLOCK_DIRT, &out));
    assert(out == 0xFFFF);  /* untouched on failure */

    /* a cooked food is not itself smeltable */
    assert(!smelting_result(ITEM_COOKED_PORK, NULL));
    /* air is not smeltable */
    assert(!smelting_result((ItemId)BLOCK_AIR, NULL));
    /* a tool is not smeltable */
    assert(!smelting_result(ITEM_WOOD_PICKAXE, NULL));
    printf("PASS: recipe_invalid\n");
}

/* ---- fuel values --------------------------------------------------- */

static void test_fuel_values(void) {
    /* coal (ore block stands in for coal) is fuel */
    assert(fuel_burn_ticks((ItemId)BLOCK_COAL_ORE) > 0);
    /* wood and planks are fuel */
    assert(fuel_burn_ticks((ItemId)BLOCK_WOOD) > 0);
    assert(fuel_burn_ticks((ItemId)BLOCK_PLANKS) > 0);
    /* coal should out-burn a single plank */
    assert(fuel_burn_ticks((ItemId)BLOCK_COAL_ORE) > fuel_burn_ticks((ItemId)BLOCK_PLANKS));

    /* non-fuel items return 0 */
    assert(fuel_burn_ticks((ItemId)BLOCK_DIRT) == 0);
    assert(fuel_burn_ticks((ItemId)BLOCK_STONE) == 0);
    assert(fuel_burn_ticks((ItemId)BLOCK_AIR) == 0);
    assert(fuel_burn_ticks(ITEM_IRON_INGOT) == 0);

    /* is_fuel predicate agrees with the table */
    assert(is_fuel((ItemId)BLOCK_COAL_ORE));
    assert(!is_fuel((ItemId)BLOCK_DIRT));
    printf("PASS: fuel_values\n");
}

/* ---- furnace tick -------------------------------------------------- */

static FurnaceState make_furnace(ItemId input, uint8_t in_n,
                                 ItemId fuel, uint8_t fuel_n) {
    FurnaceState f;
    memset(&f, 0, sizeof(f));
    f.input = input; f.input_count = in_n;
    f.fuel  = fuel;  f.fuel_count  = fuel_n;
    f.output = (ItemId)BLOCK_AIR; f.output_count = 0;
    return f;
}

/* Smelts a full item when input + fuel are present. */
static void test_tick_smelts_with_fuel(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 1,
                                  (ItemId)BLOCK_COAL_ORE, 1);
    furnace_tick(&f, SMELT_TICKS_PER_ITEM);

    assert(f.output == ITEM_IRON_INGOT);
    assert(f.output_count == 1);
    assert(f.input_count == 0);          /* one input consumed */
    assert(f.cook_progress == 0);        /* reset after completing */
    printf("PASS: tick_smelts_with_fuel\n");
}

/* No fuel -> no progress, input preserved. */
static void test_tick_stalls_without_fuel(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 1,
                                  (ItemId)BLOCK_AIR, 0);
    furnace_tick(&f, SMELT_TICKS_PER_ITEM);

    assert(f.output_count == 0);
    assert(f.input_count == 1);
    assert(f.cook_progress == 0);
    assert(f.burn_ticks_left == 0);
    printf("PASS: tick_stalls_without_fuel\n");
}

/* Output full -> no progress even with fuel + input. */
static void test_tick_stalls_when_output_full(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 5,
                                  (ItemId)BLOCK_COAL_ORE, 5);
    f.output = ITEM_IRON_INGOT;
    f.output_count = FURNACE_STACK_MAX;   /* no room */

    int fuel_before = f.fuel_count;
    furnace_tick(&f, SMELT_TICKS_PER_ITEM * 2);

    assert(f.output_count == FURNACE_STACK_MAX);  /* unchanged */
    assert(f.input_count == 5);                   /* nothing consumed */
    assert(f.cook_progress == 0);
    assert(f.fuel_count == fuel_before);          /* no fuel wasted */
    printf("PASS: tick_stalls_when_output_full\n");
}

/* Output of a different item -> can't stack, stalls. */
static void test_tick_stalls_when_output_mismatched(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 1,
                                  (ItemId)BLOCK_COAL_ORE, 1);
    f.output = ITEM_COOKED_PORK;   /* wrong item occupies output */
    f.output_count = 1;

    furnace_tick(&f, SMELT_TICKS_PER_ITEM);
    assert(f.output == ITEM_COOKED_PORK);
    assert(f.output_count == 1);   /* unchanged */
    assert(f.input_count == 1);
    printf("PASS: tick_stalls_when_output_mismatched\n");
}

/* Consumes exactly one input per completed smelt; output accumulates. */
static void test_tick_one_input_per_smelt(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 3,
                                  (ItemId)BLOCK_COAL_ORE, 8);
    /* Run long enough for exactly 3 smelts. */
    furnace_tick(&f, SMELT_TICKS_PER_ITEM * 3);

    assert(f.input_count == 0);
    assert(f.output == ITEM_IRON_INGOT);
    assert(f.output_count == 3);
    printf("PASS: tick_one_input_per_smelt\n");
}

/* Fuel burn-ticks decrement as the furnace runs. */
static void test_tick_fuel_decrements(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 4,
                                  (ItemId)BLOCK_COAL_ORE, 2);
    int burn_one = fuel_burn_ticks((ItemId)BLOCK_COAL_ORE);

    /* Tick a small amount: one fuel unit is lit and partly burned. */
    furnace_tick(&f, 50);
    assert(f.fuel_count == 1);                 /* one unit consumed/lit */
    assert(f.burn_ticks_left == burn_one - 50);
    assert(f.cook_progress == 50);
    printf("PASS: tick_fuel_decrements\n");
}

/* Partial progress is preserved and resumes when fuel is added later. */
static void test_tick_partial_progress_resumes(void) {
    /* Start with input but no fuel: cook does not advance. */
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 1,
                                  (ItemId)BLOCK_AIR, 0);

    /* Give it a tiny fuel unit by simulating: first run with fuel for part of
     * the smelt, then remove remaining burn to model running out mid-cook. */
    f.fuel = (ItemId)BLOCK_PLANKS;
    f.fuel_count = 1;
    int plank_burn = fuel_burn_ticks((ItemId)BLOCK_PLANKS);
    /* Plank burn must be shorter than a full smelt so it runs out mid-cook. */
    assert(plank_burn < SMELT_TICKS_PER_ITEM);

    furnace_tick(&f, plank_burn);   /* burns the whole plank */
    assert(f.fuel_count == 0);
    assert(f.burn_ticks_left == 0);
    assert(f.cook_progress == plank_burn);  /* partial, preserved */
    assert(f.output_count == 0);            /* not done yet */
    assert(f.input_count == 1);

    /* Now add coal and finish; progress resumes from where it stalled. */
    f.fuel = (ItemId)BLOCK_COAL_ORE;
    f.fuel_count = 1;
    furnace_tick(&f, SMELT_TICKS_PER_ITEM - plank_burn);
    assert(f.output == ITEM_IRON_INGOT);
    assert(f.output_count == 1);
    assert(f.input_count == 0);
    printf("PASS: tick_partial_progress_resumes\n");
}

/* Determinism: N ticks at once equals 1 tick applied N times. */
static void test_tick_determinism(void) {
    FurnaceState a = make_furnace((ItemId)BLOCK_IRON_ORE, 3,
                                  (ItemId)BLOCK_COAL_ORE, 4);
    FurnaceState b = a;

    furnace_tick(&a, 777);
    for (int i = 0; i < 777; i++) furnace_tick(&b, 1);

    assert(a.input == b.input && a.input_count == b.input_count);
    assert(a.fuel == b.fuel && a.fuel_count == b.fuel_count);
    assert(a.output == b.output && a.output_count == b.output_count);
    assert(a.burn_ticks_left == b.burn_ticks_left);
    assert(a.cook_progress == b.cook_progress);
    printf("PASS: tick_determinism\n");
}

/* furnace_can_smelt reflects input/output/recipe validity. */
static void test_can_smelt_predicate(void) {
    FurnaceState f = make_furnace((ItemId)BLOCK_IRON_ORE, 1,
                                  (ItemId)BLOCK_AIR, 0);
    assert(furnace_can_smelt(&f));         /* fuel irrelevant to can_smelt */

    f.input_count = 0;
    assert(!furnace_can_smelt(&f));        /* no input */

    f.input = (ItemId)BLOCK_DIRT; f.input_count = 1;
    assert(!furnace_can_smelt(&f));        /* not smeltable */

    f.input = (ItemId)BLOCK_IRON_ORE;
    f.output = ITEM_IRON_INGOT; f.output_count = FURNACE_STACK_MAX;
    assert(!furnace_can_smelt(&f));        /* output full */

    f.output_count = 1;
    assert(furnace_can_smelt(&f));         /* room + matches result */

    f.output = ITEM_COOKED_PORK;
    assert(!furnace_can_smelt(&f));        /* output occupied by wrong item */
    printf("PASS: can_smelt_predicate\n");
}

int main(void) {
    test_recipe_valid();
    test_recipe_invalid();
    test_fuel_values();
    test_tick_smelts_with_fuel();
    test_tick_stalls_without_fuel();
    test_tick_stalls_when_output_full();
    test_tick_stalls_when_output_mismatched();
    test_tick_one_input_per_smelt();
    test_tick_fuel_decrements();
    test_tick_partial_progress_resumes();
    test_tick_determinism();
    test_can_smelt_predicate();
    printf("ALL SMELTING TESTS PASSED\n");
    return 0;
}
