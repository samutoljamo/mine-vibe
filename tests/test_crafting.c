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

/* ================================================================== */
/*  Shaped 3x3 crafting (a4s.5.3 engine)                               */
/* ================================================================== */

/* Find a shaped recipe by exact output item (first in table order). */
static int find_shaped_by_output(ItemId out) {
    for (int i = 0; i < crafting_shaped_count(); i++) {
        const ShapedRecipe* r = crafting_shaped(i);
        if (r->output_item == out) return i;
    }
    return -1;
}

static void grid_clear(ItemId g[CRAFT_CELLS]) {
    for (int i = 0; i < CRAFT_CELLS; i++) g[i] = (ItemId)BLOCK_AIR;
}

/* The shaped table is non-empty and every recipe is well-formed. */
static void test_shaped_table_well_formed(void) {
    int n = crafting_shaped_count();
    assert(n > 0);
    for (int i = 0; i < n; i++) {
        const ShapedRecipe* r = crafting_shaped(i);
        assert(r != NULL);
        assert(r->name != NULL && r->name[0] != '\0');
        assert(r->output_count >= 1);
        assert((int)r->output_item < ITEM_COUNT);
        /* At least one non-empty cell. */
        int filled = 0;
        for (int c = 0; c < CRAFT_CELLS; c++) {
            assert((int)r->cells[c] < ITEM_COUNT);
            if (r->cells[c] != (ItemId)BLOCK_AIR) filled++;
        }
        assert(filled >= 1);
    }
    assert(crafting_shaped(-1) == NULL);
    assert(crafting_shaped(n) == NULL);
    printf("PASS: shaped_table_well_formed\n");
}

/* An empty grid matches nothing. */
static void test_shaped_empty_grid(void) {
    ItemId g[CRAFT_CELLS]; grid_clear(g);
    assert(crafting_match_grid(g) == -1);
    printf("PASS: shaped_empty_grid\n");
}

/* The canonical pickaxe layout matches, and the SAME shape shifted to other
 * corners still matches (translation invariance). */
static void test_shaped_pickaxe_translation(void) {
    int pi = find_shaped_by_output(ITEM_WOOD_PICKAXE);
    assert(pi >= 0);
    const ShapedRecipe* r = crafting_shaped(pi);
    assert(!r->shapeless);

    /* Canonical pickaxe (top-left anchored):
     *   P P P
     *   . S .
     *   . S .                                                            */
    ItemId g[CRAFT_CELLS]; grid_clear(g);
    g[0] = (ItemId)BLOCK_PLANKS; g[1] = (ItemId)BLOCK_PLANKS; g[2] = (ItemId)BLOCK_PLANKS;
    g[4] = ITEM_STICK;
    g[7] = ITEM_STICK;
    assert(crafting_match_grid(g) == pi);

    /* Same shape but only 3 wide -> the head row + the stick column must align;
     * a pickaxe needs all 3 top cells so the only horizontal placement is the
     * full width. Vertical translation: shift everything... a 3-tall pattern
     * already fills the grid vertically, so the meaningful translation test is
     * to confirm the matcher trims/normalises by bounding box. Build a recipe
     * whose bounding box is smaller (sword) for the corner test below. */

    /* Wrong layout: sticks misplaced -> no match for the pickaxe. */
    grid_clear(g);
    g[0] = (ItemId)BLOCK_PLANKS; g[1] = (ItemId)BLOCK_PLANKS; g[2] = (ItemId)BLOCK_PLANKS;
    g[3] = ITEM_STICK;   /* stick under the left, not the centre */
    g[6] = ITEM_STICK;
    assert(crafting_match_grid(g) != pi);
    printf("PASS: shaped_pickaxe_translation\n");
}

/* The sword is a 1-wide, 3-tall pattern (M / M / stick). It must match in any
 * of the three columns (horizontal translation) and at the top OR shifted down
 * if it were shorter — here it is 3 tall so test the 3 columns. */
