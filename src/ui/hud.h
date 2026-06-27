#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include "../block.h"

#define HUD_SLOT_COUNT 6

/* Forward declarations to avoid circular includes. */
struct Inventory;

void    hud_build(const struct Inventory* inv, int player_health, float sw, float sh);
BlockID hud_selected_block(const struct Inventory* inv);

/* Latch the latest server-reported survival stats so hud_build (whose
 * signature is fixed by its renderer caller) can draw the hunger + oxygen
 * bars. food: 0..20, air: 0..20. The client calls this from the
 * PKT_PLAYER_HEALTH handler. */
void    hud_set_survival(int food, int air);

#endif
