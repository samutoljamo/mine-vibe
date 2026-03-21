# UI Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hand-rolled HUD Vulkan pipeline with a unified UI rendering system that supports font atlas text and will underpin all future UI — while keeping the in-game HUD visually identical.

**Architecture:** New `src/ui/` folder owns all 2D rendering. `ui.c` owns the Vulkan pipeline (replacing the old `hud_*` pipeline in `renderer.c`), bakes a stb_truetype font atlas at startup, and exposes `ui_rect()` / `ui_text()` primitives. `hud.c` is moved to `src/ui/` and rewritten to call those primitives — zero Vulkan code in hud.c. The renderer wires `ui_frame_begin/end` around the HUD draw call; everything else in the renderer is untouched. Plan 2 adds flexbox layout, scene system, and the pause menu on top of this foundation.

**Tech Stack:** C11, Vulkan (via volk), VMA, stb_truetype, CMake/ctest. All build/test commands run inside `distrobox enter cyberismo`.

---

## File Map

| File | Change |
|------|--------|
| `assets/fonts/ui.ttf` | New — public-domain monospace TTF committed to repo |
| `src/ui/font_data.h` | New — generated C byte array of the TTF |
| `src/ui/ui.h` | New — public API: UiVertex, types, all function declarations |
| `src/ui/ui.c` | New — font baking, Vulkan pipeline, draw primitives |
| `shaders/ui.vert` | New — replaces hud.vert; adds UV in/out |
| `shaders/ui.frag` | New — replaces hud.frag; samples atlas |
| `src/ui/hud.h` | Moved + trimmed — only HUD gameplay struct + build/init/selected_block |
| `src/ui/hud.c` | Moved + rewritten — calls ui_rect/ui_text, no Vulkan |
| `src/renderer.h` | Remove all `hud_*` fields; `renderer_draw_frame` keeps `const HUD*` (removed in Plan 2) |
| `src/renderer.c` | Remove HUD pipeline/buffer/framebuffer/renderpass creation; call `ui_init`/`ui_cleanup` |
| `src/renderer_frame.c` | Replace hud_build+memcpy block with `ui_frame_begin` / `hud_build` / `ui_frame_end` |
| `tests/test_ui.c` | New — tests `ui_text_width` without Vulkan |
| `tests/test_hud.c` | Deleted — tests HudVertex directly; no longer valid after refactor |
| `CMakeLists.txt` | Add ui/ sources, test_ui target, ui shaders; remove hud shaders |

---

## Task 1: Font asset and generated header

**Files:**
- Create: `assets/fonts/ui.ttf`
- Create: `tools/gen_font_data.py`
- Create: `src/ui/font_data.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Download a public-domain monospace TTF**

Download [Cousine-Regular.ttf](https://fonts.google.com/specimen/Cousine) (Apache 2.0 license) or any free monospace TTF. Save it as `assets/fonts/ui.ttf`.

```bash
mkdir -p assets/fonts
# Download manually and place at assets/fonts/ui.ttf
ls -la assets/fonts/ui.ttf  # verify it exists and is non-zero
```

- [ ] **Step 2: Create the font header generator**

Create `tools/gen_font_data.py`:

```python
#!/usr/bin/env python3
"""Embed a binary file as a C byte array header."""
import sys

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} input.ttf output.h", file=sys.stderr)
    sys.exit(1)

with open(sys.argv[1], 'rb') as f:
    data = f.read()

with open(sys.argv[2], 'w') as out:
    out.write('#pragma once\n')
    out.write(f'/* Auto-generated from {sys.argv[1]} — do not edit */\n')
    out.write(f'static const unsigned int  ui_font_data_len = {len(data)};\n')
    out.write( 'static const unsigned char ui_font_data[] = {\n')
    for i, b in enumerate(data):
        if i % 16 == 0:
            out.write('    ')
        out.write(f'0x{b:02x},')
        if i % 16 == 15:
            out.write('\n')
    out.write('\n};\n')
```

- [ ] **Step 3: Generate the header**

```bash
mkdir -p src/ui
python3 tools/gen_font_data.py assets/fonts/ui.ttf src/ui/font_data.h
head -5 src/ui/font_data.h  # verify it looks like a C header
```

Expected: first few lines show `#pragma once`, `ui_font_data_len`, `ui_font_data[]`.

- [ ] **Step 4: Commit**

```bash
git add assets/fonts/ui.ttf src/ui/font_data.h tools/gen_font_data.py
git commit -m "assets: add Cousine-Regular as ui.ttf; generate font_data.h"
```

---

## Task 2: New UI shaders

**Files:**
- Create: `shaders/ui.vert`
- Create: `shaders/ui.frag`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `shaders/ui.vert`**

```glsl
#version 450

layout(location = 0) in vec2 in_pos;    /* NDC [-1, 1] */
layout(location = 1) in vec2 in_uv;     /* atlas UV */
layout(location = 2) in vec4 in_color;  /* RGBA tint */

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
    frag_uv    = in_uv;
    frag_color = in_color;
}
```

- [ ] **Step 2: Create `shaders/ui.frag`**

```glsl
#version 450

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    /* Solid quads: UV = white pixel → atlas.r = 1.0 → color passes through.
     * Glyphs: UV = glyph region → atlas.r = glyph alpha. */
    out_color = vec4(frag_color.rgb, frag_color.a * texture(atlas, frag_uv).r);
}
```

- [ ] **Step 3: Add shader targets to `CMakeLists.txt`**

After the existing `compile_shader(${SHADER_DIR}/hud.vert)` and `compile_shader(${SHADER_DIR}/hud.frag)` lines, add:

```cmake
compile_shader(${SHADER_DIR}/ui.vert)
compile_shader(${SHADER_DIR}/ui.frag)
```

