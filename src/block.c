#include "block.h"

static const BlockDef block_defs[BLOCK_COUNT] = {
    /*                  solid  transp gravity absorb emit  top side bottom hardness */
    [BLOCK_AIR]     = { "air",     false, true,  false, 0,  0, 0,  0,  0,  HARDNESS_INSTANT },
    [BLOCK_STONE]   = { "stone",   true,  false, false, 15, 0, 0,  0,  0,  HARDNESS_HARD },
    [BLOCK_DIRT]    = { "dirt",    true,  false, false, 15, 0, 1,  1,  1,  HARDNESS_SOFT },
    [BLOCK_GRASS]   = { "grass",   true,  false, false, 15, 0, 2,  3,  1,  HARDNESS_SOFT },
    [BLOCK_SAND]    = { "sand",    true,  false, true,  15, 0, 4,  4,  4,  HARDNESS_SOFT },
    [BLOCK_WOOD]    = { "wood",    true,  false, false, 15, 0, 5,  6,  5,  HARDNESS_MEDIUM },
    [BLOCK_LEAVES]  = { "leaves",  true,  true,  false, 2,  0, 7,  7,  7,  HARDNESS_SOFT },
    [BLOCK_WATER]   = { "water",   false, true,  false, 2,  0, 16, 16, 16, HARDNESS_INSTANT },
    [BLOCK_BEDROCK] = { "bedrock", true,  false, false, 15, 0, 17, 17, 17, HARDNESS_UNBREAKABLE },
    /* Ores: stone-bodied blocks with a coloured speckle texture per face. */
    [BLOCK_COAL_ORE]    = { "coal_ore",    true, false, false, 15, 0,  8,  8,  8,  HARDNESS_HARD },
    [BLOCK_IRON_ORE]    = { "iron_ore",    true, false, false, 15, 0,  9,  9,  9,  HARDNESS_HARD },
    [BLOCK_GOLD_ORE]    = { "gold_ore",    true, false, false, 15, 0, 10, 10, 10, HARDNESS_HARD },
    [BLOCK_DIAMOND_ORE] = { "diamond_ore", true, false, false, 15, 0, 11, 11, 11, HARDNESS_HARD },
    /* Village decorative blocks. GLASS is solid but transparent with low light
     * absorption so windows let light leak through and neighbour faces render. */
    [BLOCK_PLANKS]  = { "planks", true, false, false, 15, 0, 12, 12, 12, HARDNESS_MEDIUM },
    [BLOCK_COBBLE]  = { "cobble", true, false, false, 15, 0, 13, 13, 13, HARDNESS_HARD },
    [BLOCK_GLASS]   = { "glass",  true, true,  false, 2,  0, 14, 14, 14, HARDNESS_LOW },
    [BLOCK_PATH]    = { "path",   true, false, false, 15, 0, 15, 15, 15, HARDNESS_SOFT },
    /* Torch: a full-cube emissive block (v1 keeps the mesher generic). Solid
     * and opaque so it occludes like any block, but seeds block light 14 that
     * propagates into surrounding air. */
    [BLOCK_TORCH]   = { "torch",  true, false, false, 15, 14, 18, 18, 18, HARDNESS_INSTANT },
    /* Furnace: stone-bodied crafting block. Distinct front face (firebox) vs
     * the plain stone sides/top. Stone-like hardness so it needs a pickaxe. */
    [BLOCK_FURNACE] = { "furnace", true, false, false, 15, 0, 51, 50, 51, HARDNESS_HARD },
    /* Chest: wood-bodied storage block. Wood-like hardness (axe-friendly). */
    [BLOCK_CHEST]   = { "chest",   true, false, false, 15, 0, 52, 53, 52, HARDNESS_MEDIUM },
};

const BlockDef* block_get_def(BlockID id) {
    if (id >= BLOCK_COUNT) return &block_defs[BLOCK_AIR];
    return &block_defs[id];
}

float block_break_time(BlockID block) {
    uint8_t h = block_get_def(block)->hardness;
    if (h == HARDNESS_UNBREAKABLE) return BLOCK_BREAK_UNBREAKABLE;
    if (h == HARDNESS_INSTANT)     return 0.0f;
    /* Linear in hardness tier: 0.25s per unit. Yields dirt 0.25s, glass 0.5s,
     * wood/planks 0.75s, stone/cobble/ore 1.5s, hardest 3.0s. Strictly
     * increasing in hardness, so ordering is preserved. */
    return 0.25f * (float)h;
}

void block_representative_color(BlockID id, uint8_t* r, uint8_t* g, uint8_t* b) {
    switch (id) {
        case BLOCK_STONE:   *r = 130; *g = 130; *b = 130; break;
        case BLOCK_DIRT:    *r = 134; *g =  96; *b =  67; break;
        case BLOCK_GRASS:   *r =  91; *g = 153; *b =  72; break;
        case BLOCK_SAND:    *r = 219; *g = 211; *b = 160; break;
        case BLOCK_WOOD:    *r = 109; *g =  82; *b =  47; break;
        case BLOCK_LEAVES:  *r =  60; *g = 117; *b =  44; break;
        case BLOCK_WATER:   *r =  64; *g = 128; *b = 220; break;
        case BLOCK_BEDROCK: *r =  60; *g =  60; *b =  60; break;
        case BLOCK_COAL_ORE:    *r =  54; *g =  54; *b =  54; break;
        case BLOCK_IRON_ORE:    *r = 184; *g = 150; *b = 117; break;
        case BLOCK_GOLD_ORE:    *r = 222; *g = 188; *b =  72; break;
        case BLOCK_DIAMOND_ORE: *r = 102; *g = 214; *b = 213; break;
        case BLOCK_PLANKS:  *r = 160; *g = 124; *b =  74; break;
        case BLOCK_COBBLE:  *r = 110; *g = 110; *b = 110; break;
        case BLOCK_GLASS:   *r = 198; *g = 226; *b = 232; break;
        case BLOCK_PATH:    *r = 120; *g = 105; *b =  84; break;
        case BLOCK_TORCH:   *r = 240; *g = 180; *b =  60; break;
        case BLOCK_FURNACE: *r = 100; *g = 100; *b = 100; break;
        case BLOCK_CHEST:   *r = 162; *g = 120; *b =  62; break;
        default:            *r = 255; *g =   0; *b = 255; break;   /* magenta = bug */
    }
}
