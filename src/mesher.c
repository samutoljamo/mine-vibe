#include "mesher.h"
#include "chunk.h"
#include "block_physics.h"   /* WATER_SOURCE_LEVEL */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Atlas: 16 tiles per row in 256x256 texture */
#define TILE_UV     (1.0f / 16.0f)
/* Inset UVs by 1.5 mip-0 texels so the bilinear footprint stays inside the
 * tile at mip 0 and mip 1 (where 1 mip-1 texel = 2 mip-0 texels). Mip 2+
 * still bleed at the edge — that's mitigated by capping maxLod in the
 * sampler. Without this larger inset, atlas neighbors (stone, bedrock,
 * empty padding) bleed into water at distance and read as colored specks. */
#define HALF_TEXEL  (1.5f / 256.0f)

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
                      const uint8_t ao[4],
                      const uint8_t light[4])
{
    ensure_capacity(md, 4, 6);
    uint32_t base = md->vertex_count;

    for (int i = 0; i < 4; i++) {
        BlockVertex* v = &md->vertices[md->vertex_count++];
        v->pos[0] = pos[i][0];
        v->pos[1] = pos[i][1];
        v->pos[2] = pos[i][2];
        v->uv[0]  = uv[i][0];
        v->uv[1]  = uv[i][1];
        v->normal = normal_id;
        v->ao     = ao[i];
        v->light  = light[i];
        v->_pad   = 0;
    }

    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 3;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
    } else {
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

/* Standard 3-block AO test. side1, side2 are the two edge-adjacent
 * neighbors on the air side of the face; corner is the corner-adjacent
 * neighbor. Returns 0 (max occlusion) to 3 (none). */
static uint8_t ao_value(bool side1, bool side2, bool corner)
{
    if (side1 && side2) return 0;
    return (uint8_t)(3 - ((int)side1 + (int)side2 + (int)corner));
}

/* Lookup helper that returns true if the block at chunk-local (x,y,z),
 * accounting for crossing into a neighbor's BlockID slice, is solid
 * (i.e. would occlude AO/light). */
static bool is_solid_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_Y) return false;

    BlockID b;
    if (x < 0) {
        if (!nb || !nb->neg_x) return false;
        if (z < 0 || z >= CHUNK_Z) return false; /* diagonal corner: no data */
        b = nb->neg_x[z * CHUNK_Y + y];
    } else if (x >= CHUNK_X) {
        if (!nb || !nb->pos_x) return false;
        if (z < 0 || z >= CHUNK_Z) return false;
        b = nb->pos_x[z * CHUNK_Y + y];
    } else if (z < 0) {
        if (!nb || !nb->neg_z) return false;
        b = nb->neg_z[x * CHUNK_Y + y];
    } else if (z >= CHUNK_Z) {
        if (!nb || !nb->pos_z) return false;
        b = nb->pos_z[x * CHUNK_Y + y];
    } else {
        b = chunk_get_block(c, x, y, z);
    }
    return !block_is_transparent(b) && b != BLOCK_AIR;
}

/* Read packed [block:4][sky:4] light byte at (x,y,z), crossing into
 * neighbor light slices as needed. Returns 0 if neighbor has no slice. */
static uint8_t light_byte_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_Y) return 0;

    if (x < 0) {
        if (!nb || !nb->neg_x_lights) return 0;
        if (z < 0 || z >= CHUNK_Z) return 0; /* diagonal corner: no data */
        return nb->neg_x_lights[z * CHUNK_Y + y];
    } else if (x >= CHUNK_X) {
        if (!nb || !nb->pos_x_lights) return 0;
        if (z < 0 || z >= CHUNK_Z) return 0;
        return nb->pos_x_lights[z * CHUNK_Y + y];
    } else if (z < 0) {
        if (!nb || !nb->neg_z_lights) return 0;
        return nb->neg_z_lights[x * CHUNK_Y + y];
    } else if (z >= CHUNK_Z) {
        if (!nb || !nb->pos_z_lights) return 0;
        return nb->pos_z_lights[x * CHUNK_Y + y];
    } else {
        if (!c->lights) return 0;
        return c->lights[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
    }
}

static inline uint8_t sky_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    return light_byte_at(c, nb, x, y, z) & 0x0F;
}

/* Smooth corner light: average non-zero sky values from up to 4 cells
 * meeting at the corner on the air side of the face. Returns 0..15. */
static uint8_t corner_sky(uint8_t face_block, uint8_t side1, uint8_t side2, uint8_t corner)
{
    int sum = 0, count = 0;
    if (face_block) { sum += face_block; count++; }
    if (side1)      { sum += side1;      count++; }
    if (side2)      { sum += side2;      count++; }
    if (corner)     { sum += corner;     count++; }
    return count == 0 ? 0 : (uint8_t)(sum / count);
}

