#ifndef PLAYER_MODEL_H
#define PLAYER_MODEL_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Renderer Renderer;

typedef struct {
    float   x, y, z;
    float   u, v;
    uint8_t face_idx;
    uint8_t _pad[3];
} PlayerVertex;

_Static_assert(sizeof(PlayerVertex) == 24, "PlayerVertex must be 24 bytes");

typedef struct {
    float pos[3];   /* feet position in world space */
    float yaw;      /* rotation in radians around Y axis */
} PlayerRenderState;

typedef struct {
    VkBuffer      vertex_buffer;
    VmaAllocation vertex_alloc;
    VkBuffer      index_buffer;
    VmaAllocation index_alloc;
    uint32_t      index_count;
} PlayerModel;

bool player_model_init(Renderer* r, PlayerModel* m);
void player_model_destroy(Renderer* r, PlayerModel* m);
void player_model_draw(Renderer* r, VkCommandBuffer cmd,
                       const PlayerModel* m,
                       const PlayerRenderState* states, uint32_t count);

#endif
