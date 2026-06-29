#include "crafting.h"
#include "block.h"
#include "inventory.h"
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Recipe table — the single source of truth                          */
/*                                                                     */
/*  Shapeless recipes: ingredient order does not matter. Recipe index   */
/*  in this table is the recipe id sent over the wire in PKT_CRAFT, so   */
/*  appending new recipes is safe (existing ids are stable); reordering  */
/*  or removing entries changes the wire meaning and so requires a       */
/*  protocol bump. Planks and cobble are block ids (placeable); sticks   */
/*  and tools are non-block item ids.                                    */
/* ------------------------------------------------------------------ */

static const Recipe g_recipe_table[] = {
    /* Wood log -> 4 planks. */
    { .name = "Planks", .input_count = 1,
      .inputs = { {(ItemId)BLOCK_WOOD, 1} },
      .output = { (ItemId)BLOCK_PLANKS, 4 } },

    /* 2 planks -> 4 sticks. */
    { .name = "Sticks", .input_count = 1,
      .inputs = { {(ItemId)BLOCK_PLANKS, 2} },
      .output = { ITEM_STICK, 4 } },

    /* --- Wooden tools: planks + sticks --- */
    { .name = "Wooden Pickaxe", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_PLANKS, 3}, {ITEM_STICK, 2} },
      .output = { ITEM_WOOD_PICKAXE, 1 } },
    { .name = "Wooden Axe", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_PLANKS, 3}, {ITEM_STICK, 2} },
      .output = { ITEM_WOOD_AXE, 1 } },
    { .name = "Wooden Shovel", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_PLANKS, 1}, {ITEM_STICK, 2} },
      .output = { ITEM_WOOD_SHOVEL, 1 } },

    /* --- Stone tools: cobble + sticks --- */
    { .name = "Stone Pickaxe", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_COBBLE, 3}, {ITEM_STICK, 2} },
      .output = { ITEM_STONE_PICKAXE, 1 } },
    { .name = "Stone Axe", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_COBBLE, 3}, {ITEM_STICK, 2} },
      .output = { ITEM_STONE_AXE, 1 } },
    { .name = "Stone Shovel", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_COBBLE, 1}, {ITEM_STICK, 2} },
      .output = { ITEM_STONE_SHOVEL, 1 } },

    /* --- A few more sensible recipes --- */
    /* Stone -> cobble is not a thing; instead cobble -> stone is also not.
     * Torches: a stick + coal lumps. With no coal *item* (only ore block),
     * use coal ore directly as the "fuel" ingredient. */
    { .name = "Torches", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_COAL_ORE, 1}, {ITEM_STICK, 1} },
      .output = { (ItemId)BLOCK_TORCH, 4 } },

    /* --- Armour materials --- */
    /* Iron ore is SMELTED into an ingot in a furnace (see smelting.c), not
     * crafted; the old placeholder 1:1 ore->ingot craft was removed (a4s.2.7). */
    /* Leather stand-in: tan a hide from planks (no animals/hide drops yet). */
    { .name = "Leather", .input_count = 1,
      .inputs = { {(ItemId)BLOCK_PLANKS, 3} },
      .output = { ITEM_LEATHER, 1 } },

    /* --- Leather armour: vanilla shapes' material counts (5/8/7/4) --- */
    { .name = "Leather Helmet", .input_count = 1,
      .inputs = { {ITEM_LEATHER, 5} },
      .output = { ITEM_LEATHER_HELMET, 1 } },
    { .name = "Leather Chestplate", .input_count = 1,
      .inputs = { {ITEM_LEATHER, 8} },
      .output = { ITEM_LEATHER_CHESTPLATE, 1 } },
    { .name = "Leather Leggings", .input_count = 1,
      .inputs = { {ITEM_LEATHER, 7} },
      .output = { ITEM_LEATHER_LEGGINGS, 1 } },
    { .name = "Leather Boots", .input_count = 1,
      .inputs = { {ITEM_LEATHER, 4} },
      .output = { ITEM_LEATHER_BOOTS, 1 } },

    /* --- Iron armour: iron ingots --- */
    { .name = "Iron Helmet", .input_count = 1,
      .inputs = { {ITEM_IRON_INGOT, 5} },
      .output = { ITEM_IRON_HELMET, 1 } },
    { .name = "Iron Chestplate", .input_count = 1,
      .inputs = { {ITEM_IRON_INGOT, 8} },
      .output = { ITEM_IRON_CHESTPLATE, 1 } },
    { .name = "Iron Leggings", .input_count = 1,
      .inputs = { {ITEM_IRON_INGOT, 7} },
      .output = { ITEM_IRON_LEGGINGS, 1 } },
    { .name = "Iron Boots", .input_count = 1,
      .inputs = { {ITEM_IRON_INGOT, 4} },
      .output = { ITEM_IRON_BOOTS, 1 } },

    /* --- Diamond tools: diamond ore (no gem item) + sticks ---            */
    /* Counts mirror the wood/stone tool shapes: pickaxe/axe = 3 mat + 2     */
    /* sticks, shovel = 1 mat + 2 sticks.                                    */
    { .name = "Diamond Pickaxe", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_DIAMOND_ORE, 3}, {ITEM_STICK, 2} },
      .output = { ITEM_DIAMOND_PICKAXE, 1 } },
    { .name = "Diamond Axe", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_DIAMOND_ORE, 3}, {ITEM_STICK, 2} },
      .output = { ITEM_DIAMOND_AXE, 1 } },
    { .name = "Diamond Shovel", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_DIAMOND_ORE, 1}, {ITEM_STICK, 2} },
      .output = { ITEM_DIAMOND_SHOVEL, 1 } },

    /* --- Swords (all tiers): 2 material + 1 stick --- */
    { .name = "Wooden Sword", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_PLANKS, 2}, {ITEM_STICK, 1} },
      .output = { ITEM_WOOD_SWORD, 1 } },
    { .name = "Stone Sword", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_COBBLE, 2}, {ITEM_STICK, 1} },
      .output = { ITEM_STONE_SWORD, 1 } },
    { .name = "Iron Sword", .input_count = 2,
      .inputs = { {ITEM_IRON_INGOT, 2}, {ITEM_STICK, 1} },
      .output = { ITEM_IRON_SWORD, 1 } },
    { .name = "Diamond Sword", .input_count = 2,
      .inputs = { {(ItemId)BLOCK_DIAMOND_ORE, 2}, {ITEM_STICK, 1} },
      .output = { ITEM_DIAMOND_SWORD, 1 } },

    /* --- Utility blocks --- */
    { .name = "Furnace", .input_count = 1,
      .inputs = { {(ItemId)BLOCK_COBBLE, 8} },
      .output = { (ItemId)BLOCK_FURNACE, 1 } },
    { .name = "Chest", .input_count = 1,
      .inputs = { {(ItemId)BLOCK_PLANKS, 8} },
      .output = { (ItemId)BLOCK_CHEST, 1 } },
};

