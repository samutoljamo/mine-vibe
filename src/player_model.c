#include "player_model.h"
#include "renderer.h"
#include "mob_model.h"
#include "mob_render.h"
#include <cglm/cglm.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* 6 body parts × 6 faces × 4 verts = 144 verts, 6 indices per face = 216 */
#define PLAYER_VERTEX_COUNT 144
#define PLAYER_INDEX_COUNT  216

/* ── UV helpers ─────────────────────────────────────────────────────────────
   All UV coords in this file are pixel coordinates on the 64×32 skin.
   Normalized to [0,1] when writing vertices.                               */

typedef struct { float u0, v0, u1, v1; } UVRect;

typedef struct {
    UVRect top, bot, px, frt, mx, bck;  /* +Y, -Y, +X, +Z, -X, -Z */
} BoxUV;

static const BoxUV HEAD_UV = {
    .top={8,0,16,8},   .bot={16,0,24,8},
    .px ={0,8,8,16},   .frt={8,8,16,16},
    .mx ={16,8,24,16}, .bck={24,8,32,16},
};
static const BoxUV BODY_UV = {
    .top={20,16,28,20}, .bot={28,16,36,20},
    .px ={16,20,20,32}, .frt={20,20,28,32},
    .mx ={28,20,32,32}, .bck={32,20,40,32},
};
static const BoxUV ARM_UV = {
    .top={44,16,48,20}, .bot={48,16,52,20},
    .px ={40,20,44,32}, .frt={44,20,48,32},
    .mx ={48,20,52,32}, .bck={52,20,56,32},
};
static const BoxUV LEG_UV = {
    .top={4,16,8,20},  .bot={8,16,12,20},
    .px ={0,20,4,32},  .frt={4,20,8,32},
    .mx ={8,20,12,32}, .bck={12,20,16,32},
};

/* ── Mesh builder ───────────────────────────────────────────────────────── */

/* Positions are in BL, BR, TR, TL order as seen from OUTSIDE the cube.
 * This produces CCW-from-outside winding (matching the block mesher),
 * so faces render front-facing in Vulkan rather than getting back-culled. */
static void add_face(PlayerVertex* verts, uint32_t* vi,
                     uint32_t* idxs, uint32_t* ii,
                     /* BL, BR, TR, TL positions */
                     float v0x, float v0y, float v0z,
                     float v1x, float v1y, float v1z,
                     float v2x, float v2y, float v2z,
                     float v3x, float v3y, float v3z,
                     UVRect uv, bool mirror_u, uint8_t face_idx, uint8_t part)
{
    float u0 = uv.u0/64.f, v0 = uv.v0/32.f;
    float u1 = uv.u1/64.f, v1 = uv.v1/32.f;
    if (mirror_u) { float tmp = u0; u0 = u1; u1 = tmp; }

    uint32_t base = *vi;
    /* UVs paired with each corner (image-V grows downward, so v1 > v0). */
    verts[(*vi)++] = (PlayerVertex){v0x,v0y,v0z, u0,v1, face_idx, part, {0}}; /* BL */
    verts[(*vi)++] = (PlayerVertex){v1x,v1y,v1z, u1,v1, face_idx, part, {0}}; /* BR */
    verts[(*vi)++] = (PlayerVertex){v2x,v2y,v2z, u1,v0, face_idx, part, {0}}; /* TR */
    verts[(*vi)++] = (PlayerVertex){v3x,v3y,v3z, u0,v0, face_idx, part, {0}}; /* TL */

    idxs[(*ii)++] = base+0; idxs[(*ii)++] = base+1; idxs[(*ii)++] = base+2;
    idxs[(*ii)++] = base+0; idxs[(*ii)++] = base+2; idxs[(*ii)++] = base+3;
}

/* CCW winding per face, viewed from outside (matches block-mesher).
   mirror_x: used for left arm/leg — swaps ±X face UVs and mirrors U. */
