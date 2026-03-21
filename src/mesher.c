#include "mesher.h"
#include "chunk.h"
#include "block_physics.h"   /* WATER_SOURCE_LEVEL */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Atlas: 16 tiles per row in 256x256 texture */
#define TILE_UV     (1.0f / 16.0f)
#define HALF_TEXEL  (0.5f / 256.0f)  /* inset UVs to avoid sampling adjacent tiles */

void mesh_data_init(MeshData* md)
{
    md->vertex_cap = 4096;
    md->index_cap = 6144;
    md->vertex_count = 0;
    md->index_count = 0;
    md->vertices = malloc(md->vertex_cap * sizeof(BlockVertex));
    md->indices = malloc(md->index_cap * sizeof(uint32_t));
}

void mesh_data_free(MeshData* md)
{
    free(md->vertices);
    free(md->indices);
    md->vertices = NULL;
    md->indices = NULL;
    md->vertex_count = 0;
    md->index_count = 0;
}

static void ensure_capacity(MeshData* md, uint32_t need_verts, uint32_t need_idx)
{
    while (md->vertex_count + need_verts > md->vertex_cap) {
        uint32_t new_cap = md->vertex_cap * 2;
        void* tmp = realloc(md->vertices, new_cap * sizeof(BlockVertex));
        if (!tmp) {
            fprintf(stderr, "ensure_capacity: out of memory (vertices)\n");
            abort();
        }
        md->vertices   = tmp;
        md->vertex_cap = new_cap;
    }
    while (md->index_count + need_idx > md->index_cap) {
        uint32_t new_cap = md->index_cap * 2;
        void* tmp = realloc(md->indices, new_cap * sizeof(uint32_t));
        if (!tmp) {
            fprintf(stderr, "ensure_capacity: out of memory (indices)\n");
            abort();
        }
        md->indices   = tmp;
        md->index_cap = new_cap;
    }
}

static void emit_quad(MeshData* md,
                      float pos[4][3],
                      float uv[4][2],
                      uint8_t normal_id,
                      uint8_t ao[4])
{
    ensure_capacity(md, 4, 6);

    uint32_t base = md->vertex_count;

    for (int i = 0; i < 4; i++) {
        BlockVertex* v = &md->vertices[md->vertex_count++];
        v->pos[0] = pos[i][0];
        v->pos[1] = pos[i][1];
        v->pos[2] = pos[i][2];
        v->uv[0] = uv[i][0];
        v->uv[1] = uv[i][1];
        v->normal = normal_id;
        v->ao = ao[i];
        v->_pad[0] = 0;
        v->_pad[1] = 0;
    }

    /* AO-based triangle flip:
     * if ao[0]+ao[2] > ao[1]+ao[3], flip the diagonal */
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        /* Flipped: 0-1-3, 1-2-3 */
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 3;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
    } else {
        /* Normal: 0-1-2, 0-2-3 */
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
    }
}

static BlockID get_neighbor_block(const Chunk* chunk,
                                  const ChunkNeighbors* neighbors,
                                  int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_Y) return BLOCK_AIR;

    if (x < 0) {
        if (neighbors && neighbors->neg_x)
            return neighbors->neg_x[z * CHUNK_Y + y];
        return BLOCK_AIR;
    }
    if (x >= CHUNK_X) {
        if (neighbors && neighbors->pos_x)
            return neighbors->pos_x[z * CHUNK_Y + y];
        return BLOCK_AIR;
    }
    if (z < 0) {
        if (neighbors && neighbors->neg_z)
            return neighbors->neg_z[x * CHUNK_Y + y];
        return BLOCK_AIR;
    }
    if (z >= CHUNK_Z) {
        if (neighbors && neighbors->pos_z)
            return neighbors->pos_z[x * CHUNK_Y + y];
        return BLOCK_AIR;
    }

    return chunk_get_block(chunk, x, y, z);
}

static bool face_visible(BlockID block, BlockID neighbor)
{
    if (block == BLOCK_AIR) return false;
    if (neighbor == BLOCK_AIR) return true;
    if (block_is_transparent(neighbor) && neighbor != block) return true;
    return false;
}

static uint8_t get_tex_for_face(const BlockDef* def, int face_dir)
{
    /* face_dir: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z */
    if (face_dir == 2) return def->tex_top;
    if (face_dir == 3) return def->tex_bottom;
    return def->tex_side;
}

static void get_tile_uv(uint8_t tile, float* u0, float* v0, float* u1, float* v1)
{
    int tx = tile % 16;
    int ty = tile / 16;
    *u0 = (float)tx * TILE_UV + HALF_TEXEL;
    *v0 = (float)ty * TILE_UV + HALF_TEXEL;
    *u1 = (float)(tx + 1) * TILE_UV - HALF_TEXEL;
    *v1 = (float)(ty + 1) * TILE_UV - HALF_TEXEL;
}

