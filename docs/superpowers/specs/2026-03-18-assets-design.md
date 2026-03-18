# Assets Design

**Date:** 2026-03-18
**Status:** Draft
**Scope:** Asset pipeline, block textures, player model, self-contained binary

## Overview

Replace procedurally generated block textures with pixel art and add a voxel character model for remote players. Introduce an asset pipeline where PNG files are the source of truth for art, compiled into C arrays by developer-run tools so the binary remains self-contained with no runtime file I/O. Also fix the requirement to run the binary from the project root by embedding compiled SPIR-V shaders as C arrays.

## Directory Structure

```
assets/
  blocks/
    stone.png         (16×16, tile index 0)
    dirt.png          (16×16, tile index 1)
    grass_top.png     (16×16, tile index 2)
    grass_side.png    (16×16, tile index 3)
    sand.png          (16×16, tile index 4)
    wood_top.png      (16×16, tile index 5)
    wood_side.png     (16×16, tile index 6)
    leaves.png        (16×16, tile index 7)
    water.png         (16×16, tile index 16)
    bedrock.png       (16×16, tile index 17)
  player_skin.png     (64×32)

tools/
  gen_assets.py       (generates PNGs + src/assets_generated.c)
  embed_shaders.py    (embeds compiled SPIR-V into src/shaders_generated.c)

src/
  assets_generated.c  (committed; uint8_t[] for block atlas + player skin)
  shaders_generated.c (committed; uint8_t[] for block.vert.spv + block.frag.spv)
  assets.h            (extern declarations for all arrays + dimension constants)

shaders/
  player.vert         (new — player model vertex shader)
  player.frag         (new — player model fragment shader)
```

## Asset Pipeline

### Texture Generator (`tools/gen_assets.py`)

A Python script using Pillow. Run manually whenever art changes; output is checked into git. **No CMake changes.**

Behaviour regarding PNGs:
- If a PNG does not exist, the script generates it (draws pixel art programmatically).
- If a PNG already exists, the script reads it without overwriting. This means you can edit a PNG in any pixel art editor (e.g. Aseprite), re-run the script, and your edits are preserved and packed into the C array.
- Pass `--regenerate` to force all PNGs to be redrawn from the programmatic definitions.

What it writes:
1. Individual PNGs to `assets/blocks/` and `assets/player_skin.png` (if missing or `--regenerate`)
2. `src/assets_generated.c` containing:
   - `const uint8_t g_atlas_pixels[256*256*4]` — full RGBA block atlas assembled from block PNGs
   - `const uint8_t g_player_skin_pixels[64*32*4]` — full RGBA player skin

**Tile index preservation:** the block atlas is a 256×256 image divided into 16-pixel tiles. Each PNG is placed at its specific tile index (see directory listing above). Indices 8–15 remain black/transparent. Tile indices in `block.c` (`tex_top`, `tex_side`, `tex_bottom`) and UV generation in `mesher.c` are unchanged.

### Shader Embedder (`tools/embed_shaders.py`)

A separate developer-run script. Run after recompiling shaders whenever GLSL source changes; output is checked into git. **No CMake changes.**

Workflow:
```
cmake --build build --target shaders     # compiles .spv to build/shaders/
python tools/embed_shaders.py            # reads build/shaders/*.spv, writes src/shaders_generated.c
git add src/shaders_generated.c && git commit
```

`src/shaders_generated.c` contains:
```c
const uint8_t g_block_vert_spv[]  = { ... };
const size_t  g_block_vert_spv_size = sizeof(g_block_vert_spv);
const uint8_t g_block_frag_spv[]  = { ... };
const size_t  g_block_frag_spv_size = sizeof(g_block_frag_spv);
const uint8_t g_player_vert_spv[] = { ... };
const size_t  g_player_vert_spv_size = sizeof(g_player_vert_spv);
const uint8_t g_player_frag_spv[] = { ... };
const size_t  g_player_frag_spv_size = sizeof(g_player_frag_spv);
```

`assets.h` declares all of these as `extern`. Because both generated files are committed, a clean checkout builds without Python or any developer tools.

## Block Textures

`texture.c`'s `generate_atlas_pixels()` function is removed. `texture_create_atlas()` reads from `g_atlas_pixels` (declared in `assets.h`). The Vulkan upload path — staging buffer, image layout transitions, mipmap generation, sampler — is unchanged.

