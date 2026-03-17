#ifndef CHUNK_MESH_H
#define CHUNK_MESH_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "vertex.h"

typedef struct ChunkMesh {
    VkBuffer       vertex_buffer;
    VmaAllocation  vertex_alloc;
    VkBuffer       index_buffer;
    VmaAllocation  index_alloc;
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
