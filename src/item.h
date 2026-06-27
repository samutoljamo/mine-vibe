#ifndef ITEM_H
#define ITEM_H

#include <stdint.h>
#include <stdbool.h>
#include "block.h"

/* ------------------------------------------------------------------ */
/*  Unified item id space                                              */
/*                                                                     */
/*  ItemId is a single id space spanning both placeable blocks and     */
/*  tools. The low range [0, BLOCK_COUNT) is identical to BlockID, so  */
/*  a block item id IS its BlockID and can be placed directly. Tools   */
/*  occupy a contiguous range starting at BLOCK_COUNT. This keeps the  */
/*  block <-> item conversion a no-op for placeable blocks and leaves  */
/*  headroom above the tool range for future non-block items (armour,  */
/*  food, crafting materials) without renumbering.                     */
/* ------------------------------------------------------------------ */
typedef uint16_t ItemId;

/* Tool kinds: which block category a tool is the "right tool" for. */
typedef enum {
    TOOL_NONE = 0,   /* not a tool (block item) */
    TOOL_PICKAXE,    /* stone, cobble, ores */
    TOOL_AXE,        /* wood, planks */
    TOOL_SHOVEL,     /* dirt, sand, grass, path */
} ToolKind;

/* Material tier: governs mining speed multiplier and durability. */
typedef enum {
    MATERIAL_NONE = 0,
    MATERIAL_WOOD,
    MATERIAL_STONE,
    MATERIAL_IRON,
} ToolMaterial;

/* Tool item ids. Layout is material-major (wood, stone, iron) × kind
 * (pickaxe, axe, shovel) so a tier "block" sits together. The exact
 * numbering is not relied on outside item.c; use the named constants. */
enum {
    ITEM_TOOL_FIRST = BLOCK_COUNT,

    ITEM_WOOD_PICKAXE = ITEM_TOOL_FIRST,
    ITEM_WOOD_AXE,
    ITEM_WOOD_SHOVEL,

    ITEM_STONE_PICKAXE,
    ITEM_STONE_AXE,
    ITEM_STONE_SHOVEL,

    ITEM_IRON_PICKAXE,
    ITEM_IRON_AXE,
    ITEM_IRON_SHOVEL,

    ITEM_TOOL_END,                                  /* one past last tool */
    ITEM_TOOL_LAST  = ITEM_TOOL_END - 1,
    ITEM_TOOL_COUNT = ITEM_TOOL_END - ITEM_TOOL_FIRST,

    ITEM_COUNT = ITEM_TOOL_END,
};

/* Immutable per-item metadata. For block items most tool fields are zeroed
 * (TOOL_NONE / MATERIAL_NONE / durability 0). For tools, `block` is unused. */
typedef struct ItemDef {
    const char*  name;
    bool         is_tool;
    ToolKind     tool_kind;       /* TOOL_NONE for blocks */
    ToolMaterial material;        /* MATERIAL_NONE for blocks */
    uint16_t     max_durability;  /* 0 for blocks / unbreakable use */
    uint8_t      atlas_tile;      /* atlas tile index for the tool icon (tools only) */
} ItemDef;

/* ------------------------------------------------------------------ */
/*  Pure classification / conversion helpers                          */
/* ------------------------------------------------------------------ */

static inline bool item_is_block(ItemId id) {
    return id < BLOCK_COUNT;
}

static inline bool item_is_tool(ItemId id) {
    return id >= ITEM_TOOL_FIRST && id <= ITEM_TOOL_LAST;
}

/* Block id -> item id (identity in the low range). */
static inline ItemId item_from_block(BlockID b) {
    return (ItemId)b;
}

/* Item id -> block id. Only meaningful when item_is_block(id); returns
 * BLOCK_AIR for non-block items so callers can't accidentally "place" a tool. */
static inline BlockID item_as_block(ItemId id) {
    return item_is_block(id) ? (BlockID)id : (BlockID)BLOCK_AIR;
}

/* Metadata lookup. Out-of-range ids return a safe "air"/non-tool def. */
const ItemDef* item_get_def(ItemId id);

/* Max stack size for an item: tools are unstackable (1), blocks stack high. */
uint8_t item_stack_max(ItemId id);

/* Representative RGB for an item icon. Block items defer to
 * block_representative_color; tools use a material-tier tint. Lets the UI draw
 * a recognisable icon for tools without a dedicated icon-baking path. */
void item_representative_color(ItemId id, uint8_t* r, uint8_t* g, uint8_t* b);

/* ------------------------------------------------------------------ */
/*  Pure mining math                                                   */
/* ------------------------------------------------------------------ */

/* Seconds of sustained mining to break `block` while holding `tool`.
 *
 * Pure: a function of (tool, block) only. Starts from block_break_time(block)
 * and, when `tool` is a tool whose kind matches the block's category, divides
 * by the material's speed multiplier (iron fastest, then stone, then wood).
 * A wrong-category tool, a block item, or no match yields the base hand time.
 * Unbreakable blocks (bedrock) stay unbreakable regardless of tool. */
float tool_break_time(ItemId tool, BlockID block);

#endif /* ITEM_H */
