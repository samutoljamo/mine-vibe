# Server-Side World Persistence (save/load)

Beads issue: **mine-vibe-qln**

Status: design / plan only. No source changes accompany this document.

---

## 0. Problem statement and grounding

The world is **seed-only** today. `world_create_headless(seed, ...)` builds an
authoritative headless `World` on the server (`src/server.c:620`), and every
chunk is regenerated identically each run by `worldgen_generate(chunk, seed)`
(`src/worldgen.c:170`, called from the worker thread at `src/world.c:126`).
Because worldgen is a pure function of `(cx, cz, seed)`, terrain is reproducible
for free — but **player edits are lost on restart**.

Critically, the server does not even apply edits to its own world right now:

- `handle_block_break` (`src/server.c:210`) credits inventory and calls
  `server_broadcast_block_change(... BLOCK_AIR)` (`src/server.c:245`) — it never
  touches `s->world`.
- `handle_block_place` (`src/server.c:249`) consumes inventory and broadcasts
  the placed block (`src/server.c:296`) — again, no world write.

So the server's `World` only ever contains pristine worldgen output (used for
mob terrain/collision in `server_simulate_mobs`, `src/server.c:394`). The
comments at `src/net.h:211` and `src/server.c:236` even call this out: "Server
doesn't own a world to validate replaceability against."

This means persistence work has **two halves**:

1. Make the server authoritatively record edits (apply to `World` + persist).
2. Replay persisted edits into chunks as they regenerate from seed.

---

## 1. What to persist: block-delta overlay vs full-chunk serialization

**Recommendation: a block-delta / overlay format.** Persist only blocks that
differ from worldgen output, keyed by absolute world coordinates
`(x, y, z) -> (block, meta)`.

Justification:

- **Disk size.** A chunk is `CHUNK_BLOCKS = 16 * 256 * 16 = 65 536` blocks
  (`src/chunk.h:16`). Full-chunk serialization costs ~64 KB of `blocks[]` plus
  optional `meta[]` per *touched* chunk regardless of how few blocks changed.
  Player activity is overwhelmingly sparse — a handful of edits per chunk. A
  delta record is ~14 bytes (see section 2); even 100 000 edits is ~1.4 MB.
- **Simplicity.** The overlay is a flat list of independent records. No chunk
  framing, no per-chunk versioning, no compression needed for v1. Serialize =
  walk a hash map and emit records; deserialize = read records into a hash map.
- **Correctness with worldgen.** Worldgen is the source of truth for everything
  not edited. Storing only deltas means a worldgen tuning change (new ores,
  retuned caves) is automatically picked up for untouched terrain on the next
  load; only the explicit edits are pinned. Full-chunk saves would freeze entire
  chunks at their old worldgen output and silently mask worldgen changes — a
  correctness footgun. The seed+version guard (section 8) bounds the risk that
  worldgen drift moves an edit's "base" out from under it.
- **Determinism already exists.** `ore_select` and the noise layers are pure
  functions of position+seed (see `tests/test_ore.c`), so "regenerate then apply
  overlay" is reproducible and testable.

Trade-off accepted: a block edited *back* to its original worldgen value still
occupies a record unless we prune it (section 4 handles this — we compare
against a freshly generated value and drop no-op deltas).

---

## 2. On-disk format

### 2.1 Location

```
saves/<world>/region.delta        # the overlay (single file for v1)
saves/<world>/meta.txt            # optional human-readable info (future)
```

- `<world>` defaults to `world` (single-world server). Make it a parameter of
  `server_run` later; for v1 hardcode `saves/world/`.
- The `saves/` directory is created on demand (`mkdir -p` equivalent) at first
  save. Add `saves/` to `.gitignore` (currently only contains `.worktrees/`,
  `.dolt/`, `*.db`, `.beads-credential-key`).

### 2.2 Endianness

**Little-endian**, matching the netcode wire format. `src/net.h:36-100` already
provides `net_write_u8/u16/u32/i32/float` and `net_read_*` as LE primitives.
Reuse those exact helpers for the file format so there is one serialization
convention in the codebase and zero new endianness code.

