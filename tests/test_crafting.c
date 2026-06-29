#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/crafting.h"
#include "../src/item.h"
#include "../src/block.h"

/* Helpers to build an ItemCounts snapshot inline. */
static ItemCounts counts_of(ItemId a, uint16_t na) {
    ItemCounts c; memset(&c, 0, sizeof(c));
    c.n[a] = na;
    return c;
}
static void add(ItemCounts* c, ItemId id, uint16_t n) { c->n[id] = (uint16_t)(c->n[id] + n); }

/* Look up a recipe by exact output item (first match in table order). */
static const Recipe* find_by_output(ItemId out) {
    for (int i = 0; i < crafting_recipe_count(); i++) {
        const Recipe* r = crafting_recipe(i);
        if (r->output.item == out) return r;
    }
    return NULL;
}

/* The table must be non-empty and every recipe well-formed. */
static void test_table_well_formed(void) {
    int n = crafting_recipe_count();
    assert(n > 0);
    for (int i = 0; i < n; i++) {
        const Recipe* r = crafting_recipe(i);
        assert(r != NULL);
        assert(r->name != NULL && r->name[0] != '\0');
        assert(r->input_count >= 1 && r->input_count <= CRAFT_MAX_INPUTS);
        assert(r->output.count >= 1);
        for (int k = 0; k < r->input_count; k++) {
            assert(r->inputs[k].count >= 1);
            assert((int)r->inputs[k].item < ITEM_COUNT);
        }
    }
    /* Out-of-range index is NULL. */
    assert(crafting_recipe(-1) == NULL);
    assert(crafting_recipe(n) == NULL);
    printf("PASS: table_well_formed\n");
}

/* The required example recipes exist with the specified outputs. */
static void test_example_recipes_present(void) {
    /* wood log -> 4 planks */
    const Recipe* planks = find_by_output((ItemId)BLOCK_PLANKS);
    assert(planks);
    assert(planks->input_count == 1);
    assert(planks->inputs[0].item == (ItemId)BLOCK_WOOD);
    assert(planks->inputs[0].count == 1);
    assert(planks->output.count == 4);

    /* planks -> sticks */
    const Recipe* sticks = find_by_output(ITEM_STICK);
    assert(sticks);
    assert(sticks->inputs[0].item == (ItemId)BLOCK_PLANKS);

    /* planks + sticks -> wood pickaxe (2 inputs) */
    const Recipe* wpick = find_by_output(ITEM_WOOD_PICKAXE);
    assert(wpick);
    assert(wpick->input_count == 2);

    /* cobble + sticks -> stone pickaxe */
    const Recipe* spick = find_by_output(ITEM_STONE_PICKAXE);
    assert(spick);
    assert(spick->input_count == 2);
    bool has_cobble = false, has_stick = false;
    for (int k = 0; k < spick->input_count; k++) {
        if (spick->inputs[k].item == (ItemId)BLOCK_COBBLE) has_cobble = true;
        if (spick->inputs[k].item == ITEM_STICK)           has_stick  = true;
    }
    assert(has_cobble && has_stick);
    printf("PASS: example_recipes_present\n");
}

/* can_make: present vs missing ingredients. */
static void test_can_make_have_and_not_have(void) {
    const Recipe* planks = find_by_output((ItemId)BLOCK_PLANKS);
    assert(planks);

    /* Exactly enough. */
    ItemCounts c = counts_of((ItemId)BLOCK_WOOD, planks->inputs[0].count);
    assert(crafting_can_make(planks, &c));

    /* One short. */
    ItemCounts c2 = counts_of((ItemId)BLOCK_WOOD,
                              (uint16_t)(planks->inputs[0].count - 1));
    assert(!crafting_can_make(planks, &c2));

    /* None at all. */
    ItemCounts empty; memset(&empty, 0, sizeof(empty));
    assert(!crafting_can_make(planks, &empty));

    /* Plenty. */
    ItemCounts c3 = counts_of((ItemId)BLOCK_WOOD, 64);
    assert(crafting_can_make(planks, &c3));
    printf("PASS: can_make_have_and_not_have\n");
}

/* Multi-ingredient recipe needs ALL inputs; partial does not match. */
static void test_can_make_multi_input(void) {
    const Recipe* wpick = find_by_output(ITEM_WOOD_PICKAXE);
    assert(wpick && wpick->input_count == 2);

    /* Have only the first ingredient. */
    ItemCounts a = counts_of(wpick->inputs[0].item, wpick->inputs[0].count);
    assert(!crafting_can_make(wpick, &a));

    /* Have only the second ingredient. */
    ItemCounts b = counts_of(wpick->inputs[1].item, wpick->inputs[1].count);
    assert(!crafting_can_make(wpick, &b));

    /* Have both at exact amounts. */
    ItemCounts both; memset(&both, 0, sizeof(both));
    add(&both, wpick->inputs[0].item, wpick->inputs[0].count);
    add(&both, wpick->inputs[1].item, wpick->inputs[1].count);
    assert(crafting_can_make(wpick, &both));
    printf("PASS: can_make_multi_input\n");
}

