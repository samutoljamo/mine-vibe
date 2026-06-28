#include "chunk_mesh.h"
#include "renderer.h"
#include <string.h>

/* Use host-visible buffers directly instead of staging + vkQueueWaitIdle.
 * This eliminates per-upload GPU stalls. The GPU reads from mappable memory,
 * which is slightly slower per-draw but avoids the massive pipeline stalls
 * that were causing 10 FPS. */

static bool create_mapped_buffer(VmaAllocator allocator, const void* data,
                                  VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkBuffer* out_buffer, VmaAllocation* out_alloc)
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
                        out_buffer, out_alloc, &map_info) != VK_SUCCESS) {
        return false;
    }
    memcpy(map_info.pMappedData, data, size);
    vmaFlushAllocation(allocator, *out_alloc, 0, size);
    return true;
}

bool chunk_mesh_upload(Renderer* r, ChunkMesh* mesh,
                       BlockVertex* vertices, uint32_t vertex_count,
                       uint32_t* indices, uint32_t index_count)
{
    if (vertex_count == 0 || index_count == 0) return false;

    VkDeviceSize vb_size = (VkDeviceSize)vertex_count * sizeof(BlockVertex);
    VkDeviceSize ib_size = (VkDeviceSize)index_count * sizeof(uint32_t);

    if (!create_mapped_buffer(r->allocator, vertices, vb_size,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              &mesh->vertex_buffer, &mesh->vertex_alloc)) {
        return false;
    }

    if (!create_mapped_buffer(r->allocator, indices, ib_size,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
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
