# HUD Foundation Design
**Date:** 2026-03-18
**Status:** Approved

## Overview

Add a 2D HUD rendering layer on top of the existing 3D world. Initial contents: a crosshair and a 6-slot hotbar with scroll-wheel selection. This is the foundation for all future in-game UI (block interaction, health bar, debug overlay).

---

## Architecture

Six areas of change:

| Component | Change |
|-----------|--------|
| `src/hud.h` / `src/hud.c` | New. HUD state + CPU-side quad geometry builder. |
| `shaders/hud.vert` / `shaders/hud.frag` | New. Minimal 2D NDC + flat-color shaders. |
| `src/renderer.h` / `src/renderer.c` | Modified. Second Vulkan renderpass + pipeline; `renderer_draw_frame()` gains `const HUD*` parameter. |
| `src/main.c` | Modified. Scroll callback, `hud_init()`, updated `renderer_draw_frame()` call. |
| `src/agent.h` / `src/agent.c` | Modified. `CMD_SELECT_SLOT`, `selected_slot` + `hotbar[6]` in snapshot. |
| `CMakeLists.txt` | Modified. Add `src/hud.c`, compile new shaders. |

---

## Data Model

### HUD state (`src/hud.h`)

```c
#define HUD_SLOT_COUNT 6

typedef struct HUD {
    int     selected_slot;                  /* 0–5, changed by scroll wheel or agent */
    BlockID slot_blocks[HUD_SLOT_COUNT];    /* fixed: stone, dirt, grass, sand, wood, leaves */
} HUD;
```

`slot_blocks` is populated by `hud_init()` and never changes at runtime.

### Vertex format

```c
typedef struct HudVertex {
    float x, y;         /* NDC [-1, 1], computed CPU-side by hud_build() */
    float r, g, b, a;   /* linear color */
} HudVertex;  /* 24 bytes */
```

Geometry is built CPU-side each frame. All pixel-to-NDC conversion happens in `hud_build()` — no push constants or GPU-side screen_size needed. Stored in a host-visible, persistently-mapped Vulkan buffer — no staging required.

Vertex/index count derivation:
- Crosshair: 2 quads × 4 verts = 8 verts, 12 indices
- Slot fills: 6 × 4 = 24 verts, 36 indices
- Slot borders: 6 slots × 4 edges × 4 verts = 96 verts, 144 indices
- Total: 128 verts / 192 indices — budget set to 256/384 for headroom

```c
#define HUD_MAX_VERTS   256
#define HUD_MAX_INDICES 384
```

### Public API

```c
void     hud_init(HUD* hud);
/* Fills verts and indices; returns vertex count.
 * screen_w/h used for pixel→NDC conversion. */
uint32_t hud_build(const HUD* hud, float screen_w, float screen_h,
                   HudVertex* verts, uint32_t* indices);
BlockID  hud_selected_block(const HUD* hud);
```

No `hud_destroy()` needed — the `HUD` struct has no heap allocations. Vulkan resources are owned by `Renderer` and freed in `renderer_cleanup()`.

---

## Renderer Integration

### Command buffer strategy

`renderer_draw_frame()` gains a `const HUD*` parameter. The world renderpass and the HUD renderpass are **both recorded in the same command buffer** in a single call before submit + present. The public call site in `main.c` becomes:

```c
renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir,
                    &hud, dump_frame, dump_path);
```

`dump_frame` (bool) and `dump_path` (char[256]) are passed explicitly so `renderer_draw_frame()` can call `renderer_dump_frame()` at the correct point (after the HUD pass, before submit). This avoids splitting begin/end/submit across multiple functions and requires no extra synchronisation.

### Renderpass sequence

1. **World renderpass** — unchanged except `finalLayout` of color attachment changes from `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` to `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`.
2. **HUD renderpass** — `loadOp = LOAD`, no depth attachment, alpha blending, `finalLayout = PRESENT_SRC_KHR`.
3. **Submit + present** — unchanged, happens once after both passes are recorded.

### New renderer fields

```c
/* Added to Renderer struct */
VkRenderPass      hud_render_pass;
VkPipeline        hud_pipeline;
VkPipelineLayout  hud_pipeline_layout;  /* pushConstantRangeCount = 0 */
VkFramebuffer*    hud_framebuffers;     /* one per swapchain image, color-only */
VkBuffer          hud_vertex_buffer;
VmaAllocation     hud_vertex_alloc;
VkBuffer          hud_index_buffer;
VmaAllocation     hud_index_alloc;
void*             hud_vb_mapped;        /* persistently mapped */
void*             hud_ib_mapped;
```

