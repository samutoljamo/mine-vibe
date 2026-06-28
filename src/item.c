#include "item.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Item metadata table                                                */
/*                                                                     */
/*  Indexed by tool slot (id - ITEM_TOOL_FIRST). Block items don't need*/
/*  a table entry — their metadata comes from block.c — so item_get_def*/
/*  synthesizes a block ItemDef on the fly for the low id range.        */
/* ------------------------------------------------------------------ */

/* Durability per material tier (uses). Strictly increasing wood<stone<iron. */
#define DUR_WOOD   60
#define DUR_STONE  132
#define DUR_IRON   251

static const ItemDef g_tool_defs[ITEM_TOOL_COUNT] = {
    /*                          name             is_tool kind          material        durability      atlas */
    [ITEM_WOOD_PICKAXE  - ITEM_TOOL_FIRST] = { "wooden pickaxe", true, TOOL_PICKAXE, MATERIAL_WOOD,  DUR_WOOD,  19 },
    [ITEM_WOOD_AXE      - ITEM_TOOL_FIRST] = { "wooden axe",     true, TOOL_AXE,     MATERIAL_WOOD,  DUR_WOOD,  20 },
    [ITEM_WOOD_SHOVEL   - ITEM_TOOL_FIRST] = { "wooden shovel",  true, TOOL_SHOVEL,  MATERIAL_WOOD,  DUR_WOOD,  21 },
    [ITEM_STONE_PICKAXE - ITEM_TOOL_FIRST] = { "stone pickaxe",  true, TOOL_PICKAXE, MATERIAL_STONE, DUR_STONE, 22 },
    [ITEM_STONE_AXE     - ITEM_TOOL_FIRST] = { "stone axe",      true, TOOL_AXE,     MATERIAL_STONE, DUR_STONE, 23 },
    [ITEM_STONE_SHOVEL  - ITEM_TOOL_FIRST] = { "stone shovel",   true, TOOL_SHOVEL,  MATERIAL_STONE, DUR_STONE, 24 },
    [ITEM_IRON_PICKAXE  - ITEM_TOOL_FIRST] = { "iron pickaxe",   true, TOOL_PICKAXE, MATERIAL_IRON,  DUR_IRON,  25 },
    [ITEM_IRON_AXE      - ITEM_TOOL_FIRST] = { "iron axe",       true, TOOL_AXE,     MATERIAL_IRON,  DUR_IRON,  26 },
    [ITEM_IRON_SHOVEL   - ITEM_TOOL_FIRST] = { "iron shovel",    true, TOOL_SHOVEL,  MATERIAL_IRON,  DUR_IRON,  27 },
};

/* Armour durability per tier (uses/hits). Iron sturdier than leather. */
#define DUR_LEATHER 80
#define DUR_IRON_AR 240

/* Armour metadata, indexed by armour slot (id - ITEM_ARMOR_FIRST). Defence
 * points mirror vanilla's distribution: chest > legs > head ≈ feet, with iron
 * worth more than leather in every slot. atlas tiles follow the materials. */
