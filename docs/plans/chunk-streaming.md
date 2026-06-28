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

## Mode 2 — Remote client: RLE chunk streaming (DONE)

### Wire additions (protocol v8)

- `PKT_CONNECT_REQUEST` gains a trailing `u8 shared_world` flag (after the
  version): the integrated host that renders the server world in-process sets it
  so the server **does not** stream chunks to it; a true remote client clears it
  and is streamed. Legacy/short reads default to 0 (remote).
- `PKT_CHUNK_DATA` (server → one): the chunkwire body `{ cx:i32, cz:i32,
  rle_len:u32, rle... }` is split into **one or more fragments**, each an
  ORDINARY reliable packet carrying a `{ msg_id:u16, index:u16, total:u16 }`
  subheader after the 8-byte packet header. Each fragment is independently
  acked + retransmitted by the existing reliable layer (we deliberately do NOT
  use the unacked magic-byte `reliable_send_fragmented` path — chunk delivery
  must be reliable). The client reassembles per `msg_id` at
  `CHUNK_DATA_FRAG_BYTES` stride, then RLE-decodes.
- `PKT_CHUNK_UNLOAD` (server → one, reliable): `{ cx:i32, cz:i32 }`.
- **Transport sizing:** `RELIABLE_MAX_PAYLOAD` 256 → **1200**, `RELIABLE_WINDOW`
  32 → **64**, `NET_THREAD_MAX_MSG` 512 → **1400**. A real RLE terrain column is
  ~6-8 KiB (NOT a few hundred bytes — terrain has dirt/grass/stone transitions,
  ores and trees, so it does not compress to almost nothing), i.e. ~6-7
  fragments at 1186 data bytes/fragment. The bigger payload keeps a whole
  column's fragments inside the reliable window so none get evicted.
- `NET_PROTOCOL_VERSION` bumped 7 → **8**.

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

### On-demand server world authority (`world_ensure_chunk`)

`world_ensure_chunk(World*, cx, cz)` synchronously generates a column on the
calling (server) thread — worldgen + overlay replay — and inserts it at
`CHUNK_GENERATED`. It is idempotent and thread-safe (see locking below).
`handle_block_break` / `handle_block_place` now call it before reading the
target cell, so block-edit validation consults the TRUE block instead of the
old fail-open AIR band-aid. `world_copy_chunk_blocks` reads a full column
through it for streaming.

### Map-mutex locking (host shared-world safety)

`World` gained a `map_mutex` guarding all STRUCTURAL map mutations. In host
shared-world mode the server thread (`world_ensure_chunk`) and the main/render
thread (`world_update`) both mutate the chunk map, and a `chunk_map_put` can
trigger a rehash that reallocs the entries array — so a lock-free probe could
crash. The lock serializes them:
- `world_update` holds it across its whole structural body.
- `world_ensure_chunk` / `world_insert_network_chunk` / `world_evict_chunk` hold
  it for their put/remove.
- `world_get_block` / `world_set_block` / `world_get_meta` / `world_set_meta`
  take it around the map lookup (the block-byte read/write itself stays
  lock-free; a chunk pointer is stable once published until removed under the
  lock). This also closes a pre-existing latent rehash race.

### Server push loop (`server_stream_chunks`)