#define RECIPE_COUNT ((int)(sizeof(g_recipe_table) / sizeof(g_recipe_table[0])))

int crafting_recipe_count(void) { return RECIPE_COUNT; }

const Recipe* crafting_recipe(int index) {
    if (index < 0 || index >= RECIPE_COUNT) return NULL;
    return &g_recipe_table[index];
}

bool crafting_can_make(const Recipe* recipe, const ItemCounts* counts) {
    if (!recipe || !counts) return false;
    for (int k = 0; k < recipe->input_count; k++) {
        ItemId  id  = recipe->inputs[k].item;
        uint8_t need = recipe->inputs[k].count;
        if ((int)id < 0 || (int)id >= ITEM_COUNT) return false;
        if (counts->n[id] < need) return false;
    }
    return true;
}

int crafting_find(const ItemCounts* counts) {
    if (!counts) return -1;
    for (int i = 0; i < RECIPE_COUNT; i++) {
        if (crafting_can_make(&g_recipe_table[i], counts)) return i;
    }
    return -1;
}

int crafting_affordable(const ItemCounts* counts, int* out_indices, int max) {
    if (!counts || !out_indices || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < RECIPE_COUNT && n < max; i++) {
        if (crafting_can_make(&g_recipe_table[i], counts))
            out_indices[n++] = i;
    }
    return n;
}

void crafting_counts_from_inventory(const struct Inventory* inv,
                                    ItemCounts* out) {
    memset(out, 0, sizeof(*out));
    if (!inv) return;
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        ItemId  id = inv->slots[i].item;
        uint8_t ct = inv->slots[i].count;
        if (ct == 0 || (int)id < 0 || (int)id >= ITEM_COUNT) continue;
        uint32_t v = (uint32_t)out->n[id] + ct;
        out->n[id] = v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
    }
}

