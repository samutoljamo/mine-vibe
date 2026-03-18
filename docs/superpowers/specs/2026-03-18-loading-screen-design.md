# Loading Screen — Design Spec

**Date:** 2026-03-18
**Status:** Approved
**Scope:** Display a title-bar progress indicator while initial chunks load

## Overview

Before the game loop begins, show a loading state in the window title
(`"Minecraft | Loading... 45%"`) while the world generates and meshes
enough nearby chunks. Transition to normal gameplay once 30% of the
circular render area is ready.

## Changes

**`src/main.c` only** — no other files change.

### Startup

Compute the expected chunk count for the circular render area:

```c
int rd = world_get_render_distance(world);
int expected_chunks = 0;
for (int dx = -rd; dx <= rd; dx++)
    for (int dz = -rd; dz <= rd; dz++)
        if (dx*dx + dz*dz <= rd*rd)
            expected_chunks++;

int threshold = (int)(0.30f * expected_chunks);
```

### Loading loop

Run before the main game loop. Each iteration:

1. `glfwPollEvents()` — keep the window responsive
2. `player_update(...)` — camera and input still work during load
3. `world_update(world, player.position)` — generation/meshing progresses
4. `renderer_draw_frame(..., meshes=NULL, mesh_count=0, ...)` — swapchain
   stays alive, renders sky-blue clear color
5. Update window title: `"Minecraft | Loading... <pct>%"`
6. Exit loop when `world_get_ready_count(world) >= threshold`

### Main game loop

Unchanged. On first frame after the loading loop exits, the FPS counter
title update takes over.

## Progress formula

```c
float progress = (float)world_get_ready_count(world) / (float)threshold;
int pct = (int)(progress * 100.0f);
if (pct > 100) pct = 100;
```

## Non-goals

- No on-screen rendered progress bar (deferred to future sub-project)
- No minimum display time
- No animated spinner or Minecraft-style branding
