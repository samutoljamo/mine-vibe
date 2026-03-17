#ifndef CHUNK_MESH_H
#define CHUNK_MESH_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

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

#endif
