#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include "../block.h"

#define HUD_SLOT_COUNT 6

/* Forward declarations to avoid circular includes. */
struct Inventory;

void    hud_build(const struct Inventory* inv, int player_health, float sw, float sh);
BlockID hud_selected_block(const struct Inventory* inv);

#endif