### 2.3 Binary layout

All multi-byte fields little-endian.

```
Header (fixed, 24 bytes):
  offset 0   u8[4]  magic         = { 'M','V','W','S' }  (Mine-Vibe World Save)
  offset 4   u32    version       = 1
  offset 8   i32    seed          (must match server seed on load)
  offset 12  u32    record_count
  offset 16  u32    reserved      = 0   (future flags / chunk-index offset)
  offset 20  u32    header_crc    = CRC32 of bytes [0..20)   (optional v1, see risks)

Records (record_count entries, 14 bytes each):
  i32  x
  i32  y          (0..CHUNK_Y-1; stored full-width for layout uniformity)
  i32  z
  u8   block      (BlockID)
  u8   meta       (water level etc.; 0 when unused)
```

Notes:

- Record is fixed-width (14 bytes) so the file can be memory-checked
  (`file_size == 24 + 14*record_count`) and records can be bulk-read.
- `y` could be `u16` (range 0..255), but keeping it `i32` keeps the read/write
  loop trivially symmetric with the `x`/`z` `net_read_i32` calls and the on-disk
  saving is negligible. The serializer can switch to `u8` later under a new
  version if size ever matters.
- `block` is `u8`: `BLOCK_COUNT` fits in a byte (same assumption the wire
  format makes at `src/net.h:227`).
- A future v2 may add a per-chunk index for partial loads; `reserved` is the
  hook. Not needed while the whole overlay fits in memory.

### 2.4 In-memory representation

A dedicated `WorldOverlay` keyed by packed `(x,y,z)`:

```c
typedef struct { int32_t x, y, z; uint8_t block, meta; } OverlayRecord;

typedef struct WorldOverlay {
    /* open-addressed hash map keyed on packed coords, mirroring chunk_map.c */
    OverlayEntry* entries;
    uint32_t      capacity;   /* power of two */
    uint32_t      count;
    int32_t       seed;       /* for the load-time guard */
} WorldOverlay;
```

Key packing: `y` is 0..255 (8 bits); `x`/`z` are world ints. Pack with the same
splitmix64 mixing style already in `chunk_map.c:7` (`hash_key`). A 64-bit key is
insufficient for full 32-bit x/z + 8-bit y (72 bits), so either:
- store the full `(x,y,z)` in the entry and hash a 96-bit-derived value, or
- reuse `chunk_map`'s pattern: hash `x,z`, then a secondary structure per cell.

Simplest correct choice for v1: store full `(x,y,z)` in each entry and compute
the probe hash from all three via two splitmix64 rounds. Lookup compares all
three fields (exactly how `chunk_map` compares `cx,cz` at `chunk_map.c:81`).

---

## 3. Server integration

### 3.1 Where to record an edit

Both edit handlers must (a) apply to the authoritative world and (b) record the
delta. Add a single helper used by both:

```c
/* in server.c */
static void server_apply_edit(Server* s, int x, int y, int z, BlockID b, uint8_t meta);
```

It must:
1. `world_set_block(s->world, x, y, z, b)` (`src/world.h:33`).
   - Note `world_set_block` returns `false` when the chunk is unloaded or being
     meshed/lit (`src/world.c:905-917`). For persistence correctness the
     **overlay write must happen regardless** of whether the live world write
     landed — the overlay is the durable record and will be replayed at
     regen time anyway. So: record the delta unconditionally; the world write is
     a best-effort live-state update for mob collision and future server-side
     validation.
2. `world_overlay_set(&s->overlay, x, y, z, b, meta)` (records the delta;
   prunes if equal to worldgen, see section 4).
3. mark overlay dirty for the save scheduler.

Call sites:
- In `handle_block_break` replace the bare broadcast at `src/server.c:245` with
  `server_apply_edit(s, p.x, p.y, p.z, BLOCK_AIR, 0)` *before* broadcasting.
