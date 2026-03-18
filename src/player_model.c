#include "player_model.h"
#include "renderer.h"
#include <cglm/cglm.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

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

static void add_face(PlayerVertex* verts, uint32_t* vi,
                     uint32_t* idxs, uint32_t* ii,
                     /* TL, TR, BR, BL positions */
                     float v0x, float v0y, float v0z,
                     float v1x, float v1y, float v1z,
                     float v2x, float v2y, float v2z,
                     float v3x, float v3y, float v3z,
                     UVRect uv, bool mirror_u, uint8_t face_idx)
{
    float u0 = uv.u0/64.f, v0 = uv.v0/32.f;
    float u1 = uv.u1/64.f, v1 = uv.v1/32.f;
    if (mirror_u) { float tmp = u0; u0 = u1; u1 = tmp; }

    uint32_t base = *vi;
    verts[(*vi)++] = (PlayerVertex){v0x,v0y,v0z, u0,v0, face_idx, {0}};
    verts[(*vi)++] = (PlayerVertex){v1x,v1y,v1z, u1,v0, face_idx, {0}};
    verts[(*vi)++] = (PlayerVertex){v2x,v2y,v2z, u1,v1, face_idx, {0}};
    verts[(*vi)++] = (PlayerVertex){v3x,v3y,v3z, u0,v1, face_idx, {0}};

    idxs[(*ii)++] = base+0; idxs[(*ii)++] = base+1; idxs[(*ii)++] = base+2;
    idxs[(*ii)++] = base+0; idxs[(*ii)++] = base+2; idxs[(*ii)++] = base+3;
}

/* CW winding per face, viewed from outside.
   mirror_x: used for left arm/leg — swaps ±X face UVs and mirrors U. */
static void add_box(PlayerVertex* verts, uint32_t* vi,
                    uint32_t* idxs, uint32_t* ii,
                    float x0, float y0, float z0,
                    float x1, float y1, float z1,
                    const BoxUV* uv, bool mirror_x)
{
    /* +X face (right), face_idx=0 */
    add_face(verts,vi,idxs,ii,
        x1,y1,z0, x1,y1,z1, x1,y0,z1, x1,y0,z0,
        mirror_x ? uv->mx : uv->px, mirror_x, 0);

    /* -X face (left), face_idx=1 */
    add_face(verts,vi,idxs,ii,
        x0,y1,z1, x0,y1,z0, x0,y0,z0, x0,y0,z1,
        mirror_x ? uv->px : uv->mx, mirror_x, 1);

    /* +Y face (top), face_idx=2 */
    add_face(verts,vi,idxs,ii,
        x0,y1,z1, x1,y1,z1, x1,y1,z0, x0,y1,z0,
        uv->top, mirror_x, 2);

    /* -Y face (bottom), face_idx=3 */
    add_face(verts,vi,idxs,ii,
        x0,y0,z0, x1,y0,z0, x1,y0,z1, x0,y0,z1,
        uv->bot, mirror_x, 3);

    /* +Z face (front), face_idx=4 */
    add_face(verts,vi,idxs,ii,
        x0,y1,z1, x1,y1,z1, x1,y0,z1, x0,y0,z1,
        uv->frt, mirror_x, 4);

    /* -Z face (back), face_idx=5 */
    add_face(verts,vi,idxs,ii,
        x1,y1,z0, x0,y1,z0, x0,y0,z0, x1,y0,z0,
        uv->bck, mirror_x, 5);
}

static void build_player_mesh(PlayerVertex* verts, uint32_t* idxs)
{
    uint32_t vi = 0, ii = 0;

    /* Head: 0.5×0.5×0.5, center (0, 1.50, 0) */
    add_box(verts,&vi,idxs,&ii, -0.25f,1.25f,-0.25f, 0.25f,1.75f,0.25f, &HEAD_UV, false);
    /* Torso: 0.5×0.75×0.25, center (0, 0.875, 0) */
    add_box(verts,&vi,idxs,&ii, -0.25f,0.50f,-0.125f, 0.25f,1.25f,0.125f, &BODY_UV, false);
    /* Right arm: 0.25×0.75×0.25, center (+0.375, 0.875, 0) */
    add_box(verts,&vi,idxs,&ii,  0.25f,0.50f,-0.125f, 0.50f,1.25f,0.125f, &ARM_UV, false);
    /* Left arm (mirrored): center (-0.375, 0.875, 0) */
    add_box(verts,&vi,idxs,&ii, -0.50f,0.50f,-0.125f,-0.25f,1.25f,0.125f, &ARM_UV, true);
    /* Right leg: 0.25×0.75×0.25, center (+0.125, 0.25, 0) */
    add_box(verts,&vi,idxs,&ii,  0.00f,-0.125f,-0.125f, 0.25f,0.625f,0.125f, &LEG_UV, false);
    /* Left leg (mirrored): center (-0.125, 0.25, 0) */
    add_box(verts,&vi,idxs,&ii, -0.25f,-0.125f,-0.125f, 0.00f,0.625f,0.125f, &LEG_UV, true);

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

void player_model_destroy(Renderer* r, PlayerModel* m)
{
    if (m->index_buffer)
        vmaDestroyBuffer(r->allocator, m->index_buffer, m->index_alloc);
    if (m->vertex_buffer)
        vmaDestroyBuffer(r->allocator, m->vertex_buffer, m->vertex_alloc);
    memset(m, 0, sizeof(*m));
}

void player_model_draw(Renderer* r, VkCommandBuffer cmd,
                       const PlayerModel* m,
                       const PlayerRenderState* states, uint32_t count)
{
    if (!count || !m->vertex_buffer) return;

    for (uint32_t i = 0; i < count; i++) {
        mat4 model;
        glm_mat4_identity(model);
        vec3 pos = { states[i].pos[0], states[i].pos[1], states[i].pos[2] };
        glm_translate(model, pos);
        glm_rotate(model, states[i].yaw, (vec3){0.0f, 1.0f, 0.0f});

        vkCmdPushConstants(cmd, r->player_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), model);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
    }
}
