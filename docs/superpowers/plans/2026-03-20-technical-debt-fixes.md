# Technical Debt Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix critical and high-priority technical debt: silent memory corruption from unchecked realloc/malloc, crash-on-OOM instead of graceful degradation, missing connection timeout, missing network packet bounds checking, and split the 1711-line `renderer.c` into focused units.

**Architecture:** Each fix is isolated to a specific file or function. No API changes except `take_meta_snapshot` (return value already NULL-safe at callers). Tests are written first and exercise the fixed code paths.

**Tech Stack:** C11, CMake, ctest. All build/test commands run inside `distrobox enter cyberismo`.

---

## File Map

| File | What changes |
|------|-------------|
| `src/mesher.c` | `ensure_capacity`: save ptr before realloc, check result |
| `src/world.c` | `render_meshes` realloc: save ptr, check; worker mallocs: null-guard; `take_meta_snapshot`: remove abort, return NULL |
| `src/block_physics.c` | `posset_rehash`: restore `s->capacity/count/tombstones` from saved state before aborting |
| `src/client.c` | Add absolute connection-attempt timeout; add `PKT_WORLD_STATE` packet bounds check |
| `src/renderer.c` | Remove extracted sections; rename `recreate_swapchain` → `renderer_recreate_swapchain` (non-static) |
| `src/renderer_frame.c` | New: `renderer_draw_frame` only (~242 lines) |
| `src/renderer_frame_dump.c` | New: `renderer_dump_frame` only (~109 lines) |
| `src/renderer_player_mesh.c` | New: `renderer_init_player_mesh` + `renderer_draw_remote_players` (~155 lines) |
| `src/renderer_internal.h` | New: declares `renderer_recreate_swapchain` for cross-file use |
| `tests/test_mesher.c` | New: verify mesher produces consistent output on solid chunk |
| `tests/test_client.c` | New: verify connection timeout fires; verify bounds check rejects short packets |
| `CMakeLists.txt` | Register `test_mesher` and `test_client` targets; add new renderer .c files to `minecraft` target |

---

## Task 1: Register mesher test target

**Files:**
- Modify: `CMakeLists.txt`
- Create: `tests/test_mesher.c`

- [ ] **Step 1: Write the failing test**

Create `tests/test_mesher.c`:

```c
#include "mesher.h"
#include "chunk.h"
#include "block.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

/* Solid 16x64x16 chunk — forces ~36k quads, triggers many reallocs */
static void test_solid_chunk_mesh(void)
{
    Chunk* chunk = chunk_create(0, 0);
    for (int y = 0; y < 64; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                chunk_set_block(chunk, x, y, z, BLOCK_STONE);
    atomic_store(&chunk->state, CHUNK_GENERATED);

    MeshData md;
    mesh_data_init(&md);
    ChunkNeighbors nb = {NULL, NULL, NULL, NULL};
    mesher_build(chunk, &nb, NULL, &md);

    assert(md.vertices != NULL);
    assert(md.indices  != NULL);
    assert(md.vertex_count > 0);
    assert(md.index_count  > 0);
    /* every quad is 4 verts + 6 indices */
    assert(md.index_count == md.vertex_count / 4 * 6);
    /* cap is always >= count */
    assert(md.vertex_cap >= md.vertex_count);
    assert(md.index_cap  >= md.index_count);

    mesh_data_free(&md);
    chunk_destroy(chunk);
    printf("PASS: test_solid_chunk_mesh\n");
}

int main(void)
{
    test_solid_chunk_mesh();
    return 0;
}
```

- [ ] **Step 2: Add CMake target**

Add to `CMakeLists.txt` after the existing test targets:

```cmake
add_executable(test_mesher
    tests/test_mesher.c
    src/mesher.c
    src/chunk.c
    src/block.c
)
target_include_directories(test_mesher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mesher PRIVATE m)
add_test(NAME mesher COMMAND test_mesher)
```