/* Per-corner smooth light. Same basis vectors as compute_face_ao. */
static void compute_face_light(const Chunk* c, const ChunkNeighbors* nb,
                               int x, int y, int z, int face, uint8_t light[4])
{
    static const int fdx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int fdy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int fdz[6] = { 0,  0,  0,  0,  1, -1 };

    int ux[6] = {0,0,1,1,-1,1}, uy[6] = {0,0,0,0,0,0}, uz[6] = {1,-1,0,0,0,0};
    int vx[6] = {0,0,0,0,0,0},  vy[6] = {1,1,0,0,1,1}, vz[6] = {0,0,1,-1,0,0};
    int signs_u[4] = { -1, +1, +1, -1 };
    int signs_v[4] = { -1, -1, +1, +1 };

    int ax = x + fdx[face], ay = y + fdy[face], az = z + fdz[face];

    uint8_t face_block = sky_at(c, nb, ax, ay, az);

    for (int i = 0; i < 4; i++) {
        int su = signs_u[i], sv = signs_v[i];
        uint8_t s1 = sky_at(c, nb,
            ax + su * ux[face], ay + su * uy[face], az + su * uz[face]);
        uint8_t s2 = sky_at(c, nb,
            ax + sv * vx[face], ay + sv * vy[face], az + sv * vz[face]);
        uint8_t cn = sky_at(c, nb,
            ax + su * ux[face] + sv * vx[face],
            ay + su * uy[face] + sv * vy[face],
            az + su * uz[face] + sv * vz[face]);
        light[i] = corner_sky(face_block, s1, s2, cn);
    }
}

/* For face dir (0=+X..5=-Z), fill ao[4] with computed AO for the 4 corners.
 * Vertex ordering matches the existing emit_quad order.
 *
 * Per-face basis vectors (u, v) span the face plane. The 4 corners are
 * placed according to signs_u/signs_v = {(-1,-1),(+1,-1),(+1,+1),(-1,+1)},
 * derived directly from the vertex positions emitted by mesher_build's switch:
 *
 *  Face 0 (+X): v0=(x+1,y,  z  ), v1=(x+1,y,  z+1), v2=(x+1,y+1,z+1), v3=(x+1,y+1,z  )
 *               air=(x+1,y,z), u=+Z (0,0,1), v=+Y (0,1,0)
 *  Face 1 (-X): v0=(x,  y,  z+1), v1=(x,  y,  z  ), v2=(x,  y+1,z  ), v3=(x,  y+1,z+1)
 *               air=(x-1,y,z), u=-Z (0,0,-1), v=+Y (0,1,0)
 *  Face 2 (+Y): v0=(x,  y+1,z  ), v1=(x+1,y+1,z  ), v2=(x+1,y+1,z+1), v3=(x,  y+1,z+1)
 *               air=(x,y+1,z), u=+X (1,0,0), v=+Z (0,0,1)
 *  Face 3 (-Y): v0=(x,  y,  z+1), v1=(x+1,y,  z+1), v2=(x+1,y,  z  ), v3=(x,  y,  z  )
 *               air=(x,y-1,z), u=+X (1,0,0), v=-Z (0,0,-1)
 *  Face 4 (+Z): v0=(x+1,y,  z+1), v1=(x,  y,  z+1), v2=(x,  y+1,z+1), v3=(x+1,y+1,z+1)
 *               air=(x,y,z+1), u=-X (-1,0,0), v=+Y (0,1,0)
 *  Face 5 (-Z): v0=(x,  y,  z  ), v1=(x+1,y,  z  ), v2=(x+1,y+1,z  ), v3=(x,  y+1,z  )
 *               air=(x,y,z-1), u=+X (1,0,0), v=+Y (0,1,0)
 */
