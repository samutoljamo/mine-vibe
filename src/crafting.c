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
    /* No furnace yet: "smelt" iron ore directly into an ingot via a 1:1 craft. */
    { .name = "Iron Ingot", .input_count = 1,
      .inputs = { {(ItemId)BLOCK_IRON_ORE, 1} },
      .output = { ITEM_IRON_INGOT, 1 } },
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
