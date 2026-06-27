#include "block.h"

static const BlockDef block_defs[BLOCK_COUNT] = {
    /*                  solid  transp gravity absorb emit  top side bottom */
    [BLOCK_AIR]     = { "air",     false, true,  false, 0,  0, 0,  0,  0 },
    [BLOCK_STONE]   = { "stone",   true,  false, false, 15, 0, 0,  0,  0 },
    [BLOCK_DIRT]    = { "dirt",    true,  false, false, 15, 0, 1,  1,  1 },
    [BLOCK_GRASS]   = { "grass",   true,  false, false, 15, 0, 2,  3,  1 },
    [BLOCK_SAND]    = { "sand",    true,  false, true,  15, 0, 4,  4,  4 },
    [BLOCK_WOOD]    = { "wood",    true,  false, false, 15, 0, 5,  6,  5 },
    [BLOCK_LEAVES]  = { "leaves",  true,  true,  false, 2,  0, 7,  7,  7 },
    [BLOCK_WATER]   = { "water",   false, true,  false, 2,  0, 16, 16, 16 },
    [BLOCK_BEDROCK] = { "bedrock", true,  false, false, 15, 0, 17, 17, 17 },
    /* Ores: stone-bodied blocks with a coloured speckle texture per face. */
    [BLOCK_COAL_ORE]    = { "coal_ore",    true, false, false, 15, 0,  8,  8,  8 },
    [BLOCK_IRON_ORE]    = { "iron_ore",    true, false, false, 15, 0,  9,  9,  9 },
    [BLOCK_GOLD_ORE]    = { "gold_ore",    true, false, false, 15, 0, 10, 10, 10 },
    [BLOCK_DIAMOND_ORE] = { "diamond_ore", true, false, false, 15, 0, 11, 11, 11 },
};

const BlockDef* block_get_def(BlockID id) {
    if (id >= BLOCK_COUNT) return &block_defs[BLOCK_AIR];
    return &block_defs[id];
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
        default:            *r = 255; *g =   0; *b = 255; break;   /* magenta = bug */
    }
}
