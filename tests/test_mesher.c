#undef NDEBUG
#include "mesher.h"
#include "chunk.h"
#include "block.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

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

int main(void)
{
    test_solid_chunk_mesh();
    test_ao_isolated_block();
    test_ao_with_neighbor_on_top();
    test_smooth_light_uniform();
    test_smooth_light_partial();
    test_greedy_flat_plane_top_merges_to_one();
    test_greedy_checkerboard_no_overmerge();
    test_greedy_light_prevents_merge();
    test_greedy_fewer_than_naive();
    return 0;
}
