# HUD + Inventory + Block Edits Design

**Date:** 2026-04-27
**Status:** Approved

## Overview

Make the existing hotbar a real inventory and let the player break and place blocks. The 6 hotbar slots become `{BlockID, count}` stacks. Left-click breaks the targeted block and adds it to inventory; right-click consumes one from the selected slot and places it on the targeted face. The server is authoritative for both world edits and inventory; clients send intent packets, the server validates (reach + replaceability + inventory) and broadcasts results. The agent protocol gains break/place commands and surfaces inventory and the current raycast target in the snapshot.

The full inventory grid (27 storage slots, drag-and-drop) is explicitly deferred — this spec covers only the 6-slot hotbar.

---

## Architecture

| File | Action | Purpose |
|------|--------|---------|
| `src/inventory.h` / `.c` | Create | `InventorySlot {BlockID, count}` and `Inventory` struct, `inventory_add` / `inventory_consume`. Shared by client and server. |
| `src/raycast.h` / `.c` | Create | Voxel DDA from camera. Returns `{hit, x, y, z, face}`. |
| `src/ui/hud.h` / `.c` | Modify | Drop `slot_blocks[]`; render block icons + count text from an `Inventory*`. New `hud_build_target` for the world-space outline. |
| `src/ui/ui.c` / `.h` | Modify | Bake 6 isometric block icons into the UI atlas at init. New `ui_block_icon(BlockID, x, y, size)` primitive. |
| `src/server.c` / `.h` | Modify | Per-player `Inventory`. Handlers for `PKT_BLOCK_BREAK` / `PKT_BLOCK_PLACE`. Broadcasts `PKT_BLOCK_CHANGE` and sends `PKT_INVENTORY` to the affected player. |
| `src/client.c` / `.h` | Modify | Send break/place packets. Apply incoming `PKT_BLOCK_CHANGE` (with chunk re-mesh) and `PKT_INVENTORY`. |
| `src/net.h` | Modify | New packet types and (de)serializers; `PKT_BLOCK_CHANGE` becomes real. |
| `src/agent.h` / `.c` | Modify | `CMD_BREAK`, `CMD_PLACE`, `CMD_GIVE` (debug). Inventory + target in snapshot. |
| `src/main.c` | Modify | Mouse button callback → raycast → send packet. Each frame: raycast for outline rendering. |
| `src/renderer.h` / `.c` | Modify | New small "block outline" pipeline (alpha-blended, depth-tested) drawn inside the world renderpass. |
| `shaders/outline.vert` / `.frag` | Create | Plain-color world-space line/quad shader for the outline. |
| `tests/test_inventory.c` | Create | Stacking + edge cases. |
| `tests/test_raycast.c` | Create | Hit/miss across axes, max-distance termination. |
| `CMakeLists.txt` | Modify | Add new sources, shaders, test targets. |

---

## Data model

### Inventory (`src/inventory.h`)

```c
#define INVENTORY_SLOTS     HUD_SLOT_COUNT    /* 6 */
#define INVENTORY_STACK_MAX 64

typedef struct {
    BlockID block;     /* BLOCK_AIR for empty slot */
    uint8_t count;     /* 0..INVENTORY_STACK_MAX */
} InventorySlot;

typedef struct {
    InventorySlot slots[INVENTORY_SLOTS];
    int           selected;                 /* 0..INVENTORY_SLOTS-1 */
} Inventory;

void    inventory_init(Inventory*);
/* Adds `count` of `block`. Returns leftover count that didn't fit (0 = all picked up).
 * Strategy: fill matching non-full stacks first, then first empty slot, drop the rest. */
uint8_t inventory_add(Inventory*, BlockID block, uint8_t count);
/* Decrement slot[selected] by 1. Returns true on success, false if slot was empty.
 * If count reaches 0, sets block back to BLOCK_AIR. */
bool    inventory_consume(Inventory*, int slot);
```

