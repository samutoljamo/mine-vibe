# Server-Authoritative World / Chunk Streaming

Beads epic: `mine-vibe-0w8`.

## Problem

Today the server and *each* client independently generate the world from the
shared seed; only block-change deltas (`PKT_BLOCK_CHANGE`) plus a persistence
overlay are synced. This is fragile:

- **Validation gaps**: server interaction depends on the server having
  *generated* the chunk the player touched.
- **Late-joiner divergence**: a client that joins regenerates from seed and can
  miss persisted edits, or diverge from server terrain if worldgen ever changes.

There are effectively *two* authoritative worlds. The fix is **one** world:

1. **Host / singleplayer** (the common case): the client renders the
   integrated server's world directly — no second generated copy.
2. **Remote client** (true multiplayer): the server *streams* chunk data; the
   client stops seed-generating and builds its world from received chunks.

## Mode 1 — Host / singleplayer: shared world (DONE)

### Before

`main.c` (host) created its own `World*` via `world_create(&renderer, seed, rd)`
which generates terrain; the server thread created a *separate*
`world_create_headless(seed, 8)` and drove `world_update` every tick. Two
worlds generated from the same seed; only block deltas kept them roughly in
sync.

### After

There is exactly one `World`, owned by the server, shared with the host's
client/renderer:

- The server creates the world with the renderer attached and the client's
  render distance: a new `server_run_ex(port, max, seed, save_path, renderer,
  render_distance)` overload. `renderer != NULL` ⇒ GPU uploads happen; render
  distance matches the client so the visible world is fully streamed.
- The server publishes the live `World*` through `server_get_world()` (returns
  NULL until the world exists; the host spins briefly waiting for it, same
  pattern as the existing 200 ms connect grace).
- **Exactly one thread drives `world_update`.** In host mode that is the *main
  (render) thread* — it must own GPU uploads anyway (Vulkan). The server thread
  therefore **does not** call `world_update` when a renderer is attached
  (`s->drives_world_update == false`); it only reads/writes blocks.
- `main.c` host path: instead of `world_create`, it grabs `server_get_world()`
  and uses it for `world_update`, meshing, raycast, physics, rendering. It does
  **not** create or destroy a second world (the server owns the lifetime).

### Why this is safe (cross-thread access)

The robustness pass already made the hot cross-thread fields atomic:

- `chunk->state` is `_Atomic`; `chunk->meta` / `chunk->lights` are
  `_Atomic(uint8_t*)` published via CAS (`chunk_lazy_alloc`).
- `world_get_block` / `world_set_block` / `world_get_meta` / `world_set_meta`
  gate on `chunk->state` with `atomic_load` and refuse writes while a chunk is
  `MESHING`/`LIGHTING`. The block byte array itself is fixed-size and only
  *content* mutates after `GENERATED`; readers tolerate a torn single-byte read
  (a block id is one byte — reads/writes are atomic at the byte granularity on
  all supported targets).

The remaining shared-map mutation (adding/removing chunks in `world->map`,
advancing chunk states, building the render-mesh array) lives **entirely in
`world_update`, which only the main thread calls in host mode.** The server
thread only ever calls `world_get_block` / `world_set_block` (block edits +
mob/collision/survival queries) — all of which are the
already-concurrency-safe atomic ops. So we never have two threads mutating the
chunk map.

### How we verified host mode no longer double-generates

- Host path in `main.c` no longer calls `world_create`; it asserts it received
  a non-NULL world from `server_get_world()`. Only the server's
  `world_create*` runs, so terrain is generated once.
- The server, when a renderer is attached, sets `drives_world_update = false`
  and skips its own `world_update` call — so the chunk pipeline (generate →
  light → mesh) is pumped by exactly one thread.
- Block edits applied by the server (`world_set_block` on the shared world) are
  visible to the renderer immediately; the host main loop skips re-applying
  `PKT_BLOCK_CHANGE` echoes to the shared world (they would be idempotent
  no-ops anyway, but we skip to avoid the redundant relight) — it still needs
  them only to wake block physics, which it does.

## Mode 2 — Remote client: RLE chunk streaming (PARTIAL — see status)