static void compute_face_ao(const Chunk* c, const ChunkNeighbors* nb,
                            int x, int y, int z, int face, uint8_t ao[4])
{
    /* Air block one step in face direction. */
    static const int fdx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int fdy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int fdz[6] = { 0,  0,  0,  0,  1, -1 };

    /* u and v basis vectors for each face (integer offsets in world space).
     * Derived from the vertex positions in mesher_build's switch statement. */
    static const int ux[6] = { 0,  0,  1,  1, -1,  1 };
    static const int uy[6] = { 0,  0,  0,  0,  0,  0 };
    static const int uz[6] = { 1, -1,  0,  0,  0,  0 };
    static const int vx[6] = { 0,  0,  0,  0,  0,  0 };
    static const int vy[6] = { 1,  1,  0,  0,  1,  1 };
    static const int vz[6] = { 0,  0,  1, -1,  0,  0 };

    /* Corners (in emit_quad order):
     *   v0 = (-u, -v)
     *   v1 = (+u, -v)
     *   v2 = (+u, +v)
     *   v3 = (-u, +v) */
    static const int signs_u[4] = { -1, +1, +1, -1 };
    static const int signs_v[4] = { -1, -1, +1, +1 };

    int ax = x + fdx[face], ay = y + fdy[face], az = z + fdz[face];

    for (int i = 0; i < 4; i++) {
        int su = signs_u[i], sv = signs_v[i];
        bool side1 = is_solid_at(c, nb,
            ax + su * ux[face], ay + su * uy[face], az + su * uz[face]);
        bool side2 = is_solid_at(c, nb,
            ax + sv * vx[face], ay + sv * vy[face], az + sv * vz[face]);
        bool corner = is_solid_at(c, nb,
            ax + su * ux[face] + sv * vx[face],
            ay + su * uy[face] + sv * vy[face],
            az + su * uz[face] + sv * vz[face]);
        ao[i] = ao_value(side1, side2, corner);
    }
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
                    uint8_t ao[4];
                    uint8_t light[4];
                    compute_face_ao   (chunk, neighbors, x, y, z, face, ao);
                    compute_face_light(chunk, neighbors, x, y, z, face, light);

                    /* Flatten AO *and* light on water so per-corner
                     * variation doesn't draw visible bands across the
                     * uniform liquid surface. With AO flat but light
                     * still per-corner, the quad's two triangles linearly
                     * interpolate light differently and a visible
                     * diagonal "fold" shows per cell — at distance these
                     * folds aggregate into faint horizontal banding. Use
                     * the brightest corner's light so water near shadows
                     * isn't artificially dark. */
                    if (block == BLOCK_WATER) {
                        ao[0] = ao[1] = ao[2] = ao[3] = 3;
                        uint8_t lmax = light[0];
                        if (light[1] > lmax) lmax = light[1];
                        if (light[2] > lmax) lmax = light[2];
                        if (light[3] > lmax) lmax = light[3];
                        light[0] = light[1] = light[2] = light[3] = lmax;
                    }

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
                            /* Lift water surface by 0.005 above the block top.
                             * Beach sand at h=SEA_LEVEL has its +Y face at
                             * exactly the same y as water +Y in adjacent
                             * columns (y = SEA_LEVEL + 1). At oblique view
                             * angles the depth test flips between the two
                             * per-sample under MSAA — producing the
                             * "see-through to the lake floor" smudge across
                             * a wide screen-space band. A 5mm lift settles
                             * the depth test (invisible to the eye, several
                             * orders of magnitude above 24-bit depth
                             * precision at lake distances) without creating
                             * a visible step at the shoreline. */
                            fy_top += 0.005f;
                        }
                        /* Break per-tile moire on water by rotating UVs
                         * 0/90/180/270° per cell using a position hash.
                         * The water texture is symmetric enough that
                         * rotation isn't visually noticeable, but adjacent
                         * cells no longer share a tiling axis so the
                         * regular pattern that produces concentric-ring
                         * moire at oblique angles is broken. */
                        if (block == BLOCK_WATER) {
                            uint32_t h = (uint32_t)x * 73856093u
                                       ^ (uint32_t)z * 19349663u
                                       ^ (uint32_t)y * 83492791u;
                            int rot = (int)((h >> 8) & 3u);
                            if (rot != 0) {
                                float tmp[4][2];
                                for (int k = 0; k < 4; k++) {
                                    int s = (k + rot) & 3;
                                    tmp[k][0] = uv[s][0];
                                    tmp[k][1] = uv[s][1];
                                }
                                for (int k = 0; k < 4; k++) {
                                    uv[k][0] = tmp[k][0];
                                    uv[k][1] = tmp[k][1];
                                }
                            }
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

                    emit_quad(out, pos, uv, (uint8_t)face, ao, light);
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

void mesher_extract_light_boundary(const Chunk* chunk, int face, uint8_t* out)
{
    switch (face) {
    case 0: /* x=0 */
        for (int z = 0; z < CHUNK_Z; z++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[z * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[0 + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    case 1: /* x=15 */
        for (int z = 0; z < CHUNK_Z; z++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[z * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[(CHUNK_X - 1) + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    case 2: /* z=0 */
        for (int x = 0; x < CHUNK_X; x++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[x * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[x + 0 * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    case 3: /* z=15 */
        for (int x = 0; x < CHUNK_X; x++)
            for (int y = 0; y < CHUNK_Y; y++)
                out[x * CHUNK_Y + y] =
                    chunk->lights ? chunk->lights[x + (CHUNK_Z - 1) * CHUNK_X + y * CHUNK_X * CHUNK_Z] : 0;
        break;
    }
}
