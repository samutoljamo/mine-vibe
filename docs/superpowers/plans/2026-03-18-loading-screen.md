# Loading Screen Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show a title-bar progress indicator (`"Minecraft | Loading... 45%"`) before the game starts, transitioning to gameplay once 30% of the circular render area has uploaded meshes.

**Architecture:** Single-file change to `src/main.c`. Add a loading loop before the main game loop that drives world generation/meshing and updates the window title each frame. Move timing variable initialization to after the loading loop to avoid a first-frame dt spike.

**Tech Stack:** C11, GLFW (window title), existing `world_get_meshes` / `world_get_render_distance` API.

---

### Task 1: Compute the loading threshold

**Files:**
- Modify: `src/main.c` — after `world_create`, before the main loop

Add the expected chunk count and 30% threshold immediately after `world_create` (currently line 48).

- [ ] **Step 1: Add threshold computation after `world_create`**

Insert this block after `World* world = world_create(&renderer, 42, 32);`:

```c
/* Loading threshold: 30% of circular render area */
int _rd = world_get_render_distance(world);
int expected_chunks = 0;
for (int _dx = -_rd; _dx <= _rd; _dx++)
    for (int _dz = -_rd; _dz <= _rd; _dz++)
        if (_dx*_dx + _dz*_dz <= _rd*_rd)
            expected_chunks++;
int load_threshold = (int)(0.30f * (float)expected_chunks);
if (load_threshold < 1) load_threshold = 1;
```

- [ ] **Step 2: Build to verify it compiles**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: `[100%] Built target minecraft`

- [ ] **Step 3: Commit**

```bash
git add src/main.c
git commit -m "feat: compute loading threshold (30% of circular render area)"
```

---

### Task 2: Add the loading loop

**Files:**
- Modify: `src/main.c` — insert loading loop after threshold computation, before the main loop

- [ ] **Step 1: Insert the loading loop**

After the threshold block (after `if (load_threshold < 1) load_threshold = 1;`) and before the existing `double last_time = glfwGetTime();` line, insert:

```c
/* Loading loop: run until 30% of chunks are meshed */
{
    ChunkMesh* meshes;
    uint32_t   mesh_count = 0;
    char       title[128];

    while (!glfwWindowShouldClose(window)
           && mesh_count < (uint32_t)load_threshold)
    {
        glfwPollEvents();
        player_update(&g_player, window, world, 0.0f);
        world_update(world, g_player.position);
        world_get_meshes(world, &meshes, &mesh_count);

        mat4 view, proj;
        camera_get_view(&g_player.camera, g_player.eye_pos, view);
        float aspect = (float)renderer.swapchain.extent.width
                     / (float)renderer.swapchain.extent.height;
        camera_get_proj(&g_player.camera, aspect, proj);

        renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir);

        uint32_t pct = (uint32_t)(100.0f * (float)mesh_count
                                          / (float)load_threshold);
        if (pct > 100) pct = 100;
        snprintf(title, sizeof(title), "Minecraft | Loading... %u%%", pct);
        glfwSetWindowTitle(window, title);
    }
}
```

- [ ] **Step 2: Build to verify it compiles**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: `[100%] Built target minecraft`

---

### Task 3: Move timing variables after the loading loop

**Files:**
- Modify: `src/main.c` — relocate `last_time`, `frame_count`, `fps_timer`

Currently these three lines sit before the loading loop (lines 53–55 of the original file):
```c
double last_time = glfwGetTime();
int frame_count = 0;
double fps_timer = last_time;
```

If left there, `last_time` captures the time before loading begins. The first real frame's `dt = now - last_time` would span the entire load duration, causing a physics spike.

- [ ] **Step 1: Delete the three timing lines from their current location**

Remove:
```c
double last_time = glfwGetTime();
int frame_count = 0;
double fps_timer = last_time;
```

- [ ] **Step 2: Re-add them immediately before the main `while` loop**

Insert directly before `while (!glfwWindowShouldClose(window)) {`:

```c
double last_time = glfwGetTime();
int frame_count = 0;
double fps_timer = last_time;
```

- [ ] **Step 3: Build to verify it compiles**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: `[100%] Built target minecraft`

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "feat: loading screen — title-bar progress bar, 30% threshold"
```

---

### Task 4: Manual verification

- [ ] **Step 1: Run the game**

```bash
./build/minecraft
```

- [ ] **Step 2: Observe the window title during startup**

Expected sequence:
1. Title reads `"Minecraft | Loading... 0%"` immediately on open
2. Percentage climbs as chunks generate and mesh
3. At 100% the game loop starts and title switches to the FPS counter format: `"Minecraft | FPS: 60 | Chunks: N | Pos: ..."`

- [ ] **Step 3: Verify no physics spike on first frame**

Watch the player position — it should not jump or teleport on the first frame after loading completes.

- [ ] **Step 4: Verify ESC closes the window during loading**

Press ESC while the loading title is visible — the window should close cleanly.
