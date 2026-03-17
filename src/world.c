#include "world.h"
#include "chunk.h"
#include "chunk_map.h"
#include "chunk_mesh.h"
#include "mesher.h"
#include "worldgen.h"
#include "renderer.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Work / Result types                                               */
/* ------------------------------------------------------------------ */

typedef enum WorkType {
    WORK_GENERATE,
    WORK_MESH,
} WorkType;

typedef struct WorkItem {
    WorkType         type;
    Chunk*           chunk;
    int              seed;
    /* For WORK_MESH: boundary slices (malloc'd, freed after meshing) */
    BlockID*         boundary_pos_x;  /* x=0 slice of +X neighbor */
    BlockID*         boundary_neg_x;  /* x=15 slice of -X neighbor */
    BlockID*         boundary_pos_z;  /* z=0 slice of +Z neighbor */
    BlockID*         boundary_neg_z;  /* z=15 slice of -Z neighbor */
    struct WorkItem* next;
} WorkItem;

typedef struct ResultItem {
    Chunk*             chunk;
    MeshData*          mesh_data; /* NULL for generate results */
    struct ResultItem* next;
} ResultItem;

/* ------------------------------------------------------------------ */
/*  World struct                                                      */
/* ------------------------------------------------------------------ */

struct World {
    Renderer*      renderer;
    ChunkMap       map;
    int            seed;
    int            render_distance;

    /* Worker threads */
    pthread_t*     workers;
    int            worker_count;
    _Atomic bool   shutdown;

    /* Work queue (linked list, protected by mutex + condvar) */
    WorkItem*      work_head;
    pthread_mutex_t work_mutex;
    pthread_cond_t  work_cond;

    /* Result queue (linked list, protected by mutex) */
    ResultItem*    result_head;
    pthread_mutex_t result_mutex;

    /* Render mesh array */
    ChunkMesh*     render_meshes;
    uint32_t       render_count;
    uint32_t       render_cap;
};

/* ------------------------------------------------------------------ */
/*  Worker thread function                                            */
/* ------------------------------------------------------------------ */