- [ ] **Step 4: Build shaders to verify they compile**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake .. 2>&1 | tail -3 && cmake --build . --target shaders 2>&1 | tail -10"
```

Expected: `Compiling shader ui.vert`, `Compiling shader ui.frag`, no errors. GLSL errors will show here — fix before proceeding.

- [ ] **Step 5: Commit**

```bash
git add shaders/ui.vert shaders/ui.frag CMakeLists.txt
git commit -m "shader: add ui.vert and ui.frag for unified UI rendering pipeline"
```

---

## Task 3: ui.h types + ui.c skeleton + CMakeLists

**Files:**
- Create: `src/ui/ui.h`
- Create: `src/ui/ui.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/ui/ui.h`**

```c
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <cglm/cglm.h>

/* Forward-declare Renderer to avoid circular include (renderer.h includes hud.h) */
struct Renderer;

/* ------------------------------------------------------------------ */
/*  Vertex format                                                      */
/* ------------------------------------------------------------------ */

#define UI_MAX_VERTS   4096
#define UI_MAX_INDICES 6144

typedef struct {
    float x, y;       /* NDC [-1, 1] — converted from pixel coords in emit */
    float u, v;       /* atlas UV */
    float r, g, b, a; /* RGBA tint */
} UiVertex;           /* 32 bytes */

/* ------------------------------------------------------------------ */
/*  Layout types (used in Plan 2 flexbox; declared here for completeness) */
/* ------------------------------------------------------------------ */

typedef enum { UI_DIR_COLUMN, UI_DIR_ROW }                      UiDir;
typedef enum { UI_JUSTIFY_START, UI_JUSTIFY_CENTER,
               UI_JUSTIFY_END,   UI_JUSTIFY_SPACE_BETWEEN }     UiJustify;
typedef enum { UI_ALIGN_START, UI_ALIGN_CENTER,
               UI_ALIGN_END,   UI_ALIGN_STRETCH }                UiAlign;
typedef enum { UI_ANCHOR_CENTER,
               UI_ANCHOR_TOP_LEFT,    UI_ANCHOR_TOP_RIGHT,
               UI_ANCHOR_BOTTOM_LEFT, UI_ANCHOR_BOTTOM_RIGHT }  UiAnchor;

/* ------------------------------------------------------------------ */
/*  Init / shutdown                                                    */
/* ------------------------------------------------------------------ */

/* Bake font atlas from embedded TTF (no Vulkan). Called standalone for tests. */
bool ui_font_bake(void);

/* Full init: ui_font_bake + create Vulkan pipeline, buffers, atlas texture. */
void ui_init(struct Renderer* r);
void ui_cleanup(struct Renderer* r);

/* Call when swapchain is recreated (framebuffers must be rebuilt). */
void ui_on_swapchain_recreate(struct Renderer* r);

/* ------------------------------------------------------------------ */
/*  Frame lifecycle                                                    */
/* ------------------------------------------------------------------ */

/* Call before any ui_* draw calls. Stores cmd, image_index, frame_index for use by ui_frame_end. */
void ui_frame_begin(VkCommandBuffer cmd, uint32_t image_index,
                    int frame_index, float screen_w, float screen_h);

/* Upload vertex/index data and issue the draw call into the stored cmd. */
void ui_frame_end(void);

/* ------------------------------------------------------------------ */
/*  Input (Plan 2 — declared now, implemented in Plan 2)              */
/* ------------------------------------------------------------------ */

void ui_set_input(float mouse_x, float mouse_y, bool pressed, bool released);

/* ------------------------------------------------------------------ */
/*  Draw primitives                                                    */
/* ------------------------------------------------------------------ */

/* All coordinates are screen pixels, (0,0) = top-left, y increases down. */
void  ui_rect(float x, float y, float w, float h, vec4 color);
void  ui_text(float x, float y, float size, const char* text, vec4 color);
float ui_text_width(const char* text, float size);

/* ------------------------------------------------------------------ */
/*  Layout (Plan 2 — stubs now, full impl in Plan 2)                  */
/* ------------------------------------------------------------------ */

void ui_flex_begin(UiAnchor anchor, float w, float h,
                   UiDir dir, UiJustify justify, UiAlign align,
                   float gap, float padding);
void ui_flex_end(void);

/* Widgets — return true the frame they are clicked */
bool ui_button(int id, const char* label, float flex);
void ui_label(const char* text, float size, vec4 color, float flex);
void ui_spacer(float flex);

#endif /* UI_H */
```

- [ ] **Step 2: Create `src/ui/ui.c` (skeleton — all stubs)**

```c
#include "ui.h"
#include "../renderer.h"
#include "../pipeline.h"
#include "font_data.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Atlas constants                                                    */
/* ------------------------------------------------------------------ */

#define ATLAS_W      512
#define ATLAS_H      512
#define ATLAS_BAKE_PX 20.0f
#define GLYPH_FIRST  32
#define GLYPH_COUNT  95   /* ASCII 32-126 */

typedef struct {
    float u0, v0, u1, v1;
    float advance, bearing_x, bearing_y;
    float width, height;
} UiGlyph;

/* ------------------------------------------------------------------ */
/*  Module globals                                                     */
/* ------------------------------------------------------------------ */

static UiGlyph   g_glyphs[GLYPH_COUNT];
static uint8_t   g_atlas_cpu[ATLAS_W * ATLAS_H];  /* R8 bitmap */
static bool      g_font_baked = false;

/* Vulkan objects — populated by ui_init */
static VkDevice          g_device;
static VmaAllocator      g_allocator;
static VkRenderPass      g_render_pass;
static VkPipeline        g_pipeline;
static VkPipelineLayout  g_pipeline_layout;
static VkFramebuffer*    g_framebuffers;      /* one per swapchain image */
static uint32_t          g_framebuffer_count;
static VkBuffer          g_vb[2];
static VmaAllocation     g_vb_alloc[2];
static VkBuffer          g_ib[2];
static VmaAllocation     g_ib_alloc[2];
static void*             g_vb_mapped[2];
static void*             g_ib_mapped[2];
static VkImage           g_atlas_image;
static VmaAllocation     g_atlas_alloc;
static VkImageView       g_atlas_view;
static VkSampler         g_sampler;
static VkDescriptorSetLayout g_dsl;
static VkDescriptorPool  g_desc_pool;
static VkDescriptorSet   g_desc_sets[2];

