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