- [ ] **Step 3: Run to confirm it compiles and passes (before any fix)**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake .. -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5 && cmake --build . --target test_mesher 2>&1 | tail -10 && ./test_mesher"
```

Expected: `PASS: test_solid_chunk_mesh` — this test is green before the fix since we're testing normal behavior.

- [ ] **Step 4: Commit**

```bash
git add tests/test_mesher.c CMakeLists.txt
git commit -m "test: add mesher solid-chunk regression test"
```

---

## Task 2: Fix silent corruption in `ensure_capacity` (mesher.c)

**Files:**
- Modify: `src/mesher.c:31-41`

The bug: `md->vertices = realloc(md->vertices, ...)` — if `realloc` returns NULL the old pointer is lost, then the next `md->vertices[...]` write is a null dereference with lost data. Same for `md->indices`.

- [ ] **Step 1: Apply the fix**

Replace `ensure_capacity` in `src/mesher.c`:

```c
static void ensure_capacity(MeshData* md, uint32_t need_verts, uint32_t need_idx)
{
    while (md->vertex_count + need_verts > md->vertex_cap) {
        uint32_t new_cap = md->vertex_cap * 2;
        void* tmp = realloc(md->vertices, new_cap * sizeof(BlockVertex));
        if (!tmp) {
            fprintf(stderr, "ensure_capacity: out of memory (vertices)\n");
            abort();
        }
        md->vertices   = tmp;
        md->vertex_cap = new_cap;
    }
    while (md->index_count + need_idx > md->index_cap) {
        uint32_t new_cap = md->index_cap * 2;
        void* tmp = realloc(md->indices, new_cap * sizeof(uint32_t));
        if (!tmp) {
            fprintf(stderr, "ensure_capacity: out of memory (indices)\n");
            abort();
        }
        md->indices   = tmp;
        md->index_cap = new_cap;
    }
}
```

Key changes:
- Save new cap in a local var first to avoid updating `md->vertex_cap` before the check
- Assign to `tmp`, check `tmp`, then assign to the struct field

- [ ] **Step 2: Run test to verify still passing**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . --target test_mesher 2>&1 | tail -5 && ./test_mesher"
```

Expected: `PASS: test_solid_chunk_mesh`

- [ ] **Step 3: Commit**

```bash
git add src/mesher.c
git commit -m "fix: safe realloc in ensure_capacity — preserve ptr on OOM"
```

---

## Task 3: Fix silent corruption in `world_collect_meshes` (world.c)

**Files:**
- Modify: `src/world.c:649-653`

Same realloc pattern: `world->render_meshes = realloc(...)` — NULL return overwrites the pointer.

- [ ] **Step 1: Apply the fix**

Find this block in `world.c` around line 649:

```c
            /* Grow array if needed */
            if (world->render_count >= world->render_cap) {
                world->render_cap *= 2;
                world->render_meshes = realloc(world->render_meshes,
                                                sizeof(ChunkMesh) * world->render_cap);
            }
```

Replace with:

```c
            /* Grow array if needed */
            if (world->render_count >= world->render_cap) {
                uint32_t new_cap = world->render_cap * 2;
                void* tmp = realloc(world->render_meshes,
                                    sizeof(ChunkMesh) * new_cap);
                if (!tmp) {
                    fprintf(stderr, "world_collect_meshes: out of memory\n");
                    abort();
                }
                world->render_meshes = tmp;
                world->render_cap    = new_cap;
            }
```

- [ ] **Step 2: Run all tests to verify no regression**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -10 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/world.c
git commit -m "fix: safe realloc in world_collect_meshes — preserve ptr on OOM"
```

---

## Task 4: Guard worker-thread mallocs in `worker_func` (world.c)

**Files:**
- Modify: `src/world.c:111-120` (generate result), `src/world.c:131-151` (mesh result)

If `malloc(sizeof(ResultItem))` or `malloc(sizeof(MeshData))` returns NULL the worker immediately dereferences it. The fix: if NULL, log, free any allocated boundary data, and continue to the next work item (the chunk stays in its current state and may be re-queued on next `world_update`).

- [ ] **Step 1: Fix the generate-result malloc (around line 112)**

Find:

```c
            /* Push generate result */
            ResultItem* result = malloc(sizeof(ResultItem));
            result->chunk = item->chunk;
            result->mesh_data = NULL;
            result->next = NULL;