static const ItemDef g_armor_defs[ITEM_ARMOR_COUNT] = {
    /*                                       name                is_tool kind        material        durability   atlas slot               points */
    [ITEM_LEATHER_HELMET     - ITEM_ARMOR_FIRST] = { "leather helmet",     false, TOOL_NONE, MATERIAL_NONE, DUR_LEATHER, 31, ARMOR_SLOT_HEAD,  1 },
    [ITEM_LEATHER_CHESTPLATE - ITEM_ARMOR_FIRST] = { "leather chestplate", false, TOOL_NONE, MATERIAL_NONE, DUR_LEATHER, 32, ARMOR_SLOT_CHEST, 3 },
    [ITEM_LEATHER_LEGGINGS   - ITEM_ARMOR_FIRST] = { "leather leggings",   false, TOOL_NONE, MATERIAL_NONE, DUR_LEATHER, 33, ARMOR_SLOT_LEGS,  2 },
    [ITEM_LEATHER_BOOTS      - ITEM_ARMOR_FIRST] = { "leather boots",      false, TOOL_NONE, MATERIAL_NONE, DUR_LEATHER, 34, ARMOR_SLOT_FEET,  1 },
    [ITEM_IRON_HELMET        - ITEM_ARMOR_FIRST] = { "iron helmet",        false, TOOL_NONE, MATERIAL_NONE, DUR_IRON_AR, 35, ARMOR_SLOT_HEAD,  2 },
    [ITEM_IRON_CHESTPLATE    - ITEM_ARMOR_FIRST] = { "iron chestplate",    false, TOOL_NONE, MATERIAL_NONE, DUR_IRON_AR, 36, ARMOR_SLOT_CHEST, 6 },
    [ITEM_IRON_LEGGINGS      - ITEM_ARMOR_FIRST] = { "iron leggings",      false, TOOL_NONE, MATERIAL_NONE, DUR_IRON_AR, 37, ARMOR_SLOT_LEGS,  5 },
    [ITEM_IRON_BOOTS         - ITEM_ARMOR_FIRST] = { "iron boots",         false, TOOL_NONE, MATERIAL_NONE, DUR_IRON_AR, 38, ARMOR_SLOT_FEET,  2 },
};

/* A reusable non-tool def for block items (and out-of-range ids). The name is
 * filled from the BlockDef so block items still report a sensible name. */
static ItemDef make_block_def(ItemId id) {
    ItemDef d = {0};
    d.name           = block_get_def(item_as_block(id))->name;
    d.is_tool        = false;
    d.tool_kind      = TOOL_NONE;
    d.material       = MATERIAL_NONE;
    d.max_durability = 0;
    d.atlas_tile     = 0;
    d.armor_slot     = ARMOR_SLOT_NONE;
    d.armor_points   = 0;
    return d;
}

/* Names for non-block crafting materials (indexed by id - ITEM_MATERIAL_FIRST). */
static const char* g_material_names[ITEM_MATERIAL_COUNT] = {
    [ITEM_STICK      - ITEM_MATERIAL_FIRST] = "stick",
    [ITEM_LEATHER    - ITEM_MATERIAL_FIRST] = "leather",
    [ITEM_IRON_INGOT - ITEM_MATERIAL_FIRST] = "iron ingot",
};

const ItemDef* item_get_def(ItemId id) {
    if (item_is_tool(id))
        return &g_tool_defs[id - ITEM_TOOL_FIRST];
    if (item_is_armor(id))
        return &g_armor_defs[id - ITEM_ARMOR_FIRST];
    static ItemDef def;
    if (item_is_material(id)) {
        /* Crafting materials: non-tool, non-block. Stackable, no durability. */
        def = (ItemDef){0};
        def.name       = g_material_names[id - ITEM_MATERIAL_FIRST];
        def.armor_slot = ARMOR_SLOT_NONE;
        return &def;
    }
    /* Block items and any out-of-range id: synthesize a non-tool def.
     * Static storage so callers can hold the pointer for the call's duration. */
    def = make_block_def(id);
    return &def;
}

ArmorSlot item_armor_slot(ItemId id) {
    if (!item_is_armor(id)) return ARMOR_SLOT_NONE;
    return (ArmorSlot)g_armor_defs[id - ITEM_ARMOR_FIRST].armor_slot;
}

int item_armor_points(ItemId id) {
    if (!item_is_armor(id)) return 0;
    return g_armor_defs[id - ITEM_ARMOR_FIRST].armor_points;
}

int armor_points_total(const ItemId equipped[ARMOR_SLOT_COUNT]) {
    int total = 0;
    for (int i = 0; i < ARMOR_SLOT_COUNT; i++)
        total += item_armor_points(equipped[i]);
    if (total > ARMOR_MAX_POINTS) total = ARMOR_MAX_POINTS;
    return total;
}

