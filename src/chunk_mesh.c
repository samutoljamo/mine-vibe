#include "chunk_mesh.h"
#include "renderer.h"
#include <string.h>

/* Use host-visible buffers directly instead of staging + vkQueueWaitIdle.
 * This eliminates per-upload GPU stalls. The GPU reads from mappable memory,
 * which is slightly slower per-draw but avoids the massive pipeline stalls
 * that were causing 10 FPS.
 *
 * Vertices and indices are packed into a SINGLE combined buffer (vertices at
 * offset 0, indices aligned after them). One buffer per chunk instead of two
 * halves the number of VkBuffer objects and VMA allocations the driver tracks,
 * which trims per-frame and memory-management overhead — especially relevant
 * on iGPUs where draw-call/buffer bookkeeping dominates. */

bool chunk_mesh_upload(Renderer* r, ChunkMesh* mesh,
                       BlockVertex* vertices, uint32_t vertex_count,
                       uint32_t* indices, uint32_t index_count)
{
    if (vertex_count == 0 || index_count == 0) return false;

    VkDeviceSize vb_size = (VkDeviceSize)vertex_count * sizeof(BlockVertex);
    VkDeviceSize ib_size = (VkDeviceSize)index_count * sizeof(uint32_t);

    /* Place indices after the vertices, aligned to 4 bytes (UINT32 indices and
     * 24-byte vertices already keep this 4-aligned, but be explicit). */
    VkDeviceSize index_offset = (vb_size + 3u) & ~(VkDeviceSize)3u;
    VkDeviceSize total_size   = index_offset + ib_size;

    VkBufferCreateInfo buf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = total_size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
               | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
    };
    VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };

    VkBuffer      buffer;
    VmaAllocation alloc;
    VmaAllocationInfo map_info;
    if (vmaCreateBuffer(r->allocator, &buf_info, &alloc_info,
                        &buffer, &alloc, &map_info) != VK_SUCCESS) {
        return false;
    }

    uint8_t* base = (uint8_t*)map_info.pMappedData;
    memcpy(base, vertices, vb_size);
    memcpy(base + index_offset, indices, ib_size);
    vmaFlushAllocation(r->allocator, alloc, 0, total_size);

    mesh->vertex_buffer = buffer;
    mesh->vertex_alloc  = alloc;
    mesh->index_buffer  = buffer;          /* same handle */
    mesh->index_alloc   = VK_NULL_HANDLE;  /* owned via vertex_alloc */
    mesh->index_offset  = index_offset;
    mesh->index_count   = index_count;
    mesh->uploaded      = true;
    return true;
}

void chunk_mesh_destroy(VmaAllocator allocator, ChunkMesh* mesh)
{
    /* Single combined buffer: destroy once via the vertex allocation.
     * (index_buffer aliases vertex_buffer; index_alloc is NULL.) */
    if (mesh->vertex_buffer) {
        vmaDestroyBuffer(allocator, mesh->vertex_buffer, mesh->vertex_alloc);
    }
    memset(mesh, 0, sizeof(*mesh));
}