void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors,
                  const uint8_t* meta_snapshot, MeshData* out)
{
    out->vertex_count = 0;
    out->index_count = 0;

    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                BlockID block = chunk_get_block(chunk, x, y, z);
                if (block == BLOCK_AIR) continue;

                const BlockDef* def = block_get_def(block);

                /* Neighbor offsets for 6 faces */
                static const int dx[6] = { 1, -1,  0,  0,  0,  0};
                static const int dy[6] = { 0,  0,  1, -1,  0,  0};
                static const int dz[6] = { 0,  0,  0,  0,  1, -1};

                float fx = (float)x;
                float fy = (float)y;
                float fz = (float)z;

                for (int face = 0; face < 6; face++) {
                    BlockID nb = get_neighbor_block(chunk, neighbors,
                                                    x + dx[face],
                                                    y + dy[face],
                                                    z + dz[face]);

                    if (!face_visible(block, nb)) continue;

                    uint8_t tex = get_tex_for_face(def, face);
                    float u0, v0, u1, v1;
                    get_tile_uv(tex, &u0, &v0, &u1, &v1);

                    float pos[4][3];
                    float uv[4][2];
                    uint8_t ao[4] = {3, 3, 3, 3}; /* Default: no AO */

                    /* UV mapping: v0=(u0,v1) v1=(u1,v1) v2=(u1,v0) v3=(u0,v0) */
                    uv[0][0] = u0; uv[0][1] = v1;
                    uv[1][0] = u1; uv[1][1] = v1;
                    uv[2][0] = u1; uv[2][1] = v0;
                    uv[3][0] = u0; uv[3][1] = v0;

                    switch (face) {
                    case 0: /* +X */
                        pos[0][0] = fx+1; pos[0][1] = fy;   pos[0][2] = fz;
                        pos[1][0] = fx+1; pos[1][1] = fy;   pos[1][2] = fz+1;
                        pos[2][0] = fx+1; pos[2][1] = fy+1; pos[2][2] = fz+1;
                        pos[3][0] = fx+1; pos[3][1] = fy+1; pos[3][2] = fz;
                        break;
                    case 1: /* -X */
                        pos[0][0] = fx;   pos[0][1] = fy;   pos[0][2] = fz+1;
                        pos[1][0] = fx;   pos[1][1] = fy;   pos[1][2] = fz;
                        pos[2][0] = fx;   pos[2][1] = fy+1; pos[2][2] = fz;
                        pos[3][0] = fx;   pos[3][1] = fy+1; pos[3][2] = fz+1;
                        break;
                    case 2: /* +Y */
                    {
                        float fy_top = fy + 1.0f;
                        if (block == BLOCK_WATER && meta_snapshot) {
                            uint8_t wlvl = meta_snapshot[
                                x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
                            if (wlvl > 0 && wlvl < WATER_SOURCE_LEVEL)
                                fy_top = fy + (float)wlvl / (float)WATER_SOURCE_LEVEL;
                        }
                        pos[0][0] = fx;   pos[0][1] = fy_top; pos[0][2] = fz;
                        pos[1][0] = fx+1; pos[1][1] = fy_top; pos[1][2] = fz;
                        pos[2][0] = fx+1; pos[2][1] = fy_top; pos[2][2] = fz+1;
                        pos[3][0] = fx;   pos[3][1] = fy_top; pos[3][2] = fz+1;
                    }
                        break;
                    case 3: /* -Y */
                        pos[0][0] = fx;   pos[0][1] = fy;   pos[0][2] = fz+1;
                        pos[1][0] = fx+1; pos[1][1] = fy;   pos[1][2] = fz+1;
                        pos[2][0] = fx+1; pos[2][1] = fy;   pos[2][2] = fz;
                        pos[3][0] = fx;   pos[3][1] = fy;   pos[3][2] = fz;
                        break;
                    case 4: /* +Z */
                        pos[0][0] = fx+1; pos[0][1] = fy;   pos[0][2] = fz+1;
                        pos[1][0] = fx;   pos[1][1] = fy;   pos[1][2] = fz+1;
                        pos[2][0] = fx;   pos[2][1] = fy+1; pos[2][2] = fz+1;
                        pos[3][0] = fx+1; pos[3][1] = fy+1; pos[3][2] = fz+1;
                        break;
                    case 5: /* -Z */
                        pos[0][0] = fx;   pos[0][1] = fy;   pos[0][2] = fz;
                        pos[1][0] = fx+1; pos[1][1] = fy;   pos[1][2] = fz;
                        pos[2][0] = fx+1; pos[2][1] = fy+1; pos[2][2] = fz;
                        pos[3][0] = fx;   pos[3][1] = fy+1; pos[3][2] = fz;
                        break;
                    }

                    emit_quad(out, pos, uv, (uint8_t)face, ao);
                }
            }
        }
    }
}

void mesher_extract_boundary(const Chunk* chunk, int face, BlockID* out)
{
    /*
     * face 0: x=0  slice -> out[z * CHUNK_Y + y]
     * face 1: x=15 slice -> out[z * CHUNK_Y + y]
     * face 2: z=0  slice -> out[x * CHUNK_Y + y]
     * face 3: z=15 slice -> out[x * CHUNK_Y + y]
     */
    switch (face) {
    case 0: /* x=0 */
        for (int z = 0; z < CHUNK_Z; z++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[z * CHUNK_Y + y] = chunk_get_block(chunk, 0, y, z);
        break;
    case 1: /* x=15 */
        for (int z = 0; z < CHUNK_Z; z++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[z * CHUNK_Y + y] = chunk_get_block(chunk, CHUNK_X - 1, y, z);
        break;
    case 2: /* z=0 */
        for (int x = 0; x < CHUNK_X; x++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[x * CHUNK_Y + y] = chunk_get_block(chunk, x, y, 0);
        break;
    case 3: /* z=15 */
        for (int x = 0; x < CHUNK_X; x++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[x * CHUNK_Y + y] = chunk_get_block(chunk, x, y, CHUNK_Z - 1);
        break;
    }
}