The pixel art style: bold, readable 16×16 tiles with a retro palette. Distinct silhouettes so blocks are identifiable even at small mip levels.

## Player Model

### Dedicated Shaders

The player model uses **new `player.vert` / `player.frag` shaders** and a separate `VkPipeline`. This decouples the player renderer from the chunk pipeline entirely and avoids conflicts with the chunk push constant layout and vertex format.

`player.vert` push constants:
```glsl
layout(push_constant) uniform PushConstants {
    mat4 model;   // 64 bytes — per-player model matrix
} pc;
```

`player.frag` uses a simple directional light model (no AO — player geometry has no ambient occlusion). The six face normals are baked into the vertex buffer as a face index (0–5, matching the existing axis lookup), and the fragment shader applies the same directional multipliers as `block.frag` for visual consistency. Binding 0 (UBO) and binding 1 (combined sampler) match the existing descriptor set layout so the same `VkDescriptorSetLayout` is reused.

### Descriptor Set

The player skin texture uses the **same `VkDescriptorSetLayout`** as the chunk pipeline (binding 0 = UBO, binding 1 = combined sampler). A dedicated descriptor set is created at `renderer_init()` time with:
- Binding 0: same per-frame UBO (view/proj matrices)
- Binding 1: player skin sampler

The descriptor pool is expanded to accommodate the additional per-frame player skin sets:
- `maxSets`: 2 → 4
- `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` count: 2 → 4
- `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` count: 2 → 4

### Vertex Format

Player model vertices use a dedicated lightweight struct (not `BlockVertex`):
```c
typedef struct {
    float    x, y, z;       // position
    float    u, v;           // UV into 64×32 skin texture
    uint8_t  face_idx;       // 0–5 for directional lighting (same as block normals)
    uint8_t  _pad[3];
} PlayerVertex;
```

This avoids the AO field entirely and keeps the static mesh compact.

### Geometry

A voxel character made of 6 box parts. All dimensions in block units, origin at feet:

| Part       | Width | Height | Depth | Center (x, y, z)      |
|------------|-------|--------|-------|-----------------------|
| Head       | 0.50  | 0.50   | 0.50  | (0, 1.50, 0)          |
| Torso      | 0.50  | 0.75   | 0.25  | (0, 0.875, 0)         |
| Right arm  | 0.25  | 0.75   | 0.25  | (+0.375, 0.875, 0)    |
| Left arm   | 0.25  | 0.75   | 0.25  | (−0.375, 0.875, 0)    |
| Right leg  | 0.25  | 0.75   | 0.25  | (+0.125, 0.25, 0)     |
| Left leg   | 0.25  | 0.75   | 0.25  | (−0.125, 0.25, 0)     |

Total height ≈ 1.75 blocks — fits within the existing 1.8-block player AABB. The head is slightly oversized relative to classic Steve proportions for better readability at multiplayer distances.

### Skin UV Layout

Standard 64×32 Minecraft skin. All coordinates are in pixels; the shader normalizes to [0,1] by dividing by 64 and 32 respectively.

```
Head:
  top    (8,0)–(16,8)      bottom  (16,0)–(24,8)
  right  (0,8)–(8,16)      front   (8,8)–(16,16)
  left   (16,8)–(24,16)    back    (24,8)–(32,16)

Body:
  top    (20,16)–(28,20)   bottom  (28,16)–(36,20)
  right  (16,20)–(20,32)   front   (20,20)–(28,32)
  left   (28,20)–(32,32)   back    (32,20)–(40,32)

Right arm:
  top    (44,16)–(48,20)   bottom  (48,16)–(52,20)
  right  (40,20)–(44,32)   front   (44,20)–(48,32)
  left   (48,20)–(52,32)   back    (52,20)–(56,32)

Right leg:
  top    (4,16)–(8,20)     bottom  (8,16)–(12,20)
  right  (0,20)–(4,32)     front   (4,20)–(8,32)
  left   (8,20)–(12,32)    back    (12,20)–(16,32)

Left arm and left leg (64×32 has no separate region for these):
  Share the same pixel regions as right arm and right leg respectively.
  U coordinates are reflected within the sub-region and baked into the static
  vertex buffer — no shader logic required. For each face vertex:
    u_baked = u_region_min + (u_region_max - u_original)
  V coordinates are unchanged.
```

### Rendering