/* NULL recipe is safely rejected. */
static void test_can_make_null(void) {
    ItemCounts c = counts_of((ItemId)BLOCK_WOOD, 64);
    assert(!crafting_can_make(NULL, &c));
    printf("PASS: can_make_null\n");
}

/* crafting_find returns the first affordable recipe, -1 when none. */
static void test_find_first_and_none(void) {
    ItemCounts empty; memset(&empty, 0, sizeof(empty));
    assert(crafting_find(&empty) == -1);

    /* With a log we can at least make planks; find returns a valid, affordable
     * recipe index. */
    ItemCounts c = counts_of((ItemId)BLOCK_WOOD, 1);
    int idx = crafting_find(&c);
    assert(idx >= 0);
    const Recipe* r = crafting_recipe(idx);
    assert(crafting_can_make(r, &c));
    printf("PASS: find_first_and_none\n");
}

/* No false matches: a snapshot full of an unrelated item crafts nothing. */
static void test_no_false_match(void) {
    ItemCounts c = counts_of((ItemId)BLOCK_DIRT, 64);
    /* Dirt isn't an ingredient of any recipe, so nothing is craftable. */
    for (int i = 0; i < crafting_recipe_count(); i++) {
        assert(!crafting_can_make(crafting_recipe(i), &c));
    }
    assert(crafting_find(&c) == -1);
    printf("PASS: no_false_match\n");
}

/* Determinism: repeated identical queries return identical answers, and the
 * table itself is stable (same pointer/content across calls). */
static void test_determinism(void) {
    ItemCounts c; memset(&c, 0, sizeof(c));
    add(&c, (ItemId)BLOCK_WOOD, 3);
    add(&c, (ItemId)BLOCK_PLANKS, 4);
    add(&c, ITEM_STICK, 2);
    add(&c, (ItemId)BLOCK_COBBLE, 3);

    for (int t = 0; t < 100; t++) {
        assert(crafting_find(&c) == crafting_find(&c));
        for (int i = 0; i < crafting_recipe_count(); i++) {
            const Recipe* r1 = crafting_recipe(i);
            const Recipe* r2 = crafting_recipe(i);
            assert(r1 == r2);  /* stable storage */
            assert(crafting_can_make(r1, &c) == crafting_can_make(r2, &c));
        }
    }
    printf("PASS: determinism\n");
}

/* Consumption math: removing a recipe's inputs from a count snapshot and adding
 * its output produces the arithmetic the server relies on. This mirrors what
 * handle_craft does, expressed purely on ItemCounts. */
static void test_consumption_and_output_math(void) {
    const Recipe* planks = find_by_output((ItemId)BLOCK_PLANKS);
    assert(planks);

    ItemCounts c = counts_of((ItemId)BLOCK_WOOD, 5);
    assert(crafting_can_make(planks, &c));

    /* Consume inputs. */
    for (int k = 0; k < planks->input_count; k++)
        c.n[planks->inputs[k].item] -= planks->inputs[k].count;
    /* Add output. */
    c.n[planks->output.item] += planks->output.count;

    assert(c.n[BLOCK_WOOD] == 4);              /* 5 - 1 */
    assert(c.n[BLOCK_PLANKS] == planks->output.count);  /* 4 produced */
    printf("PASS: consumption_and_output_math\n");
}

/* a4s.5.1 — diamond tools + swords (all tiers). a4s.5.2 — furnace + chest.
 * Each new recipe is craftable from an exact ItemCounts snapshot built from its
 * own inputs, and produces the expected output stack. */

/* Generic: a recipe with the given output exists, is affordable when its inputs
 * are present in exact amount, and yields >= 1 of the output. */
static void assert_craftable(ItemId out) {
    const Recipe* r = find_by_output(out);
    assert(r);
    ItemCounts c; memset(&c, 0, sizeof(c));
    for (int k = 0; k < r->input_count; k++)
        add(&c, r->inputs[k].item, r->inputs[k].count);
    assert(crafting_can_make(r, &c));
    assert(r->output.item == out);
    assert(r->output.count >= 1);
    /* One short on the first ingredient must fail. */
    ItemCounts c2 = c;
    c2.n[r->inputs[0].item] = (uint16_t)(c2.n[r->inputs[0].item] - 1);
    assert(!crafting_can_make(r, &c2));
}

/* Diamond tools: BLOCK_DIAMOND_ORE + sticks, matching wood/stone tool counts
 * (pickaxe/axe = 3 material + 2 sticks, shovel = 1 material + 2 sticks). */
