#ifndef CHUNKWIRE_H
#define CHUNKWIRE_H

/* Pure, allocation-free RLE (de)serialization for streaming a chunk column
 * (16 x 256 x 16 BlockIDs) over the network. No dependency on chunk.h /
 * world.h / Vulkan, so it is unit-testable in isolation. The number of blocks
 * per column is fixed by the engine's chunk dimensions; we re-derive it here to
 * avoid pulling in chunk.h (CHUNK_X*CHUNK_Y*CHUNK_Z = 16*256*16). */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CHUNKWIRE_COLUMN_BLOCKS (16 * 256 * 16)   /* must equal CHUNK_BLOCKS */

/* Worst-case encoded size for an n-block RLE payload. Each run is a varint
 * count (<=3 bytes for counts up to 65536) + 1 block byte. The pathological
 * input alternates every byte, giving n single-element runs of 2 bytes each. */
size_t chunkwire_rle_bound(size_t n);

/* RLE-encode `n` bytes from `blocks` into `out` (capacity `out_cap`).
 * Returns the number of bytes written, or 0 if `out_cap` is insufficient
 * (caller should size with chunkwire_rle_bound). Never writes past out_cap. */
size_t chunkwire_rle_encode(const uint8_t* blocks, size_t n,
                            uint8_t* out, size_t out_cap);

/* Decode an RLE payload `in`/`in_len` into `out` (capacity `out_cap`).
 * On success writes the decoded byte count to *out_n and returns true.
 * Returns false (without overrunning either buffer) on malformed input,
 * truncated varints, or if the decoded length would exceed out_cap. */
bool chunkwire_rle_decode(const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap, size_t* out_n);

/* ---- Full PKT_CHUNK_DATA body: {cx:i32, cz:i32, rle_len:u32, rle...} ---- */

/* Upper bound on the encoded chunk-body size (header fields + RLE bound). */
size_t chunkwire_encode_bound(void);

/* Serialize a full column: little-endian cx, cz, the RLE byte length, then the
 * RLE payload. `blocks` must point to CHUNKWIRE_COLUMN_BLOCKS bytes. Returns
 * the body length, or 0 if `out_cap` is insufficient. */
size_t chunkwire_encode_chunk(int32_t cx, int32_t cz, const uint8_t* blocks,
                              uint8_t* out, size_t out_cap);

/* Parse a chunk body produced by chunkwire_encode_chunk. Writes cx, cz and
 * the decoded CHUNKWIRE_COLUMN_BLOCKS bytes into `blocks` (capacity
 * `blocks_cap`, must be >= CHUNKWIRE_COLUMN_BLOCKS). Returns false on any
 * malformed / truncated input without overrunning. */
bool chunkwire_decode_chunk(const uint8_t* in, size_t in_len,
                            int32_t* cx, int32_t* cz,
                            uint8_t* blocks, size_t blocks_cap);

#endif /* CHUNKWIRE_H */
