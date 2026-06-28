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

/* Emit one quad.
 *   pos   : 4 corner positions (emit order v0..v3)
 *   uv    : 4 corner UVs. For greedy-merged quads these carry tile-REPEAT
 *           coordinates (range [0..W] / [0..H]); the fragment shader wraps
 *           them back into a single atlas tile via the per-vertex `tile`.
 *           For 1x1 quads they are the usual in-tile UVs and tile==255 tells
 *           the shader to use them verbatim (no wrap).
 *   tile  : atlas tile index, or 255 for "no tiling, use uv directly".
 */
static void emit_quad(MeshData* md,
                      float pos[4][3],
                      float uv[4][2],
                      uint8_t normal_id,
                      const uint8_t ao[4],
                      const uint8_t light[4],
                      uint8_t tile)
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
        v->tile   = tile;
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

/* Combined per-cell light used by the renderer: max(sky, block). Sky lives in
 * the low nibble, emissive block light in the high nibble. Taking the max means
 * torch-lit areas stay bright at night / underground while daylight still wins
 * outdoors. */
static inline uint8_t sky_at(const Chunk* c, const ChunkNeighbors* nb, int x, int y, int z)
{
    uint8_t packed = light_byte_at(c, nb, x, y, z);
    uint8_t sky    = packed & 0x0F;
    uint8_t block  = (packed >> 4) & 0x0F;
    return sky > block ? sky : block;
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

/* Per-face basis vectors. Same convention used throughout: u and v span the
 * face plane; the 4 corners are placed at signs (-u,-v)(+u,-v)(+u,+v)(-u,+v),
 * matching the vertex positions emitted below. fd* is the air-side step. */
static const int fdx[6] = { 1, -1,  0,  0,  0,  0 };
static const int fdy[6] = { 0,  0,  1, -1,  0,  0 };
static const int fdz[6] = { 0,  0,  0,  0,  1, -1 };
static const int ux[6]  = { 0,  0,  1,  1, -1,  1 };
static const int uy[6]  = { 0,  0,  0,  0,  0,  0 };
static const int uz[6]  = { 1, -1,  0,  0,  0,  0 };
static const int vx[6]  = { 0,  0,  0,  0,  0,  0 };
static const int vy[6]  = { 1,  1,  0,  0,  1,  1 };
static const int vz[6]  = { 0,  0,  1, -1,  0,  0 };
static const int signs_u[4] = { -1, +1, +1, -1 };
static const int signs_v[4] = { -1, -1, +1, +1 };

/* Per-corner smooth light. */
static void compute_face_light(const Chunk* c, const ChunkNeighbors* nb,
                               int x, int y, int z, int face, uint8_t light[4])
{
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
 * Vertex ordering matches emit order. */
static void compute_face_ao(const Chunk* c, const ChunkNeighbors* nb,
                            int x, int y, int z, int face, uint8_t ao[4])
{
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

/* Water and other special blocks are emitted on the legacy per-cell path
 * (partial heights, per-cell UV rotation, surface lift) and never greedy
 * merged. Returns true if the block must be emitted per-cell. */
static bool needs_per_cell(BlockID block)
{
    return block == BLOCK_WATER;
}

/* ---- Per-cell (legacy) emit, for water / special blocks ----------------- */
static void emit_cell_face(const Chunk* chunk, const ChunkNeighbors* neighbors,
                           const uint8_t* meta_snapshot, MeshData* out,
                           BlockID block, const BlockDef* def,
                           int x, int y, int z, int face)
{
    float fx = (float)x, fy = (float)y, fz = (float)z;

    uint8_t tex = get_tex_for_face(def, face);
    float u0, v0, u1, v1;
    get_tile_uv(tex, &u0, &v0, &u1, &v1);

    float pos[4][3];
    float uv[4][2];
    uint8_t ao[4];
    uint8_t light[4];
    compute_face_ao   (chunk, neighbors, x, y, z, face, ao);
    compute_face_light(chunk, neighbors, x, y, z, face, light);

    if (block == BLOCK_WATER) {
        ao[0] = ao[1] = ao[2] = ao[3] = 3;
        uint8_t lmax = light[0];
        if (light[1] > lmax) lmax = light[1];
        if (light[2] > lmax) lmax = light[2];
        if (light[3] > lmax) lmax = light[3];
        light[0] = light[1] = light[2] = light[3] = lmax;
    }

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
            fy_top += 0.005f;
        }
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

    /* 255 = no tiling: use UVs verbatim (single in-tile quad). */
    emit_quad(out, pos, uv, (uint8_t)face, ao, light, 255);
}

/* ---- Greedy meshing ----------------------------------------------------- *
 *
 * For each of the 6 face directions we sweep slices perpendicular to the face
 * normal. Within each slice we build a per-cell descriptor for every visible,
 * mergeable face (tile id + per-corner AO + per-corner light), then greedily
 * extract maximal rectangles whose cells share an identical descriptor. Each
 * rectangle becomes a single quad with stretched geometry and tile-repeat
 * UVs (the fragment shader wraps them back into one atlas tile).
 *
 * The slice is addressed by two in-plane integer axes (a, b). For each face we
 * map (a, b) and the fixed slice coordinate to world (x, y, z), and a/b to the
 * quad's u (width) / v (height) directions so corner order / UV / AO / light
 * all match the legacy per-cell emit exactly when W=H=1.
 */

#define MAX_DIM CHUNK_Y   /* largest of CHUNK_X/Y/Z */

typedef struct FaceDesc {
    uint8_t valid;     /* 1 if a visible mergeable face exists here */
    uint8_t tile;
    uint8_t ao[4];
    uint8_t light[4];
} FaceDesc;

static bool desc_eq(const FaceDesc* p, const FaceDesc* q)
{
    if (!p->valid || !q->valid) return false;
    if (p->tile != q->tile) return false;
    for (int i = 0; i < 4; i++)
        if (p->ao[i] != q->ao[i] || p->light[i] != q->light[i]) return false;
    return true;
}

void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors,
                  const uint8_t* meta_snapshot, MeshData* out)
{
    out->vertex_count = 0;
    out->index_count = 0;

    /* Slice/in-plane axis setup per face.
     * normal axis: the axis the face normal runs along (0=X,1=Y,2=Z).
     * The face is at world coord `slice` along that axis; the air cell is at
     * slice + step. (a) spans the u direction, (b) spans the v direction.
     * a_dim/b_dim are the extents; a_dir/b_dir are +1/-1 mapping a/b index to
     * world offset along the quad's u/v so corner placement matches legacy. */
    static const int normal_axis[6] = { 0, 0, 1, 1, 2, 2 };
    /* a (u) axis and b (v) axis as world axis indices. */
    static const int a_axis[6] = { 2, 2, 0, 0, 0, 0 }; /* +X,-X:Z  +Y,-Y:X  +Z,-Z:X */
    static const int b_axis[6] = { 1, 1, 2, 2, 1, 1 }; /* +X,-X:Y  +Y,-Y:Z  +Z,-Z:Y */
    /* direction (+1/-1) that index a/b advances along world u/v. Derived from
     * ux/uz/vy/vz above: +X u=+Z(+1) v=+Y(+1); -X u=-Z(-1) v=+Y(+1);
     * +Y u=+X(+1) v=+Z(+1); -Y u=+X(+1) v=-Z(-1); +Z u=-X(-1) v=+Y(+1);
     * -Z u=+X(+1) v=+Y(+1). */
    static const int a_dir[6] = { +1, -1, +1, +1, -1, +1 };
    static const int b_dir[6] = { +1, +1, +1, -1, +1, +1 };

    static const int dims[3] = { CHUNK_X, CHUNK_Y, CHUNK_Z };

    /* Reusable per-slice descriptor mask and "used" bitmap. */
    static FaceDesc mask[MAX_DIM * MAX_DIM];
    static uint8_t  used[MAX_DIM * MAX_DIM];

    for (int face = 0; face < 6; face++) {
        int na = normal_axis[face];
        int aa = a_axis[face];
        int ba = b_axis[face];
        int n_dim = dims[na];
        int a_dim = dims[aa];
        int b_dim = dims[ba];
        int step  = (face & 1) ? -1 : +1;   /* +X/+Y/+Z = +1, -X/-Y/-Z = -1 */
        int adir  = a_dir[face];
        int bdir  = b_dir[face];

        for (int slice = 0; slice < n_dim; slice++) {
            /* ---- Build the descriptor mask for this slice. ---- */
            for (int b = 0; b < b_dim; b++) {
                for (int a = 0; a < a_dim; a++) {
                    FaceDesc* fd = &mask[b * a_dim + a];
                    fd->valid = 0;

                    /* Map (slice, a, b) -> world (x,y,z). a/b index space is
                     * 0..dim, mapped to the world cell. We iterate cells in
                     * natural axis order (a along a_axis ascending, b along
                     * b_axis ascending) so neighboring mask entries are
                     * world-adjacent — required for correct rectangle merge.
                     * The a_dir/b_dir only affect corner placement, not which
                     * world cell a mask entry refers to. */
                    int coord[3];
                    coord[na] = slice;
                    coord[aa] = a;
                    coord[ba] = b;
                    int x = coord[0], y = coord[1], z = coord[2];

                    BlockID block = chunk_get_block(chunk, x, y, z);
                    if (block == BLOCK_AIR) continue;
                    if (needs_per_cell(block)) continue; /* handled elsewhere */

                    int nx = x + fdx[face], ny = y + fdy[face], nz = z + fdz[face];
                    BlockID nbk = get_neighbor_block(chunk, neighbors, nx, ny, nz);
                    if (!face_visible(block, nbk)) continue;

                    const BlockDef* def = block_get_def(block);
                    fd->tile = get_tex_for_face(def, face);
                    compute_face_ao   (chunk, neighbors, x, y, z, face, fd->ao);
                    compute_face_light(chunk, neighbors, x, y, z, face, fd->light);
                    fd->valid = 1;
                }
            }

            memset(used, 0, (size_t)a_dim * b_dim);

            /* ---- Greedy rectangle extraction over (a, b). ---- */
            for (int b = 0; b < b_dim; b++) {
                for (int a = 0; a < a_dim; a++) {
                    int idx = b * a_dim + a;
                    if (used[idx] || !mask[idx].valid) continue;
                    FaceDesc base = mask[idx];

                    /* Grow width along a. */
                    int w = 1;
                    while (a + w < a_dim) {
                        int j = b * a_dim + (a + w);
                        if (used[j] || !desc_eq(&base, &mask[j])) break;
                        w++;
                    }
                    /* Grow height along b: each candidate row must match the
                     * full run [a, a+w). */
                    int h = 1;
                    while (b + h < b_dim) {
                        bool ok = true;
                        for (int k = 0; k < w; k++) {
                            int j = (b + h) * a_dim + (a + k);
                            if (used[j] || !desc_eq(&base, &mask[j])) { ok = false; break; }
                        }
                        if (!ok) break;
                        h++;
                    }
                    /* Mark used. */
                    for (int hb = 0; hb < h; hb++)
                        for (int wa = 0; wa < w; wa++)
                            used[(b + hb) * a_dim + (a + wa)] = 1;

                    /* ---- Emit the merged quad. ----
                     * World cell of the (a,b) corner: */
                    int coord[3];
                    coord[na] = slice;
                    coord[aa] = a;
                    coord[ba] = b;
                    int cx = coord[0], cy = coord[1], cz = coord[2];

                    /* The face plane sits at slice+1 for +faces (step>0) and
                     * slice for -faces (step<0) along the normal axis. */
                    float plane[3] = { (float)cx, (float)cy, (float)cz };
                    if (step > 0) plane[na] += 1.0f;

                    /* World-space u/v edge vectors (unit per cell). */
                    float uvec[3] = {0,0,0};
                    float vvec[3] = {0,0,0};
                    uvec[aa] = (float)adir;
                    vvec[ba] = (float)bdir;

                    /* Corner origin in world space: the (-u,-v) corner. The
                     * legacy emit places v0 at the (-u,-v) corner. Because the
                     * a/b loop starts at the minimal world cell while the u/v
                     * basis may point negative, shift the origin so that the
                     * merged quad still covers world cells [a,a+w)x[b,b+h). */
                    float org[3] = { plane[0], plane[1], plane[2] };
                    /* If adir<0, the -u corner is at the high world-a edge. */
                    if (adir > 0) org[aa] += 0.0f; else org[aa] += (float)w;
                    if (bdir > 0) org[ba] += 0.0f; else org[ba] += (float)h;
                    /* Also: for +u the cell origin is the low edge; legacy uses
                     * the block's low corner. For adir>0 that's coord[aa]; for
                     * adir<0 the -u corner is the high edge = coord+ w. Same as
                     * above. v0 = org. */

                    float pos[4][3];
                    for (int i = 0; i < 3; i++) {
                        pos[0][i] = org[i];                                   /* (-u,-v) */
                        pos[1][i] = org[i] + uvec[i] * w;                     /* (+u,-v) */
                        pos[2][i] = org[i] + uvec[i] * w + vvec[i] * h;       /* (+u,+v) */
                        pos[3][i] = org[i] + vvec[i] * h;                     /* (-u,+v) */
                    }

                    /* Tile-repeat UVs: span [0..w] x [0..h]; shader wraps. */
                    float uv[4][2];
                    uv[0][0] = 0;          uv[0][1] = (float)h;
                    uv[1][0] = (float)w;   uv[1][1] = (float)h;
                    uv[2][0] = (float)w;   uv[2][1] = 0;
                    uv[3][0] = 0;          uv[3][1] = 0;

                    emit_quad(out, pos, uv, (uint8_t)face,
                              base.ao, base.light, base.tile);

                    a += w - 1; /* skip consumed columns */
                }
            }
        }
    }

    /* ---- Per-cell pass for water / special blocks. ---- */
    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                BlockID block = chunk_get_block(chunk, x, y, z);
                if (block == BLOCK_AIR || !needs_per_cell(block)) continue;
                const BlockDef* def = block_get_def(block);
                for (int face = 0; face < 6; face++) {
                    BlockID nbk = get_neighbor_block(chunk, neighbors,
                        x + fdx[face], y + fdy[face], z + fdz[face]);
                    if (!face_visible(block, nbk)) continue;
                    emit_cell_face(chunk, neighbors, meta_snapshot, out,
                                   block, def, x, y, z, face);
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
