# UI Foundation Design

**Date:** 2026-03-21
**Status:** Approved

## Problem

The project has a minimal HUD (crosshair + hotbar) implemented as a flat Vulkan quad renderer with no text, no mouse interaction, and no widget system. Adding a pause menu, launcher, and future game UIs requires:

- Text rendering
- A layout system that avoids manual x/y placement everywhere
- Mouse input handling
- A scene dispatch mechanism (launcher vs. in-game)
- A clear owner for all 2D Vulkan rendering

## Scope

This spec covers:
1. `src/ui/` subsystem — UI toolkit, scene dispatch, refactored HUD
2. `scene_launcher` — a basic launcher scene with a Start button
3. `scene_game` — existing game loop wrapped as a scene, with pause overlay
4. Cursor and camera gating tied to UI/pause state

Out of scope: full world-select UI, settings screen, block interaction UI, inventory. These are future specs that build on this foundation.

---

## Directory Structure

New folder `src/ui/` introduced for the UI subsystem. Existing `src/` files stay flat; the reorganization of non-UI code is a separate future task.

```
src/
  ui/
    ui.h / ui.c        — rendering toolkit, font atlas, flexbox layout, widgets
    scene.h / scene.c  — dumb scene dispatch (no game/UI knowledge)
    hud.h / hud.c      — moved here; refactored to call ui_* primitives
```

`CMakeLists.txt` updated with new paths. Files that include `hud.h` updated from `"hud.h"` → `"ui/hud.h"`.

---

## 1. UI Toolkit (`src/ui/ui.h`, `src/ui/ui.c`)

### 1.1 Vertex Format

```c
typedef struct {
    float x, y;       /* NDC [-1, 1], converted from pixel coords */
    float u, v;       /* texture atlas coords */
    float r, g, b, a; /* color / tint */
} UiVertex;           /* 32 bytes */
```

Replaces `HudVertex`. The existing HUD pipeline is extended to use this format; the pipeline wiring in `renderer.c` is updated accordingly.

Buffer capacities increase from 256 verts / 384 indices to **4096 verts / 6144 indices** to handle text (each glyph = 1 quad = 4 verts + 6 indices; a dense screen has ~500+ glyphs).

### 1.2 Vulkan Pipeline

One pipeline handles everything — colored quads and textured text — by always sampling a font atlas texture:

```glsl
/* fragment shader */
out_color = vec4(frag_color.rgb, frag_color.a * texture(atlas, frag_uv).r);
```

- **Solid colored quads**: UV points to a 2×2 white pixel reserved in the top-left corner of the font atlas. `atlas.r = 1.0`, so `color.a * 1.0 = color.a`. Result is the plain color. ✓
- **Text glyphs**: UV points to the glyph region. `atlas.r` is the glyph alpha. Result is colored text with correct anti-aliasing. ✓

Same blend mode as current HUD (src-alpha / one-minus-src-alpha). Depth test off. Renders after the 3D pass by loading the existing swapchain image.

### 1.3 Font Atlas

- **Library**: stb_truetype (already a project dependency via FetchContent stb)
- **Font**: a public-domain monospace TTF embedded as a C byte array in `src/ui/ui.c` (same pattern as `shaders_generated.c` / `assets_generated.c`)
- **Atlas size**: 512×512, single channel R8
- **Bake size**: 20px — one size baked at startup; smaller text is scaled in the vertex layout
- **Glyph range**: ASCII 32–126 (printable characters)
- **White pixel**: top-left 2×2 pixels set to 0xFF before glyph packing

Glyph metrics stored as a 95-entry array indexed by `(char - 32)`:

```c
typedef struct {
    float u0, v0, u1, v1;   /* UV rect in atlas */
    float advance, bearing_x, bearing_y;
    int   width, height;
} UiGlyph;
```

### 1.4 Draw Primitives

Low-level, coordinate-explicit. Used internally by widgets and by callers who need precise placement (e.g. HUD crosshair):

