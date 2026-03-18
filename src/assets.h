#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>
#include <stddef.h>

extern const uint8_t g_atlas_pixels[];
extern const uint8_t g_player_skin_pixels[];

extern const uint8_t g_block_vert_spv[];
extern const size_t  g_block_vert_spv_size;
extern const uint8_t g_block_frag_spv[];
extern const size_t  g_block_frag_spv_size;
extern const uint8_t g_player_vert_spv[];
extern const size_t  g_player_vert_spv_size;
extern const uint8_t g_player_frag_spv[];
extern const size_t  g_player_frag_spv_size;

#define ATLAS_SIZE       256
#define ATLAS_MIP_LEVELS 4   /* 256 -> 128 -> 64 -> 32; keeps bleeding minimal */
#define TILE_SIZE        16
#define SKIN_WIDTH       64
#define SKIN_HEIGHT      32

#endif
