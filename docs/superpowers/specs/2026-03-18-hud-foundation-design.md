# HUD Foundation Design
**Date:** 2026-03-18
**Status:** Approved

## Overview

Add a 2D HUD rendering layer on top of the existing 3D world. Initial contents: a crosshair and a 6-slot hotbar with scroll-wheel selection. This is the foundation for all future in-game UI (block interaction, health bar, debug overlay).

---

## Architecture

Four areas of change:

| Component | Change |
|-----------|--------|
| `src/hud.h` / `src/hud.c` | New. HUD state + CPU-side quad geometry builder. |
| `shaders/hud.vert` / `shaders/hud.frag` | New. Minimal 2D NDC + flat-color shaders. |
| `src/renderer.h` / `src/renderer.c` | Modified. Second Vulkan renderpass + pipeline, `renderer_draw_hud()`. |
| `src/main.c` | Modified. Scroll callback, `hud_init()`, `renderer_draw_hud()` call. |
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
    float x, y;         /* NDC [-1, 1] */
    float r, g, b, a;   /* linear color */
} HudVertex;  /* 24 bytes */
```

Geometry is built CPU-side each frame. Crosshair + 6 slots + borders ≈ 200 vertices maximum. Stored in a host-visible, persistently-mapped Vulkan buffer — no staging required.

### Public API

```c
void     hud_init(HUD* hud);
/* Fills verts and indices; returns vertex count. indices must hold at least
 * HUD_MAX_INDICES entries. */
uint32_t hud_build(const HUD* hud, float screen_w, float screen_h,
                   HudVertex* verts, uint32_t* indices);
BlockID  hud_selected_block(const HUD* hud);

#define HUD_MAX_VERTS   256
#define HUD_MAX_INDICES 384
```

---

## Renderer Integration

### Second renderpass strategy

The existing main renderpass final layout changes from `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` to `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. The HUD renderpass then:

- `loadOp = VK_ATTACHMENT_LOAD_OP_LOAD` — preserve the world image
- `storeOp = VK_ATTACHMENT_STORE_OP_STORE`
- `finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
- No depth attachment
- Alpha blending enabled (`srcAlpha = SRC_ALPHA`, `dstAlpha = ONE_MINUS_SRC_ALPHA`)

Both passes record into the same command buffer, same submit — no extra synchronisation.

### New renderer fields

```c
/* Added to Renderer struct */
VkRenderPass      hud_render_pass;
VkPipeline        hud_pipeline;
VkPipelineLayout  hud_pipeline_layout;
VkBuffer          hud_vertex_buffer;
VmaAllocation     hud_vertex_alloc;
VkBuffer          hud_index_buffer;
VmaAllocation     hud_index_alloc;
void*             hud_vb_mapped;   /* persistently mapped */
void*             hud_ib_mapped;
```

### New renderer function

```c
/* Called after renderer_draw_frame(), within the same frame's command buffer.
 * Calls hud_build(), uploads result, records second renderpass, transitions
 * swapchain image to PRESENT_SRC_KHR. */
void renderer_draw_hud(Renderer* r, const HUD* hud);
```

---

## Shaders

### `shaders/hud.vert`

```glsl
#version 450
layout(location=0) in vec2 pos;
layout(location=1) in vec4 color;
layout(location=0) out vec4 v_color;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_color = color;
}
```

### `shaders/hud.frag`

```glsl
#version 450
layout(location=0) in vec4 v_color;
layout(location=0) out vec4 out_color;
void main() { out_color = v_color; }
```

Screen size is passed via a push constant (`vec2 screen_size`) so `hud_build()` can convert pixel measurements to NDC at any window size.

---

## Visual Layout

All pixel measurements converted to NDC inside `hud_build()` using `screen_size`.

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
renderer_draw_frame()       ← world; leaves image COLOR_ATTACHMENT_OPTIMAL
renderer_draw_hud()         ← HUD; transitions to PRESENT_SRC_KHR
```

`hud_init()` called after `world_create()`. No explicit `hud_destroy()` needed — no heap allocations in HUD state itself.

---

## Agent Mode

### New command

```c
/* In AgentCommand */
CMD_SELECT_SLOT,   /* { int slot; }  — 0-indexed, clamped to [0, HUD_SLOT_COUNT) */
```

JSON wire format: `{"type":"select_slot","slot":2}`

### Snapshot additions

```c
/* Added to AgentSnapshot */
int selected_slot;               /* 0–5 */
int hotbar[HUD_SLOT_COUNT];      /* BlockID for each slot */
```

JSON emission:

```json
{
  "selected_slot": 2,
  "hotbar": [1, 2, 3, 4, 5, 6]
}
```

The hotbar array is constant after init, but included in every snapshot so the agent protocol is self-describing.

---

## Testing

- Build test: both `minecraft` and `test_block_physics` compile clean
- Manual smoke test: scroll wheel cycles selected slot highlight; crosshair visible at screen center
- Agent test: `CMD_SELECT_SLOT` changes `selected_slot`; snapshot reflects correct `hotbar[]` values
