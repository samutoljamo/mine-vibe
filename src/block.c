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
};

const BlockDef* block_get_def(BlockID id) {
    if (id >= BLOCK_COUNT) return &block_defs[BLOCK_AIR];
    return &block_defs[id];
}