**Player skin sampler:** `magFilter = VK_FILTER_NEAREST`, `minFilter = VK_FILTER_NEAREST`, `mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST`, `mipLevels = 1` (no mipmaps — 64×32 is small and pixel art benefits from sharp sampling), `addressMode = CLAMP_TO_EDGE`.

**PlayerVertex input descriptors** (defined in `player_model.c`, not `vertex.h`):
- Binding: stride = `sizeof(PlayerVertex)`, input rate = vertex
- Location 0: position — `VK_FORMAT_R32G32B32_SFLOAT`, offset 0
- Location 1: UV — `VK_FORMAT_R32G32_SFLOAT`, offset 12
- Location 2: face_idx — `VK_FORMAT_R8_UINT`, offset 20

**Winding order:** clockwise front-face (same as chunk pipeline — `VK_FRONT_FACE_CLOCKWISE`, `VK_CULL_MODE_BACK_BIT`). Static mesh vertices are wound to match.

**Pipeline API:**
- `pipeline_create()` signature changes from file paths to SPIR-V byte arrays: `pipeline_create(VkDevice, VkRenderPass, VkDescriptorSetLayout, const uint8_t* vert_spv, size_t vert_size, const uint8_t* frag_spv, size_t frag_size)`. The existing call site in `renderer.c` updates to pass the arrays from `shaders_generated.c`.
- `player_pipeline_create()` — new function in `pipeline.c` with the same signature but creates a pipeline with the `PlayerVertex` descriptors and 64-byte push constant range.

**New files:**
- `src/player_model.h/.c` — builds static `VkBuffer` from hardcoded `PlayerVertex` geometry at `player_model_init(Renderer*)`; `player_model_draw(Renderer*, RemotePlayer*, uint32_t count)` pushes model matrix per player and draws

**Renderer changes:**
- `renderer_init()` creates player skin texture + per-frame descriptor sets + player `VkPipeline`
- `renderer_cleanup()` destroys player skin `VkImage`/`VkImageView`/`VkSampler`, player `VkPipeline`, player `VkPipelineLayout`, and player descriptor sets
- `renderer_draw_remote_players()` — calls `player_model_draw()` instead of placeholder AABB

**Draw sequence per frame:**
1. Bind chunk pipeline + chunk descriptor set → draw chunk meshes (unchanged)
2. Bind player pipeline + player skin descriptor set → for each remote player: push `mat4 model`, draw player mesh

Model matrix = `T(position) × R(yaw)`. Full body rotates with yaw. No per-bone animation in v1.

## Modified Files Summary

| File | Change |
|------|--------|
| `src/pipeline.c` | Accept SPIR-V arrays; add `player_pipeline_create()` |
| `src/renderer.c` | Use shader arrays; add player skin texture, descriptor sets, player pipeline; update `renderer_draw_remote_players()` and `renderer_cleanup()` |
| `src/texture.c` | Remove `generate_atlas_pixels()`; use `g_atlas_pixels` |
| `src/assets_generated.c` | New (committed) — atlas + skin pixel arrays |
| `src/shaders_generated.c` | New (committed) — SPIR-V arrays for all 4 shaders |
| `src/assets.h` | New — extern declarations |
| `src/player_model.h/.c` | New — static mesh + draw function |
| `shaders/player.vert` | New — player vertex shader with mat4 push constant |
| `shaders/player.frag` | New — player fragment shader, no AO |
| `tools/gen_assets.py` | New — pixel art generator |
| `tools/embed_shaders.py` | New — SPIR-V embedder |
| `CMakeLists.txt` | Add `player.vert`/`player.frag` to shader compile list; add `player_model.c` to sources |

## Non-Goals

- Player animations (walking, arm swing) — static pose only in v1
- Per-player skin customization — one shared skin for all remote players
- Local player model (first-person view unchanged)
- Head pitch tracking
- Icon / HUD rendering (separate future spec)

## Success Criteria

1. `./build/minecraft` and `cd build && ./minecraft` both work
2. Remote players appear as voxel characters at correct position and yaw
3. Block textures are pixel art (not XOR-pattern flat colors)
4. `python tools/gen_assets.py` regenerates `assets_generated.c` and `assets/*.png` (existing PNGs preserved unless `--regenerate` passed)
5. `python tools/embed_shaders.py` regenerates `shaders_generated.c` from `build/shaders/*.spv`
6. Binary has no runtime file I/O — self-contained
7. No Vulkan validation errors when drawing remote players