### Wire additions (protocol v7)

- `PKT_CHUNK_DATA` (server → client, reliable + fragmented via
  `reliable_send_fragmented`, since a 64 KiB column far exceeds one datagram):
  payload `{ cx:i32, cz:i32, rle_len:u32, rle_payload }` where `rle_payload`
  is the RLE-compressed `BlockID[CHUNK_BLOCKS]` column.
- `PKT_CHUNK_UNLOAD` (server → client, reliable): `{ cx:i32, cz:i32 }` — tells
  the client to drop a now-distant chunk.
- `NET_PROTOCOL_VERSION` bumped 6 → 7.

### RLE format (`src/chunkwire.{c,h}` — pure, tested)

Column blocks are serialized in the same flat index order as
`chunk->blocks[]` (`x + z*16 + y*256`). Long vertical runs of air (above
terrain) and stone (below) dominate, so run-length encoding shrinks a 64 KiB
column to a few hundred bytes typically.

Encoding: a sequence of `(count:varint, block:u8)` runs. `count` is a
LEB128-style varint (7 bits/byte, high bit = continue) so runs up to
`CHUNK_BLOCKS` fit; `block` is the repeated `BlockID`. The decoder validates
that the decoded length is exactly `CHUNK_BLOCKS` and never overruns the output
or input buffers (returns failure otherwise — never aborts).

Pure API (no allocation surprises; caller provides buffers):

```c
size_t chunkwire_rle_encode(const uint8_t* blocks, size_t n,
                            uint8_t* out, size_t out_cap); /* 0 = out too small */
bool   chunkwire_rle_decode(const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t out_cap, size_t* out_n);
size_t chunkwire_rle_bound(size_t n); /* worst-case encoded size for buffer sizing */
```

Chunk (de)serialize helpers build/parse the `PKT_CHUNK_DATA` body around the
RLE payload (`chunkwire_encode_chunk` / `chunkwire_decode_chunk`).

### Server side

`Server` tracks, per client, the set of chunk coords already sent
(`loaded[]`). Each tick, for the client's render distance around its position,
the server: ensures the chunk is generated (reusing `world_update`'s pipeline /
an on-demand generate), encodes it, and sends it (throttled to a few per tick).
Far chunks get `PKT_CHUNK_UNLOAD` and are dropped from the set.

### Client side

The remote client **stops seed-generating**: its `World` is created in a
"network-fed" mode where `world_update` does *not* submit `WORK_GENERATE` for
missing chunks. On `PKT_CHUNK_DATA` it decodes the column, fills the chunk's
blocks, marks it `GENERATED`, and lets the existing lighting+mesh pipeline run.
`PKT_BLOCK_CHANGE` applies on top exactly as today.

## TDD

`tests/test_chunkwire.c` (pure, links only `chunkwire.c`):

- RLE round-trip on random columns, run-heavy columns, all-air, all-stone,
  single block, alternating (worst case).
- Encode into an undersized buffer returns 0; decode of truncated/garbage
  input returns false without overrun.
- `chunkwire_rle_bound` is a true upper bound for every tested input.
- Full chunk-body (de)serialize equivalence: encode `{cx,cz,blocks}` → decode →
  identical cx, cz, and all `CHUNK_BLOCKS` bytes.

Integration (host shared-world, remote streaming) is verified by a clean build
+ the reasoning above; host mode is what the user tests interactively.

## Status

- **Host / singleplayer shared world: DONE** — fully implemented + builds; the
  one-world invariant and no-double-generate are argued above.
- **RLE module + tests: DONE** — pure, round-trip tested.
- **Wire packets + version bump: DONE** (`PKT_CHUNK_DATA`, `PKT_CHUNK_UNLOAD`,
  v7).
- **Remote streaming end-to-end (server push loop + client receive/apply):
  PARTIAL / TODO** — wire format, RLE, and (de)serialize are in place and
  tested; the server per-client chunk push loop and client network-fed world
  mode are the remaining integration work, tracked as a follow-up bead. Remote
  multiplayer still works via the legacy seed-gen + block-delta path until then.
