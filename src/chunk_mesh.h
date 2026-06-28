#ifndef CHUNK_MESH_H
#define CHUNK_MESH_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "vertex.h"

typedef struct ChunkMesh {
    /* Vertices and indices live in ONE VkBuffer/allocation: vertices at
     * offset 0, indices at `index_offset`. This halves the number of buffer
     * objects and VMA allocations per chunk (less driver/memory bookkeeping
     * on the per-frame draw path, which matters on iGPUs). vertex_buffer and
     * index_buffer below are kept as separate fields for call-site clarity but
     * hold the SAME handle. */
    VkBuffer       vertex_buffer;
    VmaAllocation  vertex_alloc;
    VkBuffer       index_buffer;     /* == vertex_buffer (single combined buffer) */
    VmaAllocation  index_alloc;      /* VK_NULL_HANDLE (shares vertex_alloc) */
    VkDeviceSize   index_offset;     /* byte offset of the index data within the buffer */
    uint32_t       index_count;
    vec3           aabb_min;
    vec3           aabb_max;
    vec3           chunk_origin;
    bool           uploaded;
} ChunkMesh;

typedef struct Renderer Renderer;

bool chunk_mesh_upload(Renderer* r, ChunkMesh* mesh,
                       BlockVertex* vertices, uint32_t vertex_count,
                       uint32_t* indices, uint32_t index_count);

void chunk_mesh_destroy(VmaAllocator allocator, ChunkMesh* mesh);

#endif
