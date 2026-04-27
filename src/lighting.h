#ifndef LIGHTING_H
#define LIGHTING_H

#include <stdint.h>
#include "block.h"

typedef struct Chunk Chunk;

typedef struct LightingNeighbors {
    Chunk* neg_x;
    Chunk* pos_x;
    Chunk* neg_z;
    Chunk* pos_z;
} LightingNeighbors;

/* Run the initial lighting pass for a freshly generated chunk.
 * Requires neighbors to be at least GENERATED so we can read their
 * block data (NULL neighbors are treated as fully-sky-lit at the
 * boundary, falling back gracefully at world edges).
 *
 * Updates this chunk's lights array and queues boundary deltas onto
 * neighbor chunks for them to pick up via lighting_consume_pending. */
void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb);

/* Consume any pending boundary deltas accumulated on this chunk by
 * neighbors. Runs targeted addition-BFS to apply them. Cheap if the
 * pending queue is empty. */
void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb);

/* Block-change relight for an in-place modification at local (x,y,z).
 * Walks removal-BFS (if light dropped) followed by addition-BFS (if
 * light rose) within the chunk and queues neighbor deltas as needed. */
void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id);

#endif