```c
void  ui_rect(float x, float y, float w, float h, vec4 color);
void  ui_text(float x, float y, float size, const char* text, vec4 color);
float ui_text_width(const char* text, float size);   /* for centering */
```

Coordinates are screen pixels (0,0 = top-left). Conversion to NDC happens in `ui_rect` / glyph emit, identical to the current `emit_quad` approach.

### 1.5 Flexbox Layout

Immediate-mode flexbox. Widget calls inside a `ui_flex_begin / ui_flex_end` block record descriptors to an internal buffer instead of drawing immediately. `ui_flex_end()` computes layout, emits geometry, and resolves click state against the computed rects.

Click state uses the **previous frame's computed rects** — one-frame lag, imperceptible at 60fps. This is standard immediate-mode IMGUI practice.

**Supported features:**

| Feature | Supported |
|---------|-----------|
| Direction: row / column | ✅ |
| justify-content: start, center, end, space-between | ✅ |
| align-items: start, center, end, stretch | ✅ |
| flex-grow (proportional fill) | ✅ |
| gap, padding | ✅ |
| Auto height (fit children) | ✅ |
| Nesting (flex inside flex) | ✅ |
| flex-wrap | ❌ |
| flex-shrink | ❌ |

**API:**

```c
typedef enum { UI_DIR_COLUMN, UI_DIR_ROW } UiDir;
typedef enum { UI_JUSTIFY_START, UI_JUSTIFY_CENTER,
               UI_JUSTIFY_END, UI_JUSTIFY_SPACE_BETWEEN } UiJustify;
typedef enum { UI_ALIGN_START, UI_ALIGN_CENTER,
               UI_ALIGN_END, UI_ALIGN_STRETCH } UiAlign;
typedef enum {
    UI_ANCHOR_CENTER,
    UI_ANCHOR_TOP_LEFT, UI_ANCHOR_TOP_RIGHT,
    UI_ANCHOR_BOTTOM_LEFT, UI_ANCHOR_BOTTOM_RIGHT,
} UiAnchor;

void ui_flex_begin(UiAnchor anchor,
                  float w, float h,        /* h=0 → auto-fit children */
                  UiDir dir,
                  UiJustify justify,
                  UiAlign align,
                  float gap,
                  float padding);
void ui_flex_end(void);
```

**Widgets** (valid inside or outside a flex container):

```c
/* Returns true the frame it is clicked */
bool ui_button(int id, const char* label, float flex);  /* flex=0 → fixed height */
void ui_label(const char* text, float size, vec4 color, float flex);
void ui_spacer(float flex);
```

Usage example — a centered pause menu:

```c
ui_flex_begin(UI_ANCHOR_CENTER, 220, 0, UI_DIR_COLUMN,
              UI_JUSTIFY_CENTER, UI_ALIGN_STRETCH, 10, 20);
    if (ui_button(1, "Resume",       0)) g_pause_open = false;
    if (ui_button(2, "Quit to Menu", 0)) scene_set(&scene_launcher);
    if (ui_button(3, "Quit",         0)) exit(0);
ui_flex_end();
```

### 1.6 Input

```c
void ui_set_input(float mouse_x, float mouse_y,
                  bool left_pressed, bool left_released);
```

Called each frame from `main.c` before `scene_update()`. GLFW callbacks (`glfwSetMouseButtonCallback`, `glfwSetCursorPosCallback`) write into a small input struct; `ui_set_input` is called at the top of the frame with those values.

### 1.7 Frame Lifecycle

Called from `renderer_frame.c`:

```c
ui_frame_begin(float screen_w, float screen_h);
/* ... scene_render() calls ui_* to build geometry ... */
ui_frame_end();   /* uploads vertex/index data, issues draw call */
```

Replaces the current `hud_build()` + manual memcpy + draw sequence.

---

## 2. Scene System (`src/ui/scene.h`, `src/ui/scene.c`)

Dumb dispatch. Knows nothing about the game world, UI, or rendering details — those are the scenes' concern.