/* ================================================================== */
/*  Shaped 3x3 crafting table (a4s.5.3 engine)                         */
/*                                                                     */
/*  ADDITIVE: this runs alongside the shapeless API above, which is     */
/*  unchanged. Cells are row-major (index = y*CRAFT_GRID + x). 0 /      */
/*  BLOCK_AIR is empty. Shaped recipes mirror real Minecraft layouts    */
/*  using the SAME item ids the shapeless table uses (so the world's    */
/*  materials craft the same outputs). Tool/material counts (e.g. 3     */
/*  planks for a pickaxe head) follow vanilla shapes.                    */
/* ================================================================== */

/* Convenience aliases so the literal grids below read like the wiki. */
#define _A  ((ItemId)BLOCK_AIR)
#define _W  ((ItemId)BLOCK_WOOD)
#define _P  ((ItemId)BLOCK_PLANKS)
#define _C  ((ItemId)BLOCK_COBBLE)
#define _D  ((ItemId)BLOCK_DIAMOND_ORE)
#define _I  ITEM_IRON_INGOT
#define _L  ITEM_LEATHER
#define _S  ITEM_STICK

/* Tool/sword/utility shapes are TIER-parameterised below; we spell each one out
 * to keep the table a flat literal (no codegen) — that makes the wire ids (the
 * table index) obvious and stable.                                            */
