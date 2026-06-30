#ifndef PLAYER_MODEL_H
#define PLAYER_MODEL_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdint.h>
#include <stdbool.h>
#include "mob_model.h"   /* MobModel (per-type box data the baker consumes) */
#include "player_anim.h" /* AnimPart / ANIM_PART_COUNT */

typedef struct Renderer Renderer;

typedef struct {
    float   x, y, z;
    float   u, v;
    uint8_t face_idx;
    uint8_t part;        /* AnimPart this vertex belongs to (limb id, 0..ANIM_PART_COUNT) */
    uint8_t _pad[2];
} PlayerVertex;

_Static_assert(sizeof(PlayerVertex) == 24, "PlayerVertex must be 24 bytes");

typedef struct {
    float pos[3];   /* feet position in world space */
    float yaw;      /* rotation in radians around Y axis */
    float tint[4];  /* a == 0: real player skin (rgb ignored).
                     * a  > 0: two-tone mob, rgb = upper/primary colour. */
    float tint2[3]; /* mob only: lower/secondary two-tone colour (rgb) */
    float scale[3]; /* per-axis body scale (1,1,1 = default model dims).
                     * Mobs scale the shared box to their silhouette. */
    int   mesh_type; /* which baked mesh to draw: -1 = humanoid player model;
                      * >= 0 = MobType, drawn with its per-type baked mesh. */

    /* Per-limb animation. limb_angle[part] is a pitch (radians, rotation about
     * the part's pivot on the X axis); head_yaw is an extra yaw (radians, about
     * Y) applied only to the head. Each part rotates about its joint pivot (see
     * player_anim_pivot) before model*yaw*scale. ALL-ZERO == rigid default pose
     * (no visual change vs. the old single-mesh look). Callers that leave this
     * zero-initialized get the static pose. Fill via player_anim_walk() etc. */
    float limb_angle[ANIM_PART_COUNT];
    float head_yaw;
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

/* Draw a single render state with a specific baked mesh (player humanoid or a
 * per-type mob mesh). Caller has already bound the player pipeline + descriptor
 * set. Used by the frame loop to select a mesh per render state. */
void player_model_draw_one(Renderer* r, VkCommandBuffer cmd,
                           const PlayerModel* m,
                           const PlayerRenderState* state);

/* Bake an arbitrary box-list mob model (mob_model_for) into a PlayerModel
 * (vertex/index VkBuffers) using the SAME PlayerVertex format/pipeline as the
 * humanoid. Each box becomes 6 quads; per-box two-tone is expressed by routing
 * upper-body parts to a top-half UV (shader paints with tint) and legs to a
 * bottom-half UV (tint2), so the existing player shaders/pipeline are unchanged.
 * Destroy with player_model_destroy(). Returns false on allocation failure. */
bool mob_mesh_bake(Renderer* r, const MobModel* model, PlayerModel* out);

#endif
