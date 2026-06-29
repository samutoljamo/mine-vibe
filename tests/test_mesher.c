#undef NDEBUG
#include "mesher.h"
#include "chunk.h"
#include "block.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "platform_thread.h"

static void test_solid_chunk_mesh(void)
{
    Chunk* chunk = chunk_create(0, 0);
    for (int y = 0; y < 64; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                chunk_set_block(chunk, x, y, z, BLOCK_STONE);
    atomic_store(&chunk->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(chunk, &nb, NULL, &md);

    assert(md.vertices != NULL);
    assert(md.indices  != NULL);
    assert(md.vertex_count > 0);
    assert(md.index_count  > 0);
    assert(md.index_count == md.vertex_count / 4 * 6);
    assert(md.vertex_cap >= md.vertex_count);
    assert(md.index_cap  >= md.index_count);

    mesh_data_free(&md);
    chunk_destroy(chunk);
    printf("PASS: test_solid_chunk_mesh\n");
}

/* Place a single isolated stone block; verify all 4 corners of every face
 * have AO=3 (no neighbors → no occlusion). */
static void test_ao_isolated_block(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* All 24 vertices (6 faces × 4 corners) of the block should have ao=3. */
    assert(md.vertex_count == 24);
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        assert(md.vertices[i].ao == 3);
    }

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_ao_isolated_block\n");
}

/* Place a stone block with one side-neighbor on top. The +Y face's vertex
 * adjacent to that neighbor should have AO < 3. */
static void test_ao_with_neighbor_on_top(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    /* Side neighbor at (9, 65, 8): touches the +X edge of the +Y face's
     * top vertex. */
    chunk_set_block(c, 9, 65, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* Find the +Y face vertices of the (8,64,8) block. The +Y face is
     * normal_id == 2. Quad has 4 vertices; the two with x≈9 (positive-X
     * side) should have AO < 3 because of the (9,65,8) neighbor. */
    int saw_occluded = 0;
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        if (md.vertices[i].normal != 2) continue;
        if (md.vertices[i].pos[1] < 64.5f || md.vertices[i].pos[1] > 65.5f) continue;
        /* The +Y face quad of (8,64,8) lives at y=65. Its 4 corners are
         * at x in {8,9} × z in {8,9}. */
        if (md.vertices[i].pos[0] > 8.5f) {
            assert(md.vertices[i].ao < 3); /* occluded by +X neighbor */
            saw_occluded = 1;
        } else {
            assert(md.vertices[i].ao == 3); /* the -X side is unoccluded */
        }
    }
    assert(saw_occluded);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_ao_with_neighbor_on_top\n");
}

/* Ticket 80l.3.1 named configuration: a single block standing on a flat
 * plane. The plane's top (+Y) face cells immediately around the standing
 * block must darken at the corners that touch it (AO < 3), while +Y plane
 * vertices far from the block stay fully bright (AO == 3). This exercises AO
 * caused by a neighbor that is NOT part of the same merged face. */
static void test_ao_block_on_flat_plane(void)
{
    Chunk* c = chunk_create(0, 0);
    const int PY = 64;           /* plane top is at y = PY+1 = 65 */
    for (int z = 0; z < CHUNK_Z; z++)
        for (int x = 0; x < CHUNK_X; x++)
            chunk_set_block(c, x, PY, z, BLOCK_STONE);
    /* One block standing on the plane. */
    chunk_set_block(c, 8, PY + 1, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* Scan the plane's +Y faces (normal id 2) at y == PY+1 == 65. Vertices in
     * the 2x2 footprint of world corners {8,9}x{8,9} sit at the base of the
     * standing block and must be occluded; a vertex far away (e.g. at the
     * chunk's opposite corner) must be fully bright. */
    int saw_darkened = 0;
    int saw_far_bright = 0;
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        if (md.vertices[i].normal != 2) continue;             /* +Y only */
        float vy = md.vertices[i].pos[1];
        if (vy < (float)(PY + 1) - 0.5f || vy > (float)(PY + 1) + 0.5f)
            continue;                                          /* plane top */
        float vx = md.vertices[i].pos[0], vz = md.vertices[i].pos[2];

        int touches_block = (vx > 7.5f && vx < 9.5f) &&
                            (vz > 7.5f && vz < 9.5f);
        if (touches_block) {
            if (md.vertices[i].ao < 3) saw_darkened = 1;
        } else if (vx < 0.5f && vz < 0.5f) {
            /* far corner of the plane, no occluders */
            assert(md.vertices[i].ao == 3);
            saw_far_bright = 1;
        }
    }
    assert(saw_darkened);    /* AO appears next to the standing block */
    assert(saw_far_bright);  /* open ground stays at full AO level 3 */

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_ao_block_on_flat_plane\n");
}

/* With a fully-lit chunk (sky=15 everywhere), every emitted vertex should
 * have light=15. */
static void test_smooth_light_uniform(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    /* Manually flood the chunk with sky=15 (skip lighting module to keep
     * mesher tests independent). */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    for (uint32_t i = 0; i < md.vertex_count; i++) {
        assert(md.vertices[i].light == 15);
    }

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_smooth_light_uniform\n");
}

/* With sky=0 everywhere except one cell at light=12, a +Y face vertex
 * adjacent to that lit cell should average down toward 12. */
static void test_smooth_light_partial(void)
{
    Chunk* c = chunk_create(0, 0);
    chunk_set_block(c, 8, 64, 8, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    /* Default sky=0 everywhere (no allocation). Set just one cell. */
    chunk_set_skylight(c, 8, 65, 8, 12);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* The +Y face should average non-zero contributions across 4 corners.
     * Only the face_block (8,65,8) has light=12; the side1/side2/corner
     * neighbors are all 0. With our averaging "non-zero only" rule, the
     * single non-zero contributor gives light=12 at every corner. */
    int saw = 0;
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        if (md.vertices[i].normal != 2) continue;
        assert(md.vertices[i].light == 12);
        saw++;
    }
    assert(saw == 4);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_smooth_light_partial\n");
}

/* ---- Greedy meshing tests ----------------------------------------------- */

/* Count emitted quads whose 4 vertices all carry the given normal id. A quad
 * is 4 consecutive vertices (emit order is per-quad). */
static uint32_t count_quads_for_normal(const MeshData* md, uint8_t normal)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i + 4 <= md->vertex_count; i += 4) {
        if (md->vertices[i].normal == normal &&
            md->vertices[i+1].normal == normal &&
            md->vertices[i+2].normal == normal &&
            md->vertices[i+3].normal == normal)
            n++;
    }
    return n;
}