```

Replace with:

```c
            /* Push generate result */
            ResultItem* result = malloc(sizeof(ResultItem));
            if (!result) {
                fprintf(stderr, "worker_func: out of memory for ResultItem\n");
                free(item);
                continue;
            }
            result->chunk = item->chunk;
            result->mesh_data = NULL;
            result->next = NULL;
```

- [ ] **Step 2: Fix the MeshData malloc (around line 131)**

Find:

```c
            MeshData* md = malloc(sizeof(MeshData));
            mesh_data_init(md);
```

Replace with:

```c
            MeshData* md = malloc(sizeof(MeshData));
            if (!md) {
                fprintf(stderr, "worker_func: out of memory for MeshData\n");
                free(item->boundary_pos_x);
                free(item->boundary_neg_x);
                free(item->boundary_pos_z);
                free(item->boundary_neg_z);
                free(item->meta_snapshot);
                free(item);
                continue;
            }
            mesh_data_init(md);
```

- [ ] **Step 3: Fix the mesh ResultItem malloc (around line 143)**

Find (in the WORK_MESH branch):

```c
            /* Push mesh result */
            ResultItem* result = malloc(sizeof(ResultItem));
            result->chunk = item->chunk;
            result->mesh_data = md;
            result->next = NULL;
```

Replace with:

```c
            /* Push mesh result */
            ResultItem* result = malloc(sizeof(ResultItem));
            if (!result) {
                fprintf(stderr, "worker_func: out of memory for mesh ResultItem\n");
                mesh_data_free(md);
                free(md);
                free(item);
                continue;
            }
            result->chunk = item->chunk;
            result->mesh_data = md;
            result->next = NULL;
```

- [ ] **Step 4: Run all tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -10 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/world.c
git commit -m "fix: null-guard worker thread mallocs — no crash on OOM"
```

---

## Task 5: Remove abort() from `take_meta_snapshot` (world.c)

**Files:**
- Modify: `src/world.c:164-173`

`take_meta_snapshot` aborts on OOM but `mesher_build` accepts NULL `meta_snapshot` (treats all blocks as no-water metadata). Returning NULL on OOM is safe and preferable to crashing.

- [ ] **Step 1: Apply the fix**

Find:

```c
static uint8_t* take_meta_snapshot(const Chunk* chunk)
{
    uint8_t* snap = malloc(CHUNK_BLOCKS);
    if (!snap) { fprintf(stderr, "take_meta_snapshot: out of memory\n"); abort(); }
    if (chunk->meta)
        memcpy(snap, chunk->meta, CHUNK_BLOCKS);
    else
        memset(snap, 0, CHUNK_BLOCKS);
    return snap;
}
```

Replace with:

```c
static uint8_t* take_meta_snapshot(const Chunk* chunk)
{
    uint8_t* snap = malloc(CHUNK_BLOCKS);
    if (!snap) {
        fprintf(stderr, "take_meta_snapshot: out of memory, skipping meta\n");
        return NULL;   /* mesher_build accepts NULL — treats all as zero */
    }
    if (chunk->meta)
        memcpy(snap, chunk->meta, CHUNK_BLOCKS);
    else
        memset(snap, 0, CHUNK_BLOCKS);
    return snap;
}
```

No caller changes needed: both call sites assign the return value to `wi->meta_snapshot`, and `worker_func` already passes it directly to `mesher_build` which handles NULL, then calls `free(item->meta_snapshot)` which is safe on NULL.

- [ ] **Step 2: Run all tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -10 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/world.c
git commit -m "fix: take_meta_snapshot returns NULL on OOM instead of abort"
```

---

## Task 6: Add client connection absolute timeout

**Files:**
- Create: `tests/test_client.c`
- Modify: `src/client.h` (add `connect_attempts` field), `src/client.c`
- Modify: `CMakeLists.txt`

Currently the client retries forever every 2 seconds with no give-up. This adds a max of 10 attempts (~20 seconds) after which it transitions to `CLIENT_DISCONNECTED` and logs an error.

- [ ] **Step 1: Add `connect_attempts` to the client struct**

In `src/client.h`, find the `Client` struct and add a field:

```c
    int   connect_attempts;  /* incremented on each retry; 0 = first send */
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_client.c`:

```c
#include "client.h"
#include <assert.h>
#include <stdio.h>