static void test_diamond_tools(void) {
    const ItemId tools[3] = { ITEM_DIAMOND_PICKAXE, ITEM_DIAMOND_AXE,
                              ITEM_DIAMOND_SHOVEL };
    for (int i = 0; i < 3; i++) {
        const Recipe* r = find_by_output(tools[i]);
        assert(r && r->input_count == 2);
        bool has_diamond = false, has_stick = false;
        for (int k = 0; k < r->input_count; k++) {
            if (r->inputs[k].item == (ItemId)BLOCK_DIAMOND_ORE) has_diamond = true;
            if (r->inputs[k].item == ITEM_STICK)                has_stick   = true;
        }
        assert(has_diamond && has_stick);
        assert_craftable(tools[i]);
    }
    printf("PASS: diamond_tools\n");
}

/* Swords, all four tiers: 2 material + 1 stick. */
static void test_swords(void) {
    struct { ItemId out; ItemId mat; } sw[4] = {
        { ITEM_WOOD_SWORD,    (ItemId)BLOCK_PLANKS },
        { ITEM_STONE_SWORD,   (ItemId)BLOCK_COBBLE },
        { ITEM_IRON_SWORD,    ITEM_IRON_INGOT },
        { ITEM_DIAMOND_SWORD, (ItemId)BLOCK_DIAMOND_ORE },
    };
    for (int i = 0; i < 4; i++) {
        const Recipe* r = find_by_output(sw[i].out);
        assert(r && r->input_count == 2);
        bool has_mat = false, has_stick = false;
        for (int k = 0; k < r->input_count; k++) {
            if (r->inputs[k].item == sw[i].mat && r->inputs[k].count == 2) has_mat = true;
            if (r->inputs[k].item == ITEM_STICK && r->inputs[k].count == 1) has_stick = true;
        }
        assert(has_mat && has_stick);
        assert_craftable(sw[i].out);
    }
    printf("PASS: swords\n");
}

/* Furnace = 8 cobblestone; chest = 8 planks. */
static void test_furnace_and_chest(void) {
    const Recipe* furnace = find_by_output((ItemId)BLOCK_FURNACE);
    assert(furnace && furnace->input_count == 1);
    assert(furnace->inputs[0].item == (ItemId)BLOCK_COBBLE);
    assert(furnace->inputs[0].count == 8);
    assert_craftable((ItemId)BLOCK_FURNACE);

    const Recipe* chest = find_by_output((ItemId)BLOCK_CHEST);
    assert(chest && chest->input_count == 1);
    assert(chest->inputs[0].item == (ItemId)BLOCK_PLANKS);
    assert(chest->inputs[0].count == 8);
    assert_craftable((ItemId)BLOCK_CHEST);
    printf("PASS: furnace_and_chest\n");
}

/* Armour sets (leather + iron tiers) are all present — no diamond armour. */
static void test_armor_sets_present(void) {
    const ItemId armor[8] = {
        ITEM_LEATHER_HELMET, ITEM_LEATHER_CHESTPLATE,
        ITEM_LEATHER_LEGGINGS, ITEM_LEATHER_BOOTS,
        ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE,
        ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS,
    };
    for (int i = 0; i < 8; i++) assert_craftable(armor[i]);
    printf("PASS: armor_sets_present\n");
}

/* The table grew to accommodate the new recipes. */
static void test_recipe_count_grew(void) {
    /* Pre-existing rows (planks, sticks, 6 wood/stone tools, torch, leather,
     * 8 armour) = 18, plus 3 diamond tools + 4 swords + furnace + chest = 9 new
     * => at least 27. The placeholder 1:1 iron-ore->ingot craft was removed
     * (a4s.2.7); ore->ingot is now smelted in a furnace, not crafted. */
    assert(crafting_recipe_count() >= 27);
    printf("PASS: recipe_count_grew (%d recipes)\n", crafting_recipe_count());
}

/* a4s.2.7 — the placeholder 1:1 ore->ingot CRAFT was removed once furnace
 * smelting existed. Holding only raw iron ore must NOT craft an iron ingot, and
 * no recipe in the table may output an ingot from raw ore as its sole input. */
static void test_ore_not_craftable_to_ingot(void) {
    /* No recipe outputs ITEM_IRON_INGOT at all — ingots come from smelting. */
    assert(find_by_output(ITEM_IRON_INGOT) == NULL);

    /* A snapshot of only raw iron ore crafts nothing (no recipe takes ore as a
     * lone ingredient). */
    ItemCounts ore = counts_of((ItemId)BLOCK_IRON_ORE, 64);
    assert(crafting_find(&ore) == -1);

    printf("PASS: ore_not_craftable_to_ingot\n");
}

int main(void) {
    test_table_well_formed();
    test_example_recipes_present();
    test_diamond_tools();
    test_swords();
    test_furnace_and_chest();
    test_armor_sets_present();
    test_recipe_count_grew();
    test_ore_not_craftable_to_ingot();
    test_can_make_have_and_not_have();
    test_can_make_multi_input();
    test_can_make_null();
    test_find_first_and_none();
    test_no_false_match();
    test_determinism();
    test_consumption_and_output_math();
    printf("ALL CRAFTING TESTS PASSED\n");
    return 0;
}
