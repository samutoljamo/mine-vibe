#ifndef CHUNK_STREAM_H
#define CHUNK_STREAM_H

/* Pure streaming-policy core for server-authoritative chunk streaming.
 *
 * Given a client's current center chunk, its render distance, and the set of
 * chunk coords already streamed to it, compute:
 *   - which in-range chunks are NOT yet sent (the "to send" set), and
 *   - which already-sent chunks have left range (the "to unload" set).
 *
 * No allocation, no world/net/Vulkan dependency, so it is unit-testable in
 * isolation (mirrors the chunkwire / ore test pattern). The server uses the
 * "to send" list to drive on-demand generation + PKT_CHUNK_DATA, and the
 * "to unload" list to drive PKT_CHUNK_UNLOAD.
 *
 * Selection policy:
 *   - "in range" uses the same circular disc test as world_update:
 *     dx*dx + dz*dz <= render_dist*render_dist.
 *   - the "to send" list is ordered nearest-first (by squared distance to the
 *     center) so the player's immediate surroundings stream in before the
 *     fringe.
 *   - the "to send" list is capped at `send_budget` entries per call so a
 *     freshly-joined client doesn't flood the link in a single tick; the
 *     remainder is picked up on subsequent calls (the already-sent set grows
 *     as the server records what it sent). `send_budget <= 0` means "no cap".
 *   - the "to unload" list is NOT budget-capped: dropping distant chunks is
 *     cheap (a tiny packet) and we want them gone promptly to bound memory.
 */

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int32_t cx, cz;
} ChunkCoord;

/* Compute the send/unload diff.
 *
 *   center_cx/cz : client's current center chunk.
 *   render_dist  : client render distance in chunks (>= 0).
 *   sent         : array of `sent_count` chunk coords already streamed.
 *   to_send      : out array (capacity `send_cap`) of in-range, not-yet-sent
 *                  coords, nearest-first; count written to *out_send.
 *   send_budget  : max coords to emit into `to_send` (<= 0 == unlimited).
 *   to_unload    : out array (capacity `unload_cap`) of sent coords now out of
 *                  range; count written to *out_unload.
 *
 * Output counts are clamped to their capacities (never overruns). Idempotent
 * when stationary and fully streamed: with `sent` covering the whole disc both
 * output counts are 0.
 */
void chunk_stream_diff(int32_t center_cx, int32_t center_cz, int render_dist,
                       const ChunkCoord* sent, size_t sent_count,
                       ChunkCoord* to_send, size_t send_cap, size_t* out_send,
                       int send_budget,
                       ChunkCoord* to_unload, size_t unload_cap,
                       size_t* out_unload);

/* True if (cx,cz) is within `render_dist` (circular) of the center. Exposed for
 * callers that want to test a single coord (e.g. validating a late edit). */
int chunk_stream_in_range(int32_t center_cx, int32_t center_cz,
                          int32_t cx, int32_t cz, int render_dist);

#endif /* CHUNK_STREAM_H */
