#include "hud.h"

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

/* hud_build is a stub; the HUD will be blank until Task 6 rewrites it
 * using ui_rect/ui_text. The old emit_quad helper is unused for now. */
void hud_build(const HUD* hud, float sw, float sh)
{
    (void)hud; (void)sw; (void)sh;
}