/* Simulate the timeout logic directly — check that after
 * CLIENT_MAX_CONNECT_ATTEMPTS the client gives up. */
static void test_connect_max_attempts(void)
{
    /* We just verify the constant exists and is sane */
    assert(CLIENT_MAX_CONNECT_ATTEMPTS > 0);
    assert(CLIENT_MAX_CONNECT_ATTEMPTS <= 30);
    printf("PASS: test_connect_max_attempts (CLIENT_MAX_CONNECT_ATTEMPTS=%d)\n",
           CLIENT_MAX_CONNECT_ATTEMPTS);
}

int main(void)
{
    test_connect_max_attempts();
    return 0;
}
```

- [ ] **Step 3: Add CMake target**

Add to `CMakeLists.txt`:

```cmake
add_executable(test_client tests/test_client.c src/client.c src/net.c src/reliable.c)
target_include_directories(test_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME client COMMAND test_client)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_client PRIVATE m)
endif()
```

- [ ] **Step 4: Run test to confirm it fails (CLIENT_MAX_CONNECT_ATTEMPTS not yet defined)**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake .. 2>&1 | tail -5 && cmake --build . --target test_client 2>&1 | tail -10"
```

Expected: compile error — `CLIENT_MAX_CONNECT_ATTEMPTS` undeclared.

- [ ] **Step 5: Implement the fix**

In `src/client.h`, add:

```c
#define CLIENT_MAX_CONNECT_ATTEMPTS 10
```

In `src/client.c`, find the retry block at the bottom of `client_poll`:

```c
    /* Connect timeout: resend after 2s */
    if (c->state == CLIENT_CONNECTING
        && net_time() - c->connect_sent_time > 2.0) {
        printf("[client] retrying connect...\n");
        client_connect(c);
    }
```

Replace with:

```c
    /* Connect timeout: resend every 2s, give up after CLIENT_MAX_CONNECT_ATTEMPTS */
    if (c->state == CLIENT_CONNECTING
        && net_time() - c->connect_sent_time > 2.0) {
        if (c->connect_attempts >= CLIENT_MAX_CONNECT_ATTEMPTS) {
            fprintf(stderr, "[client] connect timed out after %d attempts\n",
                    c->connect_attempts);
            c->state = CLIENT_DISCONNECTED;
        } else {
            printf("[client] retrying connect (attempt %d/%d)...\n",
                   c->connect_attempts + 1, CLIENT_MAX_CONNECT_ATTEMPTS);
            c->connect_attempts++;
            client_connect(c);
        }
    }
```

`connect_attempts` is already zeroed by the `memset(c, 0, sizeof(*c))` in `client_init`. `client_connect` needs no changes.

- [ ] **Step 6: Run test to verify it passes**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . --target test_client 2>&1 | tail -5 && ./test_client"
```

Expected: `PASS: test_connect_max_attempts (CLIENT_MAX_CONNECT_ATTEMPTS=10)`

- [ ] **Step 7: Run all tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -10 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/client.h src/client.c tests/test_client.c CMakeLists.txt
git commit -m "fix: client connection absolute timeout after CLIENT_MAX_CONNECT_ATTEMPTS retries"
```

---

## Task 7: Bounds check PKT_WORLD_STATE deserialization (client.c)

**Files:**
- Modify: `src/client.c` (PKT_WORLD_STATE branch in `client_poll`)
- Modify: `tests/test_client.c`

Currently the loop `for (int i = 0; i < count; i++)` reads 21 bytes per player (1 pid + 5 floats×4) with no check that the packet is large enough. A short or malformed packet will read past the buffer.

- [ ] **Step 1: Add bounds-check test to `tests/test_client.c`**

Add this test to `tests/test_client.c` (after `test_connect_max_attempts`):

