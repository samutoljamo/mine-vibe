#ifndef VERTEX_H
#define VERTEX_H

#include <stdint.h>
#include <cglm/cglm.h>
#include <vulkan/vulkan.h>

typedef struct BlockVertex {
    float    pos[3];   /* 12 */
    float    uv[2];    /* 8 */
    uint8_t  normal;   /* 1 — 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z */
    uint8_t  ao;       /* 1 — 0..3 */
    uint8_t  light;    /* 1 — 0..15 (sky channel; spec 1) */
    uint8_t  tile;     /* 1 — atlas tile index for greedy-merged quads whose
                        *     UVs span [0..W]x[0..H] tile-repeat units; the
                        *     fragment shader wraps them into this tile. 255 =
                        *     no tiling (use uv verbatim, single in-tile quad). */
} BlockVertex;

_Static_assert(sizeof(BlockVertex) == 24, "BlockVertex must be 24 bytes");

typedef struct GlobalUBO {
    mat4  view;
    mat4  proj;
    vec4  sun_direction;
    vec4  sun_color;
    float ambient;
    float underwater;   /* 1.0 when camera is inside a water block, else 0.0 */
    float _pad[2];
} GlobalUBO;

typedef struct ChunkPushConstants {
    vec4 chunk_offset;
} ChunkPushConstants;

static inline VkVertexInputBindingDescription vertex_binding_desc(void) {
    return (VkVertexInputBindingDescription){
        .binding = 0,
        .stride = sizeof(BlockVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
}

static inline void vertex_attr_descs(VkVertexInputAttributeDescription out[6]) {
    out[0] = (VkVertexInputAttributeDescription){
        .location = 0, .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0,
    };
    out[1] = (VkVertexInputAttributeDescription){
        .location = 1, .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT, .offset = 12,
    };
    out[2] = (VkVertexInputAttributeDescription){
        .location = 2, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 20,
    };
    out[3] = (VkVertexInputAttributeDescription){
        .location = 3, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 21,
    };
    out[4] = (VkVertexInputAttributeDescription){
        .location = 4, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 22,
    };
    out[5] = (VkVertexInputAttributeDescription){
        .location = 5, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 23,
    };
}

#endif