A slot is "empty" when `block == BLOCK_AIR` (or, equivalently, `count == 0` — the two are kept in sync by `inventory_consume`). The HUD treats `count == 0` as empty.

### Raycast (`src/raycast.h`)

```c
typedef enum {
    FACE_NX, FACE_PX, FACE_NY, FACE_PY, FACE_NZ, FACE_PZ
} BlockFace;

typedef struct {
    bool      hit;
    int       x, y, z;     /* hit cell, valid only if hit */
    BlockFace face;        /* face of the hit cell that was crossed */
} RaycastHit;

/* Voxel DDA against the world. max_dist in blocks. Returns first non-air cell. */
RaycastHit raycast_voxel(const World* w, vec3 origin, vec3 dir, float max_dist);

/* Adjacent cell on the given face (used for placement). */
void block_face_offset(BlockFace, int* dx, int* dy, int* dz);
```

`MAX_REACH = 6.0f` lives in a single header (likely `inventory.h` or a new `gameplay.h`) and is used by both the client (to limit the raycast) and the server (to validate).

### HUD changes

`HUD.slot_blocks[]` is removed. `HUD` keeps only `selected` mirroring (or, simpler, the HUD just borrows a `const Inventory*` per frame). Concretely:

```c
/* New shape */
typedef struct {
    /* nothing — HUD is now stateless. */
} HUD;

void hud_init(HUD*);                         /* keeps the no-op for symmetry */
void hud_build(const Inventory*, float sw, float sh);
void hud_build_target(const RaycastHit*, vec3 cam_pos, mat4 view_proj);
BlockID hud_selected_block(const Inventory*);
```

If keeping `HUD` as a struct adds nothing, we can delete it and call inventory functions directly. The decision is left to the implementer once they touch the call sites — the spec only requires that `Inventory` is the source of truth for which blocks the player has.

---

## Wire protocol (`src/net.h`)

Four new packets, all reliable (use the existing `reliable.c` channel):

| Type | Direction | Payload |
|------|-----------|---------|
| `PKT_BLOCK_BREAK`  | client → server | `i32 x, i32 y, i32 z`                                  |
| `PKT_BLOCK_PLACE`  | client → server | `i32 x, i32 y, i32 z, u8 face, u8 slot`                |
| `PKT_BLOCK_CHANGE` | server → all    | `i32 x, i32 y, i32 z, u8 block`                        |
| `PKT_INVENTORY`    | server → one    | `u8 slot_count, then per slot: u8 block, u8 count`     |

Payload sizes (excluding the existing per-packet `PacketHeader` with `player_id`): BREAK=12, PLACE=14, CHANGE=13, INVENTORY=13 (1 slot count byte + 6×2 slot bytes). All well under MTU. `PKT_BLOCK_CHANGE` was already reserved in `net.h`; this spec gives it a concrete payload.

`PKT_INVENTORY` is a full snapshot (all 6 slots) on every change. Simpler than deltas, and 13 payload bytes is fine for an event that happens at most a few times per second per player.

---

## Server logic (`src/server.c`)

The `Server` struct gains `Inventory inventories[MAX_PLAYERS]`, initialised empty (`BLOCK_AIR, 0` in every slot) on `PKT_CONNECT_REQUEST`.

### Break handler

```
on PKT_BLOCK_BREAK from pid (x,y,z):
    if dist(players[pid].pos, (x+0.5, y+0.5, z+0.5)) > MAX_REACH: drop
    BlockID old = world_get_block(x,y,z)
    if old == BLOCK_AIR or old == BLOCK_WATER or old == BLOCK_BEDROCK: drop
    world_set_block(x,y,z, BLOCK_AIR)
    inventory_add(&inventories[pid], old, 1)
    broadcast PKT_BLOCK_CHANGE(x,y,z, BLOCK_AIR)
    send    PKT_INVENTORY to pid
```

### Place handler

