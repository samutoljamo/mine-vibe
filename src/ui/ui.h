#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <volk.h>
#include <cglm/cglm.h>

#include "../block.h"

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
/*  Mouse input handler / hit-testing                                  */
/* ------------------------------------------------------------------ */
/*
 * Data-driven, retained-per-frame interaction model for screens
 * (buttons, inventory-style slots). Usage per frame:
 *
 *   ui_input_begin();                          // clear registered elements
 *   ui_add_element(SLOT_0, x, y, w, h);        // register hit rects
 *   ...
 *   ui_handle_mouse(mx, my, clicked);          // resolve hover + click
 *   if (ui_clicked_element() == SLOT_0) { ... }
 *
 * Elements are axis-aligned rectangles in screen pixels (same coordinate
 * space as the draw primitives: (0,0) = top-left, y increases down).
 * Hit-test is inclusive of the top-left edge and exclusive of the
 * bottom-right edge. On overlap the most-recently-added element wins
 * (it is treated as topmost / drawn last). Element ids are caller-defined
 * and should be >= 0; -1 is reserved to mean "none".
 */

#define UI_MAX_ELEMENTS 256
#define UI_ELEMENT_NONE (-1)

/* Optional per-element click callback. Invoked by ui_handle_mouse when the
 * element is clicked. `id` is the element id, `user` the pointer passed to
 * ui_add_element_cb. */
typedef void (*UiClickFn)(int id, void* user);

/* Begin a new input frame: clears all registered elements and resets the
 * hovered/clicked state. Call once before registering elements each frame. */
void ui_input_begin(void);

/* Register a rectangular interactive element. */
void ui_add_element(int id, float x, float y, float w, float h);

/* Register an element with a click callback (and user data). */
void ui_add_element_cb(int id, float x, float y, float w, float h,
                       UiClickFn on_click, void* user);

/* Resolve the cursor against the registered elements. Sets the hovered
 * element (topmost under the cursor, or UI_ELEMENT_NONE). If `clicked` is
 * true and an element is under the cursor, it becomes the clicked element
 * and its callback (if any) is invoked. */
void ui_handle_mouse(float x, float y, bool clicked);

/* Id of the element currently under the cursor, or UI_ELEMENT_NONE. */
int  ui_hovered_element(void);

/* Id of the element clicked in the most recent ui_handle_mouse call, or
 * UI_ELEMENT_NONE if the last call was not a click on an element. Reset by
 * ui_input_begin. */
int  ui_clicked_element(void);

/* ------------------------------------------------------------------ */
/*  Draw primitives                                                    */
/* ------------------------------------------------------------------ */

/* All coordinates are screen pixels, (0,0) = top-left, y increases down. */
void  ui_rect(float x, float y, float w, float h, vec4 color);
void  ui_text(float x, float y, float size, const char* text, vec4 color);
float ui_text_width(const char* text, float size);

/* Draw a baked isometric block icon at (x, y) with the given square pixel size. */
void  ui_block_icon(BlockID id, float x, float y, float size);

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