- In `handle_block_place` replace the bare broadcast at `src/server.c:296` with
  `server_apply_edit(s, tx, ty, tz, b, 0)` *before* broadcasting.

Broadcast (`PKT_BLOCK_CHANGE`) ordering is unchanged — it stays after the apply.

### 3.2 Applying the overlay at chunk-generation time

Chunks are generated by `worldgen_generate(item->chunk, item->seed)` on a worker
thread (`src/world.c:126`). The overlay must be applied to the chunk **after**
worldgen fills it and **before** the chunk is marked `CHUNK_GENERATED` /
lighting begins, so loaded chunks reflect edits from the first mesh.

Implementation: add an overlay pointer to `World` and apply inside the worker
right after `worldgen_generate`:

```c
/* world.c worker_func, WORK_GENERATE branch */
worldgen_generate(item->chunk, item->seed);
if (world->overlay)
    world_overlay_apply_chunk(world->overlay, item->chunk);  /* reads overlay, writes chunk->blocks/meta */
```

`world_overlay_apply_chunk` iterates the chunk's world-coord bounds
(`base_x = cx*16 .. +16`, full y, `base_z = cz*16 .. +16`) and for each overlay
record in range calls `chunk_set_block` / `chunk_set_meta` (`src/chunk.h:66,89`).
To avoid scanning all records per chunk, the apply path should look up by chunk:
either (a) keep a secondary index `chunk(cx,cz) -> list of records`, or (b) for
v1 simplicity, probe the 65 536 cells is wrong — instead iterate the overlay's
record array once and bucket by chunk at load, or just iterate all records and
test `cx,cz` membership. Given edit counts are small, **iterate all records,
filter by chunk** is acceptable for v1; revisit with a per-chunk index if the
overlay grows large (section 8).

`World` needs the overlay handle. Pass it via a new
`world_set_overlay(World*, WorldOverlay*)` (called once after
`world_create_headless`) rather than widening the constructor, to keep
`world_create` signature stable and the client (no overlay) untouched.

### 3.3 Save cadence

Three triggers, layered:

1. **On shutdown** (mandatory): flush in `server_run` after the loop, before
   `world_destroy(s.world)` at `src/server.c:644`. This is the baseline
   guarantee.
2. **Periodic flush** (mandatory): every N seconds (e.g. 30 s) on the tick
   thread, only if the overlay is dirty. Drive it from the tick counter in
   `server_tick` (`src/server.c:488`) — e.g. `if (tick_num % (SERVER_TICK_RATE*30) == 0)`.
3. **On edit (optional, off by default):** do *not* fsync on every edit — at 20
   Hz with multiple clients this would thrash disk. The dirty flag + periodic
   flush bounds data loss to the flush interval, which is acceptable for a game.

All saves go through the atomic temp-file+rename path (section 8).

---

## 4. Determining "changed from worldgen"

**Recommendation (simplest correct): record every server-applied edit, then
prune at write time by comparing against a fresh worldgen sample.**

Rationale: the server is the only authority that mutates blocks. Every mutation
flows through the two handlers in section 3.1. So "what changed" is exactly "what
the server applied" — we do not need to diff whole chunks against worldgen at
runtime. Record each applied edit into the overlay; the latest write for a cell
wins (hash map upsert).

Pruning (keeps the overlay from accumulating no-op records, e.g. break a
player-placed block back to the air that worldgen produced):

- When applying an edit, compute the worldgen value for that single cell and
  compare. If the new block (and meta) equals the worldgen value, **remove** the
  cell from the overlay instead of storing it.
- Per-cell worldgen sampling needs a pure function. `worldgen_generate` fills a
  whole chunk; for a single cell we either:
  - (a) add a `worldgen_block_at(int x, int y, int z, int seed)` pure helper
    (preferred — small, testable, mirrors `ore_select`), or
  - (b) skip pruning in v1 and accept that reverted edits leave a record (the
    record is harmless: applying `AIR` over worldgen-`AIR` is a no-op).

For v1, **ship without pruning (option b)** to keep scope tight, and file a
follow-up for `worldgen_block_at` + pruning. The format and apply path are
unaffected; pruning is a pure optimization.

