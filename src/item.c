#include "item.h"

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
    return d;
}

const ItemDef* item_get_def(ItemId id) {
    if (item_is_tool(id))
        return &g_tool_defs[id - ITEM_TOOL_FIRST];
    /* Block items and any out-of-range id: synthesize a non-tool def.
     * Static storage so callers can hold the pointer for the call's duration. */
    static ItemDef block_def;
    block_def = make_block_def(id);
    return &block_def;
}

uint8_t item_stack_max(ItemId id) {
    return item_is_tool(id) ? 1 : 64;
}

void item_representative_color(ItemId id, uint8_t* r, uint8_t* g, uint8_t* b) {
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
