# Lighting Foundation Design

**Status:** Draft — 2026-04-27

**Goal:** Replace the current N·L sun shading with a proper voxel lighting system: per-block skylight propagation, ambient occlusion, and smooth (4-corner-averaged) per-vertex lighting. After this lands, caves are dark, overhangs cast soft shadow, and blocks have realistic edge falloff. This spec is sub-project 1 of three planned lighting specs.

## Roadmap context

This spec is the **foundation** for full Minecraft-style lighting. Two follow-up specs build on it:

| # | Spec | Adds |
|---|------|------|
| 1 | **This spec — Lighting Foundation** | Skylight propagation, AO, smooth lighting, relight on block change |
| 2 | Block light + emitting blocks | Torches/glowstone using the same propagation engine on a second nibble |
| 3 | Day/night cycle | Sun color/direction over time, sky color, twilight tinting |

The storage layout (`[sky:4][block:4]` packed nibble) and propagation engine introduced here are designed to be reused by spec 2 with no re-layout.

## Architecture

```
chunk lifecycle:
  UNLOADED → GENERATING → GENERATED → LIGHTING → LIT → MESHING → READY
                                       ▲          │
                                       │          ▼
                              LightUpdate jobs (block change relight)
```

A new lighting worker stage sits between generation and meshing. It computes per-block skylight and writes it into `Chunk.lights`. Mesher consumes `lights`, samples 4-corner averages for smooth lighting, and computes per-vertex AO from neighbor occupancy.

The mesher's old per-face N·L shading is removed. Face shading now emerges naturally from the skylight gradient across the 4 corner samples per vertex.

The vertex format keeps its 24-byte size — one of the existing pad bytes becomes a `light` byte. Shader is rewritten to read this value instead of computing N·L from sun direction.

## Components

### `chunk.h` / `chunk.c` — light storage

Add a new lazy array on each chunk:

```c
typedef struct Chunk {
    ...
    uint8_t* lights;            // CHUNK_BLOCKS bytes; lazily allocated; NULL if unused
    uint16_t pending_delta_count;
    uint16_t pending_delta_cap;
    BoundaryDelta* pending_deltas;  // queue from neighbors; consumed on next lighting pass
} Chunk;

typedef struct BoundaryDelta {
    uint8_t  face;       // 0=+X, 1=-X, 2=+Z, 3=-Z (horizontal only)
    uint8_t  axis_coord; // 0..15 along the boundary's other horizontal axis
    uint16_t y;          // 0..CHUNK_Y-1
    uint8_t  new_light;  // packed sky+block nibble
} BoundaryDelta;
```

Helpers (mirror the `meta` accessor pattern):

```c
void    chunk_ensure_lights(Chunk* c);     // calloc on first write
uint8_t chunk_get_skylight(const Chunk*, int x, int y, int z);   // 0-15
uint8_t chunk_get_blocklight(const Chunk*, int x, int y, int z); // 0-15 (always 0 in spec 1)
void    chunk_set_skylight(Chunk*, int x, int y, int z, uint8_t v);
void    chunk_set_blocklight(Chunk*, int x, int y, int z, uint8_t v); // for spec 2
```

Encoding: byte = `(block << 4) | sky`, both nibbles 0-15.

`chunk_destroy` frees `lights` along with `meta` and the mesh.

Memory cost: 64 KiB per allocated chunk on top of the existing 64 KiB blocks + lazy 64 KiB meta.

### `block.h` / `block.c` — light absorption metadata

Extend `BlockDef` with two fields:

```c
typedef struct BlockDef {
    const char* name;
    bool        is_solid;
    bool        is_transparent;
    bool        is_gravity;
    uint8_t     light_absorb;   // 0 = transmits fully (air), 15 = blocks all light
    uint8_t     light_emit;     // 0 in spec 1; spec 2 fills this in
    uint8_t     tex_top;
    uint8_t     tex_side;
    uint8_t     tex_bottom;
} BlockDef;
```

Initial values:
- `BLOCK_AIR`: absorb=0, emit=0
- `BLOCK_LEAVES`: absorb=2, emit=0  (lets some light through)
- `BLOCK_WATER`: absorb=2, emit=0   (same — gentle dimming under water)
- All other solid blocks: absorb=15, emit=0
- `BLOCK_BEDROCK`: absorb=15, emit=0

The propagation rule uses `cost = max(1, light_absorb)` so a fully transmissive block (`absorb=0`) still costs 1 distance per step. This matches Minecraft's "light decreases by 1 per block in air."

### New module: `src/lighting.h` / `src/lighting.c`

Pure C, no Vulkan dependency. Operates on `Chunk` + `ChunkNeighbors`.

