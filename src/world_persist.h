#ifndef WORLD_PERSIST_H
#define WORLD_PERSIST_H

/* Server-side world persistence: a block-delta overlay.
 *
 * The world is deterministic from its seed, so we only need to persist the
 * *differences* a player makes — every block break/place. Each edit is
 * recorded as (global x,y,z) -> BlockID. On chunk generation the server
 * replays the matching deltas on top of the freshly generated terrain; the
 * result is byte-identical to the player's last session.
 *
 * Concurrency contract (see threading notes in the persistence plan):
 *   - A single mutex guards every mutation/read of the in-memory table.
 *   - File I/O (save/load) snapshots under the lock, then does the actual
 *     read/write OUTSIDE the lock so a slow disk never stalls the tick loop.
 *   - overlay_apply_chunk only reads, taking the lock briefly per lookup.
 *
 * This module is pure C: no Vulkan, no GLFW, no rendering. It is unit-tested
 * directly (tests/test_world_persist.c).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "block.h"
#include "platform_thread.h"

typedef struct Chunk Chunk;

/* One persisted edit. Packed coordinate key + the block placed there. */
typedef struct OverlayEntry {
    int32_t x, y, z;
    uint8_t block;     /* BlockID */
    uint8_t used;      /* slot occupancy flag for the open-addressing table */
} OverlayEntry;

typedef struct BlockOverlay {
    int32_t       seed;
    OverlayEntry* slots;
    size_t        cap;     /* power of two */
    size_t        count;   /* number of occupied slots */
    PT_Mutex      mutex;
    bool          mutex_init;
} BlockOverlay;

/* Initialize an empty overlay for the given world seed. */
void   overlay_init(BlockOverlay* ov, int32_t seed);

/* Free all memory. Safe to call on a deserialized or loaded overlay. */
void   overlay_free(BlockOverlay* ov);

/* Record (or overwrite) an edit. Thread-safe. */
void   overlay_record(BlockOverlay* ov, int32_t x, int32_t y, int32_t z, BlockID block);

/* Look up an edit. Returns true and writes *out if present. Thread-safe. */
bool   overlay_get(const BlockOverlay* ov, int32_t x, int32_t y, int32_t z, BlockID* out);

/* Number of distinct recorded edits. Thread-safe. */
size_t overlay_count(const BlockOverlay* ov);

/* The seed this overlay was created/loaded with. */
int32_t overlay_seed(const BlockOverlay* ov);

/* Apply every overlay entry that falls inside the given chunk to that
 * chunk's block array. Call right after worldgen_generate, before lighting.
 * (cx,cz are chunk coords; chunk must be CHUNK_X x CHUNK_Z.) Thread-safe. */
void   overlay_apply_chunk(const BlockOverlay* ov, Chunk* chunk);

/* ---- Pure serialize / deserialize (the TDD core; no file I/O) ---- */

/* Serialize the overlay into a freshly malloc'd buffer. Caller frees *out_buf.
 * Returns false on allocation failure. Thread-safe (snapshots under lock). */
bool   overlay_serialize(const BlockOverlay* ov, uint8_t** out_buf, size_t* out_len);

/* Parse a buffer produced by overlay_serialize into a fresh overlay.
 * Returns false (and leaves *ov uninitialized) on any format/length error. */
bool   overlay_deserialize(BlockOverlay* ov, const uint8_t* buf, size_t len);

/* ---- File I/O (serialize/deserialize wrapped around a path) ---- */

/* Atomically write the overlay to path (via a temp file + rename). */
bool   overlay_save(const BlockOverlay* ov, const char* path);

/* Load an overlay from path. Returns false if the file is missing or
 * malformed (caller should then start from a fresh overlay). */
bool   overlay_load(BlockOverlay* ov, const char* path);

#endif /* WORLD_PERSIST_H */
