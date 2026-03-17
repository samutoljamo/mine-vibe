# Minecraft Clone — Sub-project 1: Foundation

**Date:** 2026-03-17
**Status:** Draft
**Scope:** Vulkan renderer, chunk-based voxel world, terrain generation, first-person camera

## Overview

Build the rendering and world foundation for a survival-lite Minecraft clone in C with Vulkan. This is the first of three sub-projects:

1. **Foundation** (this spec) — Vulkan renderer, chunk meshing, world gen, camera
2. **Interaction** (future) — Block place/destroy, collision, player physics
3. **Gameplay** (future) — Inventory, crafting, day/night, basic mob

## Tech Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Language | C11 | Simple, direct, no abstraction overhead |
| Graphics API | Vulkan | Maximum control, modern API |
| Function loader | volk | Dynamic loading, no Vulkan SDK linkage required |
| Memory allocator | VMA | Industry standard GPU memory management |
| Windowing | GLFW | Lightweight, Vulkan-native surface support |
| Math | cglm | Header-only C port of glm |
| Noise | FastNoiseLite | Header-only, Perlin FBM |
| Image loading | stb_image | Header-only PNG loader |
| Build | CMake 3.20+ | Cross-platform, FetchContent for deps |

**Note:** vk-bootstrap was originally planned but has no C API (C++ only). Vulkan initialization is written by hand (~300 lines of mechanical setup code).

**Note:** VMA's implementation requires C++ compilation. A single `vma_impl.cpp` file handles this; the VMA header is usable from C.

## Architecture

### Project Structure

```
minecraft/
├── CMakeLists.txt
├── src/
│   ├── main.c              # Entry point, GLFW window, game loop
│   ├── renderer.h/.c       # Vulkan init, frame rendering
│   ├── swapchain.h/.c      # Swapchain + depth buffer + framebuffers
│   ├── pipeline.h/.c       # Shader loading, graphics pipeline
│   ├── texture.h/.c        # Atlas load + staging upload
│   ├── frustum.h/.c        # Frustum culling (cglm wrappers)
│   ├── camera.h/.c         # First-person camera
│   ├── vertex.h            # Vertex struct, UBO, push constants
│   ├── block.h/.c          # Block type definitions
│   ├── chunk.h/.c          # Chunk data structure (16x256x16)
│   ├── chunk_map.h/.c      # Hash map for chunk storage
│   ├── chunk_mesh.h/.c     # Per-chunk GPU buffer management
│   ├── mesher.h/.c         # Greedy meshing algorithm
│   ├── world.h/.c          # World management, async orchestration
│   ├── worldgen.h/.c       # Terrain generation
│   └── vma_impl.cpp        # VMA implementation (C++ compilation unit)
├── shaders/
│   ├── block.vert           # Vertex shader (GLSL 450)
│   └── block.frag           # Fragment shader
└── assets/
    └── atlas.png            # 256x256 texture atlas, 16x16 tiles
```

### Data Flow

```
Game Loop (main.c)
  │
  ├── poll_input() → camera_update()
  │
  ├── world_update(camera_pos)
  │     ├── Poll completed async gen/mesh results
  │     ├── Upload ready meshes to GPU (max 8/frame)
  │     ├── Unload distant chunks (render_dist + 4 hysteresis)
  │     ├── Submit generation for missing chunks (max 16/frame)
  │     └── Submit meshing for generated chunks with neighbors ready
  │
  └── renderer_draw_frame(view, proj, chunks)
        ├── Wait fence, acquire swapchain image
        ├── Update UBO (view/proj/sun)
        ├── Extract frustum planes
        ├── For each chunk: frustum cull → push offset → draw indexed
        └── Present
```

## Rendering

### Vertex Format (24 bytes)

| Field | Type | Size | Description |
|-------|------|------|-------------|
| pos | float[3] | 12B | World-space position |
| uv | float[2] | 8B | Texture atlas coordinates |
| normal | uint8_t | 1B | Packed face normal (0-5 for +/-X, +/-Y, +/-Z) |
| ao | uint8_t | 1B | Ambient occlusion level (0-3) |
| _pad | uint8_t[2] | 2B | Alignment padding |

### Uniform Buffer (176 bytes, std140)

- `mat4 view` — camera view matrix
- `mat4 proj` — perspective projection (Y-flipped for Vulkan)
- `vec4 sun_direction` — directional light
- `vec4 sun_color` — light color
- `float ambient` — ambient light factor

### Push Constants (16 bytes)

- `vec4 chunk_offset` — world-space chunk origin, avoids per-chunk descriptor updates

### Pipeline Configuration