static void add_box(PlayerVertex* verts, uint32_t* vi,
                    uint32_t* idxs, uint32_t* ii,
                    float x0, float y0, float z0,
                    float x1, float y1, float z1,
                    const BoxUV* uv, bool mirror_x, uint8_t part)
{
    /* +X face (right), face_idx=0. View from +X: up=+Y, right=+Z. */
    add_face(verts,vi,idxs,ii,
        x1,y0,z0, x1,y0,z1, x1,y1,z1, x1,y1,z0,
        mirror_x ? uv->mx : uv->px, mirror_x, 0, part);

    /* -X face (left), face_idx=1. View from -X: up=+Y, right=-Z. */
    add_face(verts,vi,idxs,ii,
        x0,y0,z1, x0,y0,z0, x0,y1,z0, x0,y1,z1,
        mirror_x ? uv->px : uv->mx, mirror_x, 1, part);

    /* +Y face (top), face_idx=2. View from above: up=-Z, right=-X. */
    add_face(verts,vi,idxs,ii,
        x1,y1,z1, x0,y1,z1, x0,y1,z0, x1,y1,z0,
        uv->top, mirror_x, 2, part);

    /* -Y face (bottom), face_idx=3. View from below: up=-Z, right=+X. */
    add_face(verts,vi,idxs,ii,
        x0,y0,z1, x1,y0,z1, x1,y0,z0, x0,y0,z0,
        uv->bot, mirror_x, 3, part);

    /* +Z face (front), face_idx=4. View from +Z: up=+Y, right=-X. */
    add_face(verts,vi,idxs,ii,
        x1,y0,z1, x0,y0,z1, x0,y1,z1, x1,y1,z1,
        uv->frt, mirror_x, 4, part);

    /* -Z face (back), face_idx=5. View from -Z: up=+Y, right=+X. */
    add_face(verts,vi,idxs,ii,
        x0,y0,z0, x1,y0,z0, x1,y1,z0, x0,y1,z0,
        uv->bck, mirror_x, 5, part);
}

static void build_player_mesh(PlayerVertex* verts, uint32_t* idxs)
{
    uint32_t vi = 0, ii = 0;

    /* Head: 0.5×0.5×0.5, center (0, 1.50, 0) */
    add_box(verts,&vi,idxs,&ii, -0.25f,1.25f,-0.25f, 0.25f,1.75f,0.25f, &HEAD_UV, false, ANIM_PART_HEAD);
    /* Torso: 0.5×0.75×0.25, center (0, 0.875, 0) */
    add_box(verts,&vi,idxs,&ii, -0.25f,0.50f,-0.125f, 0.25f,1.25f,0.125f, &BODY_UV, false, ANIM_PART_TORSO);
    /* Right arm: 0.25×0.75×0.25, center (+0.375, 0.875, 0) */
    add_box(verts,&vi,idxs,&ii,  0.25f,0.50f,-0.125f, 0.50f,1.25f,0.125f, &ARM_UV, false, ANIM_PART_ARM_R);
    /* Left arm (mirrored): center (-0.375, 0.875, 0) */
    add_box(verts,&vi,idxs,&ii, -0.50f,0.50f,-0.125f,-0.25f,1.25f,0.125f, &ARM_UV, true, ANIM_PART_ARM_L);
    /* Right leg: 0.25×0.75×0.25, center (+0.125, 0.25, 0) */
    add_box(verts,&vi,idxs,&ii,  0.00f,-0.125f,-0.125f, 0.25f,0.625f,0.125f, &LEG_UV, false, ANIM_PART_LEG_R);
    /* Left leg (mirrored): center (-0.125, 0.25, 0) */
    add_box(verts,&vi,idxs,&ii, -0.25f,-0.125f,-0.125f, 0.00f,0.625f,0.125f, &LEG_UV, true, ANIM_PART_LEG_L);

    assert(vi == PLAYER_VERTEX_COUNT);
    assert(ii == PLAYER_INDEX_COUNT);
}

/* ── VkBuffer upload helper ─────────────────────────────────────────────── */

