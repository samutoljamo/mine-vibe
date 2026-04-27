# HUD + Inventory + Block Edits Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the existing 6-slot hotbar into a real inventory and let the player break and place blocks (with multiplayer + agent support).

**Architecture:** Server-authoritative break/place. Clients send `PKT_BLOCK_BREAK` / `PKT_BLOCK_PLACE` (validated for reach + replaceability + inventory), server applies the change to its world, broadcasts `PKT_BLOCK_CHANGE`, and sends `PKT_INVENTORY` to the affected player. The HUD reads from a per-player `Inventory` and renders isometric block icons baked into the existing UI atlas (upgraded R8 → RGBA8 in the process). Targeting uses a client-side voxel DDA whose result also drives a thin world-space outline.

**Tech Stack:** C11, Vulkan via volk, VMA, cglm, GLFW. Existing UI pipeline (`src/ui/ui.c`), reliable UDP channel (`src/reliable.c`), agent JSON protocol (`src/agent.c`).

**Spec:** `docs/superpowers/specs/2026-04-27-hud-inventory-block-edits-design.md`

---

## File Map

| File | Action | Purpose |
|------|--------|---------|
| `src/inventory.h` / `.c` | Create | `InventorySlot`, `Inventory`, `inventory_init/add/consume`. Pure CPU. |
| `src/raycast.h` / `.c` | Create | Voxel DDA + `block_face_offset`. Pure CPU. |
| `src/gameplay.h` | Create | Shared constants — `MAX_REACH`. |
| `src/net.h` | Modify | `net_write_i32` / `net_read_i32`; new packet types and (de)serializers. |
| `src/server.h` / `.c` | Modify | Per-player `Inventory`; break/place handlers; broadcast/send helpers. |
| `src/client.h` / `.c` | Modify | Inventory storage; send break/place; apply incoming block-change/inventory. |
| `src/main.c` | Modify | Mouse callback → raycast → packet; per-frame raycast for outline. |
| `src/ui/ui.h` / `.c` | Modify | Atlas R8 → RGBA8; bake block icons; `ui_block_icon` primitive; fragment shader update. |
| `shaders/ui.frag` | Modify | `out = texture(atlas, uv) * frag_color`. |
| `src/ui/hud.h` / `.c` | Modify | Read from `Inventory`; draw icons + counts; new `hud_build_target`. |
| `src/renderer.h` / `.c` (or new `src/outline.c`) | Modify | Outline pipeline + per-frame draw, in the world renderpass. |
| `shaders/outline.vert` / `.frag` | Create | World-space coloured line shader. |
| `src/agent.h` / `.c` | Modify | `CMD_BREAK` / `CMD_PLACE` / `CMD_GIVE`; snapshot inventory + target. |
| `tests/test_inventory.c` | Create | Stack add/remove edge cases. |
| `tests/test_raycast.c` | Create | DDA hit/miss across axes. |
| `CMakeLists.txt` | Modify | Add new sources, shaders, test targets. |

---

## Task 1: Inventory module (TDD)

**Files:**
- Create: `src/gameplay.h`
- Create: `src/inventory.h`
- Create: `src/inventory.c`
- Create: `tests/test_inventory.c`
- Modify: `CMakeLists.txt` (add `test_inventory` target only)

- [ ] **Step 1.1: Create `src/gameplay.h`**

```c
#ifndef GAMEPLAY_H
#define GAMEPLAY_H

/* Maximum distance (in blocks) the player may break or place from. */
#define MAX_REACH 6.0f

#endif
```

- [ ] **Step 1.2: Create `src/inventory.h`**

```c
#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "block.h"
#include "ui/hud.h"   /* HUD_SLOT_COUNT */

#define INVENTORY_SLOTS     HUD_SLOT_COUNT
#define INVENTORY_STACK_MAX 64

typedef struct {
    BlockID block;     /* BLOCK_AIR when slot is empty */
    uint8_t count;     /* 0..INVENTORY_STACK_MAX */
} InventorySlot;

typedef struct {
    InventorySlot slots[INVENTORY_SLOTS];
    int           selected;                 /* 0..INVENTORY_SLOTS-1 */
} Inventory;

void    inventory_init(Inventory* inv);

/* Add `count` of `block`. Strategy: top up matching non-full stacks first,
 * then fill the first empty slot. Returns leftover count (0 = all picked up). */
uint8_t inventory_add(Inventory* inv, BlockID block, uint8_t count);

/* Decrement slots[slot] by 1. Returns true if a unit was consumed.
 * If count reaches 0, sets block back to BLOCK_AIR. */
bool    inventory_consume(Inventory* inv, int slot);

#endif
```

- [ ] **Step 1.3: Write the failing tests at `tests/test_inventory.c`**

```c
#include "../src/inventory.h"
#include <assert.h>
#include <stdio.h>

static void test_init_is_empty(void) {
    Inventory inv;
    inventory_init(&inv);
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        assert(inv.slots[i].block == BLOCK_AIR);
        assert(inv.slots[i].count == 0);
    }
    assert(inv.selected == 0);
}

static void test_add_into_empty(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].block == BLOCK_STONE);
    assert(inv.slots[0].count == 10);
    assert(inv.slots[1].count == 0);
}

static void test_add_overflows_into_next_slot(void) {
    Inventory inv; inventory_init(&inv);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 100);
    assert(leftover == 0);
    assert(inv.slots[0].count == 64);
    assert(inv.slots[1].count == 36);
    assert(inv.slots[1].block == BLOCK_STONE);
}

static void test_add_tops_up_matching_stack(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 30);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].count == 40);
    assert(inv.slots[1].count == 0);
}

static void test_add_skips_full_matching_stack(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 64);          /* slot 0 full */
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 10);
    assert(leftover == 0);
    assert(inv.slots[0].count == 64);
    assert(inv.slots[1].count == 10);
    assert(inv.slots[1].block == BLOCK_STONE);
}

static void test_add_returns_leftover_when_full(void) {
    Inventory inv; inventory_init(&inv);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        inventory_add(&inv, BLOCK_DIRT, 64);
    uint8_t leftover = inventory_add(&inv, BLOCK_STONE, 5);
    assert(leftover == 5);
    /* No slot mutated by the failed add */
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        assert(inv.slots[i].block == BLOCK_DIRT);
        assert(inv.slots[i].count == 64);
    }
}

static void test_consume_decrements(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 5);
    assert(inventory_consume(&inv, 0));
    assert(inv.slots[0].count == 4);
}

static void test_consume_empties_slot_at_zero(void) {
    Inventory inv; inventory_init(&inv);
    inventory_add(&inv, BLOCK_STONE, 1);
    assert(inventory_consume(&inv, 0));
    assert(inv.slots[0].block == BLOCK_AIR);
    assert(inv.slots[0].count == 0);
}

static void test_consume_empty_returns_false(void) {
    Inventory inv; inventory_init(&inv);
    assert(!inventory_consume(&inv, 0));
}

int main(void) {
    test_init_is_empty();
    test_add_into_empty();
    test_add_overflows_into_next_slot();
    test_add_tops_up_matching_stack();
    test_add_skips_full_matching_stack();
    test_add_returns_leftover_when_full();
    test_consume_decrements();
    test_consume_empties_slot_at_zero();
    test_consume_empty_returns_false();
    printf("test_inventory: all passed\n");
    return 0;
}
```

- [ ] **Step 1.4: Add `test_inventory` target in `CMakeLists.txt`**

Find the existing test target pattern (e.g. `add_executable(test_block_physics ...)`) and add alongside:

```cmake
add_executable(test_inventory
    tests/test_inventory.c
    src/inventory.c
)
target_include_directories(test_inventory PRIVATE src)
add_test(NAME test_inventory COMMAND test_inventory)
```

(Match the exact style and any compile flags used by `test_block_physics`.)

- [ ] **Step 1.5: Run test — expect link failure, then compile failure on inventory.c**

```bash
distrobox enter cyberismo -- cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
distrobox enter cyberismo -- cmake --build build --target test_inventory
```

Expected: build fails with "no such file `src/inventory.c`".

- [ ] **Step 1.6: Create `src/inventory.c`**