static void* worker_func(void* arg)
{
    World* world = (World*)arg;

    for (;;) {
        /* Pop work item */
        pthread_mutex_lock(&world->work_mutex);
        while (!world->work_head && !atomic_load(&world->shutdown)) {
            pthread_cond_wait(&world->work_cond, &world->work_mutex);
        }

        if (atomic_load(&world->shutdown) && !world->work_head) {
            pthread_mutex_unlock(&world->work_mutex);
            break;
        }

        WorkItem* item = world->work_head;
        if (item) {
            world->work_head = item->next;
        }
        pthread_mutex_unlock(&world->work_mutex);

        if (!item) continue;

        if (item->type == WORK_GENERATE) {
            worldgen_generate(item->chunk, item->seed);

            /* Push generate result */
            ResultItem* result = malloc(sizeof(ResultItem));
            result->chunk = item->chunk;
            result->mesh_data = NULL;
            result->next = NULL;

            pthread_mutex_lock(&world->result_mutex);
            result->next = world->result_head;
            world->result_head = result;
            pthread_mutex_unlock(&world->result_mutex);

        } else if (item->type == WORK_MESH) {
            /* Build ChunkNeighbors from boundary slices */
            ChunkNeighbors neighbors = {
                .pos_x = item->boundary_pos_x,
                .neg_x = item->boundary_neg_x,
                .pos_z = item->boundary_pos_z,
                .neg_z = item->boundary_neg_z,
            };

            MeshData* md = malloc(sizeof(MeshData));
            mesh_data_init(md);
            mesher_build(item->chunk, &neighbors, md);

            /* Free boundary slices */
            free(item->boundary_pos_x);
            free(item->boundary_neg_x);
            free(item->boundary_pos_z);
            free(item->boundary_neg_z);

            /* Push mesh result */
            ResultItem* result = malloc(sizeof(ResultItem));
            result->chunk = item->chunk;
            result->mesh_data = md;
            result->next = NULL;

            pthread_mutex_lock(&world->result_mutex);
            result->next = world->result_head;
            world->result_head = result;
            pthread_mutex_unlock(&world->result_mutex);
        }

        free(item);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Submit helpers                                                    */
/* ------------------------------------------------------------------ */

static void submit_work(World* world, WorkItem* item)
{
    item->next = NULL;
    pthread_mutex_lock(&world->work_mutex);
    item->next = world->work_head;
    world->work_head = item;
    pthread_cond_signal(&world->work_cond);
    pthread_mutex_unlock(&world->work_mutex);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

World* world_create(Renderer* renderer, int seed, int render_distance)
{
    World* world = calloc(1, sizeof(World));
    world->renderer = renderer;
    world->seed = seed;
    world->render_distance = render_distance;

    chunk_map_init(&world->map, 8192);

    pthread_mutex_init(&world->work_mutex, NULL);
    pthread_cond_init(&world->work_cond, NULL);
    pthread_mutex_init(&world->result_mutex, NULL);

    atomic_store(&world->shutdown, false);

    /* Determine worker count: max(1, num_cores - 2), capped at 8 */
    long ncores = sysconf(_SC_NPROCESSORS_ONLN);
    int nworkers = (int)(ncores - 2);
    if (nworkers < 1) nworkers = 1;
    if (nworkers > 8) nworkers = 8;

    world->worker_count = nworkers;
    world->workers = malloc(sizeof(pthread_t) * (size_t)nworkers);

    for (int i = 0; i < nworkers; i++) {
        pthread_create(&world->workers[i], NULL, worker_func, world);
    }

    /* Render mesh array */
    world->render_cap = 4096;
    world->render_count = 0;
    world->render_meshes = malloc(sizeof(ChunkMesh) * world->render_cap);

    fprintf(stderr, "World created: seed=%d, render_distance=%d, workers=%d\n",
            seed, render_distance, nworkers);

    return world;
}

void world_destroy(World* world)
{
    /* Signal shutdown */
    atomic_store(&world->shutdown, true);
    pthread_cond_broadcast(&world->work_cond);

    /* Join all workers */
    for (int i = 0; i < world->worker_count; i++) {
        pthread_join(world->workers[i], NULL);
    }
    free(world->workers);

    /* Free remaining work items */
    WorkItem* wi = world->work_head;
    while (wi) {
        WorkItem* next = wi->next;
        if (wi->type == WORK_MESH) {
            free(wi->boundary_pos_x);
            free(wi->boundary_neg_x);
            free(wi->boundary_pos_z);
            free(wi->boundary_neg_z);
        }
        free(wi);
        wi = next;
    }

    /* Free remaining results */
    ResultItem* ri = world->result_head;
    while (ri) {
        ResultItem* next = ri->next;
        if (ri->mesh_data) {
            mesh_data_free(ri->mesh_data);
            free(ri->mesh_data);
        }
        free(ri);
        ri = next;
    }

    /* Destroy all chunks: free GPU meshes first, then chunks */
    uint32_t idx = 0;
    Chunk* chunk;
    while ((chunk = chunk_map_iter(&world->map, &idx)) != NULL) {
        if (chunk->mesh.uploaded) {
            chunk_mesh_destroy(world->renderer->allocator, &chunk->mesh);
        }
        chunk_destroy(chunk);
    }
    chunk_map_free(&world->map);

    /* Destroy sync primitives */
    pthread_mutex_destroy(&world->work_mutex);
    pthread_cond_destroy(&world->work_cond);
    pthread_mutex_destroy(&world->result_mutex);

    /* Free render mesh array */
    free(world->render_meshes);

    free(world);
}

void world_update(World* world, vec3 player_pos)
{
    int pcx = (int)floorf(player_pos[0] / 16.0f);
    int pcz = (int)floorf(player_pos[2] / 16.0f);
    int rd = world->render_distance;
    int rd_sq = rd * rd;
    int unload_rd = rd + 4;
    int unload_sq = unload_rd * unload_rd;

    /* ---- Step 1: Process results ---- */
    {
        /* Swap out the entire result list atomically */
        pthread_mutex_lock(&world->result_mutex);
        ResultItem* results = world->result_head;
        world->result_head = NULL;
        pthread_mutex_unlock(&world->result_mutex);

        int uploads = 0;

        while (results) {
            ResultItem* r = results;
            results = r->next;

            Chunk* chunk = r->chunk;

            if (r->mesh_data) {
                /* Mesh result */
                MeshData* md = r->mesh_data;

                if (md->vertex_count > 0 && uploads < 64) {
                    /* Destroy old mesh if any */
                    if (chunk->mesh.uploaded) {
                        chunk_mesh_destroy(world->renderer->allocator, &chunk->mesh);
                    }
                    memset(&chunk->mesh, 0, sizeof(ChunkMesh));

                    /* Set origin and AABB */
                    chunk->mesh.chunk_origin[0] = (float)(chunk->cx * CHUNK_X);
                    chunk->mesh.chunk_origin[1] = 0.0f;
                    chunk->mesh.chunk_origin[2] = (float)(chunk->cz * CHUNK_Z);

                    chunk->mesh.aabb_min[0] = chunk->mesh.chunk_origin[0];
                    chunk->mesh.aabb_min[1] = 0.0f;
                    chunk->mesh.aabb_min[2] = chunk->mesh.chunk_origin[2];

                    chunk->mesh.aabb_max[0] = chunk->mesh.chunk_origin[0] + (float)CHUNK_X;
                    chunk->mesh.aabb_max[1] = (float)CHUNK_Y;
                    chunk->mesh.aabb_max[2] = chunk->mesh.chunk_origin[2] + (float)CHUNK_Z;

                    chunk_mesh_upload(world->renderer, &chunk->mesh,
                                      md->vertices, md->vertex_count,
                                      md->indices, md->index_count);

                    atomic_store(&chunk->state, CHUNK_READY);
                    uploads++;

                } else if (md->vertex_count == 0) {
                    /* Empty mesh - mark as ready but nothing to draw */
                    atomic_store(&chunk->state, CHUNK_READY);
                } else {
                    /* Upload limit hit - set back to GENERATED so it
                     * re-enters the mesh pipeline next frame (mesh data
                     * is freed below, so we can't retry the upload). */
                    atomic_store(&chunk->state, CHUNK_GENERATED);
                }

                mesh_data_free(md);
                free(md);

            } else {
                /* Generate result - state already set by worldgen */
                /* Nothing else to do here */
            }

            free(r);
        }
    }

    /* ---- Step 2: Unload distant chunks ---- */
    {
        bool removed;
        do {
            removed = false;
            uint32_t idx = 0;
            Chunk* chunk;
            while ((chunk = chunk_map_iter(&world->map, &idx)) != NULL) {
                int dx = chunk->cx - pcx;
                int dz = chunk->cz - pcz;
                int dist_sq = dx * dx + dz * dz;

                if (dist_sq > unload_sq) {
                    int state = atomic_load(&chunk->state);
                    /* Don't unload chunks being processed by workers */
                    if (state == CHUNK_GENERATING || state == CHUNK_MESHING) {
                        continue;
                    }

                    chunk_map_remove(&world->map, chunk->cx, chunk->cz);

                    if (chunk->mesh.uploaded) {
                        chunk_mesh_destroy(world->renderer->allocator, &chunk->mesh);
                    }
                    chunk_destroy(chunk);

                    /* Iterator invalidated by removal; restart */
                    removed = true;
                    break;
                }
            }
        } while (removed);
    }

    /* ---- Step 3: Load missing chunks (spiral from center) ---- */
    {
        int submits = 0;
        for (int r = 0; r <= rd && submits < 16; r++) {
            if (r == 0) {
                /* Center chunk */
                if (!chunk_map_get(&world->map, pcx, pcz)) {
                    Chunk* c = chunk_create(pcx, pcz);
                    atomic_store(&c->state, CHUNK_GENERATING);
                    chunk_map_put(&world->map, c);

                    WorkItem* wi = calloc(1, sizeof(WorkItem));
                    wi->type = WORK_GENERATE;
                    wi->chunk = c;
                    wi->seed = world->seed;
                    submit_work(world, wi);
                    submits++;
                }
                continue;
            }

            /* Walk the perimeter of ring at distance r */
            for (int dx = -r; dx <= r && submits < 16; dx++) {
                for (int dz = -r; dz <= r && submits < 16; dz++) {
                    /* Only perimeter of this ring */
                    if (abs(dx) != r && abs(dz) != r) continue;

                    if (dx * dx + dz * dz > rd_sq) continue;

                    int cx = pcx + dx;
                    int cz = pcz + dz;

                    if (chunk_map_get(&world->map, cx, cz)) continue;

                    Chunk* c = chunk_create(cx, cz);
                    atomic_store(&c->state, CHUNK_GENERATING);
                    chunk_map_put(&world->map, c);

                    WorkItem* wi = calloc(1, sizeof(WorkItem));
                    wi->type = WORK_GENERATE;
                    wi->chunk = c;
                    wi->seed = world->seed;
                    submit_work(world, wi);
                    submits++;
                }
            }
        }
    }

    /* ---- Step 4: Submit meshing for GENERATED chunks ---- */
    {
        uint32_t idx = 0;
        Chunk* chunk;
        while ((chunk = chunk_map_iter(&world->map, &idx)) != NULL) {
            if (atomic_load(&chunk->state) != CHUNK_GENERATED) continue;

            /* Check neighbors: if a neighbor exists, it must be >= GENERATED */
            Chunk* nx_pos = chunk_map_get(&world->map, chunk->cx + 1, chunk->cz);
            Chunk* nx_neg = chunk_map_get(&world->map, chunk->cx - 1, chunk->cz);
            Chunk* nz_pos = chunk_map_get(&world->map, chunk->cx, chunk->cz + 1);
            Chunk* nz_neg = chunk_map_get(&world->map, chunk->cx, chunk->cz - 1);

            if (nx_pos && atomic_load(&nx_pos->state) < CHUNK_GENERATED) continue;
            if (nx_neg && atomic_load(&nx_neg->state) < CHUNK_GENERATED) continue;
            if (nz_pos && atomic_load(&nz_pos->state) < CHUNK_GENERATED) continue;
            if (nz_neg && atomic_load(&nz_neg->state) < CHUNK_GENERATED) continue;

            /* Set state to MESHING before extracting boundaries */
            atomic_store(&chunk->state, CHUNK_MESHING);

            /* Extract boundary slices (malloc'd, will be freed by worker) */
            size_t slice_size = 16 * CHUNK_Y * sizeof(BlockID);

            BlockID* b_pos_x = NULL;
            BlockID* b_neg_x = NULL;
            BlockID* b_pos_z = NULL;
            BlockID* b_neg_z = NULL;

            if (nx_pos) {
                b_pos_x = malloc(slice_size);
                mesher_extract_boundary(nx_pos, 0, b_pos_x); /* x=0 of +X neighbor */
            }
            if (nx_neg) {
                b_neg_x = malloc(slice_size);
                mesher_extract_boundary(nx_neg, 1, b_neg_x); /* x=15 of -X neighbor */
            }
            if (nz_pos) {
                b_pos_z = malloc(slice_size);
                mesher_extract_boundary(nz_pos, 2, b_pos_z); /* z=0 of +Z neighbor */
            }
            if (nz_neg) {
                b_neg_z = malloc(slice_size);
                mesher_extract_boundary(nz_neg, 3, b_neg_z); /* z=15 of -Z neighbor */
            }

            WorkItem* wi = calloc(1, sizeof(WorkItem));
            wi->type = WORK_MESH;
            wi->chunk = chunk;
            wi->boundary_pos_x = b_pos_x;
            wi->boundary_neg_x = b_neg_x;
            wi->boundary_pos_z = b_pos_z;
            wi->boundary_neg_z = b_neg_z;
            submit_work(world, wi);
        }
    }

    /* ---- Step 5: Build render mesh array ---- */
    {
        world->render_count = 0;

        uint32_t idx = 0;
        Chunk* chunk;
        while ((chunk = chunk_map_iter(&world->map, &idx)) != NULL) {
            if (atomic_load(&chunk->state) != CHUNK_READY) continue;
            if (!chunk->mesh.uploaded) continue;

            /* Grow array if needed */
            if (world->render_count >= world->render_cap) {
                world->render_cap *= 2;
                world->render_meshes = realloc(world->render_meshes,
                                                sizeof(ChunkMesh) * world->render_cap);
            }

            world->render_meshes[world->render_count++] = chunk->mesh;
        }
    }
}

void world_get_meshes(World* world, ChunkMesh** out_meshes, uint32_t* out_count)
{
    *out_meshes = world->render_meshes;
    *out_count = world->render_count;
}