static bool upload_buffer(Renderer* r,
                          VkBufferUsageFlags usage,
                          const void* data, VkDeviceSize size,
                          VkBuffer* out_buf, VmaAllocation* out_alloc)
{
    /* Staging */
    VkBufferCreateInfo st_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo st_ac = {
        .usage = VMA_MEMORY_USAGE_CPU_ONLY,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer staging; VmaAllocation staging_alloc; VmaAllocationInfo staging_info;
    if (vmaCreateBuffer(r->allocator, &st_ci, &st_ac,
                        &staging, &staging_alloc, &staging_info) != VK_SUCCESS)
        return false;
    memcpy(staging_info.pMappedData, data, size);

    /* GPU buffer */
    VkBufferCreateInfo gpu_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
    };
    VmaAllocationCreateInfo gpu_ac = { .usage = VMA_MEMORY_USAGE_GPU_ONLY };
    if (vmaCreateBuffer(r->allocator, &gpu_ci, &gpu_ac,
                        out_buf, out_alloc, NULL) != VK_SUCCESS) {
        vmaDestroyBuffer(r->allocator, staging, staging_alloc);
        return false;
    }

    VkCommandBuffer cmd = renderer_begin_single_cmd(r);
    VkBufferCopy copy = { .size = size };
    vkCmdCopyBuffer(cmd, staging, *out_buf, 1, &copy);
    renderer_end_single_cmd(r, cmd);

    vmaDestroyBuffer(r->allocator, staging, staging_alloc);
    return true;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool player_model_init(Renderer* r, PlayerModel* m)
{
    PlayerVertex verts[PLAYER_VERTEX_COUNT];
    uint32_t     idxs[PLAYER_INDEX_COUNT];
    build_player_mesh(verts, idxs);

    if (!upload_buffer(r, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       verts, sizeof(verts),
                       &m->vertex_buffer, &m->vertex_alloc))
    {
        fprintf(stderr, "Failed to upload player vertex buffer\n");
        return false;
    }
    if (!upload_buffer(r, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       idxs, sizeof(idxs),
                       &m->index_buffer, &m->index_alloc))
    {
        fprintf(stderr, "Failed to upload player index buffer\n");
        vmaDestroyBuffer(r->allocator, m->vertex_buffer, m->vertex_alloc);
        return false;
    }
    m->index_count = PLAYER_INDEX_COUNT;
    return true;
}

/* Map a mob box (its part role + side) to an AnimPart so the per-limb shader
 * applies the matching joint. Arms/legs split left/right by box X sign (cx>=0 =>
 * right). Roles without a dedicated joint (snout/beak/wing/horn) ride with the
 * head, so they inherit head yaw and never animate independently. With all-zero
 * angles every mapping is a no-op, so the static silhouette is unchanged. */
static uint8_t mob_box_anim_part(MobPartRole role, float cx)
{
    bool right = (cx >= 0.0f);
    switch (role) {
        case MOB_PART_HEAD:
        case MOB_PART_SNOUT:
        case MOB_PART_BEAK:
        case MOB_PART_HORN:
            return ANIM_PART_HEAD;
        case MOB_PART_ARM:
        case MOB_PART_WING:
            return right ? ANIM_PART_ARM_R : ANIM_PART_ARM_L;
        case MOB_PART_LEG:
            return right ? ANIM_PART_LEG_R : ANIM_PART_LEG_L;
        case MOB_PART_TORSO:
        default:
            return ANIM_PART_TORSO;
    }
}

/* ── Per-type mob mesh baker (0xm) ──────────────────────────────────────────
 * Bakes an arbitrary MobModel box list into a PlayerModel using the SAME
 * PlayerVertex format + player pipeline. Each box -> 6 faces (24 verts / 36
 * indices). Two-tone is expressed purely through UVs: upper-body parts use a
 * top-half skin region (the shader paints those with the primary tint) and legs
 * use a bottom-half region (secondary tint), so shaders/pipeline are unchanged.
 *
 * The MobModel is authored in normalized player-sized space (feet-center
 * origin). The renderer's per-type silhouette scale (PlayerRenderState.scale)
 * is applied at draw time, exactly as for the humanoid. */
bool mob_mesh_bake(Renderer* r, const MobModel* model, PlayerModel* out)
{
    memset(out, 0, sizeof(*out));
    if (!model || model->count <= 0) return false;

    const uint32_t verts_per_box = 24;   /* 6 faces × 4 */
    const uint32_t idxs_per_box  = 36;   /* 6 faces × 6 */
    uint32_t vcap = (uint32_t)model->count * verts_per_box;
    uint32_t icap = (uint32_t)model->count * idxs_per_box;

    PlayerVertex* verts = malloc(vcap * sizeof(PlayerVertex));
    uint32_t*     idxs  = malloc(icap * sizeof(uint32_t));
    if (!verts || !idxs) { free(verts); free(idxs); return false; }

    uint32_t vi = 0, ii = 0;
    for (int b = 0; b < model->count; b++) {
        const MobBox* box = &model->boxes[b];
        float hx = box->w * 0.5f, hy = box->h * 0.5f, hz = box->d * 0.5f;
        float x0 = box->cx - hx, x1 = box->cx + hx;
        float y0 = box->cy - hy, y1 = box->cy + hy;
        float z0 = box->cz - hz, z1 = box->cz + hz;
        /* Route to a skin half so the shader's v<0.5 split picks primary for
         * upper parts and secondary for legs. */
        const BoxUV* uv = mob_part_is_upper_tone(box->role) ? &HEAD_UV : &LEG_UV;
        uint8_t part = mob_box_anim_part(box->role, box->cx);
        add_box(verts, &vi, idxs, &ii, x0, y0, z0, x1, y1, z1, uv, false, part);
    }
    assert(vi == vcap && ii == icap);

    bool ok = upload_buffer(r, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            verts, vi * sizeof(PlayerVertex),
                            &out->vertex_buffer, &out->vertex_alloc);
    if (ok)
        ok = upload_buffer(r, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           idxs, ii * sizeof(uint32_t),
                           &out->index_buffer, &out->index_alloc);
    if (!ok) {
        if (out->vertex_buffer)
            vmaDestroyBuffer(r->allocator, out->vertex_buffer, out->vertex_alloc);
        memset(out, 0, sizeof(*out));
    } else {
        out->index_count = ii;
    }
    free(verts);
    free(idxs);
    return ok;
}

void player_model_destroy(Renderer* r, PlayerModel* m)
{
    if (m->index_buffer)
        vmaDestroyBuffer(r->allocator, m->index_buffer, m->index_alloc);
    if (m->vertex_buffer)
        vmaDestroyBuffer(r->allocator, m->vertex_buffer, m->vertex_alloc);
    memset(m, 0, sizeof(*m));
}

void player_model_draw_one(Renderer* r, VkCommandBuffer cmd,
                           const PlayerModel* m,
                           const PlayerRenderState* state)
{
    if (!m->vertex_buffer) return;

    mat4 model;
    glm_mat4_identity(model);
    vec3 pos = { state->pos[0], state->pos[1], state->pos[2] };
    glm_translate(model, pos);
    glm_rotate(model, (float)GLM_PI_2 - state->yaw, (vec3){0.0f, 1.0f, 0.0f});

    /* Per-axis body scale (mobs scale their box to their silhouette).
     * Treat 0 as "unspecified" -> 1.0 so the player path stays unscaled. */
    float sx = state->scale[0] != 0.0f ? state->scale[0] : 1.0f;
    float sy = state->scale[1] != 0.0f ? state->scale[1] : 1.0f;
    float sz = state->scale[2] != 0.0f ? state->scale[2] : 1.0f;
    glm_scale(model, (vec3){sx, sy, sz});

    /* 124-byte push block, laid out as a flat float array so there is no
     * struct-alignment padding (cglm may over-align mat4, which would bloat a
     * struct past the push range and corrupt later fields):
     *   [0..15]  mat4 model        (64 B)
     *   [16..19] vec4 tint         (16 B)
     *   [20..23] vec4 tint2        (16 B)
     *   [24..29] float limb_angle[ANIM_PART_COUNT] (24 B, per-part pitch)
     *   [30]     float head_yaw    (4 B)
     * Within the 128-byte guaranteed push range. All-zero angles => identity
     * limb transform in the shader => old rigid pose. */
    float pc[31];
    memcpy(pc, model, sizeof(model));   /* 64 bytes: the model matrix */
    pc[16] = state->tint[0];
    pc[17] = state->tint[1];
    pc[18] = state->tint[2];
    pc[19] = state->tint[3];
    pc[20] = state->tint2[0];
    pc[21] = state->tint2[1];
    pc[22] = state->tint2[2];
    pc[23] = 0.0f;
    for (int i = 0; i < ANIM_PART_COUNT; i++)
        pc[24 + i] = state->limb_angle[i];
    pc[30] = state->head_yaw;

    vkCmdPushConstants(cmd, r->player_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), pc);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
}

void player_model_draw(Renderer* r, VkCommandBuffer cmd,
                       const PlayerModel* m,
                       const PlayerRenderState* states, uint32_t count)
{
    if (!count || !m->vertex_buffer) return;
    for (uint32_t i = 0; i < count; i++)
        player_model_draw_one(r, cmd, m, &states[i]);
}