static const ShapedRecipe g_shaped_recipes[] = {
    /* --- Wood: log -> 4 planks (shapeless, 1 anywhere). --- */
    { .name = "Planks", .shapeless = true,
      .cells = { _W }, .output_item = _P, .output_count = 4 },

    /* --- 2 planks (vertical) -> 4 sticks. --- */
    { .name = "Sticks", .shapeless = false,
      .cells = { _P, _A, _A,
                 _P, _A, _A,
                 _A, _A, _A },
      .output_item = _S, .output_count = 4 },

    /* ---------------- Wooden tools ---------------- */
    /* Pickaxe: top row 3x material, centre column two sticks. */
    { .name = "Wooden Pickaxe", .shapeless = false,
      .cells = { _P, _P, _P,
                 _A, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_WOOD_PICKAXE, .output_count = 1 },
    /* Axe: L-shape (M M / M S / . S). */
    { .name = "Wooden Axe", .shapeless = false,
      .cells = { _P, _P, _A,
                 _P, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_WOOD_AXE, .output_count = 1 },
    /* Shovel: one material over two sticks. */
    { .name = "Wooden Shovel", .shapeless = false,
      .cells = { _P, _A, _A,
                 _S, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_WOOD_SHOVEL, .output_count = 1 },
    /* Sword: two material stacked over a stick. */
    { .name = "Wooden Sword", .shapeless = false,
      .cells = { _P, _A, _A,
                 _P, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_WOOD_SWORD, .output_count = 1 },

    /* ---------------- Stone tools ---------------- */
    { .name = "Stone Pickaxe", .shapeless = false,
      .cells = { _C, _C, _C,
                 _A, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_STONE_PICKAXE, .output_count = 1 },
    { .name = "Stone Axe", .shapeless = false,
      .cells = { _C, _C, _A,
                 _C, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_STONE_AXE, .output_count = 1 },
    { .name = "Stone Shovel", .shapeless = false,
      .cells = { _C, _A, _A,
                 _S, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_STONE_SHOVEL, .output_count = 1 },
    { .name = "Stone Sword", .shapeless = false,
      .cells = { _C, _A, _A,
                 _C, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_STONE_SWORD, .output_count = 1 },

    /* ---------------- Iron tools (ingot) ---------------- */
    { .name = "Iron Pickaxe", .shapeless = false,
      .cells = { _I, _I, _I,
                 _A, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_IRON_PICKAXE, .output_count = 1 },
    { .name = "Iron Axe", .shapeless = false,
      .cells = { _I, _I, _A,
                 _I, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_IRON_AXE, .output_count = 1 },
    { .name = "Iron Shovel", .shapeless = false,
      .cells = { _I, _A, _A,
                 _S, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_IRON_SHOVEL, .output_count = 1 },
    { .name = "Iron Sword", .shapeless = false,
      .cells = { _I, _A, _A,
                 _I, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_IRON_SWORD, .output_count = 1 },

    /* ---------------- Diamond tools (ore) ---------------- */
    { .name = "Diamond Pickaxe", .shapeless = false,
      .cells = { _D, _D, _D,
                 _A, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_DIAMOND_PICKAXE, .output_count = 1 },
    { .name = "Diamond Axe", .shapeless = false,
      .cells = { _D, _D, _A,
                 _D, _S, _A,
                 _A, _S, _A },
      .output_item = ITEM_DIAMOND_AXE, .output_count = 1 },
    { .name = "Diamond Shovel", .shapeless = false,
      .cells = { _D, _A, _A,
                 _S, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_DIAMOND_SHOVEL, .output_count = 1 },
    { .name = "Diamond Sword", .shapeless = false,
      .cells = { _D, _A, _A,
                 _D, _A, _A,
                 _S, _A, _A },
      .output_item = ITEM_DIAMOND_SWORD, .output_count = 1 },

    /* ---------------- Utility blocks ---------------- */
    /* Furnace: 8 cobble ring, hollow centre. */
    { .name = "Furnace", .shapeless = false,
      .cells = { _C, _C, _C,
                 _C, _A, _C,
                 _C, _C, _C },
      .output_item = (ItemId)BLOCK_FURNACE, .output_count = 1 },
    /* Chest: 8 planks ring, hollow centre. */
    { .name = "Chest", .shapeless = false,
      .cells = { _P, _P, _P,
                 _P, _A, _P,
                 _P, _P, _P },
      .output_item = (ItemId)BLOCK_CHEST, .output_count = 1 },
    /* Torch: coal-ore (fuel stand-in) over a stick. */
    { .name = "Torches", .shapeless = false,
      .cells = { (ItemId)BLOCK_COAL_ORE, _A, _A,
                 _S,                     _A, _A,
                 _A,                     _A, _A },
      .output_item = (ItemId)BLOCK_TORCH, .output_count = 4 },

    /* ---------------- Leather armour ---------------- */
    /* Helmet: L L L / L . L (5). */
    { .name = "Leather Helmet", .shapeless = false,
      .cells = { _L, _L, _L,
                 _L, _A, _L,
                 _A, _A, _A },
      .output_item = ITEM_LEATHER_HELMET, .output_count = 1 },
    /* Chestplate: L . L / L L L / L L L (8). */
    { .name = "Leather Chestplate", .shapeless = false,
      .cells = { _L, _A, _L,
                 _L, _L, _L,
                 _L, _L, _L },
      .output_item = ITEM_LEATHER_CHESTPLATE, .output_count = 1 },
    /* Leggings: L L L / L . L / L . L (7). */
    { .name = "Leather Leggings", .shapeless = false,
      .cells = { _L, _L, _L,
                 _L, _A, _L,
                 _L, _A, _L },
      .output_item = ITEM_LEATHER_LEGGINGS, .output_count = 1 },
    /* Boots: L . L / L . L (4). */
    { .name = "Leather Boots", .shapeless = false,
      .cells = { _L, _A, _L,
                 _L, _A, _L,
                 _A, _A, _A },
      .output_item = ITEM_LEATHER_BOOTS, .output_count = 1 },

    /* ---------------- Iron armour ---------------- */
    { .name = "Iron Helmet", .shapeless = false,
      .cells = { _I, _I, _I,
                 _I, _A, _I,
                 _A, _A, _A },
      .output_item = ITEM_IRON_HELMET, .output_count = 1 },
    { .name = "Iron Chestplate", .shapeless = false,
      .cells = { _I, _A, _I,
                 _I, _I, _I,
                 _I, _I, _I },
      .output_item = ITEM_IRON_CHESTPLATE, .output_count = 1 },
    { .name = "Iron Leggings", .shapeless = false,
      .cells = { _I, _I, _I,
                 _I, _A, _I,
                 _I, _A, _I },
      .output_item = ITEM_IRON_LEGGINGS, .output_count = 1 },
    { .name = "Iron Boots", .shapeless = false,
      .cells = { _I, _A, _I,
                 _I, _A, _I,
                 _A, _A, _A },
      .output_item = ITEM_IRON_BOOTS, .output_count = 1 },
};

#undef _A
#undef _W
#undef _P
#undef _C
#undef _D
#undef _I
#undef _L
#undef _S

#define SHAPED_COUNT ((int)(sizeof(g_shaped_recipes) / sizeof(g_shaped_recipes[0])))

int crafting_shaped_count(void) { return SHAPED_COUNT; }

const ShapedRecipe* crafting_shaped(int index) {
    if (index < 0 || index >= SHAPED_COUNT) return NULL;
    return &g_shaped_recipes[index];
}

bool shaped_recipe_output(int index, ItemId* out_item, uint8_t* out_count) {
    const ShapedRecipe* r = crafting_shaped(index);
    if (!r) return false;
    if (out_item)  *out_item  = r->output_item;
    if (out_count) *out_count = r->output_count;
    return true;
}

/* ----- shaped matching internals (pure helpers) ----- */

/* Trim a 3x3 grid to its bounding box of non-empty cells, copying the trimmed
 * content top-left anchored into `out` (also 3x3, padded with BLOCK_AIR). Sets
 * *w,*h to the bounding-box dimensions (0,0 if the grid is empty). */
static void grid_trim(const ItemId g[CRAFT_CELLS], ItemId out[CRAFT_CELLS],
                      int* w, int* h) {
    int minx = CRAFT_GRID, miny = CRAFT_GRID, maxx = -1, maxy = -1;
    for (int y = 0; y < CRAFT_GRID; y++) {
        for (int x = 0; x < CRAFT_GRID; x++) {
            if (g[y * CRAFT_GRID + x] != (ItemId)BLOCK_AIR) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    for (int i = 0; i < CRAFT_CELLS; i++) out[i] = (ItemId)BLOCK_AIR;
    if (maxx < 0) { *w = 0; *h = 0; return; }  /* empty */
    *w = maxx - minx + 1;
    *h = maxy - miny + 1;
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++)
            out[y * CRAFT_GRID + x] = g[(miny + y) * CRAFT_GRID + (minx + x)];
}

/* Horizontally mirror a trimmed (top-left anchored) wxh region in place. */
static void grid_mirror(ItemId g[CRAFT_CELLS], int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w / 2; x++) {
            ItemId tmp = g[y * CRAFT_GRID + x];
            g[y * CRAFT_GRID + x] = g[y * CRAFT_GRID + (w - 1 - x)];
            g[y * CRAFT_GRID + (w - 1 - x)] = tmp;
        }
    }
}

/* Exact compare of two trimmed (top-left anchored) regions. */
static bool grid_equal(const ItemId a[CRAFT_CELLS], int aw, int ah,
                       const ItemId b[CRAFT_CELLS], int bw, int bh) {
    if (aw != bw || ah != bh) return false;
    for (int y = 0; y < ah; y++)
        for (int x = 0; x < aw; x++)
            if (a[y * CRAFT_GRID + x] != b[y * CRAFT_GRID + x]) return false;
    return true;
}

/* Multiset (count) of non-empty cells, used for shapeless matching. */
static void grid_multiset(const ItemId g[CRAFT_CELLS], uint8_t counts[ITEM_COUNT]) {
    memset(counts, 0, sizeof(uint8_t) * ITEM_COUNT);
    for (int i = 0; i < CRAFT_CELLS; i++) {
        ItemId id = g[i];
        if (id != (ItemId)BLOCK_AIR && (int)id < ITEM_COUNT)
            counts[id]++;
    }
}

int crafting_match_grid(const ItemId grid[CRAFT_CELLS]) {
    if (!grid) return -1;

    /* Bounding-box-trimmed query grid (shared by all shaped comparisons). */
    ItemId qtrim[CRAFT_CELLS]; int qw = 0, qh = 0;
    grid_trim(grid, qtrim, &qw, &qh);
    if (qw == 0) return -1;  /* empty grid matches nothing */

    /* Horizontally-mirrored variant of the trimmed query. */
    ItemId qmir[CRAFT_CELLS];
    memcpy(qmir, qtrim, sizeof(qmir));
    grid_mirror(qmir, qw, qh);

    /* Query multiset for shapeless comparisons. */
    uint8_t qcount[ITEM_COUNT];
    grid_multiset(grid, qcount);

    for (int i = 0; i < SHAPED_COUNT; i++) {
        const ShapedRecipe* r = &g_shaped_recipes[i];

        if (r->shapeless) {
            uint8_t rcount[ITEM_COUNT];
            grid_multiset(r->cells, rcount);
            if (memcmp(qcount, rcount, sizeof(uint8_t) * ITEM_COUNT) == 0)
                return i;
            continue;
        }

        /* Shaped: trim the recipe, compare against the query and its mirror
         * (both already translation-normalised by trimming). */
        ItemId rtrim[CRAFT_CELLS]; int rw = 0, rh = 0;
        grid_trim(r->cells, rtrim, &rw, &rh);
        if (grid_equal(qtrim, qw, qh, rtrim, rw, rh) ||
            grid_equal(qmir,  qw, qh, rtrim, rw, rh))
            return i;
    }
    return -1;
}