```
on PKT_BLOCK_PLACE from pid (x,y,z, face, slot):
    if slot out of range: drop
    let target_cell = (x,y,z) + block_face_offset(face)
    if dist(players[pid].pos, target_cell + 0.5) > MAX_REACH: drop
    if world_get_block(target_cell) is not in {BLOCK_AIR, BLOCK_WATER}: drop
    if inventories[pid].slots[slot].count == 0: drop
    BlockID b = inventories[pid].slots[slot].block
    if AABB(target_cell) intersects AABB(players[pid]): drop  /* don't trap yourself */
    world_set_block(target_cell, b)
    inventory_consume(&inventories[pid], slot)
    broadcast PKT_BLOCK_CHANGE(target_cell, b)
    send    PKT_INVENTORY to pid
```

"Drop" means the server silently ignores the request — no error packet. Bad clients self-correct because the server's next `PKT_INVENTORY` / `PKT_WORLD_STATE` reflects truth.

### CMD_GIVE (agent only)

`CMD_GIVE {slot, block, count}` calls `inventory_add` directly (or, if `slot` is specified, overwrites that slot) and sends `PKT_INVENTORY` if a real client occupies that pid. In single-player + agent mode the local server owns the agent's inventory directly.

---

## Client logic

### Input → packet

Mouse button callback (registered alongside the existing scroll handler, only when not in agent mode):

```c
on left  press: r = raycast_voxel(...); if (r.hit) send PKT_BLOCK_BREAK(r.x,r.y,r.z);
on right press: r = raycast_voxel(...); if (r.hit)
                send PKT_BLOCK_PLACE(r.x,r.y,r.z, r.face, inventory.selected);
```

The client does *not* update its world or inventory locally — it waits for the server's `PKT_BLOCK_CHANGE` / `PKT_INVENTORY`. This avoids prediction/rollback complexity at the cost of one round-trip of perceived latency. Acceptable for LAN; revisit if it ever feels bad on real networks.

### Applying server packets

- `PKT_BLOCK_CHANGE` → `world_set_block(x,y,z, block)` → mark the affected chunk(s) for re-mesh. The mesher already supports incremental remeshing per chunk; this spec doesn't change that.
- `PKT_INVENTORY` → overwrite `client->inventory` wholesale.

### Per-frame raycast for outline

Each frame, before `renderer_draw_frame`, the client raycasts from the camera and stores the result for HUD/outline rendering. Pure visual — no packets sent.

---

## HUD rendering

### Block icon baking (`ui.c`)

At `ui_init`, after the font atlas is baked, bake one isometric icon per `BlockID` used in this game (stone, dirt, grass, sand, wood, leaves — 6 total, plus expansion room) into the same UI atlas:

- Icon size: 32×32 px, drawn into a reserved strip of the atlas at known UVs.
- For each block: render 3 parallelograms (top, front-right, front-left faces) sampling the corresponding faces of the block's texture (which is already in `assets_generated.c`). Apply per-face shading: top 1.0, right 0.8, left 0.6 (matches the world's directional shading).
- Store icon UVs in a `static UiBlockIcon icons[BLOCK_COUNT]` lookup.

New primitive:
```c
void ui_block_icon(BlockID id, float x, float y, float size);
```
Just emits a textured quad sampling the icon's atlas region — same vertex format as `ui_text`.

### Hotbar layout

Existing slot frames + selection highlight stay. Inside each slot:
1. If `count > 0`: `ui_block_icon(slot.block, sx + 4, sy + 4, SLOT_SIZE - 8)`
2. If `count > 1`: `ui_text(<count>, sx + SLOT_SIZE - text_w - 3, sy + SLOT_SIZE - 14, white)` (small numeric label, bottom-right of slot)

Empty slots draw the slot frame only — no icon, no text.

### Target outline