**Public API:**

```c
typedef struct LightingNeighbors {
    Chunk*  neg_x;
    Chunk*  pos_x;
    Chunk*  neg_z;
    Chunk*  pos_z;
} LightingNeighbors;

// Initial pass for a freshly generated chunk. Requires neighbors to be
// at least GENERATED so we can read their block data; updates only this
// chunk's lights array. Records boundary deltas onto neighbors for them
// to pick up next time they re-light.
void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb);

// Block-change relight. Called when a block at (x,y,z) changed from
// old_id to new_id. Runs removal-BFS + addition-BFS as needed within
// this chunk and queues neighbor updates via boundary deltas.
void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id);

// Drains pending boundary deltas accumulated by neighbors and runs
// targeted BFS to consume them. Called by the worker when re-lighting
// a chunk that has pending deltas.
void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb);
```

**Internals:**

- `static void sky_column_pass(Chunk* c)` — for each (x,z) in 16×16, walk y from `CHUNK_Y-1` down. While `light_absorb == 0`, set sky=15. On first absorbing block, decrement by `max(1, absorb)`; once 0, fill remaining cells with 0.
- `static void horizontal_bfs(Chunk* c, const LightingNeighbors* nb)` — queue every cell whose sky differs from at least one of its 6 neighbors, then BFS-relax: `nb_sky = max(nb_sky, here_sky - max(1, nb_absorb))`. Bounded to 15 levels (since max sky = 15).
- Pending boundary deltas are stored as a small ring buffer on each chunk: `BoundaryDelta { face, axis_coord, y, new_light }`. Rebuilt each lighting pass; consumed lazily.

Cross-chunk propagation: rather than recursing into a neighbor's `lights` array directly, BFS records the boundary value on the neighbor's pending-delta queue. The neighbor's lighting worker picks it up next round. Convergence: typically 1 round; pathological structures spanning many chunks may need 2-3.

### `src/mesher.c` — smooth lighting + real AO

Replace the hardcoded `ao = {3,3,3,3}` with computation. For each face-corner vertex:

```c
// AO: standard 3-block check (side1, side2, corner on the air side of the face)
uint8_t ao_value(bool side1, bool side2, bool corner) {
    if (side1 && side2) return 0;
    return 3 - (side1 + side2 + corner);   // 0..3
}

// Smooth light: average non-zero light values from 4 blocks meeting at corner
uint8_t corner_light(uint8_t side1, uint8_t side2, uint8_t corner, uint8_t face_block) {
    uint8_t s[4]   = { side1, side2, corner, face_block };
    int     sum    = 0;
    int     count  = 0;
    for (int i = 0; i < 4; i++) if (s[i] > 0) { sum += s[i]; count++; }
    return count == 0 ? 0 : (uint8_t)(sum / count);
}
```

`face_block` is the block on the air side adjacent to the face (where light is coming from). `side1`, `side2`, `corner` are the three other blocks meeting at this vertex on the air side.

Reads neighbor light through `ChunkNeighbors`, extended with parallel light slices:

```c
typedef struct ChunkNeighbors {
    const BlockID* pos_x;        // existing
    const BlockID* neg_x;
    const BlockID* pos_z;
    const BlockID* neg_z;
    const uint8_t* pos_x_lights; // new — packed sky+block nibbles, same shape
    const uint8_t* neg_x_lights;
    const uint8_t* pos_z_lights;
    const uint8_t* neg_z_lights;
} ChunkNeighbors;
```

Boundary slices have the same `[z * CHUNK_Y + y]` / `[x * CHUNK_Y + y]` layout as the existing block slices. `mesher_extract_boundary` gets a sibling that extracts a light slice. NULL slices (neighbor not yet lit) cause the mesher to treat the neighbor cell as fully sky-lit (sky=15) — the chunk will be re-meshed when the neighbor lights and pushes a boundary delta back.

### Vertex format and shaders

`src/vertex.h` — repurpose one pad byte:

```c
typedef struct BlockVertex {
    float    pos[3];    // 12
    float    uv[2];     // 8
    uint8_t  normal;    // 1
    uint8_t  ao;        // 1  (0..3)
    uint8_t  light;     // 1  (0..15, sky channel only in spec 1)
    uint8_t  _pad;      // 1  (reserved for spec 2 block-light if needed)
} BlockVertex;
_Static_assert(sizeof(BlockVertex) == 24, "BlockVertex must be 24 bytes");
```

Add the new attribute (`location = 4`, `R8_UINT`, offset 22) to `vertex_attr_descs` and bump the array size from 4 to 5.

`shaders/block.vert`:

```glsl
layout(location = 3) in uint in_ao;
layout(location = 4) in uint in_light;

layout(location = 1) out float frag_light;
layout(location = 2) out float frag_ao;

void main() {
    gl_Position = ubo.proj * ubo.view * vec4(in_pos + pc.chunk_offset.xyz, 1.0);
    frag_uv    = in_uv;
    frag_light = float(in_light) / 15.0;
    frag_ao    = float(in_ao) / 3.0;
}
```

The N·L computation and the `NORMALS[6]` table are removed.

`shaders/block.frag`:

```glsl
const float MIN_BRIGHT = 0.08;

void main() {
    vec4 tex_color = texture(tex_atlas, frag_uv);
    if (tex_color.a < 0.5) discard;

    float sky       = max(frag_light, MIN_BRIGHT);
    float ao_factor = 0.4 + 0.6 * frag_ao;
    vec3  lit       = tex_color.rgb * sky * ubo.sun_color.rgb * ao_factor;
    out_color       = vec4(lit, 1.0);
}
```

`MIN_BRIGHT = 0.08` is the cave-darkness floor — keeps caves navigable until spec 2 introduces torches. Spec 2 replaces it with `max(sky, blocklight)`.

`ubo.sun_direction` is no longer used by the shader but stays in the UBO for spec 3 (day/night will modulate `sun_color` and may tint `sun_direction` ranges for shadow direction).

### Lighting worker

A new chunk state and a new job kind plug into the existing chunk-worker pool. The job pool already manages mesh jobs; we add a parallel "lighting job" channel.

States added:
```c
typedef enum ChunkState {
    CHUNK_UNLOADED = 0,
    CHUNK_GENERATING,
    CHUNK_GENERATED,
    CHUNK_LIGHTING,    // new
    CHUNK_LIT,         // new
    CHUNK_MESHING,
    CHUNK_READY,
} ChunkState;
```

State machine on the chunk-management thread:

- `GENERATED` + 4 horizontal neighbors at `≥ GENERATED` → submit `LightingJob`, transition to `LIGHTING`.
- `LightingJob` calls `lighting_initial_pass` and (if there were pending deltas) `lighting_consume_pending`. Transitions chunk to `LIT`.
- `LIT` + neighbors at `≥ LIT` → submit `MeshJob`, transition to `MESHING`.
- `READY` chunk receiving a `LightUpdate` (block change): re-runs partial relight via `lighting_on_block_changed`; on completion, marks `needs_remesh` so the existing remesh path picks it up.

Block changes never roll the state back; the relight is incremental.

### Block-change relight integration

The existing `world_set_block(World*, x, y, z, id)` is the sole entry point for runtime block writes (already used by `block_physics.c`, agent commands, server packet handlers). We extend it: when the write succeeds and the chunk is past `LIT`, enqueue a `LightUpdate{cx, cz, x, y, z, old_id, new_id}` job. The job runs `lighting_on_block_changed` on the lighting worker and marks the chunk(s) `needs_remesh` on completion.

Worldgen-driven writes (initial chunk generation) bypass `world_set_block` — they write directly into the chunk's `blocks` array before the chunk transitions to `GENERATED`, so the initial lighting pass is the only thing that lights them.

Block-physics (water flow) goes through `world_set_block` and so triggers relight on every fluid update. Water's `light_absorb = 2` means most flow updates produce small light changes; if profiling later shows this is hot, coalescing similar updates in a tick window can be added without API changes.

## Data flow

```
Chunk loaded (server packet or worldgen)
   │
   └──> CHUNK_GENERATED
          │
          └──> [waits for 4 neighbors at ≥ GENERATED]
                 │
                 └──> LightingJob (lighting.c)
                        - sky_column_pass()
                        - horizontal_bfs()
                        - records boundary deltas on neighbors
                 │
                 └──> CHUNK_LIT
                        │
                        └──> [waits for neighbors at ≥ LIT]
                               │
                               └──> MeshJob (mesher.c)
                                      - per-face: AO + smooth corner light
                                      - emits BlockVertex with light + ao
                               │
                               └──> CHUNK_READY (uploads to GPU)

Block change (place/break)
   │
   └──> world_set_block_with_relight()
          - chunk_set_block()
          - lighting_on_block_changed()  ── may dirty neighbors
          - mark needs_remesh
          - remesh runs as it does today
```

## Testing strategy

`tests/test_lighting.c` (new), TDD-style, no Vulkan:

1. **Sky column pass on flat ground.** Empty chunk → all cells at sky=15. Single 1×1 stone pillar from y=64 to y=128 → cells directly under it are 0, all other columns are 15.
2. **Horizontal BFS basic.** Bury a single sky=15 cell with opaque blocks except one open neighbor — that neighbor receives sky=14. Two-step path → 13. Etc.
3. **Cross-chunk seam.** Two chunks, one fully open, one with a wall at x=0; verify light value at chunk-boundary cells matches expected falloff.
4. **Block change — place opaque.** Pre-lit chunk; place opaque at sky-exposed cell; verify column below goes dark and BFS re-runs leaving sides correct.
5. **Block change — break opaque.** Pre-lit chunk with a roof; break a roof block; verify cell + neighbors re-light.
6. **AO 8-config table.** All 2³ = 8 combinations of (side1, side2, corner) → expected `ao_value` output.
7. **Smooth light averaging.** Construct a known 2×2 light field at a corner; verify averaged vertex light.
8. **Light absorption.** Leaves (`absorb=2`) → light drops by 2 per step inside leaves; air (`absorb=0`) → drops by 1 (the `max(1, absorb)` rule).

Tests run under the existing `ctest` setup; no new build infrastructure needed.

## Visible behavior changes after this spec

- Caves are dark (floored at `MIN_BRIGHT = 0.08`).
- Overhangs cast soft shadows from skylight falloff.
- Block edges have AO darkening where blocks meet at corners.
- Faces gradient softly between vertices instead of being flat-shaded per face.
- The fixed sun-direction face brightness is gone — face brightness now reflects skylight visibility.

## Risks and mitigations

| Risk | Mitigation |
|------|-----------|
| Chunk-load lighting too slow (65k cells × BFS) | Sky column pass is O(blocks); BFS only seeds at horizontal differences. Most open-sky cells never enter the queue. Worker runs off the main thread. |
| Cross-chunk convergence requires multiple rounds | Initial BFS extends 1 cell into neighbors so the common case settles immediately. Pending deltas drain on subsequent re-lights. |
| Block-change BFS pathological cases (e.g. break a key block in a deep cave) | BFS bounded to 15 levels by the light range. Worst case touches ~15³ = 3,375 cells; bounded and runs on the worker. |
| Vertex format change breaks existing meshes | All chunk meshes get re-built when the new client launches; no on-disk persistence yet to migrate. |
| Memory doubles per chunk | 64 KiB extra per loaded chunk. Render-distance is the cost driver; no change there. |

## Out of scope (for this spec)

- Block-light sources (torches, glowstone) — **spec 2**.
- Day/night sun color animation, sky color, fog — **spec 3**.
- Coloured lights, sub-block lights, slabs/stairs partial occluders — not planned.
- Server-side lighting computation — client-only; lighting is purely visual today and gameplay does not yet depend on it.
- Persistence of computed light to disk — chunks aren't persisted yet; light is computed on load.

## File map

| File | Action | Purpose |
|------|--------|---------|
| `src/lighting.h` | Create | Public API: `lighting_initial_pass`, `lighting_on_block_changed`, `lighting_consume_pending` |
| `src/lighting.c` | Create | Sky column pass, horizontal BFS, removal/addition BFS, boundary delta queue |
| `src/chunk.h` / `chunk.c` | Modify | Add `lights` field, accessors, `chunk_ensure_lights`, free on destroy |
| `src/chunk_map.h` / `chunk_map.c` | Modify | Lighting worker job submission, new states `LIGHTING`/`LIT`, neighbor-readiness checks |
| `src/block.h` / `block.c` | Modify | `BlockDef.light_absorb`, `light_emit`; update `BLOCK_DEFS` table |
| `src/mesher.h` / `mesher.c` | Modify | Replace AO=`{3,3,3,3}` stub with computation; add smooth corner-light sampling; read neighbor `lights` |
| `src/vertex.h` | Modify | Add `light` byte to `BlockVertex`, add attribute descriptor at location 4 |
| `src/world.h` / `world.c` | Modify | Extend `world_set_block` to enqueue `LightUpdate` jobs after successful writes; worldgen path unchanged |
| `shaders/block.vert` | Modify | Read `in_light`, drop N·L, set `frag_light = light / 15.0` |
| `shaders/block.frag` | Modify | Apply `MIN_BRIGHT` floor; replace flat per-face shading; keep AO and `sun_color` modulation |
| `tests/test_lighting.c` | Create | Sky pass, BFS, cross-chunk, block-change, AO, smooth light, absorption |
| `CMakeLists.txt` | Modify | Add `lighting.c` to library, add `test_lighting` test target |

## Open questions

None at spec time. Tuning constants (`MIN_BRIGHT`, leaf/water absorb values) are left as constants in code and can be adjusted during implementation review.