```c
typedef struct {
    void (*on_enter)(void);
    void (*on_exit)(void);
    void (*update)(float dt);
    void (*render)(void);   /* calls ui_* + renderer_draw_frame */
} Scene;

void        scene_set(const Scene* s);   /* on_exit → swap → on_enter */
void        scene_update(float dt);
void        scene_render(void);
const Scene* scene_current(void);
```

Two scenes wired up in `main.c`:

### 2.1 `scene_launcher`

- `on_enter`: unlock cursor (`GLFW_CURSOR_NORMAL`), stop any running server/client
- `update`: calls `ui_set_input`, then nothing else (no world ticking)
- `render`: draws a centered panel with a "Start Game" button; clicking it starts a local server + client and transitions to `scene_game`

Minimal for now — a single button. World-select and settings are future work on top of this scene.

### 2.2 `scene_game`

- `on_enter`: lock cursor (`GLFW_CURSOR_DISABLED`), initialize world/server/client
- `update`: ticks world, handles input. If `pause_open`, skips camera look update. Game world continues ticking regardless (multiplayer-safe). Singleplayer freeze is deferred to a future spec.
- `render`: calls `renderer_draw_frame` (3D scene + HUD), then if `pause_open` calls `ui_screen_pause()`

**Pause overlay:**

```c
static bool pause_open = false;
```

ESC in `key_callback` toggles `pause_open` and calls `glfwSetInputMode` to toggle cursor. Camera mouse look is gated:

```c
/* in mouse_callback */
if (!pause_open) camera_update_look(dx, dy);
```

`ui_screen_pause()` is a static function in `scene_game`'s translation unit using `ui_flex_begin/end` + `ui_button`. Not in `ui.c` — UI toolkit doesn't know about game state.

---

## 3. HUD Refactor (`src/ui/hud.h`, `src/ui/hud.c`)

`hud.c` moves to `src/ui/` and loses all Vulkan code. `emit_quad()` is replaced by `ui_rect()`. The file shrinks to pure layout logic:

```c
void hud_build(const HUD* hud, float sw, float sh);
```

Called from `scene_game`'s render function before `ui_frame_end()`. Draws crosshair and hotbar using `ui_rect()` + `ui_text()`.

`HUD` struct and `HudVertex` / `HUD_MAX_VERTS` / `HUD_MAX_INDICES` constants move to `ui.h` / `ui.c`. `hud.h` retains only the `HUD` gameplay struct and `hud_build` / `hud_init` / `hud_selected_block` declarations.

---

## 4. Files Changed

| File | Change |
|------|--------|
| `src/ui/ui.h` | New — toolkit public API |
| `src/ui/ui.c` | New — font atlas, Vulkan pipeline/buffers, flexbox, widgets |
| `src/ui/scene.h` | New — Scene struct + dispatch API |
| `src/ui/scene.c` | New — scene_set, scene_launcher, scene_game |
| `src/ui/hud.h` | Moved + trimmed — gameplay struct + build API only |
| `src/ui/hud.c` | Moved + refactored — calls ui_rect/ui_text, no Vulkan |
| `src/renderer.c` | HUD pipeline → UI pipeline; UiVertex format; add atlas texture |
| `src/renderer_frame.c` | `hud_build()` → `ui_frame_begin/end()`; include path updated |
| `src/main.c` | Add mouse button callback; scene dispatch loop; ESC handler |
| `shaders/hud.vert` | Add UV input; rename to `ui.vert` |
| `shaders/hud.frag` | Add atlas sampler; rename to `ui.frag` |
| `CMakeLists.txt` | Updated source paths; add ui/ files; rename shader targets |

---

## 5. What Is NOT Changing

- The 3D renderer pipeline, world, physics, networking — untouched
- `HUD` gameplay struct fields (`selected_slot`, `slot_blocks`) — unchanged
- The immediate-mode per-frame rebuild philosophy — same as current HUD
- Coordinate system for callers — still screen pixels, 0,0 top-left