/* Per-frame draw state */
static UiVertex  g_verts[UI_MAX_VERTS];
static uint32_t  g_indices[UI_MAX_INDICES];
static uint32_t  g_vert_count;
static uint32_t  g_idx_count;
static float     g_screen_w;
static float     g_screen_h;
static VkCommandBuffer g_cmd;
static int       g_frame_index;
static uint32_t  g_image_index;

/* ------------------------------------------------------------------ */
/*  Font baking (no Vulkan)                                            */
/* ------------------------------------------------------------------ */

bool ui_font_bake(void)
{
    /* TODO: implement in Task 4 */
    return false;
}

/* ------------------------------------------------------------------ */
/*  Vulkan init                                                        */
/* ------------------------------------------------------------------ */

void ui_init(struct Renderer* r)
{
    /* TODO: implement in Task 5 */
    (void)r;
}

void ui_cleanup(struct Renderer* r)
{
    /* TODO: implement in Task 5 */
    (void)r;
}

void ui_on_swapchain_recreate(struct Renderer* r)
{
    /* TODO: implement in Task 5 */
    (void)r;
}

/* ------------------------------------------------------------------ */
/*  Frame lifecycle                                                    */
/* ------------------------------------------------------------------ */

void ui_frame_begin(VkCommandBuffer cmd, uint32_t image_index,
                    int frame_index, float sw, float sh)
{
    g_cmd         = cmd;
    g_image_index = image_index;
    g_frame_index = frame_index;
    g_screen_w    = sw;
    g_screen_h    = sh;
    g_vert_count  = 0;
    g_idx_count   = 0;
}

void ui_frame_end(void)
{
    /* TODO: implement in Task 5 */
}

/* ------------------------------------------------------------------ */
/*  Input                                                              */
/* ------------------------------------------------------------------ */

void ui_set_input(float mx, float my, bool pressed, bool released)
{
    /* TODO: Plan 2 */
    (void)mx; (void)my; (void)pressed; (void)released;
}

/* ------------------------------------------------------------------ */
/*  Draw primitives                                                    */
/* ------------------------------------------------------------------ */

