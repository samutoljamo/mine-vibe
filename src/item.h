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

    /* Non-block crafting materials. These live above the tool range and below
     * ITEM_COUNT; they are not placeable (item_is_block is false) and not
     * tools (item_is_tool is false), so they only flow through crafting and
     * the inventory. STICK is the first such material; add more here. */
    ITEM_MATERIAL_FIRST = ITEM_TOOL_END,

    ITEM_STICK = ITEM_MATERIAL_FIRST,
    ITEM_LEATHER,        /* armour crafting material (leather tier) */
    ITEM_IRON_INGOT,     /* armour crafting material (iron tier); smelted from ore */
    ITEM_FEATHER,        /* chicken drop; future arrow/fletching material */
    ITEM_BONE,           /* skeleton drop; future bonemeal/taming material */
    ITEM_ARROW,          /* skeleton drop; future ranged ammo */
    ITEM_GUNPOWDER,      /* creeper drop; future explosives material */

    ITEM_MATERIAL_END,
    ITEM_MATERIAL_LAST  = ITEM_MATERIAL_END - 1,
    ITEM_MATERIAL_COUNT = ITEM_MATERIAL_END - ITEM_MATERIAL_FIRST,

    /* Food items. Edible: each carries a hunger-restore value (see
     * item_hunger_restore). Not placeable, not tools. Raw meats restore less
     * than their cooked counterparts; rotten flesh restores a little but is the
     * zombie drop. The actual "eat -> restore hunger" server wiring is a
     * follow-up; these only add the item DATA today. */
    ITEM_FOOD_FIRST = ITEM_MATERIAL_END,

    ITEM_RAW_PORK = ITEM_FOOD_FIRST,
    ITEM_COOKED_PORK,
    ITEM_RAW_BEEF,
    ITEM_COOKED_BEEF,
    ITEM_RAW_CHICKEN,
    ITEM_COOKED_CHICKEN,
    ITEM_ROTTEN_FLESH,

    ITEM_FOOD_END,
    ITEM_FOOD_LAST  = ITEM_FOOD_END - 1,
    ITEM_FOOD_COUNT = ITEM_FOOD_END - ITEM_FOOD_FIRST,

    /* Armour items. Layout is tier-major (leather, iron) × slot
     * (helmet, chestplate, leggings, boots). Not placeable, not tools; they
     * carry an armour-points value and durability and live in dedicated
     * equipment slots (see ArmorSlot). The exact numbering is not relied on
     * outside item.c; use the named constants. */
    ITEM_ARMOR_FIRST = ITEM_FOOD_END,

    ITEM_LEATHER_HELMET = ITEM_ARMOR_FIRST,
    ITEM_LEATHER_CHESTPLATE,
    ITEM_LEATHER_LEGGINGS,
    ITEM_LEATHER_BOOTS,

    ITEM_IRON_HELMET,
    ITEM_IRON_CHESTPLATE,
    ITEM_IRON_LEGGINGS,
    ITEM_IRON_BOOTS,

    ITEM_ARMOR_END,
    ITEM_ARMOR_LAST  = ITEM_ARMOR_END - 1,
    ITEM_ARMOR_COUNT = ITEM_ARMOR_END - ITEM_ARMOR_FIRST,

    ITEM_COUNT = ITEM_ARMOR_END,
};

/* True for non-block, non-tool crafting materials (e.g. STICK, LEATHER). */
static inline bool item_is_material(uint16_t id) {
    return id >= ITEM_MATERIAL_FIRST && id <= ITEM_MATERIAL_LAST;
}

/* True for edible food items (raw/cooked meats, rotten flesh). */
static inline bool item_is_food(uint16_t id) {
    return id >= ITEM_FOOD_FIRST && id <= ITEM_FOOD_LAST;
}

/* True for wearable armour items. */
static inline bool item_is_armor(uint16_t id) {
    return id >= ITEM_ARMOR_FIRST && id <= ITEM_ARMOR_LAST;
}

/* ------------------------------------------------------------------ */
/*  Armour equipment                                                   */
/* ------------------------------------------------------------------ */

/* The four body slots a player can wear armour in. ARMOR_SLOT_COUNT armour
 * pieces are tracked per player; the enum value doubles as the equipment array
 * index. */
typedef enum {
    ARMOR_SLOT_HEAD = 0,
    ARMOR_SLOT_CHEST,
    ARMOR_SLOT_LEGS,
    ARMOR_SLOT_FEET,
    ARMOR_SLOT_COUNT,
    ARMOR_SLOT_NONE = -1,   /* item is not armour */
} ArmorSlot;

/* Max protection (each point = 4% reduction); 25 points clamps to the 80% cap
 * vanilla uses, so any full set can never exceed it. */
#define ARMOR_MAX_POINTS   25
#define ARMOR_CAP_PERCENT  80

/* Which body slot an armour item occupies, or ARMOR_SLOT_NONE for non-armour. */
ArmorSlot item_armor_slot(ItemId id);

/* Armour-defence points contributed by a single armour item (0 for non-armour
 * or empty/BLOCK_AIR). Pure lookup. */
int item_armor_points(ItemId id);

/* Immutable per-item metadata. For block items most tool fields are zeroed
 * (TOOL_NONE / MATERIAL_NONE / durability 0). For tools, `block` is unused. */
typedef struct ItemDef {
    const char*  name;
    bool         is_tool;
    ToolKind     tool_kind;       /* TOOL_NONE for blocks */
    ToolMaterial material;        /* MATERIAL_NONE for blocks */
    uint16_t     max_durability;  /* 0 for blocks / unbreakable use */
    uint8_t      atlas_tile;      /* atlas tile index for the tool icon (tools only) */
    int8_t       armor_slot;      /* ArmorSlot for armour items, ARMOR_SLOT_NONE otherwise */
    uint8_t      armor_points;    /* defence points for armour items, 0 otherwise */
    uint8_t      hunger_restore;  /* hunger points restored when eaten (food items), 0 otherwise */
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

/* Hunger points a food item restores when eaten; 0 for non-food items. Pure
 * lookup over the ItemDef table. The server-side eating wiring that consumes
 * this is a follow-up; this exposes the data. */
int item_hunger_restore(ItemId id);

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

/* ------------------------------------------------------------------ */
/*  Pure armour math                                                   */
/* ------------------------------------------------------------------ */

/* Sum of armour points across an equipment set of ARMOR_SLOT_COUNT item ids
 * (BLOCK_AIR / non-armour entries contribute 0). The result is clamped to
 * ARMOR_MAX_POINTS. Pure. */
int armor_points_total(const ItemId equipped[ARMOR_SLOT_COUNT]);

/* Damage remaining after armour reduction, Minecraft-style: each armour point
 * removes 4% of incoming damage, capped at ARMOR_CAP_PERCENT (80%). `raw` is the
 * pre-mitigation damage; `points` is the total armour points (clamped here too).
 * Always returns at least 0, and at least 1 when raw > 0 (armour never makes a
 * blow harmless). Pure. */
int damage_after_armor(int raw, int points);

#endif /* ITEM_H */
