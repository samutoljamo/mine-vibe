#include "chunk_mesh.h"
#include "renderer.h"
#include <string.h>

static bool upload_buffer(Renderer* r, const void* data, VkDeviceSize size,
                          VkBufferUsageFlags usage,
                          VkBuffer* out_buffer, VmaAllocation* out_alloc)
{
    /* Create staging buffer (host-visible, mapped) */
    VkBufferCreateInfo staging_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo staging_alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer staging_buffer;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_map_info;

    if (vmaCreateBuffer(r->allocator, &staging_info, &staging_alloc_info,
                        &staging_buffer, &staging_alloc, &staging_map_info) != VK_SUCCESS) {
        return false;
    }

    memcpy(staging_map_info.pMappedData, data, size);

    /* Create GPU buffer (device-local, transfer dst) */
    VkBufferCreateInfo gpu_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VmaAllocationCreateInfo gpu_alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };

    if (vmaCreateBuffer(r->allocator, &gpu_info, &gpu_alloc_info,
                        out_buffer, out_alloc, NULL) != VK_SUCCESS) {
        vmaDestroyBuffer(r->allocator, staging_buffer, staging_alloc);
        return false;
    }

    /* Copy via single-shot command buffer */
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);
    VkBufferCopy region = { .size = size };
    vkCmdCopyBuffer(cmd, staging_buffer, *out_buffer, 1, &region);
    renderer_end_single_cmd(r, cmd);

    /* Destroy staging buffer */
    vmaDestroyBuffer(r->allocator, staging_buffer, staging_alloc);
    return true;
}

bool chunk_mesh_upload(Renderer* r, ChunkMesh* mesh,
                       BlockVertex* vertices, uint32_t vertex_count,
                       uint32_t* indices, uint32_t index_count)
{
    if (vertex_count == 0 || index_count == 0) return false;

    VkDeviceSize vb_size = (VkDeviceSize)vertex_count * sizeof(BlockVertex);
    VkDeviceSize ib_size = (VkDeviceSize)index_count * sizeof(uint32_t);

    if (!upload_buffer(r, vertices, vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       &mesh->vertex_buffer, &mesh->vertex_alloc)) {
        return false;
    }

    if (!upload_buffer(r, indices, ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       &mesh->index_buffer, &mesh->index_alloc)) {
        vmaDestroyBuffer(r->allocator, mesh->vertex_buffer, mesh->vertex_alloc);
        memset(mesh, 0, sizeof(*mesh));
        return false;
    }

    mesh->index_count = index_count;
    mesh->uploaded = true;
    return true;
}

void chunk_mesh_destroy(VmaAllocator allocator, ChunkMesh* mesh)
{
    if (mesh->vertex_buffer) {
        vmaDestroyBuffer(allocator, mesh->vertex_buffer, mesh->vertex_alloc);
    }
    if (mesh->index_buffer) {
        vmaDestroyBuffer(allocator, mesh->index_buffer, mesh->index_alloc);
    }
    memset(mesh, 0, sizeof(*mesh));
}