static uint32_t total_quads(const MeshData* md)
{
    return md->vertex_count / 4;
}

/* (a) A solid flat single-layer plane of one block type, fully sky-lit, must
 * merge its top (+Y) face into exactly ONE quad — not 16*16 = 256. */
static void test_greedy_flat_plane_top_merges_to_one(void)
{
    Chunk* c = chunk_create(0, 0);
    const int Y = 64;
    for (int z = 0; z < CHUNK_Z; z++)
        for (int x = 0; x < CHUNK_X; x++)
            chunk_set_block(c, x, Y, z, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    /* Uniform sky so AO and light are identical across the whole top face. */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    uint32_t top = count_quads_for_normal(&md, 2 /* +Y */);
    printf("  flat-plane +Y quads = %u (naive would be 256)\n", top);
    assert(top == 1);

    /* Bottom face (-Y) is also a uniform 16x16 plane (nothing below) -> 1. */
    uint32_t bot = count_quads_for_normal(&md, 3 /* -Y */);
    assert(bot == 1);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_greedy_flat_plane_top_merges_to_one\n");
}

/* (b) A checkerboard of two block types on one plane must NOT over-merge:
 * adjacent cells differ in texture so no horizontal merge is valid. The +Y
 * faces remain individual quads (16*16 = 256 of them since every cell is
 * filled but alternates type). */
static void test_greedy_checkerboard_no_overmerge(void)
{
    Chunk* c = chunk_create(0, 0);
    const int Y = 64;
    for (int z = 0; z < CHUNK_Z; z++)
        for (int x = 0; x < CHUNK_X; x++)
            chunk_set_block(c, x, Y, z,
                            ((x + z) & 1) ? BLOCK_STONE : BLOCK_DIRT);
    atomic_store(&c->state, CHUNK_GENERATED);

    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    uint32_t top = count_quads_for_normal(&md, 2 /* +Y */);
    printf("  checkerboard +Y quads = %u (must stay 256)\n", top);
    assert(top == CHUNK_X * CHUNK_Z);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_greedy_checkerboard_no_overmerge\n");
}

/* (c) Identical block type but differing per-vertex light must prevent merge.
 * Build a flat plane where half the columns are bright and half are dark; the
 * +Y face cannot merge across the light boundary. */
static void test_greedy_light_prevents_merge(void)
{
    Chunk* c = chunk_create(0, 0);
    const int Y = 64;
    for (int z = 0; z < CHUNK_Z; z++)
        for (int x = 0; x < CHUNK_X; x++)
            chunk_set_block(c, x, Y, z, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    /* Left half (x<8) sky=15, right half (x>=8) sky=4. The air cell above
     * each top face (y=Y+1) drives the +Y face light. */
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, x < 8 ? 15 : 4);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    uint32_t top = count_quads_for_normal(&md, 2 /* +Y */);
    printf("  split-light +Y quads = %u (must be > 1)\n", top);
    assert(top > 1);   /* cannot collapse to a single quad across the seam */

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_greedy_light_prevents_merge\n");
}

/* (d) For a solid filled region the greedy quad count must be STRICTLY LESS
 * than the naive per-face count. Naive: every exposed face = 1 quad. */
static void test_greedy_fewer_than_naive(void)
{
    Chunk* c = chunk_create(0, 0);
    /* A solid 16x16x8 slab fully lit. */
    for (int y = 60; y < 68; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_block(c, x, y, z, BLOCK_STONE);
    atomic_store(&c->state, CHUNK_GENERATED);

    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    /* Naive exposed-face count for this slab:
     *  top  16x16, bottom 16x16, and 4 sides each 16 wide x 8 tall. */
    uint32_t naive = 16*16 + 16*16 + 4 * (16 * 8);
    uint32_t greedy = total_quads(&md);
    printf("  slab greedy quads = %u, naive = %u\n", greedy, naive);
    assert(greedy < naive);

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_greedy_fewer_than_naive\n");
}

/* (e) Geometry validity: every emitted vertex must lie within the chunk's
 * local bounds, be finite, and every quad (4 consecutive verts) must be planar
 * (one axis constant across all 4). A greedy merge that maps width/height to
 * the wrong axis, or overflows, produces out-of-bounds or non-planar quads —
 * which look like exploded terrain on screen but pass the count/AO tests. */
static void test_greedy_geometry_valid(void)
{
    Chunk* c = chunk_create(0, 0);
    /* Mixed terrain: a solid slab plus a couple of stragglers so several
     * face directions produce merged (W>1/H>1) quads. */
    for (int y = 60; y < 68; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_block(c, x, y, z, BLOCK_STONE);
    chunk_set_block(c, 4, 68, 4, BLOCK_DIRT);
    chunk_set_block(c, 10, 68, 11, BLOCK_DIRT);
    atomic_store(&c->state, CHUNK_GENERATED);

    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    const float EPS = 0.01f;
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        float x = md.vertices[i].pos[0];
        float y = md.vertices[i].pos[1];
        float z = md.vertices[i].pos[2];
        /* finite */
        assert(x == x && y == y && z == z);
        /* within chunk-local bounds [0..DIM] (faces sit on the 0..N edges) */
        assert(x >= -EPS && x <= (float)CHUNK_X + EPS);
        assert(y >= -EPS && y <= (float)CHUNK_Y + EPS);
        assert(z >= -EPS && z <= (float)CHUNK_Z + EPS);
    }

    /* Each quad must be planar: one of the 3 axes is constant across all 4. */
    for (uint32_t i = 0; i + 4 <= md.vertex_count; i += 4) {
        int planar = 0;
        for (int axis = 0; axis < 3; axis++) {
            float v0 = md.vertices[i].pos[axis];
            int same = 1;
            for (int k = 1; k < 4; k++)
                if (md.vertices[i+k].pos[axis] < v0 - EPS ||
                    md.vertices[i+k].pos[axis] > v0 + EPS) { same = 0; break; }
            if (same) { planar = 1; break; }
        }
        assert(planar);
    }

    /* INDEX VALIDITY: every index must be in range, and the 6 indices for
     * quad q must reference exactly that quad's 4 vertices [4q..4q+3]. A bad
     * base-index accumulation produces out-of-range / cross-quad indices that
     * render as triangles shooting across the world — invisible to the count
     * and position checks above. */
    assert(md.index_count == md.vertex_count / 4 * 6);
    for (uint32_t i = 0; i < md.index_count; i++)
        assert(md.indices[i] < md.vertex_count);
    for (uint32_t q = 0; q * 6 + 6 <= md.index_count; q++) {
        uint32_t lo = q * 4, hi = q * 4 + 3;
        for (int k = 0; k < 6; k++) {
            uint32_t idx = md.indices[q * 6 + k];
            assert(idx >= lo && idx <= hi);
        }
    }

    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_greedy_geometry_valid\n");
}

/* (f) Worst-case stress: a pseudo-random voxel volume (deterministic hash, no
 * rand) of mixed block types + air pockets. This produces faces in every
 * direction and exercises greedy rectangle extraction across many small runs —
 * the configuration most likely to expose a base-index accumulation bug that
 * the simple slab/plane cases miss. Validates bounds, planarity, and index
 * range/quad-locality (the "triangles shooting into the sky" failure mode). */
static void test_greedy_random_volume_valid(void)
{
    Chunk* c = chunk_create(0, 0);
    const BlockID types[3] = { BLOCK_STONE, BLOCK_DIRT, BLOCK_GRASS };
    for (int y = 40; y < 90; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++) {
                uint32_t h = (uint32_t)(x*73856093 ^ y*19349663 ^ z*83492791);
                h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                if ((h & 3u) != 0u)  /* ~75% filled, ~25% air pockets */
                    chunk_set_block(c, x, y, z, types[h % 3u]);
            }
    atomic_store(&c->state, CHUNK_GENERATED);
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &md);

    const float EPS = 0.01f;
    assert(md.vertex_count > 0 && md.index_count == md.vertex_count / 4 * 6);
    for (uint32_t i = 0; i < md.vertex_count; i++) {
        float x = md.vertices[i].pos[0], y = md.vertices[i].pos[1],
              z = md.vertices[i].pos[2];
        assert(x == x && y == y && z == z);
        assert(x >= -EPS && x <= (float)CHUNK_X + EPS);
        assert(y >= -EPS && y <= (float)CHUNK_Y + EPS);
        assert(z >= -EPS && z <= (float)CHUNK_Z + EPS);
    }
    for (uint32_t i = 0; i < md.index_count; i++)
        assert(md.indices[i] < md.vertex_count);
    for (uint32_t q = 0; q * 6 + 6 <= md.index_count; q++) {
        uint32_t lo = q * 4, hi = q * 4 + 3;
        for (int k = 0; k < 6; k++)
            assert(md.indices[q*6+k] >= lo && md.indices[q*6+k] <= hi);
    }
    /* planarity */
    for (uint32_t i = 0; i + 4 <= md.vertex_count; i += 4) {
        int planar = 0;
        for (int axis = 0; axis < 3; axis++) {
            float v0 = md.vertices[i].pos[axis]; int same = 1;
            for (int k = 1; k < 4; k++)
                if (md.vertices[i+k].pos[axis] < v0-EPS ||
                    md.vertices[i+k].pos[axis] > v0+EPS) { same = 0; break; }
            if (same) { planar = 1; break; }
        }
        assert(planar);
    }
    printf("  random-volume: %u verts, %u indices, all valid\n",
           md.vertex_count, md.index_count);
    mesh_data_free(&md);
    chunk_destroy(c);
    printf("PASS: test_greedy_random_volume_valid\n");
}

/* (g) THREAD SAFETY: mesher_build runs concurrently on multiple chunk-worker
 * threads in the real game. If its scratch buffers are shared `static` (not
 * _Thread_local), concurrent calls corrupt each other's rectangle merges,
 * producing garbled meshes — invisible to every single-threaded test above but
 * the actual cause of on-screen terrain explosion. This test meshes the SAME
 * chunk from many threads and asserts every result is byte-identical to a
 * single-threaded reference. */
typedef struct {
    const Chunk* chunk;
    const BlockVertex* ref_v; uint32_t ref_vc;
    const uint32_t* ref_i;    uint32_t ref_ic;
    int iterations;
    atomic_int* failures;
} ThreadArg;

static void* mesh_worker(void* p)
{
    ThreadArg* ta = (ThreadArg*)p;
    MeshData md; mesh_data_init(&md);
    ChunkNeighbors nb = {0};
    for (int it = 0; it < ta->iterations; it++) {
        md.vertex_count = 0; md.index_count = 0;
        mesher_build(ta->chunk, &nb, NULL, &md);
        if (md.vertex_count != ta->ref_vc || md.index_count != ta->ref_ic ||
            memcmp(md.vertices, ta->ref_v, ta->ref_vc * sizeof(BlockVertex)) != 0 ||
            memcmp(md.indices,  ta->ref_i, ta->ref_ic * sizeof(uint32_t)) != 0) {
            atomic_fetch_add(ta->failures, 1);
            break;
        }
    }
    mesh_data_free(&md);
    return NULL;
}

static void test_mesher_thread_safe(void)
{
    Chunk* c = chunk_create(0, 0);
    const BlockID types[3] = { BLOCK_STONE, BLOCK_DIRT, BLOCK_GRASS };
    for (int y = 40; y < 90; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++) {
                uint32_t h = (uint32_t)(x*73856093 ^ y*19349663 ^ z*83492791);
                h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                if ((h & 3u) != 0u) chunk_set_block(c, x, y, z, types[h % 3u]);
            }
    atomic_store(&c->state, CHUNK_GENERATED);
    for (int y = 0; y < CHUNK_Y; y++)
        for (int z = 0; z < CHUNK_Z; z++)
            for (int x = 0; x < CHUNK_X; x++)
                chunk_set_skylight(c, x, y, z, 15);

    /* Single-threaded reference. */
    MeshData ref; mesh_data_init(&ref);
    ChunkNeighbors nb = {0};
    mesher_build(c, &nb, NULL, &ref);
    assert(ref.vertex_count > 0);
    BlockVertex* ref_v = malloc(ref.vertex_count * sizeof(BlockVertex));
    uint32_t*    ref_i = malloc(ref.index_count  * sizeof(uint32_t));
    memcpy(ref_v, ref.vertices, ref.vertex_count * sizeof(BlockVertex));
    memcpy(ref_i, ref.indices,  ref.index_count  * sizeof(uint32_t));

    enum { NT = 8 };
    atomic_int failures; atomic_store(&failures, 0);
    PT_Thread th[NT];
    ThreadArg args[NT];
    for (int t = 0; t < NT; t++) {
        args[t] = (ThreadArg){ c, ref_v, ref.vertex_count, ref_i, ref.index_count,
                               60, &failures };
        pt_thread_create(&th[t], mesh_worker, &args[t]);
    }
    for (int t = 0; t < NT; t++) pt_thread_join(th[t]);

    int f = atomic_load(&failures);
    printf("  concurrent mesh mismatches across %d threads = %d (must be 0)\n", NT, f);
    assert(f == 0);

    free(ref_v); free(ref_i);
    mesh_data_free(&ref);
    chunk_destroy(c);
    printf("PASS: test_mesher_thread_safe\n");
}

int main(void)
{
    test_solid_chunk_mesh();
    test_ao_isolated_block();
    test_ao_with_neighbor_on_top();
    test_ao_block_on_flat_plane();
    test_smooth_light_uniform();
    test_smooth_light_partial();
    test_greedy_flat_plane_top_merges_to_one();
    test_greedy_checkerboard_no_overmerge();
    test_greedy_light_prevents_merge();
    test_greedy_fewer_than_naive();
    test_greedy_geometry_valid();
    test_greedy_random_volume_valid();
    test_mesher_thread_safe();
    return 0;
}