```c
#include "inventory.h"
#include <string.h>

void inventory_init(Inventory* inv) {
    memset(inv, 0, sizeof(*inv));   /* BLOCK_AIR == 0, count == 0 */
    inv->selected = 0;
}

uint8_t inventory_add(Inventory* inv, BlockID block, uint8_t count) {
    if (block == BLOCK_AIR || count == 0) return count;

    /* Pass 1: top up matching non-full stacks. */
    for (int i = 0; i < INVENTORY_SLOTS && count > 0; i++) {
        if (inv->slots[i].block != block) continue;
        uint8_t room = (uint8_t)(INVENTORY_STACK_MAX - inv->slots[i].count);
        uint8_t take = count < room ? count : room;
        inv->slots[i].count = (uint8_t)(inv->slots[i].count + take);
        count = (uint8_t)(count - take);
    }

    /* Pass 2: fill empty slots. */
    for (int i = 0; i < INVENTORY_SLOTS && count > 0; i++) {
        if (inv->slots[i].count != 0) continue;
        uint8_t take = count < INVENTORY_STACK_MAX ? count : INVENTORY_STACK_MAX;
        inv->slots[i].block = block;
        inv->slots[i].count = take;
        count = (uint8_t)(count - take);
    }
    return count;
}

bool inventory_consume(Inventory* inv, int slot) {
    if (slot < 0 || slot >= INVENTORY_SLOTS) return false;
    if (inv->slots[slot].count == 0) return false;
    inv->slots[slot].count--;
    if (inv->slots[slot].count == 0) inv->slots[slot].block = BLOCK_AIR;
    return true;
}
```

- [ ] **Step 1.7: Add `src/inventory.c` to the test target's source list (already done in Step 1.4) and rerun**

```bash
distrobox enter cyberismo -- cmake --build build --target test_inventory
distrobox enter cyberismo -- ./build/test_inventory
```

Expected: `test_inventory: all passed`.

- [ ] **Step 1.8: Commit**

```bash
git add src/gameplay.h src/inventory.h src/inventory.c tests/test_inventory.c CMakeLists.txt
git commit -m "feat: inventory module with stack add/consume + tests"
```

---

## Task 2: Raycast module (TDD)

**Files:**
- Create: `src/raycast.h`
- Create: `src/raycast.c`
- Create: `tests/test_raycast.c`
- Modify: `CMakeLists.txt` (add `test_raycast` target only)

- [ ] **Step 2.1: Create `src/raycast.h`**

```c
#ifndef RAYCAST_H
#define RAYCAST_H

#include <stdbool.h>
#include <cglm/cglm.h>
#include "world.h"

typedef enum {
    FACE_NX = 0, FACE_PX,
    FACE_NY,     FACE_PY,
    FACE_NZ,     FACE_PZ
} BlockFace;

typedef struct {
    bool      hit;
    int       x, y, z;     /* hit cell, valid only if hit */
    BlockFace face;        /* face of the hit cell that the ray crossed */
} RaycastHit;

/* Voxel DDA (Amanatides & Woo) against the world. `max_dist` in blocks.
 * Returns the first non-air, non-water cell hit, or {hit=false} otherwise. */
RaycastHit raycast_voxel(const World* world,
                         vec3 origin, vec3 dir, float max_dist);

/* Fills (dx,dy,dz) ∈ {-1,0,1}^3 with the unit offset from a block to the
 * neighbour on the given face. */
void block_face_offset(BlockFace face, int* dx, int* dy, int* dz);

#endif
```

- [ ] **Step 2.2: Write the failing tests at `tests/test_raycast.c`**

The test fixture needs a minimal `World` we can plant blocks in. Use the project's existing `world_create` / `world_set_block` / `world_get_block`. If `world_create` requires a server context, factor a "fixture world" that exposes `world_get_block` over a small chunk array — see how `tests/test_block_physics.c` stubs `world_set_block`. Mirror that approach.

```c
#include "../src/raycast.h"
#include "../src/world.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Minimal world stub matching the symbol signature used by raycast_voxel.
 * If raycast_voxel calls world_get_block, define a fake here. */
static BlockID g_grid[16][16][16];   /* [x][y][z], BLOCK_AIR == 0 */

BlockID world_get_block(World* w, int x, int y, int z) {
    (void)w;
    if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 16)
        return BLOCK_AIR;
    return g_grid[x][y][z];
}

static void clear_grid(void) { memset(g_grid, 0, sizeof(g_grid)); }
static void set_block(int x, int y, int z, BlockID b) { g_grid[x][y][z] = b; }

static void test_face_offset(void) {
    int dx, dy, dz;
    block_face_offset(FACE_PX, &dx, &dy, &dz); assert(dx ==  1 && dy == 0 && dz == 0);
    block_face_offset(FACE_NX, &dx, &dy, &dz); assert(dx == -1 && dy == 0 && dz == 0);
    block_face_offset(FACE_PY, &dx, &dy, &dz); assert(dx ==  0 && dy == 1 && dz == 0);
    block_face_offset(FACE_NY, &dx, &dy, &dz); assert(dx ==  0 && dy ==-1 && dz == 0);
    block_face_offset(FACE_PZ, &dx, &dy, &dz); assert(dx ==  0 && dy == 0 && dz == 1);
    block_face_offset(FACE_NZ, &dx, &dy, &dz); assert(dx ==  0 && dy == 0 && dz ==-1);
}

static void test_hit_along_x(void) {
    clear_grid();
    set_block(5, 0, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 5 && h.y == 0 && h.z == 0);
    assert(h.face == FACE_NX);   /* ray entered from -X side */
}

static void test_hit_along_negative_y(void) {
    clear_grid();
    set_block(0, 3, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 8.5f, 0.5f};
    vec3 dir    = {0.0f, -1.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 0 && h.y == 3 && h.z == 0);
    assert(h.face == FACE_PY);   /* ray entered from +Y (top) */
}

static void test_no_hit_through_air(void) {
    clear_grid();
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 4.0f);
    assert(!h.hit);
}

static void test_max_distance_terminates(void) {
    clear_grid();
    set_block(10, 0, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 5.0f);   /* block is 9.5 away */
    assert(!h.hit);
}

static void test_water_is_not_a_hit(void) {
    clear_grid();
    set_block(5, 0, 0, BLOCK_WATER);
    set_block(7, 0, 0, BLOCK_STONE);
    vec3 origin = {0.5f, 0.5f, 0.5f};
    vec3 dir    = {1.0f, 0.0f, 0.0f};
    RaycastHit h = raycast_voxel(NULL, origin, dir, 10.0f);
    assert(h.hit);
    assert(h.x == 7);   /* skipped water, hit stone */
}

int main(void) {
    test_face_offset();
    test_hit_along_x();
    test_hit_along_negative_y();
    test_no_hit_through_air();
    test_max_distance_terminates();
    test_water_is_not_a_hit();
    printf("test_raycast: all passed\n");
    return 0;
}
```

- [ ] **Step 2.3: Add `test_raycast` target in `CMakeLists.txt`**

```cmake
add_executable(test_raycast
    tests/test_raycast.c
    src/raycast.c
)
target_include_directories(test_raycast PRIVATE src)
target_link_libraries(test_raycast PRIVATE cglm)
add_test(NAME test_raycast COMMAND test_raycast)
```

- [ ] **Step 2.4: Run — expect compile failure on raycast.c**

```bash
distrobox enter cyberismo -- cmake --build build --target test_raycast
```

Expected: missing `src/raycast.c`.

- [ ] **Step 2.5: Implement `src/raycast.c`**

```c
#include "raycast.h"
#include <math.h>

void block_face_offset(BlockFace face, int* dx, int* dy, int* dz) {
    *dx = 0; *dy = 0; *dz = 0;
    switch (face) {
        case FACE_NX: *dx = -1; break;
        case FACE_PX: *dx =  1; break;
        case FACE_NY: *dy = -1; break;
        case FACE_PY: *dy =  1; break;
        case FACE_NZ: *dz = -1; break;
        case FACE_PZ: *dz =  1; break;
    }
}

static int floor_int(float v) { return (int)floorf(v); }

RaycastHit raycast_voxel(const World* world,
                         vec3 origin, vec3 dir, float max_dist) {
    RaycastHit miss = { .hit = false };
    float dx = dir[0], dy = dir[1], dz = dir[2];
    float dlen = sqrtf(dx*dx + dy*dy + dz*dz);
    if (dlen < 1e-6f) return miss;
    dx /= dlen; dy /= dlen; dz /= dlen;

    int   x = floor_int(origin[0]);
    int   y = floor_int(origin[1]);
    int   z = floor_int(origin[2]);
    int   sx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
    int   sy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    int   sz = (dz > 0) ? 1 : (dz < 0 ? -1 : 0);

    /* tDelta: distance along ray to cross one cell on each axis. */
    float tDeltaX = (sx != 0) ? fabsf(1.0f / dx) : INFINITY;
    float tDeltaY = (sy != 0) ? fabsf(1.0f / dy) : INFINITY;
    float tDeltaZ = (sz != 0) ? fabsf(1.0f / dz) : INFINITY;

    /* tMax: distance along ray to first cell boundary on each axis. */
    float nextX = (sx > 0) ? (x + 1.0f) : (float)x;
    float nextY = (sy > 0) ? (y + 1.0f) : (float)y;
    float nextZ = (sz > 0) ? (z + 1.0f) : (float)z;
    float tMaxX = (sx != 0) ? (nextX - origin[0]) / dx : INFINITY;
    float tMaxY = (sy != 0) ? (nextY - origin[1]) / dy : INFINITY;
    float tMaxZ = (sz != 0) ? (nextZ - origin[2]) / dz : INFINITY;

    BlockFace last_face = FACE_NX;   /* arbitrary; updated before first use */
    float t = 0.0f;
    /* First check the starting cell — handles ray-starts-inside-block. */
    BlockID b0 = world_get_block((World*)world, x, y, z);
    if (b0 != BLOCK_AIR && b0 != BLOCK_WATER) {
        return (RaycastHit){ .hit = true, .x = x, .y = y, .z = z, .face = FACE_NY };
    }

    while (t <= max_dist) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += sx; t = tMaxX; tMaxX += tDeltaX;
            last_face = (sx > 0) ? FACE_NX : FACE_PX;
        } else if (tMaxY < tMaxZ) {
            y += sy; t = tMaxY; tMaxY += tDeltaY;
            last_face = (sy > 0) ? FACE_NY : FACE_PY;
        } else {
            z += sz; t = tMaxZ; tMaxZ += tDeltaZ;
            last_face = (sz > 0) ? FACE_NZ : FACE_PZ;
        }
        if (t > max_dist) break;
        BlockID b = world_get_block((World*)world, x, y, z);
        if (b != BLOCK_AIR && b != BLOCK_WATER) {
            return (RaycastHit){ .hit = true, .x = x, .y = y, .z = z, .face = last_face };
        }
    }
    return miss;
}
```

