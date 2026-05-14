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

int main(void)
{
    test_solid_chunk_mesh();
    test_ao_isolated_block();
    test_ao_with_neighbor_on_top();
    test_smooth_light_uniform();
    test_smooth_light_partial();
    return 0;
}