Explicitly rejected: "diff the live chunk against a regenerated chunk to
discover edits." That is more code, requires a second generate pass, and is
strictly unnecessary because the server already sees every edit at its source.

---

## 5. Threading

Current threading contract:
- Worker threads run `worldgen_generate` and (new) overlay-apply during
  `WORK_GENERATE` (`src/world.c:125-143`).
- The tick/main thread records edits in the packet handlers and runs the
  periodic/shutdown save.

So the overlay is **read by workers** (apply at gen) and **written by the tick
thread** (record edit, prune, save). This is a classic single-writer /
multiple-reader pattern and needs synchronization.

Design:

- Add `PT_Mutex overlay_mutex` to `WorldOverlay` (use `platform_thread.h`, the
  same primitives `chunk.h:48` and `world.c` already use).
- **Writes** (`world_overlay_set`, prune, rehash): lock for the whole
  operation. These happen on the tick thread at edit rate — low contention.
- **Reads at apply time** (`world_overlay_apply_chunk`): lock for the duration
  of the per-chunk scan. The scan is O(record_count) (v1) under the lock; if
  that becomes a hot path, switch to a per-chunk index so the locked section is
  O(records in this chunk), or snapshot under lock then apply lockless.
- **Save** (`world_overlay_save`): lock while serializing into a buffer (or
  while iterating entries), then do file I/O (temp write + rename) **outside**
  the lock so disk latency does not stall workers. Simplest: under the lock,
  copy the records into a contiguous array; release; write that array to disk.

Important interaction with `world_set_block`'s own rules: the worker holds no
chunk mutex during generate, but the chunk is in `CHUNK_GENERATING` state and
not yet visible to readers that gate on `>= CHUNK_GENERATED`
(`src/world.c:889`). Applying the overlay before flipping to `CHUNK_GENERATED`
means no other thread observes a half-applied chunk. The existing pipeline
already sets `GENERATED` only when the result is processed on the main thread
(`src/world.c:464-486`), so the worker writing `chunk->blocks` during
`CHUNK_GENERATING` is consistent with how `worldgen_generate` already writes it.

Lock-ordering note: never take `overlay_mutex` while holding a chunk's
`pending_mutex` (or vice versa). The apply path only touches `chunk->blocks` /
`chunk->meta` directly (not the lighting deltas), so there is no nesting.

---

## 6. Testability

A new pure module `src/world_overlay.c` / `.h` with **no Vulkan, no threads in
the data-path** (the mutex can be a thin wrapper that the unit test can drive
single-threaded) is unit-testable exactly like `tests/test_ore.c` and
`tests/test_inventory.c`.

Split responsibilities so the serializer is pure bytes-in / bytes-out:

- `size_t world_overlay_serialize(const WorldOverlay*, uint8_t* buf, size_t cap);`
- `bool   world_overlay_deserialize(WorldOverlay*, const uint8_t* buf, size_t len);`
- File I/O (`world_overlay_save`/`load`) is a thin wrapper over those plus
  temp-file+rename, tested separately / manually.

### Test cases (`tests/test_world_overlay.c`)

1. **Empty roundtrip**: serialize an empty overlay, deserialize, count == 0;
   header magic/version/seed correct.
2. **Single record roundtrip**: set one `(x,y,z,block,meta)`, serialize,
   deserialize into a fresh overlay, read back identical block+meta.
3. **Many records roundtrip**: insert ~10 000 records at varied coords (incl.
   negative x/z, y at 0 and 255), roundtrip, assert every cell matches.
4. **Upsert / latest-write-wins**: set a cell, set it again with a different
   block, assert only one record and it holds the second value.
5. **Get-miss returns sentinel**: querying an unset cell reports "absent"
   (so apply leaves worldgen value intact).
6. **Apply-to-chunk**: build a chunk, fill `blocks` with a known pattern,
   insert overlay records inside that chunk's coord range plus some outside it,
   call `world_overlay_apply_chunk`, assert in-range cells overwritten and
   out-of-range cells/other-chunk cells untouched.