`hud_framebuffers` is separate from the existing `swapchain.framebuffers` because those were created against the world renderpass (2-attachment: color + depth). The HUD renderpass has 1 attachment (color only) and requires its own framebuffer objects.

### Swapchain recreation

`hud_framebuffers` are swapchain-size-dependent and must be rebuilt on every resize. Inside `recreate_swapchain()`:

1. Destroy existing `hud_framebuffers[i]` for all images.
2. Recreate against the new swapchain image views.

The persistent VMA buffers (`hud_vertex_buffer`, `hud_index_buffer`) survive resize unchanged.

### Agent dump_frame ordering

`renderer_dump_frame()` assumes the swapchain image is in `PRESENT_SRC_KHR`. Since the HUD renderpass is now the last pass and transitions the image to `PRESENT_SRC_KHR`, `dump_frame` must be requested inside `renderer_draw_frame()` **after** the HUD renderpass ends and before `vkQueueSubmit`. The call site in `main.c` passes a `dump_frame` flag and path to `renderer_draw_frame()` which handles it internally at the correct point.

---

## Shaders

All pixel→NDC conversion is done CPU-side in `hud_build()`. No push constants.

### `shaders/hud.vert`

```glsl
#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 v_color;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_color = color;
}
```

### `shaders/hud.frag`

```glsl
#version 450
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;
void main() { out_color = v_color; }
```

Pipeline layout: no descriptor sets, no push constants (`pushConstantRangeCount = 0`).

---

## Visual Layout

All pixel measurements converted to NDC inside `hud_build()` using `screen_w` / `screen_h`.

### Crosshair
- Two white rectangles, 2 px wide × 14 px long, centered at NDC (0, 0)
- Color: `(1, 1, 1, 0.8)`

### Hotbar
- 6 slots, each **40 × 40 px**, centered horizontally, **12 px from bottom edge**
- **4 px gap** between slots
- Slot fill: `(0.15, 0.15, 0.15, 0.75)`
- Inactive slot border (4 thin 2 px edge quads): `(0.4, 0.4, 0.4, 0.75)`
- Selected slot border: `(1.0, 1.0, 1.0, 1.0)`

---

## Main Loop Integration

### Scroll callback

```c
static void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    (void)w; (void)xoff;
    int dir = (yoff > 0) ? -1 : 1;   /* scroll up = previous slot */
    g_hud.selected_slot =
        (g_hud.selected_slot + dir + HUD_SLOT_COUNT) % HUD_SLOT_COUNT;
}
```

Registered only when not in agent mode (agent uses `CMD_SELECT_SLOT` instead).

### Frame order

```
world_update()
block_physics_update()
renderer_draw_frame(..., &hud, dump_frame, dump_path)
    ← records world pass + HUD pass
    ← calls renderer_dump_frame() if dump_frame (after HUD pass, image in PRESENT_SRC_KHR)
    ← submits + presents
```

`hud_init()` called after `world_create()`.

---

## Agent Mode

### New command

```c
/* Added to AgentCommandType enum */
CMD_SELECT_SLOT,

/* Added to AgentCommand union */
struct { int slot; } select_slot;   /* 0-indexed; clamped to [0, HUD_SLOT_COUNT) in parser */
```

JSON wire format: `{"cmd":"select_slot","slot":2}`

Clamping follows the existing pattern (`CMD_LOOK` clamps pitch in the parser): `agent_parse_command()` clamps `slot` to `[0, HUD_SLOT_COUNT - 1]` before returning.

### Snapshot additions

```c
/* Added to AgentSnapshot */
int selected_slot;               /* 0–5 */
int hotbar[HUD_SLOT_COUNT];      /* BlockID for each slot (constant after init) */
```

JSON emission adds approximately 40 bytes to the snapshot. The existing `agent_emit_snapshot()` buffer is 512 bytes; current snapshot JSON is ~200 bytes, so the total remains well within the limit. The hotbar array is constant after `hud_init()` but included every frame so the protocol is self-describing.

Example output:
```json
{ "pos": [...], ..., "selected_slot": 2, "hotbar": [1,2,3,4,5,6] }
```

---

## Testing

- Build test: `minecraft` and `test_block_physics` compile clean
- Manual smoke test: scroll wheel cycles selected slot highlight; crosshair visible at screen center; window resize preserves HUD
- Agent test: `CMD_SELECT_SLOT` changes `selected_slot`; snapshot reflects correct `hotbar[]` values; `CMD_DUMP_FRAME` still produces valid frames with HUD visible