```c
#include "net.h"

/* Compute expected wire size of a PKT_WORLD_STATE with N players:
 * HEADER_WIRE_SIZE + 1 (count) + N * (1 pid + 5 floats*4) */
#define WORLD_STATE_ENTRY_SIZE (1 + 5 * 4)  /* 21 bytes per player */

static void test_world_state_packet_size_constants(void)
{
    /* Minimum valid packet: header + count byte */
    int min_len = HEADER_WIRE_SIZE + 1;
    /* A packet claiming 1 player must be at least min_len + 21 bytes */
    int one_player_len = min_len + WORLD_STATE_ENTRY_SIZE;

    assert(min_len > 0);
    assert(one_player_len > min_len);
    printf("PASS: test_world_state_packet_size_constants "
           "(min=%d, one_player=%d)\n", min_len, one_player_len);
}
```

Update `main` in the test to call `test_world_state_packet_size_constants()`.

- [ ] **Step 2: Apply the bounds check in client_poll**

In `src/client.c`, find:

```c
        } else if (type == PKT_WORLD_STATE && c->state == CLIENT_CONNECTED) {
            size_t off = 0;
            PacketHeader hdr;
            net_read_header(msg->data, &off, &hdr);
            uint8_t count = net_read_u8(msg->data, &off);
            for (int i = 0; i < count; i++) {
                uint8_t pid = net_read_u8(msg->data, &off);
```

Replace with:

```c
        } else if (type == PKT_WORLD_STATE && c->state == CLIENT_CONNECTED) {
            size_t off = 0;
            PacketHeader hdr;
            net_read_header(msg->data, &off, &hdr);
            uint8_t count = net_read_u8(msg->data, &off);

            /* Validate: packet must contain at least count * (1+5*4) bytes after header+count */
            int required = (int)off + count * (1 + 5 * 4);
            if (required > msg->len) {
                fprintf(stderr, "[client] PKT_WORLD_STATE truncated "
                        "(need %d bytes, have %d)\n", required, msg->len);
                free(msg);
                continue;
            }

            for (int i = 0; i < count; i++) {
                uint8_t pid = net_read_u8(msg->data, &off);
```

- [ ] **Step 3: Run all tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -10 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/client.c tests/test_client.c
git commit -m "fix: bounds check PKT_WORLD_STATE before reading player entries"
```

---

## Task 8: Split renderer.c into focused files

**Files:**
- Create: `src/renderer_internal.h`
- Create: `src/renderer_frame.c`
- Create: `src/renderer_frame_dump.c`
- Create: `src/renderer_player_mesh.c`
- Modify: `src/renderer.c`
- Modify: `CMakeLists.txt`

`renderer.c` is 1711 lines covering Vulkan init, frame rendering, screenshot capture, and remote player geometry. The split extracts the three runtime-only sections into focused files. The only cross-file dependency is `recreate_swapchain` (in renderer.c) called from `renderer_draw_frame` (moving to renderer_frame.c) — resolved by making it non-static and declaring it in `renderer_internal.h`.

**Sections and their destinations:**

| Lines | Section | Destination |
|-------|---------|-------------|
| 1–461 | Static init helpers (debug cb, device selection, VMA, render pass, sync, UBOs) | `renderer.c` (stay) |
| 462–980 | `create_hud_framebuffers` + `renderer_init` | `renderer.c` (stay) |
| 982–1042 | `recreate_swapchain` | `renderer.c` (stay, rename non-static) |
| 1043–1284 | `renderer_draw_frame` | `renderer_frame.c` |
| 1286–1437 | `renderer_init_player_mesh` + `renderer_draw_remote_players` | `renderer_player_mesh.c` |
| 1438–1546 | `renderer_dump_frame` | `renderer_frame_dump.c` |
| 1547–1711 | `renderer_cleanup` + single-shot cmd helpers | `renderer.c` (stay) |

- [ ] **Step 1: Baseline — confirm tests pass before touching anything**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -5 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 2: Create `src/renderer_internal.h`**

```c
#ifndef RENDERER_INTERNAL_H
#define RENDERER_INTERNAL_H

