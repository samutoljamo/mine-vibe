# Assets Design

**Date:** 2026-03-18
**Status:** Draft
**Scope:** Asset pipeline, block textures, player model, self-contained binary

## Overview

Replace procedurally generated block textures with pixel art and add a voxel character model for remote players. Introduce an asset pipeline where PNG files are the source of truth for art, compiled into C arrays at generation time so the binary remains self-contained with no runtime file I/O. Also fix the requirement to run the binary from the project root by embedding compiled SPIR-V shaders as C arrays.

## Directory Structure

```
assets/
  blocks/
    stone.png         (16×16)
    dirt.png          (16×16)
    grass_top.png     (16×16)
    grass_side.png    (16×16)
    sand.png          (16×16)
    wood_top.png      (16×16)
    wood_side.png     (16×16)
    leaves.png        (16×16)
    water.png         (16×16)
    bedrock.png       (16×16)
  player_skin.png     (64×32)

tools/
  gen_assets.py       (generates PNGs + src/assets_generated.c)

src/
  assets_generated.c  (committed; uint8_t[] arrays for block atlas + player skin)
  shaders_generated.c (committed; uint8_t[] arrays for block.vert.spv + block.frag.spv)
  assets.h            (extern declarations for all arrays + dimension constants)
```

## Asset Pipeline

### Texture Generator (`tools/gen_assets.py`)

A Python script using Pillow. Run manually whenever art changes; output is checked into git.

What it does:
1. Draws each block tile as 16×16 pixel art with a consistent retro palette
2. Saves individual PNGs to `assets/blocks/`
3. Draws the player skin at 64×32 using the standard Minecraft UV layout
4. Saves `assets/player_skin.png`
5. Assembles the 256×256 block atlas from the individual block PNGs
6. Writes `src/assets_generated.c` containing:
   - `uint8_t g_atlas_pixels[256*256*4]` — full RGBA block atlas
   - `uint8_t g_player_skin_pixels[64*32*4]` — full RGBA player skin

The generator produces the initial pixel art. The PNGs can be edited in any pixel art editor (e.g. Aseprite) and the script re-run to regenerate the C arrays.

### Shader Embedding (`shaders_generated.c`)

CMake already compiles `shaders/block.vert` and `shaders/block.frag` to SPIR-V in `build/shaders/`. A CMake custom command runs `xxd -i` on the `.spv` outputs to produce `src/shaders_generated.c`, also committed to git. This removes all `fopen` calls from `pipeline.c` and allows the binary to run from any working directory.

`assets.h` declares:
```c
extern const uint8_t g_atlas_pixels[];
extern const uint8_t g_player_skin_pixels[];
extern const uint8_t g_block_vert_spv[];
extern const size_t  g_block_vert_spv_size;
extern const uint8_t g_block_frag_spv[];
extern const size_t  g_block_frag_spv_size;
```

No CMake changes are needed for texture assets. The shader embedding adds one CMake custom command per shader.

## Block Textures

`texture.c`'s `generate_atlas_pixels()` is removed. `texture_create_atlas()` takes a `const uint8_t* pixels` parameter (or reads from `g_atlas_pixels` directly). The Vulkan upload path (staging buffer, image layout transitions, mipmap generation, sampler) is unchanged.

The pixel art style: bold, readable 16×16 tiles with a retro palette. Distinct silhouettes so blocks are identifiable even at small mip levels.

## Player Model

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

### Skin Texture

Standard 64×32 Minecraft skin UV layout. Compatible with third-party skin editors. UV regions:

```
Head (row 0):    top(8,0,8×8)   bottom(16,0,8×8)
                 right(0,8,8×8) front(8,8,8×8) left(16,8,8×8) back(24,8,8×8)
Body (row 1):    top(20,16,8×4) bottom(28,16,8×4)
                 right(16,20,4×12) front(20,20,8×12) left(28,20,4×12) back(32,20,8×12)
Right arm:       top(44,16,4×4)
                 right(40,20,4×12) front(44,20,4×12) left(48,20,4×12) back(52,20,4×12)
Right leg:       top(4,16,4×4)
                 right(0,20,4×12) front(4,20,4×12) left(8,20,4×12) back(12,20,4×12)
Left arm/leg:    mirrored horizontally from their right-side counterparts
```

### Rendering

The player model reuses the existing `block.vert`/`block.frag` shaders with a different texture binding.

**New files:**
- `src/player_model.h/.c` — builds static vertex buffer from hardcoded box geometry; `player_model_draw(Renderer*, RemotePlayer*, uint32_t count)`

**Renderer changes:**
- `renderer_init()` creates the player skin texture (`r->player_skin_image`, `r->player_skin_view`, `r->player_skin_sampler`) and a dedicated descriptor set with that texture at binding 1
- `renderer_draw_remote_players()` — updated from placeholder AABB to call `player_model_draw()`

**Draw sequence per frame:**
1. Bind chunk descriptor set → draw chunk meshes (unchanged)
2. Bind player skin descriptor set → push model matrix per player (push constants) → draw player mesh

Model matrix = `T(position) × R(yaw)`. The full body rotates with yaw. No per-bone animation in v1.

## Modified Files Summary

| File | Change |
|------|--------|
| `src/pipeline.c` | Accept SPIR-V arrays instead of file paths |
| `src/renderer.c` | Use shader arrays; add player skin texture + descriptor set; update `renderer_draw_remote_players()` |
| `src/texture.c` | Remove `generate_atlas_pixels()`; use `g_atlas_pixels` from `assets_generated.c` |
| `src/assets_generated.c` | New (committed) — atlas + skin pixel arrays |
| `src/shaders_generated.c` | New (committed) — SPIR-V arrays |
| `src/assets.h` | New — extern declarations |
| `src/player_model.h/.c` | New — static mesh + draw function |
| `tools/gen_assets.py` | New — pixel art generator |
| `CMakeLists.txt` | Add `xxd -i` custom commands for shader embedding; add `player_model.c` to sources |

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
4. `python tools/gen_assets.py` regenerates `assets_generated.c` and `assets/*.png`
5. Binary has no runtime file I/O — self-contained