Per **remote** client (the shared-world host is skipped via the connect flag),
`ServerClient` tracks the set of chunk coords already streamed (`streamed[]`).
Each tick the pure `chunk_stream_diff` (see TDD) computes, against the client's
last-known position and the server world's render distance:
- the nearest-first set of in-range, not-yet-sent columns (capped at
  `SERVER_STREAM_BUDGET = 4` per tick so a joiner doesn't flood the link), and
- the set of already-sent columns now out of range.
For each new column it `world_copy_chunk_blocks` (on-demand gen) → chunkwire
encode → fragment → `send_reliable`, and records it. Departed columns get
`PKT_CHUNK_UNLOAD` and leave the set. A pathological incompressible column that
would exceed `SERVER_STREAM_MAX_FRAGS = 48` fragments is skipped (logged); real
terrain never approaches this.

### Client side

The remote client's `World` is created in **network-fed** mode
(`world_set_network_fed`): `world_update` skips Step 3 (`WORK_GENERATE`), so it
never regenerates terrain from the seed. On `PKT_CHUNK_DATA` (reassembled +
RLE-decoded) a callback calls `world_insert_network_chunk`, which fills the
column's blocks and marks it `CHUNK_GENERATED` so the existing lighting + mesh
pipeline lights and meshes it. `PKT_CHUNK_UNLOAD` calls `world_evict_chunk`
(frees the GPU mesh). `PKT_BLOCK_CHANGE` applies on top exactly as before. The
loading loop pumps `client_poll` so chunks actually arrive during load. The
host/singleplayer shared-world path is unchanged (network-fed stays off).

## TDD

`tests/test_chunk_stream.c` (pure, links only `chunk_stream.c`) covers the
streaming-policy core `chunk_stream_diff`:

- fresh client gets the full circular disc (no budget cap);
- send list is nearest-first (non-decreasing squared distance, center first);
- budget cap emits only the K nearest pending columns; streaming across calls
  (recording sent each time) covers the whole disc with no duplicates;
- stationary + fully streamed is idempotent (no sends, no unloads);
- moving by one chunk sends exactly the new leading edge and unloads the
  trailing edge (verified against a reference disc-difference, no overlap);
- teleport unloads the whole old set and queues the whole new disc;
- tiny output caps clamp without overrun and stay nearest-first.

`tests/test_net.c` adds a `PKT_CHUNK_DATA` fragment-subheader + `PKT_CHUNK_UNLOAD`
round-trip and the v8 connect `shared_world` flag round-trip.

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

- **Host / singleplayer shared world: DONE** — unchanged by this work; verified
  the map-mutex additions don't regress it (agent/host smoke test runs 400+
  ticks, server marks it "shared-world host", no streaming to it).
- **RLE module + tests: DONE** — pure, round-trip tested.
- **On-demand server world authority (`world_ensure_chunk`) + real validation:
  DONE** — the AIR fail-open band-aid is gone; break/place ensure-then-validate.
- **Server push loop + per-client streamed set: DONE** (`server_stream_chunks`,
  `chunk_stream_diff`, budget 4/tick, nearest-first, unload of departed chunks).
- **Client network-fed world + receive/apply: DONE** (`world_set_network_fed`,
  `world_insert_network_chunk`, `world_evict_chunk`, fragment reassembly in
  `client_poll`).
- **Wire packets + version bump: DONE** (`PKT_CHUNK_DATA` fragments,
  `PKT_CHUNK_UNLOAD`, connect `shared_world` flag, **v8**).
- **End-to-end verified by hand:** dedicated `--server` + `--client` over
  loopback — client connects as "remote", receives 100+ streamed columns
  nearest-first, zero decode failures, no errors.

### Known follow-ups (not blocking)

- **Streaming render distance is the server world's `render_distance`, not the
  client's.** In host mode they match (passed in). For a dedicated server they
  can differ (server default 8 vs client 12) so a remote client sees a slightly
  smaller disc than its render distance. Fix: carry the client's render distance
  in the connect handshake and stream at `min(server_cap, client_rd)`.
- **Single in-flight reassembly per client.** The client reassembles one column
  (`msg_id`) at a time; columns are sent fragments-consecutively so this is fine
  in practice, but a retransmit of an old column's fragment interleaving a new
  column under heavy loss could drop the old column (it would not self-heal,
  since the server marks it streamed). A per-`msg_id` reassembly ring would make
  it bulletproof. Real terrain (≤7 fragments, loopback-tested clean) is safe.
- **Interest culling / vertical interest:** still streams the whole horizontal
  disc; epic `0w8` keeps this sub-scope open.