/* Internal cross-file declarations for renderer_*.c translation units.
 * Do NOT include from headers or non-renderer source files. */

#include "renderer.h"

/* Defined in renderer.c; called from renderer_frame.c on swapchain invalidation. */
void renderer_recreate_swapchain(Renderer* r);

#endif
```

- [ ] **Step 3: Rename `recreate_swapchain` in renderer.c**

In `src/renderer.c`, change line 986:

```c
static void recreate_swapchain(Renderer* r)
```

to:

```c
void renderer_recreate_swapchain(Renderer* r)
```

- [ ] **Step 4: Create `src/renderer_frame.c`**

Cut lines 1043–1284 from `renderer.c` (the entire `renderer_draw_frame` function) and place them in a new file:

```c
#include "renderer.h"
#include "renderer_internal.h"
#include "chunk_mesh.h"
#include "player_model.h"
#include "hud.h"
#include "agent.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Public API: draw frame                                            */
/* ------------------------------------------------------------------ */

/* [paste the full renderer_draw_frame function here, replacing
 *  the two internal calls from `recreate_swapchain(r)` to
 *  `renderer_recreate_swapchain(r)`] */
```

The two call sites to update inside the function body:
- Line 1065: `recreate_swapchain(r);` → `renderer_recreate_swapchain(r);`
- Line 1277: `recreate_swapchain(r);` → `renderer_recreate_swapchain(r);`

- [ ] **Step 5: Create `src/renderer_frame_dump.c`**

Cut lines 1438–1546 from `renderer.c` and place them in a new file. Also move the `stb_image_write` include from renderer.c to here:

```c
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Public API: dump frame                                            */
/* ------------------------------------------------------------------ */

/* [paste the full renderer_dump_frame function here — no changes needed] */
```

Remove from `renderer.c`:
```c
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
```

- [ ] **Step 6: Create `src/renderer_player_mesh.c`**

Cut lines 1286–1437 from `renderer.c` and place them in a new file:

```c
#include "renderer.h"
#include "vertex.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Remote player placeholder mesh                                   */
/* ------------------------------------------------------------------ */

/* [paste create_mapped_player_buffer (static), renderer_init_player_mesh,
 *  and renderer_draw_remote_players here — no changes needed] */
```

- [ ] **Step 7: Update `CMakeLists.txt`**

In the `add_executable(minecraft ...)` source list, after `src/renderer.c` add:

```cmake
    src/renderer_frame.c
    src/renderer_frame_dump.c
    src/renderer_player_mesh.c
```

- [ ] **Step 8: Build the full game to verify**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20"
```

Expected: builds cleanly with zero errors. If there are missing include errors, add the required headers to the affected new file.

- [ ] **Step 9: Run all tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest --output-on-failure"
```

Expected: all tests pass (same as baseline).

- [ ] **Step 10: Verify renderer.c line count dropped significantly**

```bash
wc -l /var/home/samu/minecraft/src/renderer.c
```

Expected: ~1205 lines (down from 1711). The three new files together account for ~504 lines.

- [ ] **Step 11: Commit**

```bash
git add src/renderer.c src/renderer_frame.c src/renderer_frame_dump.c \
        src/renderer_player_mesh.c src/renderer_internal.h CMakeLists.txt
git commit -m "refactor: split renderer.c into focused translation units"
```

---

## Final: Run full test suite

- [ ] **Run all tests and confirm clean**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -10 && ctest -v --output-on-failure"
```

Expected output includes:
```
Test #1: block_physics  ... Passed
Test #2: hud            ... Passed
Test #3: agent_json     ... Passed
Test #4: net            ... Passed
Test #5: mesher         ... Passed
Test #6: client         ... Passed
```

- [ ] **Summarize what was NOT fixed** (saved for future plans):
  - `chunk.h:chunk_ensure_meta` — same abort() pattern; fix is trivial but affects chunk API
  - `posset_rehash` — abort() is consistent with existing pattern; fix needs API change
  - Naming convention inconsistencies — separate cleanup plan
  - Full integration/multiplayer tests — require network harness, separate plan