void ui_rect(float x, float y, float w, float h, vec4 color)
{
    /* TODO: implement in Task 5 */
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

void ui_text(float x, float y, float size, const char* text, vec4 color)
{
    /* TODO: implement in Task 7 */
    (void)x; (void)y; (void)size; (void)text; (void)color;
}

float ui_text_width(const char* text, float size)
{
    /* TODO: implement in Task 4 */
    (void)text; (void)size;
    return 0.0f;
}

/* ------------------------------------------------------------------ */
/*  Layout stubs (Plan 2)                                             */
/* ------------------------------------------------------------------ */

void ui_flex_begin(UiAnchor a, float w, float h, UiDir d,
                   UiJustify j, UiAlign al, float gap, float pad)
{
    (void)a; (void)w; (void)h; (void)d;
    (void)j; (void)al; (void)gap; (void)pad;
}
void  ui_flex_end(void)     {}
bool  ui_button(int id, const char* l, float f) { (void)id;(void)l;(void)f; return false; }
void  ui_label(const char* t, float s, vec4 c, float f) { (void)t;(void)s;(void)c;(void)f; }
void  ui_spacer(float f) { (void)f; }
```

- [ ] **Step 3: Add ui/ sources to `CMakeLists.txt`**

In the `add_executable(minecraft ...)` source list, add after `src/hud.c`:

```cmake
    src/ui/ui.c
```

Also add the stb include directory for ui.c:

In the `target_include_directories(minecraft PRIVATE ...)` block, the `${stb_SOURCE_DIR}` should already be listed. Verify it is; if not, add it.

- [ ] **Step 4: Build to verify it compiles**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake .. 2>&1 | tail -5 && cmake --build . --target minecraft 2>&1 | tail -10"
```

Expected: builds successfully. If there are missing include errors (e.g. `VkCommandBuffer` not found), check that `src/ui/ui.c` includes `"../renderer.h"` which pulls in volk.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui.h src/ui/ui.c CMakeLists.txt
git commit -m "feat: add ui.h/ui.c skeleton with UiVertex types and stub implementations"
```

---

## Task 4: Font baking + ui_text_width + tests

**Files:**
- Modify: `src/ui/ui.c` (implement `ui_font_bake`, `ui_text_width`)
- Create: `tests/test_ui.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/ui/ui.h"

static void test_font_bake_succeeds(void)
{
    bool ok = ui_font_bake();
    assert(ok && "font bake must succeed");
    printf("PASS: test_font_bake_succeeds\n");
}

static void test_text_width_empty(void)
{
    ui_font_bake();
    float w = ui_text_width("", 20.0f);
    assert(w == 0.0f);
    printf("PASS: test_text_width_empty\n");
}

static void test_text_width_positive(void)
{
    ui_font_bake();
    float w = ui_text_width("Hello", 20.0f);
    assert(w > 0.0f && "non-empty string must have positive width");
    printf("PASS: test_text_width_positive\n");
}

static void test_text_width_scales(void)
{
    ui_font_bake();
    float w20 = ui_text_width("ABC", 20.0f);
    float w40 = ui_text_width("ABC", 40.0f);
    /* 40px text should be approximately 2x the width of 20px text */
    assert(fabsf(w40 - 2.0f * w20) < 2.0f && "text width must scale with size");
    printf("PASS: test_text_width_scales\n");
}

static void test_text_width_longer_is_wider(void)
{
    ui_font_bake();
    float w1 = ui_text_width("A", 20.0f);
    float w3 = ui_text_width("AAA", 20.0f);
    assert(w3 > w1 && "longer string must be wider");
    printf("PASS: test_text_width_longer_is_wider\n");
}

int main(void)
{
    test_font_bake_succeeds();
    test_text_width_empty();
    test_text_width_positive();
    test_text_width_scales();
    test_text_width_longer_is_wider();
    printf("All ui tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Add test_ui CMake target**

Add to `CMakeLists.txt` after the `test_net` block:

```cmake
add_executable(test_ui
    tests/test_ui.c
    src/ui/ui.c
)
target_include_directories(test_ui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${cglm_SOURCE_DIR}/include
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_ui PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_ui PRIVATE cglm Vulkan::Headers m)
add_test(NAME ui COMMAND test_ui)
```

- [ ] **Step 3: Run test to confirm failure**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake .. 2>&1 | tail -3 && cmake --build . --target test_ui 2>&1 | tail -5 && ./test_ui"
```

Expected: `test_font_bake_succeeds` fails (assert fires) because `ui_font_bake` returns false.

- [ ] **Step 4: Implement `ui_font_bake` in `src/ui/ui.c`**

Replace the `ui_font_bake` stub:

```c
bool ui_font_bake(void)
{
    if (g_font_baked) return true;

    /* Set white pixel region (top-left 2×2) before packing glyphs */
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            g_atlas_cpu[y * ATLAS_W + x] = 0xFF;

    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, g_atlas_cpu, ATLAS_W, ATLAS_H, ATLAS_W, 1, NULL)) {
        fprintf(stderr, "ui_font_bake: stbtt_PackBegin failed\n");
        return false;
    }

    stbtt_packedchar packed[GLYPH_COUNT];
    stbtt_PackFontRange(&pc, ui_font_data, 0, ATLAS_BAKE_PX,
                        GLYPH_FIRST, GLYPH_COUNT, packed);
    stbtt_PackEnd(&pc);

    /* Build glyph metric table */
    for (int i = 0; i < GLYPH_COUNT; i++) {
        stbtt_packedchar* p = &packed[i];
        g_glyphs[i].u0       = (float)p->x0 / ATLAS_W;
        g_glyphs[i].v0       = (float)p->y0 / ATLAS_H;
        g_glyphs[i].u1       = (float)p->x1 / ATLAS_W;
        g_glyphs[i].v1       = (float)p->y1 / ATLAS_H;
        g_glyphs[i].width    = (float)(p->x1 - p->x0);
        g_glyphs[i].height   = (float)(p->y1 - p->y0);
        g_glyphs[i].bearing_x = p->xoff;
        g_glyphs[i].bearing_y = p->yoff;
        g_glyphs[i].advance   = p->xadvance;
    }

    g_font_baked = true;
    return true;
}
```

- [ ] **Step 5: Implement `ui_text_width` in `src/ui/ui.c`**

Replace the stub:

```c
float ui_text_width(const char* text, float size)
{
    if (!g_font_baked) return 0.0f;
    float scale = size / ATLAS_BAKE_PX;
    float w = 0.0f;
    for (const char* p = text; *p; p++) {
        int ci = (unsigned char)*p - GLYPH_FIRST;
        if (ci < 0 || ci >= GLYPH_COUNT) continue;
        w += g_glyphs[ci].advance * scale;
    }
    return w;
}
```

- [ ] **Step 6: Run tests to verify they pass**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . --target test_ui 2>&1 | tail -5 && ./test_ui"
```

Expected: all 5 `PASS:` lines, then `All ui tests passed.`

- [ ] **Step 7: Run full test suite**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -5 && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/ui/ui.c tests/test_ui.c CMakeLists.txt
git commit -m "feat: font atlas baking and ui_text_width via stb_truetype"
```

---

## Task 5: UI Vulkan infrastructure + wire into renderer

**Files:**
- Modify: `src/ui/ui.c` (implement `ui_init`, `ui_cleanup`, `ui_on_swapchain_recreate`, `ui_frame_begin`, `ui_frame_end`, `ui_rect`)
- Modify: `src/renderer.h` (remove hud_* fields)
- Modify: `src/renderer.c` (remove HUD pipeline/buffer creation; call ui_init/ui_cleanup)
- Modify: `src/renderer_frame.c` (replace hud_build block with ui_frame_begin/end)

This is the largest task. Work through it step by step. The game will be temporarily broken between steps 1 and 6.

- [ ] **Step 1: Remove hud_* fields from `src/renderer.h`**

In the `Renderer` struct, remove these fields:

```c
/* Remove these: */
VkRenderPass      hud_render_pass;
VkPipeline        hud_pipeline;
VkPipelineLayout  hud_pipeline_layout;
VkFramebuffer*    hud_framebuffers;
VkBuffer          hud_vertex_buffer[MAX_FRAMES_IN_FLIGHT];
VmaAllocation     hud_vertex_alloc[MAX_FRAMES_IN_FLIGHT];
VkBuffer          hud_index_buffer[MAX_FRAMES_IN_FLIGHT];
VmaAllocation     hud_index_alloc[MAX_FRAMES_IN_FLIGHT];
void*             hud_vb_mapped[MAX_FRAMES_IN_FLIGHT];
void*             hud_ib_mapped[MAX_FRAMES_IN_FLIGHT];
```

- [ ] **Step 2: Remove HUD pipeline/buffer/framebuffer code from `src/renderer.c`**

Remove the following blocks from `renderer_init()` (the exact line ranges shift after Task 1's edit — search for these patterns):

a) `create_hud_framebuffers(r)` function and its call — remove both
b) HUD render pass creation block (search: `hud_color_ref`, `hud_subpass`, `hud_dep`)
c) HUD pipeline creation block (search: `hud_pipeline_layout`, `hud_pipeline`)
d) HUD vertex/index buffer creation loop (search: `hud_vertex_buffer`, `hud_vertex_alloc`)

Also update `renderer_cleanup()` — remove cleanup of all hud_* objects:
```c
/* Remove: */
vkDestroyPipeline(r->device, r->hud_pipeline, NULL);
vkDestroyPipelineLayout(r->device, r->hud_pipeline_layout, NULL);
vkDestroyRenderPass(r->device, r->hud_render_pass, NULL);
for (uint32_t i = 0; i < r->swapchain.image_count; i++)
    vkDestroyFramebuffer(r->device, r->hud_framebuffers[i], NULL);
free(r->hud_framebuffers);
for (int fi = 0; fi < MAX_FRAMES_IN_FLIGHT; fi++) {
    vmaDestroyBuffer(r->allocator, r->hud_vertex_buffer[fi], r->hud_vertex_alloc[fi]);
    vmaDestroyBuffer(r->allocator, r->hud_index_buffer[fi], r->hud_index_alloc[fi]);
}
```

Also update `renderer_recreate_swapchain()` — remove HUD framebuffer destruction/recreation block.

Add `ui_init(r)` call at the end of `renderer_init()`, and `ui_cleanup(r)` at the start of `renderer_cleanup()`:
```c
/* In renderer_init, at the end before return: */
ui_init(r);

/* In renderer_cleanup, near the top: */
ui_cleanup(r);
```

Add `#include "ui/ui.h"` to `src/renderer.c`.

- [ ] **Step 3: Update `src/renderer_frame.c` HUD draw block**

Find the HUD draw section (lines ~163-193) and replace it with:

```c
    /* UI pass — HUD and any screen overlays */
    float sw = (float)r->swapchain.extent.width;
    float sh = (float)r->swapchain.extent.height;
    ui_frame_begin(cmd, image_index, r->current_frame, sw, sh);
    if (hud) hud_build(hud, sw, sh);
    ui_frame_end();
```

Also add `#include "ui/ui.h"` to `src/renderer_frame.c`.

- [ ] **Step 4: Implement `ui_init` in `src/ui/ui.c`**

The UI init needs the Renderer struct internals. Add `#include "../renderer.h"` at the top (already there from the skeleton). Implement:

```c
void ui_init(struct Renderer* r)
{
    g_device    = r->device;
    g_allocator = r->allocator;

    /* Bake font atlas (CPU side) */
    if (!ui_font_bake()) {
        fprintf(stderr, "ui_init: font bake failed\n");
        return;
    }

    /* ---- Atlas texture ---- */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8_UNORM,
        .extent        = { ATLAS_W, ATLAS_H, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo img_alloc_ci = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    vmaCreateImage(r->allocator, &img_ci, &img_alloc_ci,
                   &g_atlas_image, &g_atlas_alloc, NULL);

    /* Upload atlas via staging buffer */
    VkDeviceSize atlas_size = ATLAS_W * ATLAS_H;
    VkBuffer      staging;
    VmaAllocation staging_alloc;
    void*         staging_mapped;
    VkBufferCreateInfo stg_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = atlas_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo stg_alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo stg_info;
    vmaCreateBuffer(r->allocator, &stg_ci, &stg_alloc_ci,
                    &staging, &staging_alloc, &stg_info);
    staging_mapped = stg_info.pMappedData;
    memcpy(staging_mapped, g_atlas_cpu, atlas_size);

    /* One-time command buffer for layout transition + copy */
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);

    /* UNDEFINED → TRANSFER_DST_OPTIMAL */
    VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = g_atlas_image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    /* Copy buffer → image */
    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { ATLAS_W, ATLAS_H, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, g_atlas_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL */
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    renderer_end_single_cmd(r, cmd);
    vmaDestroyBuffer(r->allocator, staging, staging_alloc);

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = g_atlas_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCreateImageView(r->device, &view_ci, NULL, &g_atlas_view);

    /* Sampler */
    VkSamplerCreateInfo samp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    vkCreateSampler(r->device, &samp_ci, NULL, &g_sampler);

    /* ---- Descriptor set layout ---- */
    VkDescriptorSetLayoutBinding atlas_bind = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dsl_ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &atlas_bind,
    };
    vkCreateDescriptorSetLayout(r->device, &dsl_ci, NULL, &g_dsl);

    /* ---- Descriptor pool (UI-private) ---- */
    VkDescriptorPoolSize pool_sz = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_FRAMES_IN_FLIGHT,
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = MAX_FRAMES_IN_FLIGHT,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_sz,
    };
    vkCreateDescriptorPool(r->device, &pool_ci, NULL, &g_desc_pool);

    /* ---- Allocate + update descriptor sets ---- */
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) layouts[i] = g_dsl;
    VkDescriptorSetAllocateInfo alloc_i = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = g_desc_pool,
        .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
        .pSetLayouts        = layouts,
    };
    vkAllocateDescriptorSets(r->device, &alloc_i, g_desc_sets);

    VkDescriptorImageInfo img_info = {
        .sampler     = g_sampler,
        .imageView   = g_atlas_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkWriteDescriptorSet w = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_desc_sets[i],
            .dstBinding      = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo      = &img_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &w, 0, NULL);
    }

    /* ---- Render pass (load swapchain image → present) ---- */
    VkAttachmentDescription color_att = {
        .format         = r->swapchain.image_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rp_ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &color_att,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep,
    };
    vkCreateRenderPass(r->device, &rp_ci, NULL, &g_render_pass);

    /* ---- Framebuffers (one per swapchain image) ---- */
    g_framebuffer_count = r->swapchain.image_count;
    g_framebuffers = calloc(g_framebuffer_count, sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < g_framebuffer_count; i++) {
        VkFramebufferCreateInfo fb_ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &r->swapchain.image_views[i],
            .width           = r->swapchain.extent.width,
            .height          = r->swapchain.extent.height,
            .layers          = 1,
        };
        vkCreateFramebuffer(r->device, &fb_ci, NULL, &g_framebuffers[i]);
    }

    /* ---- Pipeline ---- */
    /* Load compiled SPIR-V from build dir — use public helper from pipeline.h */
    VkShaderModule vert_mod = pipeline_load_shader_module(r->device,
        "shaders/ui.vert.spv");
    VkShaderModule frag_mod = pipeline_load_shader_module(r->device,
        "shaders/ui.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vert_mod, .pName = "main" },
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = frag_mod, .pName = "main" },
    };

    VkVertexInputBindingDescription bind = {
        .binding   = 0,
        .stride    = sizeof(UiVertex),  /* 32 bytes */
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attrs[3] = {
        { .location=0, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,         .offset=0  }, /* pos */
        { .location=1, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,         .offset=8  }, /* uv  */
        { .location=2, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT,   .offset=16 }, /* color */
    };

    VkPipelineVertexInputStateCreateInfo vert_in = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1, .pVertexBindingDescriptions   = &bind,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states,
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &g_dsl,
    };
    vkCreatePipelineLayout(r->device, &layout_ci, NULL, &g_pipeline_layout);

    VkGraphicsPipelineCreateInfo pipe_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vert_in,
        .pInputAssemblyState = &ia,
        .pViewportState      = &vp,
        .pRasterizationState = &rast,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dyn,
        .layout              = g_pipeline_layout,
        .renderPass          = g_render_pass,
        .subpass             = 0,
    };
    vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipe_ci, NULL, &g_pipeline);

    vkDestroyShaderModule(r->device, vert_mod, NULL);
    vkDestroyShaderModule(r->device, frag_mod, NULL);

    /* ---- Per-frame vertex/index buffers ---- */
    for (int fi = 0; fi < MAX_FRAMES_IN_FLIGHT; fi++) {
        VkBufferCreateInfo vb_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = UI_MAX_VERTS * sizeof(UiVertex),
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        };
        VmaAllocationCreateInfo vb_alloc_ci = {
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo vb_info;
        vmaCreateBuffer(r->allocator, &vb_ci, &vb_alloc_ci,
                        &g_vb[fi], &g_vb_alloc[fi], &vb_info);
        g_vb_mapped[fi] = vb_info.pMappedData;

        VkBufferCreateInfo ib_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = UI_MAX_INDICES * sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        };
        VmaAllocationInfo ib_info;
        vmaCreateBuffer(r->allocator, &ib_ci, &vb_alloc_ci,
                        &g_ib[fi], &g_ib_alloc[fi], &ib_info);
        g_ib_mapped[fi] = ib_info.pMappedData;
    }
}
```

**Note:** The atlas upload uses `renderer_begin_single_cmd` / `renderer_end_single_cmd` (declared in `renderer.h`, already used by `texture.c` and `player_model.c`). The pipeline uses `pipeline_load_shader_module` (declared in `pipeline.h`, already used by all existing pipelines). Include both headers at the top of `ui.c`.

- [ ] **Step 5: Implement `ui_cleanup` and `ui_on_swapchain_recreate`**

```c
void ui_cleanup(struct Renderer* r)
{
    vkDeviceWaitIdle(r->device);
    for (uint32_t i = 0; i < g_framebuffer_count; i++)
        vkDestroyFramebuffer(r->device, g_framebuffers[i], NULL);
    free(g_framebuffers); g_framebuffers = NULL;
    for (int fi = 0; fi < MAX_FRAMES_IN_FLIGHT; fi++) {
        vmaDestroyBuffer(r->allocator, g_vb[fi], g_vb_alloc[fi]);
        vmaDestroyBuffer(r->allocator, g_ib[fi], g_ib_alloc[fi]);
    }
    vkDestroyPipeline(r->device, g_pipeline, NULL);
    vkDestroyPipelineLayout(r->device, g_pipeline_layout, NULL);
    vkDestroyRenderPass(r->device, g_render_pass, NULL);
    vkDestroyDescriptorPool(r->device, g_desc_pool, NULL);
    vkDestroyDescriptorSetLayout(r->device, g_dsl, NULL);
    vkDestroyImageView(r->device, g_atlas_view, NULL);
    vmaDestroyImage(r->allocator, g_atlas_image, g_atlas_alloc);
    vkDestroySampler(r->device, g_sampler, NULL);
}

void ui_on_swapchain_recreate(struct Renderer* r)
{
    for (uint32_t i = 0; i < g_framebuffer_count; i++)
        vkDestroyFramebuffer(r->device, g_framebuffers[i], NULL);
    free(g_framebuffers);
    g_framebuffer_count = r->swapchain.image_count;
    g_framebuffers = calloc(g_framebuffer_count, sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < g_framebuffer_count; i++) {
        VkFramebufferCreateInfo fb_ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &r->swapchain.image_views[i],
            .width           = r->swapchain.extent.width,
            .height          = r->swapchain.extent.height,
            .layers          = 1,
        };
        vkCreateFramebuffer(r->device, &fb_ci, NULL, &g_framebuffers[i]);
    }
}
```

Also call `ui_on_swapchain_recreate(r)` inside `renderer_recreate_swapchain()` in `renderer.c`.

- [ ] **Step 6: Implement `ui_frame_begin`, `ui_frame_end`, and `ui_rect`**

```c
void ui_frame_begin(VkCommandBuffer cmd, uint32_t image_index,
                    int frame_index, float sw, float sh)
{
    g_cmd         = cmd;
    g_image_index = image_index;
    g_frame_index = frame_index;
    g_screen_w    = sw;
    g_screen_h    = sh;
    g_vert_count  = 0;
    g_idx_count   = 0;
}

static void emit_quad(float px, float py, float pw, float ph,
                       float u0, float v0, float u1, float v1,
                       float r, float g, float b, float a)
{
    if (g_vert_count + 4 > UI_MAX_VERTS)   return;
    if (g_idx_count  + 6 > UI_MAX_INDICES) return;

    float x0 = (px        / g_screen_w) * 2.0f - 1.0f;
    float y0 = (py        / g_screen_h) * 2.0f - 1.0f;
    float x1 = ((px + pw) / g_screen_w) * 2.0f - 1.0f;
    float y1 = ((py + ph) / g_screen_h) * 2.0f - 1.0f;

    uint32_t base = g_vert_count;
    UiVertex verts[4] = {
        {x0, y0, u0, v0, r, g, b, a},
        {x1, y0, u1, v0, r, g, b, a},
        {x1, y1, u1, v1, r, g, b, a},
        {x0, y1, u0, v1, r, g, b, a},
    };
    memcpy(g_verts + g_vert_count, verts, sizeof(verts));
    g_vert_count += 4;

    uint32_t idx[6] = {base, base+1, base+2, base+2, base+3, base};
    memcpy(g_indices + g_idx_count, idx, sizeof(idx));
    g_idx_count += 6;
}

void ui_rect(float x, float y, float w, float h, vec4 color)
{
    /* UV = center of 2×2 white pixel region = (1/512, 1/512) */
    float wp = 1.0f / ATLAS_W;
    emit_quad(x, y, w, h, wp, wp, wp, wp,
              color[0], color[1], color[2], color[3]);
}

void ui_frame_end(void)
{
    int fi = g_frame_index;  /* set by ui_frame_begin */

    /* Upload geometry */
    memcpy(g_vb_mapped[fi], g_verts,   g_vert_count  * sizeof(UiVertex));
    memcpy(g_ib_mapped[fi], g_indices, g_idx_count   * sizeof(uint32_t));

    VkRenderPassBeginInfo rp_begin = {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass  = g_render_pass,
        .framebuffer = g_framebuffers[g_image_index],
        .renderArea  = { .offset = {0,0},
                         .extent = { (uint32_t)g_screen_w, (uint32_t)g_screen_h } },
        .clearValueCount = 0,
    };
    vkCmdBeginRenderPass(g_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    VkViewport viewport = { 0, 0, g_screen_w, g_screen_h, 0.0f, 1.0f };
    VkRect2D   scissor  = { {0,0}, { (uint32_t)g_screen_w, (uint32_t)g_screen_h } };
    vkCmdSetViewport(g_cmd, 0, 1, &viewport);
    vkCmdSetScissor(g_cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_pipeline_layout, 0, 1, &g_desc_sets[fi], 0, NULL);

    if (g_vert_count > 0) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_vb[fi], &offset);
        vkCmdBindIndexBuffer(g_cmd, g_ib[fi], 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g_cmd, g_idx_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(g_cmd);
}
```

- [ ] **Step 7: Build and run**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -20"
```

Fix any compile errors before proceeding. Common issues:
- `renderer_begin_single_cmd` / `renderer_end_single_cmd` not found → ensure `#include "../renderer.h"` is present
- `pipeline_load_shader_module` not found → ensure `#include "../pipeline.h"` is present

Once it compiles, run the game:

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft && ./build/minecraft"
```

Expected: game starts. HUD will be **blank** (crosshair/hotbar invisible) — this is expected because `hud_build` was not yet moved. No crash = success for this step.

- [ ] **Step 8: Run tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest --output-on-failure"
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add src/ui/ui.c src/ui/ui.h src/renderer.h src/renderer.c src/renderer_frame.c CMakeLists.txt
git commit -m "feat: ui Vulkan pipeline, atlas texture, frame begin/end, ui_rect"
```

---

## Task 6: HUD refactor — move and rewrite

**Files:**
- Create: `src/ui/hud.h` (from `src/hud.h`, trimmed)
- Create: `src/ui/hud.c` (from `src/hud.c`, rewritten)
- Delete: `src/hud.h`, `src/hud.c`
- Delete: `tests/test_hud.c`
- Modify: `CMakeLists.txt`
- Modify: any file that `#include`s `"hud.h"` (grep for it)

- [ ] **Step 1: Create `src/ui/hud.h`**

Remove `HudVertex`, `HUD_MAX_VERTS`, `HUD_MAX_INDICES` (those live in ui.h now). Keep only:

```c
#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include "ui.h"

#define HUD_SLOT_COUNT 6

/* Block type IDs — same as before */
#include "../block.h"

typedef struct {
    int     selected_slot;
    BlockID slot_blocks[HUD_SLOT_COUNT];
} HUD;

void    hud_init(HUD* hud);
void    hud_build(const HUD* hud, float sw, float sh);  /* calls ui_rect/ui_text */
BlockID hud_selected_block(const HUD* hud);

#endif
```

- [ ] **Step 2: Create `src/ui/hud.c` — rewrite using `ui_rect()`**

The visual output must be identical to the current HUD. Translate `emit_quad` calls to `ui_rect` calls. Colors and sizes are defined in the original `hud.c` — copy them exactly.

```c
#include "hud.h"
#include "ui.h"
#include <string.h>

/* Pixel sizes — must match original hud.c */
#define SLOT_SIZE   40
#define SLOT_GAP     4
#define SLOT_BORDER  4
#define CROSSHAIR_W 14
#define CROSSHAIR_T  2

void hud_init(HUD* hud)
{
    hud->selected_slot = 0;
    hud->slot_blocks[0] = BLOCK_STONE;
    hud->slot_blocks[1] = BLOCK_DIRT;
    hud->slot_blocks[2] = BLOCK_GRASS;
    hud->slot_blocks[3] = BLOCK_SAND;
    hud->slot_blocks[4] = BLOCK_WOOD;
    hud->slot_blocks[5] = BLOCK_LEAVES;
}

void hud_build(const HUD* hud, float sw, float sh)
{
    /* Crosshair */
    vec4 white = {1, 1, 1, 0.9f};
    float cx = sw * 0.5f;
    float cy = sh * 0.5f;
    /* Horizontal bar */
    ui_rect(cx - CROSSHAIR_W * 0.5f, cy - CROSSHAIR_T * 0.5f,
            CROSSHAIR_W, CROSSHAIR_T, white);
    /* Vertical bar */
    ui_rect(cx - CROSSHAIR_T * 0.5f, cy - CROSSHAIR_W * 0.5f,
            CROSSHAIR_T, CROSSHAIR_W, white);

    /* Hotbar */
    int n = HUD_SLOT_COUNT;
    float total_w = n * SLOT_SIZE + (n - 1) * SLOT_GAP;
    float hx = (sw - total_w) * 0.5f;
    float hy = sh - SLOT_SIZE - 12.0f;

    vec4 fill         = {0.15f, 0.15f, 0.15f, 0.75f};
    vec4 border_sel   = {1.0f, 1.0f, 1.0f, 1.0f};
    vec4 border_unsel = {0.4f, 0.4f, 0.4f, 0.75f};

    for (int i = 0; i < n; i++) {
        float sx = hx + i * (SLOT_SIZE + SLOT_GAP);
        vec4* border = (i == hud->selected_slot) ? &border_sel : &border_unsel;
        /* Border */
        ui_rect(sx, hy, SLOT_SIZE, SLOT_SIZE, *border);
        /* Fill (inset by SLOT_BORDER) */
        ui_rect(sx + SLOT_BORDER, hy + SLOT_BORDER,
                SLOT_SIZE - 2 * SLOT_BORDER, SLOT_SIZE - 2 * SLOT_BORDER, fill);
    }
}

BlockID hud_selected_block(const HUD* hud)
{
    return hud->slot_blocks[hud->selected_slot];
}
```

- [ ] **Step 3: Delete old hud files and test**

```bash
rm src/hud.h src/hud.c tests/test_hud.c
```

- [ ] **Step 4: Update `CMakeLists.txt`**

a) In `add_executable(minecraft ...)`: replace `src/hud.c` with `src/ui/hud.c`

