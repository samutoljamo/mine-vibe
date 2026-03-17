#ifndef MESHER_H
#define MESHER_H

#include <stdint.h>
#include "vertex.h"
#include "block.h"

typedef struct Chunk Chunk;

typedef struct MeshData {
    BlockVertex* vertices;
    uint32_t*    indices;
    uint32_t     vertex_count;
    uint32_t     index_count;
    uint32_t     vertex_cap;
    uint32_t     index_cap;
} MeshData;

typedef struct ChunkNeighbors {
    const BlockID* pos_x; /* x=0 slice of +X neighbor */
    const BlockID* neg_x; /* x=15 slice of -X neighbor */
    const BlockID* pos_z; /* z=0 slice of +Z neighbor */
    const BlockID* neg_z; /* z=15 slice of -Z neighbor */
} ChunkNeighbors;

void mesh_data_init(MeshData* md);
void mesh_data_free(MeshData* md);
void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors, MeshData* out);
void mesher_extract_boundary(const Chunk* chunk, int face, BlockID* out);

#endif