- [ ] **Step 2.6: Build and run**

```bash
distrobox enter cyberismo -- cmake --build build --target test_raycast
distrobox enter cyberismo -- ./build/test_raycast
```

Expected: `test_raycast: all passed`.

- [ ] **Step 2.7: Commit**

```bash
git add src/raycast.h src/raycast.c tests/test_raycast.c CMakeLists.txt
git commit -m "feat: voxel DDA raycast with face detection + tests"
```

---

## Task 3: Network protocol additions

**Files:**
- Modify: `src/net.h`

- [ ] **Step 3.1: Add signed-int helpers to `src/net.h`**

After the existing `net_write_u32` / `net_read_u32`, add:

```c
static inline void net_write_i32(uint8_t* buf, size_t* off, int32_t v) {
    net_write_u32(buf, off, (uint32_t)v);
}
static inline int32_t net_read_i32(const uint8_t* buf, size_t* off) {
    return (int32_t)net_read_u32(buf, off);
}
```

- [ ] **Step 3.2: Add new packet type constants**

Find the `PKT_*` enum in `src/net.h` and replace `PKT_BLOCK_CHANGE = 7,  /* reserved for future use */` with the four real types:

```c
PKT_BLOCK_CHANGE = 7,    /* server → all:    block edit broadcast      */
PKT_BLOCK_BREAK  = 8,    /* client → server: request to break a block  */
PKT_BLOCK_PLACE  = 9,    /* client → server: request to place a block  */
PKT_INVENTORY    = 10,   /* server → one:    full inventory snapshot    */
```

- [ ] **Step 3.3: Add packet structs and (de)serializers**

After the existing `WorldStatePacket` block, add:

```c
typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
} BlockBreakPacket;

typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
    uint8_t      face;
    uint8_t      slot;
} BlockPlacePacket;

typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
    uint8_t      block;
} BlockChangePacket;

#define INVENTORY_NET_SLOTS 6   /* must match INVENTORY_SLOTS */

typedef struct {
    PacketHeader header;
    uint8_t      slot_count;
    struct { uint8_t block; uint8_t count; } slots[INVENTORY_NET_SLOTS];
} InventoryPacket;

static inline size_t net_write_block_break(uint8_t* buf, const BlockBreakPacket* p) {
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    return off;
}
static inline void net_read_block_break(const uint8_t* buf, BlockBreakPacket* p) {
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->x = net_read_i32(buf, &off);
    p->y = net_read_i32(buf, &off);
    p->z = net_read_i32(buf, &off);
}

static inline size_t net_write_block_place(uint8_t* buf, const BlockPlacePacket* p) {
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    net_write_u8(buf, &off, p->face);
    net_write_u8(buf, &off, p->slot);
    return off;
}
static inline void net_read_block_place(const uint8_t* buf, BlockPlacePacket* p) {
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->x    = net_read_i32(buf, &off);
    p->y    = net_read_i32(buf, &off);
    p->z    = net_read_i32(buf, &off);
    p->face = net_read_u8(buf, &off);
    p->slot = net_read_u8(buf, &off);
}

static inline size_t net_write_block_change(uint8_t* buf, const BlockChangePacket* p) {
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    net_write_u8(buf, &off, p->block);
    return off;
}
static inline void net_read_block_change(const uint8_t* buf, BlockChangePacket* p) {
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->x     = net_read_i32(buf, &off);
    p->y     = net_read_i32(buf, &off);
    p->z     = net_read_i32(buf, &off);
    p->block = net_read_u8(buf, &off);
}

static inline size_t net_write_inventory(uint8_t* buf, const InventoryPacket* p) {
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_u8(buf, &off, p->slot_count);
    for (uint8_t i = 0; i < p->slot_count; i++) {
        net_write_u8(buf, &off, p->slots[i].block);
        net_write_u8(buf, &off, p->slots[i].count);
    }
    return off;
}
static inline void net_read_inventory(const uint8_t* buf, InventoryPacket* p) {
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->slot_count = net_read_u8(buf, &off);
    if (p->slot_count > INVENTORY_NET_SLOTS) p->slot_count = INVENTORY_NET_SLOTS;
    for (uint8_t i = 0; i < p->slot_count; i++) {
        p->slots[i].block = net_read_u8(buf, &off);
        p->slots[i].count = net_read_u8(buf, &off);
    }
}
```

- [ ] **Step 3.4: Build to confirm headers still compile**

```bash
distrobox enter cyberismo -- cmake --build build
```

Expected: clean build (no behaviour change yet).

- [ ] **Step 3.5: Commit**

```bash
git add src/net.h
git commit -m "feat: PKT_BLOCK_BREAK/PLACE/CHANGE/INVENTORY wire types"
```

---

## Task 4: Server — per-player inventory + break/place handlers

**Files:**
- Modify: `src/server.h`
- Modify: `src/server.c`

- [ ] **Step 4.1: Add `Inventory` to the server's per-client state**

Open `src/server.h`. Find the per-client struct (look for the array indexed by `player_id` that already holds position/connection data — likely `Server::clients[]`). Add at the top of the file:

```c
#include "../src/inventory.h"   /* path may simplify depending on layout */
```

Add to the per-client struct:

```c
Inventory inventory;
```

