#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include "block.h"

#define HUD_SLOT_COUNT   6
#define HUD_MAX_VERTS    256
#define HUD_MAX_INDICES  384

typedef struct HUD {
    int     selected_slot;
    BlockID slot_blocks[HUD_SLOT_COUNT];
} HUD;

typedef struct HudVertex {
    float x, y;         /* NDC [-1, 1] */
    float r, g, b, a;
} HudVertex;  /* 24 bytes */

void     hud_init(HUD* hud);
void     hud_build(const HUD* hud, float screen_w, float screen_h);
BlockID  hud_selected_block(const HUD* hud);

#endif
