#include "../renderer.h"
#include "ui.h"
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
