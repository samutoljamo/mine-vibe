#ifndef HUD_H
#define HUD_H

#include <stdint.h>

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
