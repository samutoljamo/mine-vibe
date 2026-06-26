#include "hud.h"
#include "ui.h"
#include "../inventory.h"
#include <stdio.h>

#define SLOT_SIZE   40
#define SLOT_GAP     4
#define SLOT_BORDER  4
#define CROSSHAIR_W 14
#define CROSSHAIR_T  2

/* Static assert for the cross-header invariant introduced in Task 1's fix. */
_Static_assert(HUD_SLOT_COUNT == INVENTORY_SLOTS,
    "HUD slot count must match Inventory slot count");

/* hud_build — accepts a NULL `inv`. The crosshair is always drawn (e.g., in
 * single-player / pre-connect runs before the first PKT_INVENTORY arrives);
 * the hotbar slots are only drawn when `inv` is non-NULL. */
void hud_build(const Inventory* inv, int player_health, float sw, float sh)
{
    /* Crosshair — always rendered, even with no inventory yet. */
    vec4 white = {1, 1, 1, 0.9f};
    float cx = sw * 0.5f, cy = sh * 0.5f;
    ui_rect(cx - CROSSHAIR_W * 0.5f, cy - CROSSHAIR_T * 0.5f,
            CROSSHAIR_W, CROSSHAIR_T, white);
    ui_rect(cx - CROSSHAIR_T * 0.5f, cy - CROSSHAIR_W * 0.5f,
            CROSSHAIR_T, CROSSHAIR_W, white);

    if (!inv) return;

    /* Hotbar layout. */
    int n = HUD_SLOT_COUNT;
    float total_w = n * SLOT_SIZE + (n - 1) * SLOT_GAP;
    float hx = (sw - total_w) * 0.5f;
    float hy = sh - SLOT_SIZE - 12.0f;

    vec4 fill         = {0.15f, 0.15f, 0.15f, 0.75f};
    vec4 border_sel   = {1.0f,  1.0f,  1.0f,  1.0f};
    vec4 border_unsel = {0.4f,  0.4f,  0.4f,  0.75f};
    vec4 text_white   = {1.0f,  1.0f,  1.0f,  1.0f};

    for (int i = 0; i < n; i++) {
        float sx = hx + i * (SLOT_SIZE + SLOT_GAP);
        vec4* border = (i == inv->selected) ? &border_sel : &border_unsel;
        ui_rect(sx, hy, SLOT_SIZE, SLOT_SIZE, *border);
        ui_rect(sx + SLOT_BORDER, hy + SLOT_BORDER,
                SLOT_SIZE - 2 * SLOT_BORDER, SLOT_SIZE - 2 * SLOT_BORDER, fill);

        const InventorySlot* s = &inv->slots[i];
        if (s->count == 0) continue;

        /* Block icon, inset by 4 pixels. */
        ui_block_icon(s->block, sx + 4, hy + 4, SLOT_SIZE - 8);

        /* Count, only if > 1, bottom-right of slot. */
        if (s->count > 1) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", s->count);
            float tw = ui_text_width(buf, 12.0f);
            ui_text(sx + SLOT_SIZE - tw - 3, hy + SLOT_SIZE - 14,
                    12.0f, buf, text_white);
        }
    }

    /* Health hearts — 10 hearts = 20 hp, drawn above the hotbar. */
    if (player_health >= 0) {
        const float HSZ = 16.0f, HGAP = 2.0f;
        int hearts = 10;
        float total = hearts * HSZ + (hearts - 1) * HGAP;
        float hx0 = (sw - total) * 0.5f;
        float hy0 = sh - SLOT_SIZE - 12.0f - HSZ - 8.0f;
        vec4 full  = {0.85f, 0.10f, 0.10f, 1.0f};
        vec4 half  = {0.85f, 0.10f, 0.10f, 1.0f};
        vec4 empty = {0.20f, 0.20f, 0.20f, 0.6f};
        for (int i = 0; i < hearts; i++) {
            float x = hx0 + i * (HSZ + HGAP);
            int hp_here = player_health - i * 2;   /* 2 hp per heart */
            ui_rect(x, hy0, HSZ, HSZ, empty);
            if (hp_here >= 2)      ui_rect(x, hy0, HSZ, HSZ, full);
            else if (hp_here == 1) ui_rect(x, hy0, HSZ * 0.5f, HSZ, half);
        }
        (void)half;
    }
}

BlockID hud_selected_block(const Inventory* inv)
{
    return inv->slots[inv->selected].block;
}