int damage_after_armor(int raw, int points) {
    if (raw <= 0) return 0;
    if (points < 0) points = 0;
    if (points > ARMOR_MAX_POINTS) points = ARMOR_MAX_POINTS;

    /* Each point = 4% reduction, capped at ARMOR_CAP_PERCENT. */
    int reduction = points * 4;
    if (reduction > ARMOR_CAP_PERCENT) reduction = ARMOR_CAP_PERCENT;

    /* Integer math: damage * (100 - reduction) / 100, rounded down. */
    int out = raw * (100 - reduction) / 100;
    if (out < 1) out = 1;   /* armour never makes a real blow harmless */
    return out;
}

uint8_t item_stack_max(ItemId id) {
    return item_is_tool(id) ? 1 : 64;
}

void item_representative_color(ItemId id, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (item_is_material(id)) {
        switch (id) {
            case ITEM_STICK:      *r = 120; *g = 82;  *b = 44;  break; /* wooden stick */
            case ITEM_LEATHER:    *r = 150; *g = 102; *b = 60;  break; /* tan hide */
            case ITEM_IRON_INGOT: *r = 216; *g = 216; *b = 220; break; /* iron grey */
            default:              *r = 200; *g = 120; *b = 200; break; /* flag unknown */
        }
        return;
    }
    if (item_is_armor(id)) {
        /* Tint by tier so leather/iron armour read distinctly in the UI. */
        if (id >= ITEM_IRON_HELMET) { *r = 200; *g = 200; *b = 208; }  /* iron */
        else                        { *r = 150; *g = 102; *b = 60;  }  /* leather */
        return;
    }
    if (!item_is_tool(id)) {
        block_representative_color(item_as_block(id), r, g, b);
        return;
    }
    /* Tools: tint by material tier so wood/stone/iron read distinctly. */
    switch (item_get_def(id)->material) {
        case MATERIAL_WOOD:  *r = 150; *g = 111; *b =  51; break;
        case MATERIAL_STONE: *r = 128; *g = 128; *b = 128; break;
        case MATERIAL_IRON:  *r = 216; *g = 216; *b = 220; break;
        default:             *r = 200; *g = 200; *b = 200; break;
    }
}

/* Material speed multiplier applied when the tool matches the block category.
 * Strictly increasing wood<stone<iron so higher tiers break faster. */
static float material_speed(ToolMaterial m) {
    switch (m) {
        case MATERIAL_WOOD:  return 2.0f;
        case MATERIAL_STONE: return 4.0f;
        case MATERIAL_IRON:  return 6.0f;
        default:             return 1.0f;
    }
}

/* Which tool kind is the "right tool" for a block. Kept here (not in block.c)
 * so block.h stays free of the item/tool vocabulary. Pure mapping. */
static ToolKind block_tool_category(BlockID block) {
    switch (block) {
        case BLOCK_STONE:
        case BLOCK_COBBLE:
        case BLOCK_COAL_ORE:
        case BLOCK_IRON_ORE:
        case BLOCK_GOLD_ORE:
        case BLOCK_DIAMOND_ORE:
            return TOOL_PICKAXE;
        case BLOCK_WOOD:
        case BLOCK_PLANKS:
            return TOOL_AXE;
        case BLOCK_DIRT:
        case BLOCK_SAND:
        case BLOCK_GRASS:
        case BLOCK_PATH:
            return TOOL_SHOVEL;
        default:
            return TOOL_NONE;
    }
}

float tool_break_time(ItemId tool, BlockID block) {
    float base = block_break_time(block);

    /* Unbreakable / instant blocks: tools change nothing. */
    if (base <= 0.0f || base >= BLOCK_BREAK_UNBREAKABLE) return base;

    if (!item_is_tool(tool)) return base;          /* block item / hand */

    const ItemDef* def = item_get_def(tool);
    ToolKind needed = block_tool_category(block);
    if (needed == TOOL_NONE || def->tool_kind != needed)
        return base;                               /* wrong tool: no bonus */

    return base / material_speed(def->material);
}
