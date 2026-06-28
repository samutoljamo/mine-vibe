#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <cglm/cglm.h>
#include "chunk_mesh.h"
#include "block.h"

typedef struct Renderer Renderer;
typedef struct World World;
typedef struct Chunk Chunk;
typedef struct BlockPhysics BlockPhysics;
typedef struct BlockOverlay BlockOverlay;

World* world_create(Renderer* renderer, int seed, int render_distance);

/* Create a world with no renderer (server use). Mesh generation still
 * runs on worker threads but GPU uploads are skipped. */
World* world_create_headless(int seed, int render_distance);

void   world_destroy(World* world);

/* Attach a persistence overlay. When set, generation workers replay the
 * overlay's recorded block deltas on top of each freshly generated chunk so
 * loaded worlds reflect prior player edits. Pass NULL to detach. The overlay
 * is owned by the caller (typically the server) and must outlive the world. */
void   world_set_overlay(World* world, const BlockOverlay* overlay);

/* Put the world into network-fed mode: world_update stops submitting
 * WORK_GENERATE for missing chunks. Used by a remote (--client) world that is
 * populated exclusively from server-streamed chunks via
 * world_insert_network_chunk. The host/singleplayer shared world leaves this
 * off (the default) and generates terrain normally. */
void   world_set_network_fed(World* world, bool network_fed);

/* SERVER thread: synchronously ensure the column (cx,cz) is generated and
 * present in the map at >= CHUNK_GENERATED, applying the persistence overlay,
 * so block-edit validation and chunk streaming read the true terrain instead
 * of a fail-open AIR. Idempotent (no-op if already present and generated).
 * Thread-safe with the main/render thread's world_update via the world's map
 * mutex. Returns the chunk (never NULL on success), or NULL on allocation
 * failure. The returned chunk has NOT been lit/meshed — it is server-side
 * terrain authority only; the lighting/mesh pipeline still runs for chunks the
 * renderer cares about (host mode) via world_update. */
Chunk* world_ensure_chunk(World* world, int cx, int cz);

/* Read a full column's blocks (CHUNK_BLOCKS bytes, flat x + z*16 + y*256 order)
 * into `out` for streaming. Generates the column on demand if absent (server
 * thread). Returns true on success. `out_cap` must be >= CHUNK_BLOCKS. */
bool   world_copy_chunk_blocks(World* world, int cx, int cz,
                               uint8_t* out, size_t out_cap);

/* CLIENT thread (main): insert a column received from the server (decoded RLE
 * blocks) into the map, marking it CHUNK_GENERATED so the existing lighting +
 * mesh pipeline (run by world_update) lights and meshes it. If the column
 * already exists its blocks are overwritten and it is re-lit/re-meshed. Returns
 * true on success. Only valid in network-fed worlds. */
bool   world_insert_network_chunk(World* world, int cx, int cz,
                                  const uint8_t* blocks, size_t blocks_len);

/* CLIENT thread (main): evict a column the server told us to unload
 * (PKT_CHUNK_UNLOAD), freeing its GPU mesh. No-op if absent or currently being
 * processed by a worker. */
void   world_evict_chunk(World* world, int cx, int cz);

void   world_update(World* world, BlockPhysics* bp, vec3 player_pos);
void   world_get_meshes(World* world, ChunkMesh** out_meshes, uint32_t* out_count);

uint32_t world_get_ready_count(const World* world);
int      world_get_render_distance(const World* world);

BlockID world_get_block(World* world, int x, int y, int z);

/* Physics write API.
 * Returns true if the write was applied, false if deferred
 * (chunk not loaded or currently being meshed by a worker).
 * Caller must re-queue the position if false is returned. */
bool    world_set_block(World* world, int x, int y, int z, BlockID id);
bool    world_set_meta (World* world, int x, int y, int z, uint8_t level);
uint8_t world_get_meta (World* world, int x, int y, int z);

#endif
