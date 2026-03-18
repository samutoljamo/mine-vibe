# Loading Screen — Design Spec

**Date:** 2026-03-18
**Status:** Approved
**Scope:** Display a title-bar progress indicator while initial chunks load

## Overview

Before the main game loop begins, show a loading state in the window
title (`"Minecraft | Loading... 45%"`) while the world generates and
meshes enough nearby chunks. Transition to normal gameplay once 30% of
the circular render area has uploaded meshes.

## Changes

**`src/main.c` only** — no other files change.

### Startup

Compute the expected chunk count for the circular render area and the
30% threshold:

```c
int rd = world_get_render_distance(world);
int expected_chunks = 0;
for (int dx = -rd; dx <= rd; dx++)
    for (int dz = -rd; dz <= rd; dz++)
        if (dx*dx + dz*dz <= rd*rd)
            expected_chunks++;

int threshold = (int)(0.30f * (float)expected_chunks);
if (threshold < 1) threshold = 1; /* guard: skip infinite loop on rd=0 */
```

### Loading loop

Runs before the main game loop and before `last_time` is initialized.
Each iteration:

1. `glfwPollEvents()`
2. `player_update(&g_player, window, world, 0.0f)`
3. `world_update(world, g_player.position)`
4. `world_get_meshes(world, &meshes, &mesh_count)` — get current partial mesh list
5. Compute `view` and `proj` from the player's current camera state (same as the
   main loop: `camera_get_view`, `camera_get_proj` with swapchain aspect ratio)
6. `renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir)` — renders
   whatever is loaded so far (sky-blue + any ready chunks)
7. Update window title: `"Minecraft | Loading... <pct>%"` where
   `pct = MIN(100, (int)(100.0f * mesh_count / threshold))`
8. Exit conditions: `mesh_count >= (uint32_t)threshold` OR `glfwWindowShouldClose(window)`

### Timing

`last_time` is initialized **after** the loading loop exits, immediately
before the main `while` loop. This prevents the first real frame from
seeing a huge `dt` equal to the full load duration.

```c
/* loading loop here ... */

double last_time = glfwGetTime(); /* initialized after loading, not before */
int frame_count = 0;
double fps_timer = last_time;

while (!glfwWindowShouldClose(window)) { ... }
```

### Main game loop

Unchanged. On the first frame after loading, `dt` is correct and the
FPS counter title update takes over within 2 seconds.

## Progress formula

```c
uint32_t pct = (uint32_t)(100.0f * (float)mesh_count / (float)threshold);
if (pct > 100) pct = 100;
snprintf(title, sizeof(title), "Minecraft | Loading... %u%%", pct);
```

`mesh_count` comes from `world_get_meshes` and counts only chunks with
uploaded GPU buffers — the same set the renderer will draw. This matches
player perception (progress = visible terrain loaded).

## Non-goals

- No on-screen rendered progress bar (deferred to future sub-project)
- No minimum display time
- No animated spinner or Minecraft-style branding