(If the server keeps a separate `inventories[MAX_PLAYERS]` array instead, that's fine too — match existing style. The handlers below assume the inventory is reachable as `c->inventory` for client `c`.)

- [ ] **Step 4.2: Initialise inventory on connect**

In `src/server.c`, find the place where a new client is accepted (search for `PKT_CONNECT_ACCEPT`). Before sending the accept packet, call:

```c
inventory_init(&c->inventory);
```

- [ ] **Step 4.3: Add a helper to broadcast `PKT_BLOCK_CHANGE` and send `PKT_INVENTORY`**

At the top of `src/server.c` (with the other static helpers), add:

```c
#include "raycast.h"   /* block_face_offset */

static void server_broadcast_block_change(Server* s, int x, int y, int z, BlockID b) {
    BlockChangePacket p = {
        .header = { .type = PKT_BLOCK_CHANGE, .player_id = 0 },
        .x = x, .y = y, .z = z, .block = (uint8_t)b,
    };
    uint8_t buf[64];
    size_t  len = net_write_block_change(buf, &p);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->connected) continue;
        reliable_send(&c->reliable, s->net->fd, &c->addr, buf, len);
    }
}

static void server_send_inventory(Server* s, ServerClient* c) {
    InventoryPacket p = {
        .header = { .type = PKT_INVENTORY, .player_id = 0 },
        .slot_count = INVENTORY_NET_SLOTS,
    };
    for (int i = 0; i < INVENTORY_NET_SLOTS; i++) {
        p.slots[i].block = (uint8_t)c->inventory.slots[i].block;
        p.slots[i].count = c->inventory.slots[i].count;
    }
    uint8_t buf[64];
    size_t  len = net_write_inventory(buf, &p);
    reliable_send(&c->reliable, s->net->fd, &c->addr, buf, len);
}
```

(Adjust struct/field names — `s->clients`, `c->reliable`, `c->addr`, `s->net->fd` — to match the actual server.)

- [ ] **Step 4.4: Add `handle_block_break`**

```c
static void handle_block_break(Server* s, ServerClient* c, const uint8_t* data, size_t len) {
    if (len < sizeof(PacketHeader) + 12) return;
    BlockBreakPacket p;
    net_read_block_break(data, &p);

    /* Reach check (block centre vs. player position). */
    float cx = p.x + 0.5f, cy = p.y + 0.5f, cz = p.z + 0.5f;
    float ddx = cx - c->pos[0], ddy = cy - c->pos[1], ddz = cz - c->pos[2];
    if (ddx*ddx + ddy*ddy + ddz*ddz > MAX_REACH * MAX_REACH) return;

    BlockID old = world_get_block(s->world, p.x, p.y, p.z);
    if (old == BLOCK_AIR || old == BLOCK_WATER || old == BLOCK_BEDROCK) return;

    if (!world_set_block(s->world, p.x, p.y, p.z, BLOCK_AIR)) return;
    inventory_add(&c->inventory, old, 1);

    server_broadcast_block_change(s, p.x, p.y, p.z, BLOCK_AIR);
    server_send_inventory(s, c);
}
```

(`c->pos` is whatever the server already stores for player position. If it's `c->x, c->y, c->z` separately, adjust.)

- [ ] **Step 4.5: Add `handle_block_place`**

```c
static void handle_block_place(Server* s, ServerClient* c, const uint8_t* data, size_t len) {
    if (len < sizeof(PacketHeader) + 14) return;
    BlockPlacePacket p;
    net_read_block_place(data, &p);
    if (p.face > FACE_PZ || p.slot >= INVENTORY_SLOTS) return;
    if (c->inventory.slots[p.slot].count == 0) return;

    int dx, dy, dz;
    block_face_offset((BlockFace)p.face, &dx, &dy, &dz);
    int tx = p.x + dx, ty = p.y + dy, tz = p.z + dz;

    /* Reach check on the target cell, not the clicked block. */
    float cx = tx + 0.5f, cy = ty + 0.5f, cz = tz + 0.5f;
    float ddx = cx - c->pos[0], ddy = cy - c->pos[1], ddz = cz - c->pos[2];
    if (ddx*ddx + ddy*ddy + ddz*ddz > MAX_REACH * MAX_REACH) return;

    BlockID at = world_get_block(s->world, tx, ty, tz);
    if (at != BLOCK_AIR && at != BLOCK_WATER) return;

    /* Don't trap the placing player inside their own block.
     * Player AABB: width 0.6, height 1.8, feet at (px, py, pz). */
    {
        float pminx = c->pos[0] - 0.3f, pmaxx = c->pos[0] + 0.3f;
        float pminy = c->pos[1],         pmaxy = c->pos[1] + 1.8f;
        float pminz = c->pos[2] - 0.3f, pmaxz = c->pos[2] + 0.3f;
        float bminx = (float)tx,     bmaxx = (float)tx + 1.0f;
        float bminy = (float)ty,     bmaxy = (float)ty + 1.0f;
        float bminz = (float)tz,     bmaxz = (float)tz + 1.0f;
        if (pmaxx > bminx && pminx < bmaxx &&
            pmaxy > bminy && pminy < bmaxy &&
            pmaxz > bminz && pminz < bmaxz) return;
    }

    BlockID b = c->inventory.slots[p.slot].block;
    if (!world_set_block(s->world, tx, ty, tz, b)) return;
    inventory_consume(&c->inventory, p.slot);

    server_broadcast_block_change(s, tx, ty, tz, b);
    server_send_inventory(s, c);
}
```

- [ ] **Step 4.6: Wire the dispatch**

Find the existing dispatch in `src/server.c` (the chain `if (type == PKT_POSITION) handle_position(...) else if (type == PKT_DISCONNECT) ...`). Add:

```c
else if (type == PKT_BLOCK_BREAK) handle_block_break(s, c, msg->data, msg->len);
else if (type == PKT_BLOCK_PLACE) handle_block_place(s, c, msg->data, msg->len);
```

- [ ] **Step 4.7: Build**

```bash
distrobox enter cyberismo -- cmake --build build
```

Expected: clean build. The server now applies and broadcasts edits; no client wires them yet.

- [ ] **Step 4.8: Commit**

```bash
git add src/server.h src/server.c CMakeLists.txt
git commit -m "feat: server-authoritative block break/place + inventory"
```

---

## Task 5: Client — apply incoming block-change + inventory

**Files:**
- Modify: `src/client.h`
- Modify: `src/client.c`

- [ ] **Step 5.1: Add `Inventory` to `Client`**

In `src/client.h`, with the other includes:

```c
#include "inventory.h"
```

Inside the `Client` struct, add:

```c
Inventory inventory;
/* Block-change events the client has received this tick — drained by main.c
 * each frame to call world_set_block + remesh. */
struct {
    int     x, y, z;
    uint8_t block;
} pending_block_changes[64];
int pending_block_change_count;
```

(Why a buffer rather than calling `world_set_block` directly from the network thread? `world_set_block` triggers re-meshing, and meshing isn't thread-safe on the world the renderer is reading. Buffering and applying on the main thread keeps the existing thread model intact.)

- [ ] **Step 5.2: Initialise on connect**

In `client.c`'s init/connect path, call `inventory_init(&c->inventory);` and `c->pending_block_change_count = 0;`.

- [ ] **Step 5.3: Handle incoming `PKT_BLOCK_CHANGE` and `PKT_INVENTORY`**

In the receive loop in `src/client.c` (the `else if (type == PKT_PLAYER_JOIN || ...)` chain), add before the `PKT_DISCONNECT` branch:

```c
else if (type == PKT_BLOCK_CHANGE) {
    BlockChangePacket bp;
    net_read_block_change((const uint8_t*)msg->data, &bp);
    if (c->pending_block_change_count < 64) {
        int i = c->pending_block_change_count++;
        c->pending_block_changes[i].x     = bp.x;
        c->pending_block_changes[i].y     = bp.y;
        c->pending_block_changes[i].z     = bp.z;
        c->pending_block_changes[i].block = bp.block;
    }
} else if (type == PKT_INVENTORY) {
    InventoryPacket ip;
    net_read_inventory((const uint8_t*)msg->data, &ip);
    inventory_init(&c->inventory);
    for (uint8_t i = 0; i < ip.slot_count && i < INVENTORY_SLOTS; i++) {
        c->inventory.slots[i].block = (BlockID)ip.slots[i].block;
        c->inventory.slots[i].count = ip.slots[i].count;
    }
}
```

(`inventory_init` resets `selected` to 0; preserve the previous `selected` if you want — store and restore it. For the first slice it's fine to reset.)

- [ ] **Step 5.4: Drain `pending_block_changes` in `main.c` each frame**

Find the existing per-frame block updating code in `src/main.c` (around `world_update`). After `world_update`, before rendering:

```c
for (int i = 0; i < client->pending_block_change_count; i++) {
    int x = client->pending_block_changes[i].x;
    int y = client->pending_block_changes[i].y;
    int z = client->pending_block_changes[i].z;
    BlockID b = (BlockID)client->pending_block_changes[i].block;
    world_set_block(world, x, y, z, b);
}
client->pending_block_change_count = 0;
```

(`world_set_block` already marks the affected chunk(s) dirty for re-meshing — no extra work.)

- [ ] **Step 5.5: Build**

```bash
distrobox enter cyberismo -- cmake --build build
```

Expected: clean build. Server-driven block edits now propagate to the client world (no input yet to trigger them).

- [ ] **Step 5.6: Commit**

```bash
git add src/client.h src/client.c src/main.c
git commit -m "feat: client applies PKT_BLOCK_CHANGE and PKT_INVENTORY"
```

---

## Task 6: Mouse input → break/place packets

**Files:**
- Modify: `src/main.c`
- Modify: `src/client.h` / `.c` (helpers to send packets)

- [ ] **Step 6.1: Add client-side senders in `client.c`**

Add to `src/client.h`:

```c
void client_send_break(Client* c, int x, int y, int z);
void client_send_place(Client* c, int x, int y, int z, uint8_t face, uint8_t slot);
```

In `src/client.c`:

```c
void client_send_break(Client* c, int x, int y, int z) {
    BlockBreakPacket p = {
        .header = { .type = PKT_BLOCK_BREAK, .player_id = c->player_id },
        .x = x, .y = y, .z = z,
    };
    uint8_t buf[64];
    size_t  len = net_write_block_break(buf, &p);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, len);
}

void client_send_place(Client* c, int x, int y, int z, uint8_t face, uint8_t slot) {
    BlockPlacePacket p = {
        .header = { .type = PKT_BLOCK_PLACE, .player_id = c->player_id },
        .x = x, .y = y, .z = z, .face = face, .slot = slot,
    };
    uint8_t buf[64];
    size_t  len = net_write_block_place(buf, &p);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, len);
}
```

- [ ] **Step 6.2: Add a per-frame raycast result to `main.c`**

Near the other frame-level state at the top of `src/main.c`:

```c
#include "raycast.h"
static RaycastHit g_target;   /* refreshed each frame for outline + click */
```

In the frame loop, after camera update and before input handling:

```c
{
    vec3 origin, dir;
    glm_vec3_copy(g_player.camera.pos, origin);
    glm_vec3_copy(g_player.camera.front, dir);
    g_target = raycast_voxel(world, origin, dir, MAX_REACH);
}
```

(Adjust to the actual camera struct fields — `pos` / `front` / `forward` / etc.)

- [ ] **Step 6.3: Add the mouse button callback**

```c
static void mouse_button_callback(GLFWwindow* w, int button, int action, int mods) {
    (void)w; (void)mods;
    if (action != GLFW_PRESS || agent_mode) return;
    if (!g_target.hit) return;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        client_send_break(client, g_target.x, g_target.y, g_target.z);
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        client_send_place(client, g_target.x, g_target.y, g_target.z,
                          (uint8_t)g_target.face,
                          (uint8_t)client->inventory.selected);
    }
}
```

Register it next to `glfwSetScrollCallback`:

```c
glfwSetMouseButtonCallback(window, mouse_button_callback);
```

(`agent_mode` and `client` symbol names may differ — match existing style. If the existing scroll callback is also gated `if (!agent_mode)`, do the same here.)

- [ ] **Step 6.4: Move slot-selection state from `g_hud` to `client->inventory.selected`**

Add `#include "inventory.h"` to `src/main.c`. Replace the existing scroll callback's mutation of `g_hud.selected_slot` with:

```c
client->inventory.selected =
    (client->inventory.selected + dir + INVENTORY_SLOTS) % INVENTORY_SLOTS;
```

And the `CMD_SELECT_SLOT` handler similarly. The `HUD` struct's `selected_slot` field becomes a no-op (it'll be removed entirely in Task 8).

- [ ] **Step 6.5: Build and run a smoke check**

```bash
distrobox enter cyberismo -- cmake --build build
```

(We can't yet visually verify since the HUD still uses old slot_blocks. Verification comes in Task 8.)

- [ ] **Step 6.6: Commit**

```bash
git add src/client.h src/client.c src/main.c
git commit -m "feat: mouse buttons send break/place; per-frame raycast target"
```

---

## Task 7: UI atlas R8 → RGBA8 + block icon baking

**Files:**
- Modify: `src/ui/ui.c`
- Modify: `src/ui/ui.h`
- Modify: `shaders/ui.frag`

- [ ] **Step 7.1: Update the fragment shader**

Replace `shaders/ui.frag` with:

```glsl
#version 450

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(atlas, frag_uv) * frag_color;
}
```

- [ ] **Step 7.2: Switch `g_atlas_cpu` to RGBA8 and update upload**

In `src/ui/ui.c`:

```c
/* OLD: static uint8_t g_atlas_cpu[ATLAS_W * ATLAS_H]; */
static uint8_t g_atlas_cpu[ATLAS_W * ATLAS_H * 4];   /* RGBA */
```

Find the two `VK_FORMAT_R8_UNORM` references for the atlas image + view (around lines 132 and 215). Change both to `VK_FORMAT_R8G8B8A8_UNORM`. Update any staging buffer size from `ATLAS_W * ATLAS_H` to `ATLAS_W * ATLAS_H * 4` and any `vkCmdCopyBufferToImage` row-pitch bookkeeping.

- [ ] **Step 7.3: Re-bake the font glyphs into RGBA**

After `stbtt_BakeFontBitmap` (which writes 1 byte per pixel into a temporary R8 buffer), expand to RGBA. Replace the in-place baking with:

```c
/* Bake into a temporary R8 buffer first. */
uint8_t r8[ATLAS_W * ATLAS_H];
stbtt_BakeFontBitmap(font_bytes, 0, ATLAS_BAKE_PX,
                     r8, ATLAS_W, ATLAS_H,
                     GLYPH_FIRST, GLYPH_COUNT, g_baked);

/* Splat into RGBA: glyphs get (g, g, g, g); empty pixels stay (0,0,0,0). */
memset(g_atlas_cpu, 0, sizeof(g_atlas_cpu));
for (int i = 0; i < ATLAS_W * ATLAS_H; i++) {
    uint8_t v = r8[i];
    g_atlas_cpu[i*4 + 0] = v;
    g_atlas_cpu[i*4 + 1] = v;
    g_atlas_cpu[i*4 + 2] = v;
    g_atlas_cpu[i*4 + 3] = v;
}
```

- [ ] **Step 7.4: Re-paint the white pixel region as opaque white RGBA**

The existing `ui_rect` samples a "white" 2×2 region near (0,0). After the glyph splat, write `(255,255,255,255)` to those pixels:

```c
for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
    int i = (y * ATLAS_W + x) * 4;
    g_atlas_cpu[i+0] = 255;
    g_atlas_cpu[i+1] = 255;
    g_atlas_cpu[i+2] = 255;
    g_atlas_cpu[i+3] = 255;
}
```

- [ ] **Step 7.5: Reserve an icon strip and bake isometric block icons**

Pick a region of the atlas that the font baker doesn't use. The font region typically occupies the upper portion; reserve the bottom strip. Add to `ui.c`:

```c
#define ICON_PX        32
#define ICON_STRIP_Y0  (ATLAS_H - ICON_PX)   /* one row of icons at the bottom */
#define ICON_COUNT     (BLOCK_COUNT)         /* one per BlockID, AIR slot unused */

typedef struct { float u0, v0, u1, v1; } UiBlockIcon;
static UiBlockIcon g_block_icons[ICON_COUNT];

/* Three faces of an isometric cube projected into a 32x32 cell.
 * The cell is laid out as:
 *   - Top face: parallelogram covering the upper half
 *   - Front-right face: lower-right
 *   - Front-left face:  lower-left
 * Shading factors: top 1.0, right 0.8, left 0.6
 *
 * For each block we sample one representative colour per face from the
 * existing block atlas (assets_generated.c). For the first slice, use a
 * single tint per block (e.g. average of the top-face texture); the
 * iso shape gives the depth illusion.
 */
static void bake_block_icon(int cell_x, int cell_y, BlockID b) {
    uint8_t r, g, bcol;   /* representative colour for this block */
    block_representative_color(b, &r, &g, &bcol);   /* implement as a lookup */

    /* Helpers to write a single RGBA pixel (with shading), bounds-checked. */
    #define PUT(px, py, sh) do { \
        int xx = cell_x + (px), yy = cell_y + (py); \
        if (xx < 0 || xx >= ATLAS_W || yy < 0 || yy >= ATLAS_H) break; \
        int i = (yy * ATLAS_W + xx) * 4; \
        g_atlas_cpu[i+0] = (uint8_t)(r    * (sh)); \
        g_atlas_cpu[i+1] = (uint8_t)(g    * (sh)); \
        g_atlas_cpu[i+2] = (uint8_t)(bcol * (sh)); \
        g_atlas_cpu[i+3] = 255; \
    } while (0)

    /* Iso projection inside a 32x32 cell.
     * Center of cube top at (16, 8), bottom at (16, 24).
     * Top face: rhombus with corners (16,4), (28,12), (16,20), (4,12).
     * Right face: quad (28,12),(28,28),(16,32),(16,20).
     * Left face:  quad (4,12),(16,20),(16,32),(4,28).
     * For simplicity: rasterise each face by scanline. */
    /* Top face — shading 1.0 */
    for (int y = 4; y <= 20; y++) {
        int half = (y <= 12) ? (y - 4) : (20 - y);
        for (int dx = -half * 12 / 8; dx <= half * 12 / 8; dx++) PUT(16 + dx, y, 1.0f);
    }
    /* Right face — shading 0.8 */
    for (int y = 12; y <= 32 && y < 32; y++) {
        int x0 = (y < 20) ? (28 - (y - 12) * 12 / 8) : 16;
        for (int x = x0; x < 32; x++) PUT(x, y, 0.8f);
    }
    /* Left face — shading 0.6 */
    for (int y = 12; y <= 32 && y < 32; y++) {
        int x1 = (y < 20) ? (4 + (y - 12) * 12 / 8) : 16;
        for (int x = 0; x < x1; x++) PUT(x, y, 0.6f);
    }
    #undef PUT
}

static void bake_block_icons(void) {
    for (int b = 0; b < ICON_COUNT; b++) {
        int cell_x = b * ICON_PX;
        if (cell_x + ICON_PX > ATLAS_W) break;   /* cap at strip width */
        bake_block_icon(cell_x, ICON_STRIP_Y0, (BlockID)b);
        g_block_icons[b].u0 =  cell_x              / (float)ATLAS_W;
        g_block_icons[b].v0 =  ICON_STRIP_Y0       / (float)ATLAS_H;
        g_block_icons[b].u1 = (cell_x + ICON_PX)   / (float)ATLAS_W;
        g_block_icons[b].v1 = (ICON_STRIP_Y0 + ICON_PX) / (float)ATLAS_H;
    }
}
```

Call `bake_block_icons()` from `ui_font_bake()` after the glyph + white-pixel writes, before the GPU upload.

- [ ] **Step 7.6: Implement `block_representative_color` (`block.h` / `.c`)**

The simplest version is a hand-tuned lookup. Add to `src/block.h`:

```c
void block_representative_color(BlockID id, uint8_t* r, uint8_t* g, uint8_t* b);
```

In `src/block.c`:

```c
void block_representative_color(BlockID id, uint8_t* r, uint8_t* g, uint8_t* b) {
    switch (id) {
        case BLOCK_STONE:   *r = 130; *g = 130; *b = 130; break;
        case BLOCK_DIRT:    *r = 134; *g =  96; *b =  67; break;
        case BLOCK_GRASS:   *r =  91; *g = 153; *b =  72; break;
        case BLOCK_SAND:    *r = 219; *g = 211; *b = 160; break;
        case BLOCK_WOOD:    *r = 109; *g =  82; *b =  47; break;
        case BLOCK_LEAVES:  *r =  60; *g = 117; *b =  44; break;
        case BLOCK_WATER:   *r =  64; *g = 128; *b = 220; break;
        case BLOCK_BEDROCK: *r =  60; *g =  60; *b =  60; break;
        default:            *r = 255; *g =   0; *b = 255; break;   /* visible bug */
    }
}
```

(A future improvement is to compute these by averaging each block's actual texture in the asset atlas. For this slice the lookup is fine.)

- [ ] **Step 7.7: Add `ui_block_icon` primitive**

In `src/ui/ui.h`:

```c
void ui_block_icon(BlockID id, float x, float y, float size);
```

In `src/ui/ui.c`:

```c
#include "../block.h"

void ui_block_icon(BlockID id, float x, float y, float size) {
    if ((int)id < 0 || (int)id >= ICON_COUNT) return;
    UiBlockIcon* ic = &g_block_icons[id];
    emit_quad(x, y, size, size, ic->u0, ic->v0, ic->u1, ic->v1,
              1.0f, 1.0f, 1.0f, 1.0f);
}
```

- [ ] **Step 7.8: Build and run**

```bash
distrobox enter cyberismo -- cmake --build build
distrobox enter cyberismo -- cmake --build build --target test_ui
distrobox enter cyberismo -- ./build/test_ui
```

Expected: tests pass, `minecraft` builds. The HUD still looks the same (no caller of `ui_block_icon` yet) but the atlas has icons baked.

- [ ] **Step 7.9: Commit**

```bash
git add shaders/ui.frag src/ui/ui.h src/ui/ui.c src/block.h src/block.c
git commit -m "feat: UI atlas RGBA + isometric block icon baking"
```

---

## Task 8: HUD reads inventory + draws icons + counts

**Files:**
- Modify: `src/ui/hud.h`
- Modify: `src/ui/hud.c`
- Modify: `src/main.c` (call sites)
- Modify: `src/agent.h` (compatibility — see step 8.5)

- [ ] **Step 8.1: Replace HUD struct + signatures**

In `src/ui/hud.h`:

```c
#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include "../block.h"

#define HUD_SLOT_COUNT 6

/* Forward declarations to avoid circular includes. */
struct Inventory;
struct RaycastHit;

void    hud_build(const struct Inventory* inv, float sw, float sh);
/* Emits world-space outline geometry into the renderer's outline buffer.
 * No-op if hit is null or hit->hit is false. */
void    hud_build_target(const struct RaycastHit* hit);
BlockID hud_selected_block(const struct Inventory* inv);

#endif
```

The `HUD` struct is gone entirely.

- [ ] **Step 8.2: Rewrite `hud.c`**

```c
#include "hud.h"
#include "ui.h"
#include "../inventory.h"
#include "../raycast.h"
#include <stdio.h>

#define SLOT_SIZE   40
#define SLOT_GAP     4
#define SLOT_BORDER  4
#define CROSSHAIR_W 14
#define CROSSHAIR_T  2

void hud_build(const Inventory* inv, float sw, float sh) {
    /* Crosshair. */
    vec4 white = {1, 1, 1, 0.9f};
    float cx = sw * 0.5f, cy = sh * 0.5f;
    ui_rect(cx - CROSSHAIR_W*0.5f, cy - CROSSHAIR_T*0.5f, CROSSHAIR_W, CROSSHAIR_T, white);
    ui_rect(cx - CROSSHAIR_T*0.5f, cy - CROSSHAIR_W*0.5f, CROSSHAIR_T, CROSSHAIR_W, white);

    /* Hotbar layout. */
    int n = HUD_SLOT_COUNT;
    float total_w = n * SLOT_SIZE + (n - 1) * SLOT_GAP;
    float hx = (sw - total_w) * 0.5f;
    float hy = sh - SLOT_SIZE - 12.0f;

    vec4 fill         = {0.15f, 0.15f, 0.15f, 0.75f};
    vec4 border_sel   = {1.0f,  1.0f,  1.0f,  1.0f};
    vec4 border_unsel = {0.4f,  0.4f,  0.4f,  0.75f};
    vec4 text_white   = {1.0f,  1.0f,  1.0f,  1.0f};

    for (int i = 0; i < n; i++) {
        float sx = hx + i * (SLOT_SIZE + SLOT_GAP);
        vec4* border = (i == inv->selected) ? &border_sel : &border_unsel;
        ui_rect(sx, hy, SLOT_SIZE, SLOT_SIZE, *border);
        ui_rect(sx + SLOT_BORDER, hy + SLOT_BORDER,
                SLOT_SIZE - 2 * SLOT_BORDER, SLOT_SIZE - 2 * SLOT_BORDER, fill);

        const InventorySlot* s = &inv->slots[i];
        if (s->count == 0) continue;

        /* Block icon. */
        ui_block_icon(s->block, sx + 4, hy + 4, SLOT_SIZE - 8);

        /* Count, only if > 1. Right-bottom corner. */
        if (s->count > 1) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", s->count);
            float tw = ui_text_width(buf, 12.0f);
            ui_text(sx + SLOT_SIZE - tw - 3, hy + SLOT_SIZE - 14, 12.0f, buf, text_white);
        }
    }
}

BlockID hud_selected_block(const Inventory* inv) {
    return inv->slots[inv->selected].block;
}

/* Forward to the renderer-owned outline emitter (implemented in Task 9). */
void renderer_outline_emit_block(int x, int y, int z);   /* defined in renderer */

void hud_build_target(const RaycastHit* hit) {
    if (!hit || !hit->hit) return;
    renderer_outline_emit_block(hit->x, hit->y, hit->z);
}
```

- [ ] **Step 8.3: Update `main.c` call sites**

Replace `hud_init(&g_hud);` with nothing (or delete the line). Replace the two `renderer_draw_frame(..., &g_hud, ...)` calls with `&client->inventory` arguments — the renderer signature changes in step 8.4. Replace `hud_selected_block(&g_hud)` with `hud_selected_block(&client->inventory)`. Drop the `static HUD g_hud;` declaration.

- [ ] **Step 8.4: Update `renderer_draw_frame` signature**

In `src/renderer.h`, change the parameter `const HUD* hud` to `const Inventory* inventory`. In `src/renderer_frame.c` (or wherever `hud_build` is called), update the call to `hud_build(inventory, screen_w, screen_h);` and add `hud_build_target(&g_target);` afterwards (the per-frame raycast result from Task 6 needs to be threaded in — easiest is to pass it as another parameter to `renderer_draw_frame`, or expose `g_target` via a pointer in main and read it from the renderer).

Cleanest: extend the signature with `const RaycastHit* target`:

```c
void renderer_draw_frame(Renderer* r,
                         /* ...existing... */,
                         const Inventory* inventory,
                         const RaycastHit* target,
                         bool dump_frame, const char* dump_path);
```

Update both call sites in `main.c` to pass `&g_target`.

- [ ] **Step 8.5: Update agent snapshot temporarily to compile**

`AgentSnapshot::hotbar[HUD_SLOT_COUNT]` will be replaced by `inventory[]` in Task 11. To keep this task compiling: leave the existing `selected_slot` and `hotbar[]` fields in place but stop populating them from `g_hud` (drop the assignments in `main.c`). The agent will emit zeros for `hotbar` until Task 11 — fine, this is an in-progress branch.

- [ ] **Step 8.6: Build and visual smoke test**

```bash
distrobox enter cyberismo -- cmake --build build
```

Run the game (launch it the same way the user does — outside the distrobox per the project rule):

```bash
./build/minecraft
```

Expected:
- HUD renders crosshair + hotbar with all 6 slots empty (no icons, just frames).
- Selecting an empty slot via scroll changes the highlight ring.
- Mouse-clicks send packets but the world is a single-client local server: break works (server applies, broadcasts, client applies, slot fills with the broken block + count rises).
- Right-click consumes a block from the selected slot and places it. Block icon appears in the slot when count ≥ 1; numeric label appears when count ≥ 2.

If the local game uses an in-process server, this is the first end-to-end visible behaviour. Note any visual issues for follow-up.

- [ ] **Step 8.7: Commit**

```bash
git add src/ui/hud.h src/ui/hud.c src/main.c src/renderer.h src/renderer_frame.c
git commit -m "feat: HUD reads from Inventory, renders block icons + stack counts"
```

---

## Task 9: Block outline pipeline + per-frame draw

**Files:**
- Create: `shaders/outline.vert`
- Create: `shaders/outline.frag`
- Modify: `src/renderer.h` / `.c` (or new `src/outline.c` + `outline.h`)
- Modify: `CMakeLists.txt` (compile new shaders)

For brevity: implement as additions to `renderer.c` (or a sibling `outline.c` in the renderer translation-unit family — match how `renderer_player_mesh.c` is split out).

- [ ] **Step 9.1: Create the shaders**

`shaders/outline.vert`:

```glsl
#version 450
layout(set = 0, binding = 0) uniform CamUbo { mat4 view_proj; } cam;
layout(location = 0) in vec3 pos;
void main() { gl_Position = cam.view_proj * vec4(pos, 1.0); }
```

`shaders/outline.frag`:

```glsl
#version 450
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(0.0, 0.0, 0.0, 0.6); }
```

(If the existing world pipeline already uses a per-frame UBO with view_proj, reuse it — fewer descriptor sets is simpler. Otherwise, push-constant the matrix.)

- [ ] **Step 9.2: Add the outline pipeline to the renderer**

In the renderer's struct, add:

```c
VkPipeline       outline_pipeline;
VkPipelineLayout outline_pipeline_layout;
VkBuffer         outline_vb;
VmaAllocation    outline_vb_alloc;
void*            outline_vb_mapped;
uint32_t         outline_vert_count;
```

Pipeline configuration:
- Same renderpass as the world pass.
- Vertex format: `vec3 pos` only.
- Topology: `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`.
- `lineWidth = 2.0f` (require `wideLines` device feature; if not available, fall back to drawing 12 thin quads instead — leave a TODO).
- Depth: test on, write off.
- Blend: standard alpha.

Persistent VMA buffer sized for up to 24 vertices (12 edges × 2 endpoints).

- [ ] **Step 9.3: Implement `renderer_outline_emit_block`**

```c
void renderer_outline_emit_block(int x, int y, int z) {
    /* Ignored if called outside a frame. main.c calls hud_build_target which
     * forwards here — invoked between renderer_frame_begin and the world draw. */
    if (g_renderer.outline_vert_count + 24 > 24) return;
    float pad = 0.002f;   /* tiny inflation to avoid z-fighting */
    float x0 = x - pad,        y0 = y - pad,        z0 = z - pad;
    float x1 = x + 1.0f + pad, y1 = y + 1.0f + pad, z1 = z + 1.0f + pad;

    vec3 e[24] = {
        /* bottom square */
        {x0,y0,z0},{x1,y0,z0},  {x1,y0,z0},{x1,y0,z1},
        {x1,y0,z1},{x0,y0,z1},  {x0,y0,z1},{x0,y0,z0},
        /* top square */
        {x0,y1,z0},{x1,y1,z0},  {x1,y1,z0},{x1,y1,z1},
        {x1,y1,z1},{x0,y1,z1},  {x0,y1,z1},{x0,y1,z0},
        /* verticals */
        {x0,y0,z0},{x0,y1,z0},  {x1,y0,z0},{x1,y1,z0},
        {x1,y0,z1},{x1,y1,z1},  {x0,y0,z1},{x0,y1,z1},
    };
    memcpy((vec3*)g_renderer.outline_vb_mapped + g_renderer.outline_vert_count,
           e, sizeof(e));
    g_renderer.outline_vert_count += 24;
}
```

- [ ] **Step 9.4: Reset outline buffer per frame; draw inside the world pass**

In `renderer_frame_begin` (or wherever per-frame state is reset): `g_renderer.outline_vert_count = 0;`. The HUD's `hud_build_target` (Task 8) populates it before drawing. After the world pass records its existing draws and before `vkCmdEndRenderPass`:

```c
if (renderer->outline_vert_count > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->outline_pipeline);
    /* bind the same view-proj descriptor used by the world pass */
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &renderer->outline_vb, &off);
    vkCmdSetLineWidth(cmd, 2.0f);
    vkCmdDraw(cmd, renderer->outline_vert_count, 1, 0, 0);
}
```

- [ ] **Step 9.5: Compile shaders in `CMakeLists.txt`**

Find the existing shader compilation block (the one that produces `shaders_generated.c`). Add `outline.vert` and `outline.frag` to its input list.

- [ ] **Step 9.6: Build and visual check**

```bash
distrobox enter cyberismo -- cmake --build build
```

Run the game and confirm: a thin black outline tracks whichever block the cursor is aimed at within reach. Disappears when looking at the sky.

- [ ] **Step 9.7: Commit**

```bash
git add shaders/outline.vert shaders/outline.frag src/renderer.h src/renderer.c src/renderer_frame.c CMakeLists.txt
git commit -m "feat: world-space outline on the targeted block"
```

---

## Task 10: Agent CMD_BREAK / CMD_PLACE / CMD_GIVE

**Files:**
- Modify: `src/agent.h`
- Modify: `src/agent.c`
- Modify: `src/main.c` (dispatch)
- Modify: `src/server.c` (CMD_GIVE handler)

- [ ] **Step 10.1: Add command types and union members**

In `src/agent.h`:

```c
typedef enum {
    /* ...existing... */
    CMD_BREAK,
    CMD_PLACE,
    CMD_GIVE,
} AgentCommandType;
```

Add to the union:

```c
struct { int x, y, z; }                                  break_;
struct { int x, y, z; uint8_t face; }                    place;
struct { uint8_t slot; uint8_t block; uint8_t count; }   give;
```

(The trailing underscore on `break_` avoids the C keyword.)

- [ ] **Step 10.2: Parse the new commands in `agent_parse_command`**

Following the existing pattern (one `if (strcmp(cmd_str, "...") == 0)` block per command):

```c
if (strcmp(cmd_str, "break") == 0) {
    out->type = CMD_BREAK;
    out->break_.x = out->break_.y = out->break_.z = 0;
    const char* p;
    if ((p = strstr(line, "\"x\"")))  sscanf(p, "\"x\":%d", &out->break_.x);
    if ((p = strstr(line, "\"y\"")))  sscanf(p, "\"y\":%d", &out->break_.y);
    if ((p = strstr(line, "\"z\"")))  sscanf(p, "\"z\":%d", &out->break_.z);
    return true;
}
if (strcmp(cmd_str, "place") == 0) {
    out->type = CMD_PLACE;
    out->place.x = out->place.y = out->place.z = 0;
    out->place.face = 0;
    const char* p;
    if ((p = strstr(line, "\"x\"")))    sscanf(p, "\"x\":%d", &out->place.x);
    if ((p = strstr(line, "\"y\"")))    sscanf(p, "\"y\":%d", &out->place.y);
    if ((p = strstr(line, "\"z\"")))    sscanf(p, "\"z\":%d", &out->place.z);
    if ((p = strstr(line, "\"face\""))) {
        int f = 0; sscanf(p, "\"face\":%d", &f);
        if (f < 0) f = 0; if (f > 5) f = 5;
        out->place.face = (uint8_t)f;
    }
    return true;
}
if (strcmp(cmd_str, "give") == 0) {
    out->type = CMD_GIVE;
    out->give.slot = out->give.block = out->give.count = 0;
    const char* p;
    int v;
    if ((p = strstr(line, "\"slot\"")))  { sscanf(p, "\"slot\":%d",  &v); out->give.slot  = (uint8_t)v; }
    if ((p = strstr(line, "\"block\""))) { sscanf(p, "\"block\":%d", &v); out->give.block = (uint8_t)v; }
    if ((p = strstr(line, "\"count\""))) { sscanf(p, "\"count\":%d", &v); out->give.count = (uint8_t)v; }
    return true;
}
```

- [ ] **Step 10.3: Dispatch CMD_BREAK / CMD_PLACE in `main.c`**

In the agent command consumer loop in `main.c` (the place that already handles `CMD_SELECT_SLOT`):

```c
else if (cmd->type == CMD_BREAK) {
    client_send_break(client, cmd->break_.x, cmd->break_.y, cmd->break_.z);
}
else if (cmd->type == CMD_PLACE) {
    client_send_place(client, cmd->place.x, cmd->place.y, cmd->place.z,
                      cmd->place.face,
                      (uint8_t)client->inventory.selected);
}
else if (cmd->type == CMD_GIVE) {
    /* Bypass network — local-server agent setups apply to the agent's own inventory. */
    server_apply_give(server, cmd->give.slot, cmd->give.block, cmd->give.count);
}
```

- [ ] **Step 10.4: Implement `server_apply_give`**

In `src/server.h`:

```c
void server_apply_give(Server* s, uint8_t slot, uint8_t block, uint8_t count);
```

In `src/server.c`:

```c
void server_apply_give(Server* s, uint8_t slot, uint8_t block, uint8_t count) {
    if (slot >= INVENTORY_SLOTS) return;
    /* Find the host's client slot — the project's existing convention is the
     * first connected client (player_id == server's own player). Search the
     * clients[] array for the connected entry that matches the in-process
     * client's player_id; if there's only ever one local agent player, the
     * first connected slot is correct. */
    ServerClient* c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s->clients[i].connected) { c = &s->clients[i]; break; }
    }
    if (!c) return;
    c->inventory.slots[slot].block = (BlockID)block;
    c->inventory.slots[slot].count = count;
    server_send_inventory(s, c);
}
```

- [ ] **Step 10.5: Build**

```bash
distrobox enter cyberismo -- cmake --build build
```

Expected: clean build. Existing `test_agent_json` may need updating if it asserts on the parser's command set — read the test, extend it to cover the new commands while you're there.

- [ ] **Step 10.6: Commit**

```bash
git add src/agent.h src/agent.c src/main.c src/server.h src/server.c tests/test_agent_json.c
git commit -m "feat: agent CMD_BREAK/PLACE/GIVE wired to client + server"
```

---

## Task 11: Snapshot extensions — inventory + target

**Files:**
- Modify: `src/agent.h`
- Modify: `src/agent.c`
- Modify: `src/main.c`

- [ ] **Step 11.1: Replace `hotbar[]` with `inventory[]` in `AgentSnapshot`**

In `src/agent.h`:

```c
typedef struct {
    /* ...existing fields... */
    int           selected_slot;
    InventorySlot inventory[INVENTORY_SLOTS];   /* replaces hotbar[] */
    struct {
        bool      hit;
        int       x, y, z;
        uint8_t   face;
    } target;
} AgentSnapshot;
```

(Add `#include "inventory.h"` near the top; same for `RaycastHit` if you decide to embed it directly. The local `target` struct keeps the snapshot self-contained.)

- [ ] **Step 11.2: Update `agent_format_snapshot`**

First, open `src/agent.c` and read the current `agent_format_snapshot` body — keep all of its existing fields (position, yaw, pitch, on_ground, mode, etc.) verbatim. Only the `selected_slot` + `hotbar` tail changes.

Replace the existing tail (the `"\"selected_slot\":%d,\"hotbar\":[%d,%d,%d,%d,%d,%d]}\n"` portion and its corresponding format args) with:

```c
/* Build inventory JSON into a small scratch buffer first. */
char inv_buf[256];
int  inv_off = snprintf(inv_buf, sizeof(inv_buf), "[");
for (int i = 0; i < INVENTORY_SLOTS; i++) {
    inv_off += snprintf(inv_buf + inv_off, sizeof(inv_buf) - inv_off,
                        "%s{\"block\":%d,\"count\":%d}",
                        i == 0 ? "" : ",",
                        snap->inventory[i].block,
                        snap->inventory[i].count);
}
snprintf(inv_buf + inv_off, sizeof(inv_buf) - inv_off, "]");
```

Then change the trailing portion of the existing `snprintf(buf, ...)` from
`,"selected_slot":%d,"hotbar":[%d,%d,%d,%d,%d,%d]}\n`
to
`,"selected_slot":%d,"inventory":%s,"target":{"hit":%s,"x":%d,"y":%d,"z":%d,"face":%d}}\n`
and replace the six `snap->hotbar[…]` args with `inv_buf, snap->target.hit ? "true" : "false", snap->target.x, snap->target.y, snap->target.z, snap->target.face`.

Bump `buf` (currently 512 bytes in `agent_emit_snapshot`) to 1024 — worst case is ~144 bytes for inventory + 60 for target on top of the existing ~200, fine but no reason to be tight.

- [ ] **Step 11.3: Populate the snapshot in `main.c`**

Find the existing snapshot population (where `snap.selected_slot` and `snap.hotbar[i]` are filled). Replace with:

```c
snap.selected_slot = client->inventory.selected;
for (int i = 0; i < INVENTORY_SLOTS; i++) {
    snap.inventory[i] = client->inventory.slots[i];
}
snap.target.hit  = g_target.hit;
snap.target.x    = g_target.x;
snap.target.y    = g_target.y;
snap.target.z    = g_target.z;
snap.target.face = (uint8_t)g_target.face;
```

- [ ] **Step 11.4: Update `tests/test_agent_json.c`**

If the test asserts on snapshot output, replace `hotbar` checks with `inventory` checks and add a `target` assertion.

- [ ] **Step 11.5: Build + run agent unit test**

```bash
distrobox enter cyberismo -- cmake --build build
distrobox enter cyberismo -- ./build/test_agent
```

Expected: pass.

- [ ] **Step 11.6: Commit**

```bash
git add src/agent.h src/agent.c src/main.c tests/test_agent_json.c
git commit -m "feat: agent snapshot now reports inventory + raycast target"
```

---

## Task 12: End-to-end smoke test

**Files:**
- (No new files — exercises the running game.)

- [ ] **Step 12.1: Build everything clean**

```bash
distrobox enter cyberismo -- cmake --build build
distrobox enter cyberismo -- ctest --test-dir build --output-on-failure
```

Expected: all unit tests pass.

- [ ] **Step 12.2: Manual single-client play test**

Run the game on the host (per project rule — distrobox is for build only):

```bash
./build/minecraft
```

Verify:
1. Crosshair visible at screen centre.
2. All 6 hotbar slots empty (frames only).
3. Look at a stone block within ~6 blocks; black wireframe outline appears on it.
4. Look at the sky — outline disappears.
5. Left-click the targeted stone block — it disappears (re-mesh ripples through), slot 0 fills with a stone icon and count "1" (only when ≥ 2).
6. Mine 5 more stone — slot 0 shows count "6", icon stays.
7. Scroll wheel changes selected slot (white border moves).
8. Switch back to slot 0; right-click on the side of a different block — a new stone block appears on that face. Slot 0 count drops to "5".
9. Try to place too far away (> 6 blocks) — nothing happens, no error.
10. Hold all 6 slot icons by mining 6 different block types; verify icons are visually distinguishable.

- [ ] **Step 12.3: Manual two-client play test (multiplayer)**

Start a server + two clients (same process model the project already uses). On client A, mine a block; verify client B's world updates (the broken block disappears in B's view). Verify A's inventory increments and B's does not (B has its own inventory, all empty).

- [ ] **Step 12.4: Agent-mode end-to-end**

Use the existing agent harness (`./build/minecraft --agent ...` or however the project launches it):

```
{"cmd":"give","slot":0,"block":1,"count":64}
{"cmd":"break","x":12,"y":34,"z":-5}
{"cmd":"place","x":12,"y":34,"z":-5,"face":3}
{"cmd":"dump_frame","path":"/tmp/agent_frame.png"}
```

Verify the snapshot before each command:
- After give: `inventory[0] = {block:1, count:64}`.
- After break (assuming the targeted block existed and was in reach): inventory[0].count went up by 1 (or appropriate slot filled).
- After place: inventory[0].count down by 1.
- The frame dump shows the HUD with icons and an outline.

- [ ] **Step 12.5: Final commit (if anything was tweaked during smoke testing)**

If smoke testing surfaced small fixes:

```bash
git add -A
git commit -m "fix: HUD/inventory polish from smoke testing"
```

Otherwise, no commit — the feature is complete.

---

## Out of scope (do NOT implement in this plan)

- Item drops as world entities
- Block-breaking progress / animation / per-block speeds / tools
- Full inventory grid (27 storage slots), drag-and-drop UI, `E` toggle
- Crafting / smelting
- Block-edit prediction on the client (we accept one RTT of latency)
- Anti-cheat beyond reach + replaceability + own-AABB checks