`hud_build_target(&hit, cam_pos, view_proj)` emits 12 line-quad edges (each ~2 px thick in world units, scaled by distance) outlining the targeted block. Drawn in the **world** renderpass via a new tiny pipeline:

- Vertex format: `vec3 pos`, `vec4 color`. Pushed each frame to a small persistent VMA buffer.
- Pipeline: same renderpass as the world, depth-test on, depth-write off, alpha blend.
- ~96 vertices/frame (12 edges × 8 verts) — negligible.

Drawn before the UI pass so the crosshair stays on top.

---

## Agent protocol changes (`src/agent.h`, `agent.c`)

### New commands

```c
typedef enum {
    /* ...existing... */
    CMD_BREAK,
    CMD_PLACE,
    CMD_GIVE,
} AgentCommandType;

union {
    /* ...existing... */
    struct { int x, y, z; }                                  break_;
    struct { int x, y, z; uint8_t face; }                    place;
    struct { uint8_t slot; uint8_t block; uint8_t count; }   give;
};
```

JSON wire format:
```
{"cmd":"break","x":12,"y":34,"z":-5}
{"cmd":"place","x":12,"y":34,"z":-5,"face":3}
{"cmd":"give","slot":0,"block":1,"count":64}
```

`face` is `0..5` matching `BlockFace`. The parser clamps `face` and `slot` like existing commands.

### Snapshot extensions

```c
InventorySlot inventory[INVENTORY_SLOTS];
struct {
    bool      hit;
    int       x, y, z;
    uint8_t   face;
} target;     /* current per-frame raycast result */
```

JSON:
```json
{ ...,
  "selected_slot": 0,
  "inventory": [{"block":1,"count":12},{"block":0,"count":0}, ...],
  "target":    {"hit":true,"x":12,"y":34,"z":-5,"face":3} }
```

`hotbar` from the previous spec is removed — `inventory` supersedes it. The change is intentional and breaks the agent protocol; downstream agent scripts will need a small update.

---

## Testing

### `tests/test_inventory.c` (no Vulkan)

- Empty inventory: `inventory_consume` returns false, slots remain empty.
- Add 100 stone into an empty inventory: slot 0 = 64, slot 1 = 36, leftover 0.
- Pickup into matching stack: slot 0 has 30 stone, add 10 stone → slot 0 = 40, leftover 0.
- Pickup overflows into next empty slot.
- Full inventory (6 stacks of 64) → adds return full count as leftover.
- `inventory_consume` on a 1-count slot empties it (block resets to AIR).

### `tests/test_raycast.c` (no Vulkan; uses a fixture World)

- Ray straight at a 1×1×1 block hits the expected face on each axis.
- Ray skimming an edge: deterministic face choice (whichever DDA crosses first).
- Ray through air for `max_dist` returns `hit = false`.
- Ray starting inside a block: hit on the first cell.

### Integration smoke (manual, agent mode)

- `CMD_GIVE slot=0 block=1 count=64` → snapshot's `inventory[0]` = `{1, 64}`.
- `CMD_PLACE` at adjacent face → snapshot's `inventory[0].count` = 63, world updated, second client sees the new block via `PKT_BLOCK_CHANGE`.
- `CMD_BREAK` of a placed block → inventory back to 64, second client sees AIR.
- Scroll wheel + number keys (if added) cycle `selected`.
- `CMD_DUMP_FRAME` shows the outline on the targeted block.

### Build test

`minecraft`, `test_inventory`, `test_raycast`, `test_block_physics` all compile clean and link. Existing tests pass unchanged.

---

## Out of scope

Explicitly deferred — do not add to this slice:

- Item entities (broken-block drops on the ground)
- Block-breaking progress / animation / per-block speeds
- Tools (everything breaks instantly with bare hands)
- Full inventory grid (27 storage slots), `E` toggle, drag-and-drop UI
- Crafting / smelting
- Block outline raycast prediction (we accept one RTT of input latency)
- Anti-cheat beyond the reach + replaceability checks
