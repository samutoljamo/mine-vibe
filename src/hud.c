#include "hud.h"
#include <string.h>

/* Emit a colored quad (pixel coords -> NDC) into verts/indices.
 * Returns new vertex count. px,py = top-left pixel; pw,ph = size. */
static uint32_t emit_quad(HudVertex* verts, uint32_t* idx,
                           uint32_t vi, uint32_t* ii,
                           float px, float py, float pw, float ph,
                           float sw, float sh,
                           float r, float g, float b, float a)
{
    /* Convert pixel top-left + size to NDC */
    float x0 = (px       / sw) * 2.0f - 1.0f;
    float y0 = (py       / sh) * 2.0f - 1.0f;
    float x1 = ((px + pw) / sw) * 2.0f - 1.0f;
    float y1 = ((py + ph) / sh) * 2.0f - 1.0f;

    verts[vi+0] = (HudVertex){ x0, y0, r, g, b, a };
    verts[vi+1] = (HudVertex){ x1, y0, r, g, b, a };
    verts[vi+2] = (HudVertex){ x1, y1, r, g, b, a };
    verts[vi+3] = (HudVertex){ x0, y1, r, g, b, a };

    idx[*ii+0] = vi+0; idx[*ii+1] = vi+1; idx[*ii+2] = vi+2;
    idx[*ii+3] = vi+0; idx[*ii+4] = vi+2; idx[*ii+5] = vi+3;
    *ii += 6;
    return vi + 4;
}

void hud_init(HUD* hud) {
    hud->selected_slot = 0;
    hud->slot_blocks[0] = BLOCK_STONE;
    hud->slot_blocks[1] = BLOCK_DIRT;
    hud->slot_blocks[2] = BLOCK_GRASS;
    hud->slot_blocks[3] = BLOCK_SAND;
    hud->slot_blocks[4] = BLOCK_WOOD;
    hud->slot_blocks[5] = BLOCK_LEAVES;
}

BlockID hud_selected_block(const HUD* hud) {
    return hud->slot_blocks[hud->selected_slot];
}

uint32_t hud_build(const HUD* hud, float sw, float sh,
                   HudVertex* verts, uint32_t* indices)
{
    uint32_t vi = 0, ii = 0;

    /* --- Crosshair (first 8 verts) --- */
    float cx = sw * 0.5f, cy = sh * 0.5f;
    /* Horizontal bar: 14px wide, 2px tall */
    vi = emit_quad(verts, indices, vi, &ii,
                   cx - 7.0f, cy - 1.0f, 14.0f, 2.0f,
                   sw, sh, 1.0f, 1.0f, 1.0f, 0.8f);
    /* Vertical bar: 2px wide, 14px tall */
    vi = emit_quad(verts, indices, vi, &ii,
                   cx - 1.0f, cy - 7.0f, 2.0f, 14.0f,
                   sw, sh, 1.0f, 1.0f, 1.0f, 0.8f);

    /* --- Hotbar slots (6 fills + 6 borders) --- */
    float slot_w = 40.0f, slot_h = 40.0f, gap = 4.0f, margin_b = 12.0f;
    float total_w = HUD_SLOT_COUNT * slot_w + (HUD_SLOT_COUNT - 1) * gap;
    float start_x = (sw - total_w) * 0.5f;
    float start_y = sh - margin_b - slot_h;
    float bw = 2.0f; /* border width in pixels */

    for (int i = 0; i < HUD_SLOT_COUNT; i++) {
        float sx = start_x + i * (slot_w + gap);
        float sy = start_y;

        /* Fill */
        vi = emit_quad(verts, indices, vi, &ii,
                       sx, sy, slot_w, slot_h,
                       sw, sh, 0.15f, 0.15f, 0.15f, 0.75f);

        /* Border: 4 edge quads.
         * Selected slot = white, others = dark gray */
        float br = (i == hud->selected_slot) ? 1.0f : 0.4f;
        float bg = br, bb = br, ba = (i == hud->selected_slot) ? 1.0f : 0.75f;

        /* Top edge */
        vi = emit_quad(verts, indices, vi, &ii,
                       sx, sy, slot_w, bw, sw, sh, br, bg, bb, ba);
        /* Bottom edge */
        vi = emit_quad(verts, indices, vi, &ii,
                       sx, sy + slot_h - bw, slot_w, bw, sw, sh, br, bg, bb, ba);
        /* Left edge */
        vi = emit_quad(verts, indices, vi, &ii,
                       sx, sy, bw, slot_h, sw, sh, br, bg, bb, ba);
        /* Right edge */
        vi = emit_quad(verts, indices, vi, &ii,
                       sx + slot_w - bw, sy, bw, slot_h, sw, sh, br, bg, bb, ba);
    }

    return vi;
}