7. **Negative coordinate keying**: records at `(-1,-1?,-1)`-style coords (y
   stays >=0) and chunk math `(x<0)` path (`src/world.c:884`) resolve to the
   right cell — guards the floor-division key packing.
8. **Magic mismatch rejected**: deserialize a buffer with wrong magic ->
   returns false, overlay unchanged.
9. **Version mismatch rejected**: wrong version byte -> false.
10. **Truncated buffer rejected**: `len < 24` and `len != 24+14*count` -> false,
    no OOB read.
11. **Seed guard**: deserialize records a seed; a helper
    `world_overlay_seed_matches(ov, server_seed)` returns false on mismatch.
12. **Determinism cross-check (integration-ish)**: regenerate a chunk from seed,
    apply overlay, assert the specific edited cells differ from a pristine
    regen and all others are identical (ties the overlay to worldgen purity,
    mirroring `tests/test_ore.c`'s determinism test).

Wire it into CMake mirroring the `test_inventory` block (`CMakeLists.txt:312`):
`add_executable(test_world_overlay tests/test_world_overlay.c src/world_overlay.c ...)`
+ `add_test(NAME world_overlay COMMAND test_world_overlay)`. Include `src/net.h`
for the LE helpers; link `m` on Unix if needed.

---

## 7. Step-by-step task breakdown and commit sequence

Each step is a self-contained commit. Tests precede or accompany the code they
cover (TDD-friendly).

**Commit 1 — overlay data structure + pure serialization (no integration).**
- New `src/world_overlay.h`, `src/world_overlay.c`: `WorldOverlay`,
  `world_overlay_init/free`, `world_overlay_set`, `world_overlay_get`,
  `world_overlay_serialize`, `world_overlay_deserialize`. Reuse LE helpers from
  `src/net.h:36`. Hash map mirrors `src/chunk_map.c` (open addressing,
  splitmix64, 70% rehash).
- New `tests/test_world_overlay.c` covering cases 1-5, 8-11.
- CMake: add `test_world_overlay` target near `CMakeLists.txt:312`.

**Commit 2 — apply-to-chunk + worker integration.**
- Add `world_overlay_apply_chunk(WorldOverlay*, Chunk*)` to the overlay module
  (operates on `chunk->cx/cz`, `chunk_set_block`/`chunk_set_meta`).
- Add `world_set_overlay(World*, WorldOverlay*)` and an `overlay` field to
  `struct World` (`src/world.c:68`).
- In `worker_func` WORK_GENERATE branch, after `worldgen_generate`
  (`src/world.c:126`), call apply under `overlay_mutex`.
- Tests: cases 6, 7, 12.

**Commit 3 — file save/load with atomic write.**
- `world_overlay_save(const WorldOverlay*, const char* path)`:
  serialize to buffer, write `path.tmp`, fsync, `rename(path.tmp, path)`.
- `world_overlay_load(WorldOverlay*, const char* path)`: read file, validate
  size, `world_overlay_deserialize`. Missing file -> empty overlay, success.
- `mkdir -p saves/world` helper (platform-guarded; `src/platform_*` if one
  exists, else `mkdir`/`_mkdir`).
- Add `saves/` to `.gitignore`.

**Commit 4 — server wiring: record edits + apply + cadence.**
- Add `WorldOverlay overlay;` and a `bool overlay_dirty;` (or last-save tick) to
  `struct Server` (`src/server.h:36`).
- In `server_run` (`src/server.c:601`): after `world_create_headless`
  (`src/server.c:620`), init overlay, `world_overlay_load(saves/world/...)`,
  guard seed (section 8), `world_set_overlay(s.world, &s.overlay)`.
- Add `server_apply_edit` helper; call it from `handle_block_break`
  (replace/augment `src/server.c:245`) and `handle_block_place`
  (`src/server.c:296`).
- Periodic flush in `server_tick` (`src/server.c:488`) on dirty + interval.
- Shutdown flush in `server_run` before `world_destroy` (`src/server.c:644`),
  then `world_overlay_free`.

**Commit 5 — (optional) pruning + `worldgen_block_at`.**
- Pure `worldgen_block_at(x,y,z,seed)` in `src/worldgen.c` + unit test.
- Prune no-op deltas in `server_apply_edit`.

Suggested order: 1 -> 2 -> 3 -> 4, with 5 as a follow-up. Each of 1-3 is
independently testable and mergeable; 4 is the behavioral switch-on.

---

## 8. Risks and mitigations

- **Seed / version mismatch on load.** If `saves/world/region.delta` was made
  with a different seed, replaying its deltas onto different terrain corrupts the
  world (an edit recorded as "dirt over stone" lands on air). Mitigation: store
  `seed` and `version` in the header; on load, if either mismatches the running
  server's seed/version, **refuse to apply** — log a clear error and either
  (a) start fresh (rename the old file aside) or (b) abort startup. Recommend
  abort-with-message in v1 to avoid silent data loss; make "start fresh" a flag.

- **Corruption / partial writes.** A crash mid-write must not destroy the prior
  save. Mitigation: **temp-file + rename** — write `region.delta.tmp`, `fsync`
  it, then `rename()` over `region.delta` (atomic on POSIX same-filesystem;
  on Windows use `ReplaceFile`/`MoveFileEx(MOVEFILE_REPLACE_EXISTING)`).
  Optionally `header_crc` (and/or a whole-file CRC in `reserved`/v2) to detect
  bit-rot; on CRC failure, treat as corrupt per the seed-mismatch policy.

- **Overlay growth.** A long-lived server accumulates records; v1 keeps them all
  in memory and rewrites the whole file each flush. At ~14 B/record this is fine
  into the millions of edits, but full-file rewrite cost grows. Mitigations:
  (1) pruning (Commit 5) removes reverted edits; (2) latest-write-wins upsert
  already collapses repeated edits to one record per cell; (3) future v2:
  per-chunk index + append/compact, or per-region files keyed by chunk so only
  touched regions are rewritten. The `reserved` header field is the hook.

- **Apply cost per chunk (v1 full scan).** Iterating all records per generated
  chunk is O(records) and runs on workers under the lock. Acceptable while edit
  counts are modest; if profiling shows it hurts chunk streaming, add a
  `chunk(cx,cz) -> records` index so apply is O(records in chunk) and the locked
  section shrinks.

- **Multiplayer reconnect.** Persistence is server-authoritative and orthogonal
  to reconnect: a reconnecting client receives current world state via the
  normal stream (it re-meshes chunks from its own client-side worldgen) plus
  `PKT_BLOCK_CHANGE` broadcasts for live edits. Risk: a reconnecting (or newly
  joining) client does **not** automatically learn about edits that happened
  before it connected, because the client regenerates terrain locally from seed
  and only persisted *server* state knows the deltas. This already exists today
  for in-session edits a late joiner missed. Persistence makes it visible across
  restarts. Mitigation (future, out of scope here): on join, the server could
  send the overlay records covering the client's load radius as a burst of
  `PKT_BLOCK_CHANGE` (or a new bulk packet). For v1, document the limitation;
  the *server's* mob world and the save file are correct, which is the goal of
  this issue.

- **`world_set_block` best-effort vs durable record.** Because
  `world_set_block` can return false for unloaded/meshing chunks
  (`src/world.c:905-917`), the live world may briefly diverge from the overlay.
  This is safe: the overlay is the durable truth and is re-applied at regen, and
  the chunk will reflect the edit once it is (re)generated. Document that the
  overlay write is unconditional and the world write is best-effort.

---

## Critical Files for Implementation

- /var/home/samu/mine-vibe/src/server.c
- /var/home/samu/mine-vibe/src/world.c
- /var/home/samu/mine-vibe/src/net.h
- /var/home/samu/mine-vibe/src/chunk_map.c
- /var/home/samu/mine-vibe/CMakeLists.txt