b) Remove the `test_hud` target block entirely:
```cmake
# Remove:
add_executable(test_hud tests/test_hud.c src/hud.c src/block.c)
target_include_directories(test_hud PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_hud PRIVATE m)
add_test(NAME hud COMMAND test_hud)
```

c) Verify `src/hud.c` is not referenced anywhere else.

- [ ] **Step 5: Update all `#include "hud.h"` references**

```bash
grep -rn '"hud.h"' src/ main.c 2>/dev/null
```

For each file found, change `#include "hud.h"` to `#include "ui/hud.h"` (adjusting the relative path as needed based on the including file's location).

Known files to update:
- `src/renderer.h` (if it includes hud.h — check)
- `src/renderer_frame.c`
- `src/main.c`
- `src/agent.h` or `src/agent.c` (includes hud.h for `HUD_SLOT_COUNT`)

- [ ] **Step 6: Build and run**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake .. 2>&1 | tail -5 && cmake --build . 2>&1 | tail -20"
```

Fix any compile errors. Then run the game:

```bash
distrobox enter cyberismo -- ./build/minecraft
```

Expected: **HUD looks exactly as before** — crosshair visible, hotbar with 6 slots at bottom, selected slot has white border. If anything looks different, compare `hud_build` side-by-side with the original and fix the pixel values.

- [ ] **Step 7: Run tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && ctest --output-on-failure"
```

Expected: all tests pass (test_hud is gone — that's intentional).

- [ ] **Step 8: Commit**

```bash
git add src/ui/hud.h src/ui/hud.c CMakeLists.txt src/renderer_frame.c src/main.c
git rm src/hud.h src/hud.c tests/test_hud.c
git commit -m "refactor: move hud to src/ui/, rewrite to use ui_rect, remove old HUD Vulkan code"
```

---

## Task 7: ui_text implementation + final verification

**Files:**
- Modify: `src/ui/ui.c` (implement `ui_text`)

- [ ] **Step 1: Implement `ui_text` in `src/ui/ui.c`**

Replace the stub:

```c
void ui_text(float x, float y, float size, const char* text, vec4 color)
{
    if (!g_font_baked) return;
    float scale = size / ATLAS_BAKE_PX;
    float cx = x;
    for (const char* p = text; *p; p++) {
        int ci = (unsigned char)*p - GLYPH_FIRST;
        if (ci < 0 || ci >= GLYPH_COUNT) {
            cx += 8.0f * scale;  /* advance for unknown chars */
            continue;
        }
        UiGlyph* g = &g_glyphs[ci];
        float gx = cx + g->bearing_x * scale;
        float gy = y  + g->bearing_y * scale;
        float gw = g->width  * scale;
        float gh = g->height * scale;
        emit_quad(gx, gy, gw, gh,
                  g->u0, g->v0, g->u1, g->v1,
                  color[0], color[1], color[2], color[3]);
        cx += g->advance * scale;
    }
}
```

- [ ] **Step 2: Smoke test — add a temporary debug label**

In `src/ui/hud.c`, temporarily add a text label to verify rendering:

```c
/* Temporary — add at end of hud_build, remove after verifying */
vec4 yellow = {1, 1, 0, 1};
ui_text(10, 10, 20, "UI OK", yellow);
```

Run the game and verify "UI OK" appears in yellow at the top-left.

- [ ] **Step 3: Remove the temporary debug label**

Remove the `ui_text("UI OK")` line from `hud.c`.

- [ ] **Step 4: Run full test suite**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -5 && ctest --output-on-failure"
```

Expected: all tests pass. The UI test count will be 6 (block_physics, agent_json, net, remote_player, mesher, client, ui — minus hud).

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui.c src/ui/hud.c
git commit -m "feat: ui_text glyph rendering from stb_truetype atlas"
```

---

## Final: Verify complete

- [ ] **Run all tests**

```bash
distrobox enter cyberismo -- bash -c "cd /var/home/samu/minecraft/build && cmake --build . 2>&1 | tail -5 && ctest -v --output-on-failure"
```

- [ ] **Run the game and verify**

1. HUD crosshair and hotbar render identically to before
2. No Vulkan validation errors in terminal output
3. Window resize works (swapchain recreation doesn't crash)
4. Text renders when added (verified in Task 7 step 2)

**Plan 2** (flexbox layout, scene dispatch, pause menu) builds directly on this foundation.
