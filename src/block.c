#include "block.h"

static const BlockDef block_defs[BLOCK_COUNT] = {
    [BLOCK_AIR]     = { "air",     false, true,  0,  0,  0 },
    [BLOCK_STONE]   = { "stone",   true,  false, 0,  0,  0 },
    [BLOCK_DIRT]    = { "dirt",    true,  false, 1,  1,  1 },
    [BLOCK_GRASS]   = { "grass",   true,  false, 2,  3,  1 },
    [BLOCK_SAND]    = { "sand",    true,  false, 4,  4,  4 },
    [BLOCK_WOOD]    = { "wood",    true,  false, 5,  6,  5 },
    [BLOCK_LEAVES]  = { "leaves",  true,  true,  7,  7,  7 },
    [BLOCK_WATER]   = { "water",   false, true,  16, 16, 16 },
    [BLOCK_BEDROCK] = { "bedrock", true,  false, 17, 17, 17 },
};

const BlockDef* block_get_def(BlockID id) {
    if (id >= BLOCK_COUNT) return &block_defs[BLOCK_AIR];
    return &block_defs[id];
}
