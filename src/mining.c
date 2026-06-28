#include "mining.h"

/* ------------------------------------------------------------------ */
/*  Tier model                                                         */
/*                                                                     */
/*  A single ordered "harvest level" scale underpins both the speed    */
/*  multiplier and drop gating, so adding a tier (diamond) is one row  */
/*  in each table below — no branching logic to touch.                 */
/* ------------------------------------------------------------------ */

/* Harvest level per tool material. The hand (MATERIAL_NONE) is level 0; each
 * material tier is one higher. Add MATERIAL_DIAMOND -> 4 here when it lands. */
static int material_harvest_level(ToolMaterial m) {
    switch (m) {
        case MATERIAL_WOOD:  return 1;
        case MATERIAL_STONE: return 2;
        case MATERIAL_IRON:  return 3;
        default:             return 0;   /* MATERIAL_NONE / hand */
    }
}

/* Speed multiplier per tool material when the tool matches the block category.
 * Strictly increasing so higher tiers mine faster. Mirrors item.c's table; a
 * new tier is one row. The baseline (no/wrong tool) is 1.0, handled below. */
static float material_speed_multiplier(ToolMaterial m) {
    switch (m) {
        case MATERIAL_WOOD:  return 2.0f;
        case MATERIAL_STONE: return 4.0f;
        case MATERIAL_IRON:  return 6.0f;
        default:             return 1.0f;
    }
}

/* Which tool kind is the "right tool" for a block (TOOL_NONE = any/none).
 * Kept here so block.h stays free of tool vocabulary. Pure mapping; add a row
 * per gated block. */
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

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

float mining_speed_multiplier(ItemId tool, BlockID block) {
    if (!item_is_tool(tool)) return 1.0f;          /* hand / block item */

    const ItemDef* def = item_get_def(tool);
    ToolKind needed = block_tool_category(block);
    if (needed == TOOL_NONE || def->tool_kind != needed)
        return 1.0f;                               /* wrong tool: no speedup */

    return material_speed_multiplier(def->material);
}

int tool_harvest_level(ItemId tool) {
    if (!item_is_tool(tool)) return 0;             /* hand / block item */
    return material_harvest_level(item_get_def(tool)->material);
}

/* Required harvest level per block. 0 = harvestable by hand (no gate). A gated
 * block also implies a tool *kind* (see block_tool_category); the level here is
 * the minimum material tier of that kind. One row per gated block. */
int block_required_harvest_level(BlockID block) {
    switch (block) {
        case BLOCK_STONE:
        case BLOCK_COBBLE:
        case BLOCK_COAL_ORE:
            return 1;   /* wooden pickaxe or better */
        case BLOCK_IRON_ORE:
        case BLOCK_GOLD_ORE:
            return 2;   /* stone pickaxe or better */
        case BLOCK_DIAMOND_ORE:
            return 3;   /* iron pickaxe or better */
        default:
            return 0;   /* no requirement: drops by hand */
    }
}

bool block_drops_with(ItemId tool, BlockID block) {
    int required = block_required_harvest_level(block);
    if (required <= 0) return true;                /* no gate: always drops */

    /* Gated block: must use the correct tool kind AND meet the level. */
    ToolKind needed = block_tool_category(block);
    if (needed != TOOL_NONE) {
        if (!item_is_tool(tool)) return false;
        if (item_get_def(tool)->tool_kind != needed) return false;
    }
    return tool_harvest_level(tool) >= required;
}
