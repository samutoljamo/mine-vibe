#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t BlockID;

enum {
    BLOCK_AIR = 0,
    BLOCK_STONE,
    BLOCK_DIRT,
    BLOCK_GRASS,
    BLOCK_SAND,
    BLOCK_WOOD,
    BLOCK_LEAVES,
    BLOCK_WATER,
    BLOCK_BEDROCK,
    BLOCK_COUNT,
};

typedef struct BlockDef {
    const char* name;
    bool        is_solid;
    bool        is_transparent;
    bool        is_gravity;      /* falls when unsupported */
    uint8_t     tex_top;
    uint8_t     tex_side;
    uint8_t     tex_bottom;
} BlockDef;

const BlockDef* block_get_def(BlockID id);

/* Returns a representative RGB colour for a block, used by UI block icons
 * (inventory, hotbar). Unknown IDs map to magenta to flag missing entries. */
void block_representative_color(BlockID id, uint8_t* r, uint8_t* g, uint8_t* b);

static inline bool block_is_solid(BlockID id) {
    return block_get_def(id)->is_solid;
}

static inline bool block_is_transparent(BlockID id) {
    return block_get_def(id)->is_transparent;
}

static inline bool block_is_gravity(BlockID id) {
    return block_get_def(id)->is_gravity;
}

#endif