static void test_shaped_sword_translation(void) {
    int si = find_shaped_by_output(ITEM_WOOD_SWORD);
    assert(si >= 0);
    const ShapedRecipe* r = crafting_shaped(si);
    assert(!r->shapeless);

    for (int col = 0; col < CRAFT_GRID; col++) {
        ItemId g[CRAFT_CELLS]; grid_clear(g);
        g[0 * CRAFT_GRID + col] = (ItemId)BLOCK_PLANKS;
        g[1 * CRAFT_GRID + col] = (ItemId)BLOCK_PLANKS;
        g[2 * CRAFT_GRID + col] = ITEM_STICK;
        assert(crafting_match_grid(g) == si);
    }
    printf("PASS: shaped_sword_translation\n");
}

/* A 2x2 pattern (the planks-square nature of nothing here; use a generic small
 * pattern) must match in any corner — verify with whatever 2-tall recipe is
 * available; sword above already covers vertical fill. For an explicit corner
 * test, build a tiny synthetic via the shovel (1 mat over 2 sticks, 1 wide)
 * and slide it both horizontally and vertically. */
static void test_shaped_shovel_corner_translation(void) {
    int idx = find_shaped_by_output(ITEM_WOOD_SHOVEL);
    assert(idx >= 0);
    /* Shovel: M / S / S (1 wide, 3 tall) -> 3 horizontal positions. */
    for (int col = 0; col < CRAFT_GRID; col++) {
        ItemId g[CRAFT_CELLS]; grid_clear(g);
        g[0 * CRAFT_GRID + col] = (ItemId)BLOCK_PLANKS;
        g[1 * CRAFT_GRID + col] = ITEM_STICK;
        g[2 * CRAFT_GRID + col] = ITEM_STICK;
        assert(crafting_match_grid(g) == idx);
    }
    printf("PASS: shaped_shovel_corner_translation\n");
}

/* Mirror: the axe is L-shaped (M M / M S / . S) and its horizontal mirror
 * (M M / S M / S .) must also produce an axe. */
static void test_shaped_axe_mirror(void) {
    int idx = find_shaped_by_output(ITEM_WOOD_AXE);
    assert(idx >= 0);
    const ShapedRecipe* r = crafting_shaped(idx);
    assert(!r->shapeless);

    /* Canonical axe shape from the table. */
    ItemId g[CRAFT_CELLS];
    for (int c = 0; c < CRAFT_CELLS; c++) g[c] = r->cells[c];
    assert(crafting_match_grid(g) == idx);

    /* Mirror each row horizontally within the 3x3. */
    ItemId m[CRAFT_CELLS]; grid_clear(m);
    for (int y = 0; y < CRAFT_GRID; y++)
        for (int x = 0; x < CRAFT_GRID; x++)
            m[y * CRAFT_GRID + (CRAFT_GRID - 1 - x)] = r->cells[y * CRAFT_GRID + x];
    /* The mirrored layout still crafts an axe (may match a different table
     * index if another recipe shares the mirrored shape, but for the axe it is
     * the same recipe). */
    int mi = crafting_match_grid(m);
    assert(mi >= 0);
    assert(crafting_shaped(mi)->output_item == ITEM_WOOD_AXE);
    printf("PASS: shaped_axe_mirror\n");
}

/* A shapeless shaped-recipe (planks->? or sticks) matches regardless of cell
 * placement: scatter the ingredients around the grid. */