- Topology: triangle list
- Culling: back face, counter-clockwise front
- Depth: test + write enabled, LESS compare
- Blending: disabled (opaque only in sub-project 1)
- Dynamic state: viewport + scissor
- Descriptor set: binding 0 = UBO (vert+frag), binding 1 = combined image sampler (frag)

### Shaders

**Vertex shader:**
- Adds `chunk_offset.xyz` to vertex position
- Transforms by `proj * view`
- Expands packed normal via lookup table, computes N dot L for directional light
- Normalizes AO (0-3 → 0.0-1.0)

**Fragment shader:**
- Samples texture atlas
- Alpha cutoff at 0.5 (leaves/flowers)
- Applies `light * ao_factor` where ao_factor = 0.4 + 0.6 * ao

**Shader compilation:** GLSL 450 sources compiled to SPIR-V via `glslc` (from Vulkan SDK) as CMake custom commands. The build produces `block.vert.spv` and `block.frag.spv` alongside the sources.

### Sync Strategy

- 2 frames in flight
- Per-frame: fence (created signaled), image_available semaphore, render_finished semaphore
- Command buffer reset per frame
- UBOs persistently mapped via VMA

## World System

### Chunk Format

- 16x256x16 blocks = 65,536 bytes (1 byte per block)
- Flat array indexed by `x + z*16 + y*256`
- `_Atomic` state field: UNLOADED → GENERATING → GENERATED → MESHING → MESHED → READY

### Chunk Map

- Open-addressing hash map with linear probing
- Key: (chunk_x, chunk_z), hashed via splitmix64
- Initial capacity 8192, rehash at 70% load
- Main-thread only access (no locks)

### Terrain Generation

- FastNoiseLite Perlin FBM, 3 octaves, frequency 0.005
- Height range: ~44-84, base 64
- Layers: bedrock (y < 10), stone, dirt (3-4), grass top
- Sand at sea level (y 62-64)
- Water fills below y=62
- Trees: ~2% of grass blocks, trunk 4-6 + leaf sphere, constrained to [2..13] local X/Z

### Greedy Meshing

For each of 6 face directions:
1. Sweep chunk in 2D slices
2. Build mask: face visible if current block is non-air and neighbor is transparent
3. Greedily merge adjacent same-type faces into rectangles
4. Emit 4 vertices + 6 indices per rectangle
5. Neighbor boundary faces use copied edge slices (16x256 = 4KB each)

### Async Worker Pool

- `max(1, num_cores - 2)` threads, capped at 8
- Request queue (mutex + condvar): GENERATE or MESH tasks
- Result queue (mutex): polled by main thread each frame
- Each worker gets own FastNoiseLite state (no shared mutable state)
- Graceful shutdown via atomic boolean

### Render Distance

- Default: 32 chunks (512 blocks)
- Frustum culling: extract 6 planes from VP matrix, AABB test per chunk (cglm)
- Unload hysteresis: render_distance + 4 chunks
- Load priority: sorted by distance from player

### Memory Budget (r=32)

| Component | Estimate |
|-----------|----------|
| Block data (4225 chunks x 65KB) | ~261 MiB |
| CPU mesh buffers | ~40 MiB |
| GPU vertex/index buffers | ~37 MiB |
| **Total** | **~340 MiB** |

## Texture Atlas

- 256x256 PNG, 16x16 tile grid = 256 tile slots
- Block types (9): AIR, STONE, DIRT, GRASS, SAND, WOOD, LEAVES, WATER, BEDROCK
- Per-block: separate atlas indices for top/side/bottom faces (grass has green top, brown side)
- Sampler: NEAREST filtering, CLAMP_TO_EDGE (pixel art aesthetic)
- Initial version: programmatically generated colored-square atlas for testing

## Error Handling

- Vulkan validation layers enabled in debug builds (VK_LAYER_KHRONOS_validation)
- All `vkCreate*` calls checked for `VK_SUCCESS`
- Swapchain OUT_OF_DATE handled by recreation; SUBOPTIMAL triggers recreation after present
- Window minimization: spin on `glfwWaitEvents` until framebuffer size > 0
- Thread pool: worker errors logged, chunk marked for retry

## Success Criteria

1. Project compiles with `cmake -B build && cmake --build build`
2. Window opens with sky-blue background
3. First-person camera movement (WASD + mouse look)
4. Terrain generates with visible block types and correct layering
5. Chunk boundaries seamless (no gaps)
6. Chunks load/unload as player moves
7. Smooth 60fps at render distance 32 on a discrete GPU
8. Window resize works without crash
9. Vulkan validation layers report no errors

## Non-Goals (deferred to sub-projects 2 and 3)

- Block placement/destruction
- Collision detection and physics
- Inventory and crafting
- Day/night cycle
- Mob AI
- Sound
- Multiplayer
- Saving/loading worlds
