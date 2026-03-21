#include "renderer.h"
#include "vertex.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Remote player placeholder mesh                                   */
/* ------------------------------------------------------------------ */

/* Box dimensions: 0.6 wide, 1.8 tall, 0.6 deep, feet at origin */
#define PL_W  0.6f
#define PL_H  1.8f
#define PL_D  0.6f

static bool create_mapped_player_buffer(VmaAllocator allocator,
                                         const void* data, VkDeviceSize size,
                                         VkBufferUsageFlags usage,
                                         VkBuffer* out_buf,
                                         VmaAllocation* out_alloc)
{
    VkBufferCreateInfo buf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = usage,
    };
    VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VmaAllocationInfo map_info;
    if (vmaCreateBuffer(allocator, &buf_info, &alloc_info,
                        out_buf, out_alloc, &map_info) != VK_SUCCESS) {
        return false;
    }
    memcpy(map_info.pMappedData, data, size);
    vmaFlushAllocation(allocator, *out_alloc, 0, size);
    return true;
}

void renderer_init_player_mesh(Renderer* r)
{
    /* 24 vertices (4 per face * 6 faces), 36 indices (6 per face * 6 faces) */
    BlockVertex verts[24];
    uint32_t    indices[36];

    /* Helper macro: fill one quad (4 verts) and 2 triangles (6 indices).
     * v0..v3 are BlockVertex initialisers, vi is the base vertex index. */
    uint32_t vi = 0; /* vertex index */
    uint32_t ii = 0; /* index index  */

#define QUAD(n0, n1, n2, n3, nm) \
    verts[vi+0] = (n0); verts[vi+1] = (n1); \
    verts[vi+2] = (n2); verts[vi+3] = (n3); \
    verts[vi+0].normal = (nm); verts[vi+1].normal = (nm); \
    verts[vi+2].normal = (nm); verts[vi+3].normal = (nm); \
    indices[ii+0] = vi+0; indices[ii+1] = vi+1; indices[ii+2] = vi+2; \
    indices[ii+3] = vi+0; indices[ii+4] = vi+2; indices[ii+5] = vi+3; \
    vi += 4; ii += 6;

    /* +X face (normal 0) */
    QUAD(((BlockVertex){{PL_W, 0.0f, PL_D}, {0,0}, 0, 0, {0,0}}),
         ((BlockVertex){{PL_W, 0.0f, 0.0f}, {1,0}, 0, 0, {0,0}}),
         ((BlockVertex){{PL_W, PL_H, 0.0f}, {1,1}, 0, 0, {0,0}}),
         ((BlockVertex){{PL_W, PL_H, PL_D}, {0,1}, 0, 0, {0,0}}),
         0)

    /* -X face (normal 1) */
    QUAD(((BlockVertex){{0.0f, 0.0f, 0.0f}, {0,0}, 1, 0, {0,0}}),
         ((BlockVertex){{0.0f, 0.0f, PL_D}, {1,0}, 1, 0, {0,0}}),
         ((BlockVertex){{0.0f, PL_H, PL_D}, {1,1}, 1, 0, {0,0}}),
         ((BlockVertex){{0.0f, PL_H, 0.0f}, {0,1}, 1, 0, {0,0}}),
         1)

    /* +Y face (normal 2) — top */
    QUAD(((BlockVertex){{0.0f, PL_H, 0.0f}, {0,0}, 2, 0, {0,0}}),
         ((BlockVertex){{0.0f, PL_H, PL_D}, {1,0}, 2, 0, {0,0}}),
         ((BlockVertex){{PL_W, PL_H, PL_D}, {1,1}, 2, 0, {0,0}}),
         ((BlockVertex){{PL_W, PL_H, 0.0f}, {0,1}, 2, 0, {0,0}}),
         2)

    /* -Y face (normal 3) — bottom */
    QUAD(((BlockVertex){{0.0f, 0.0f, PL_D}, {0,0}, 3, 0, {0,0}}),
         ((BlockVertex){{0.0f, 0.0f, 0.0f}, {1,0}, 3, 0, {0,0}}),
         ((BlockVertex){{PL_W, 0.0f, 0.0f}, {1,1}, 3, 0, {0,0}}),
         ((BlockVertex){{PL_W, 0.0f, PL_D}, {0,1}, 3, 0, {0,0}}),
         3)

    /* +Z face (normal 4) */
    QUAD(((BlockVertex){{0.0f, 0.0f, PL_D}, {0,0}, 4, 0, {0,0}}),
         ((BlockVertex){{PL_W, 0.0f, PL_D}, {1,0}, 4, 0, {0,0}}),
         ((BlockVertex){{PL_W, PL_H, PL_D}, {1,1}, 4, 0, {0,0}}),
         ((BlockVertex){{0.0f, PL_H, PL_D}, {0,1}, 4, 0, {0,0}}),
         4)

    /* -Z face (normal 5) */
    QUAD(((BlockVertex){{PL_W, 0.0f, 0.0f}, {0,0}, 5, 0, {0,0}}),
         ((BlockVertex){{0.0f, 0.0f, 0.0f}, {1,0}, 5, 0, {0,0}}),
         ((BlockVertex){{0.0f, PL_H, 0.0f}, {1,1}, 5, 0, {0,0}}),
         ((BlockVertex){{PL_W, PL_H, 0.0f}, {0,1}, 5, 0, {0,0}}),
         5)

#undef QUAD

    if (!create_mapped_player_buffer(r->allocator, verts, sizeof(verts),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     &r->player_vb, &r->player_vb_alloc)) {
        fprintf(stderr, "renderer_init_player_mesh: failed to create VB\n");
        return;
    }

    if (!create_mapped_player_buffer(r->allocator, indices, sizeof(indices),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                     &r->player_ib, &r->player_ib_alloc)) {
        fprintf(stderr, "renderer_init_player_mesh: failed to create IB\n");
        vmaDestroyBuffer(r->allocator, r->player_vb, r->player_vb_alloc);
        r->player_vb = VK_NULL_HANDLE;
        return;
    }

    r->player_index_count = 36;
}

void renderer_draw_remote_players(Renderer* r,
                                   const float (*positions)[3],
                                   uint32_t count,
                                   mat4 view, mat4 proj)
{
    /* view and proj are already uploaded to the UBO each frame; they are
     * accepted here to match the declared API but are intentionally unused. */
    (void)view;
    (void)proj;

    if (!r->player_vb || !r->player_ib || count == 0)
        return;

    /* Record into the command buffer that is currently being built for this
     * frame.  Must be called while a render pass is active. */
    VkCommandBuffer cmd = r->command_buffers[r->current_frame];

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->player_vb, &offset);
    vkCmdBindIndexBuffer(cmd, r->player_ib, 0, VK_INDEX_TYPE_UINT32);

    for (uint32_t i = 0; i < count; i++) {
        ChunkPushConstants pc;
        pc.chunk_offset[0] = positions[i][0];
        pc.chunk_offset[1] = positions[i][1];
        pc.chunk_offset[2] = positions[i][2];
        pc.chunk_offset[3] = 0.0f;
        vkCmdPushConstants(cmd, r->pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(ChunkPushConstants), &pc);
        vkCmdDrawIndexed(cmd, r->player_index_count, 1, 0, 0, 0);
    }
}