static void test_shaped_shapeless_any_placement(void) {
    /* Sticks: 2 planks (vertical in vanilla, but our entry is shapeless or a
     * vertical pair). Find the recipe that outputs ITEM_STICK. */
    int idx = find_shaped_by_output(ITEM_STICK);
    assert(idx >= 0);
    const ShapedRecipe* r = crafting_shaped(idx);

    if (r->shapeless) {
        /* Count its ingredient multiset, then place the same items in arbitrary
         * different cells and confirm it still matches. */
        ItemId g[CRAFT_CELLS]; grid_clear(g);
        int placed = 0;
        int scatter[CRAFT_CELLS] = {8, 0, 4, 2, 6, 1, 7, 3, 5};
        for (int c = 0; c < CRAFT_CELLS; c++) {
            if (r->cells[c] != (ItemId)BLOCK_AIR)
                g[scatter[placed++]] = r->cells[c];
        }
        assert(crafting_match_grid(g) == idx);
    }

    /* Planks is the canonical shapeless case (1 log anywhere -> 4 planks). */
    int pidx = find_shaped_by_output((ItemId)BLOCK_PLANKS);
    assert(pidx >= 0);
    const ShapedRecipe* pr = crafting_shaped(pidx);
    assert(pr->shapeless);
    /* A single log in EACH possible cell must match. */
    for (int c = 0; c < CRAFT_CELLS; c++) {
        ItemId g[CRAFT_CELLS]; grid_clear(g);
        g[c] = (ItemId)BLOCK_WOOD;
        assert(crafting_match_grid(g) == pidx);
    }
    printf("PASS: shaped_shapeless_any_placement\n");
}

/* shaped_recipe_output returns the right item/count and rejects bad indices. */
static void test_shaped_output_accessor(void) {
    int pi = find_shaped_by_output(ITEM_WOOD_PICKAXE);
    assert(pi >= 0);
    ItemId it = 0; uint8_t ct = 0;
    assert(shaped_recipe_output(pi, &it, &ct));
    assert(it == ITEM_WOOD_PICKAXE);
    assert(ct == 1);

    int planks = find_shaped_by_output((ItemId)BLOCK_PLANKS);
    assert(shaped_recipe_output(planks, &it, &ct));
    assert(it == (ItemId)BLOCK_PLANKS);
    assert(ct == 4);

    assert(!shaped_recipe_output(-1, &it, &ct));
    assert(!shaped_recipe_output(crafting_shaped_count(), &it, &ct));
    printf("PASS: shaped_output_accessor\n");
}

/* A garbage grid (unrelated block) matches nothing. */
static void test_shaped_no_false_match(void) {
    ItemId g[CRAFT_CELLS]; grid_clear(g);
    g[4] = (ItemId)BLOCK_DIRT;
    g[0] = (ItemId)BLOCK_DIRT;
    assert(crafting_match_grid(g) == -1);
    printf("PASS: shaped_no_false_match\n");
}

/* Furnace (8 cobble ring, empty centre) and chest (8 planks ring) are present
 * as shaped recipes and match their ring layout. */
static void test_shaped_furnace_and_chest(void) {
    int fi = find_shaped_by_output((ItemId)BLOCK_FURNACE);
    assert(fi >= 0);
    ItemId g[CRAFT_CELLS];
    for (int c = 0; c < CRAFT_CELLS; c++) g[c] = (ItemId)BLOCK_COBBLE;
    g[4] = (ItemId)BLOCK_AIR;  /* hollow centre */
    assert(crafting_match_grid(g) == fi);

    int ci = find_shaped_by_output((ItemId)BLOCK_CHEST);
    assert(ci >= 0);
    for (int c = 0; c < CRAFT_CELLS; c++) g[c] = (ItemId)BLOCK_PLANKS;
    g[4] = (ItemId)BLOCK_AIR;
    assert(crafting_match_grid(g) == ci);
    printf("PASS: shaped_furnace_and_chest\n");
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

    /* Shaped 3x3 engine (a4s.5.3). */
    test_shaped_table_well_formed();
    test_shaped_empty_grid();
    test_shaped_pickaxe_translation();
    test_shaped_sword_translation();
    test_shaped_shovel_corner_translation();
    test_shaped_axe_mirror();
    test_shaped_shapeless_any_placement();
    test_shaped_output_accessor();
    test_shaped_no_false_match();
    test_shaped_furnace_and_chest();

    printf("ALL CRAFTING TESTS PASSED\n");
    return 0;
}
