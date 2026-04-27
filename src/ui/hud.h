#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include "../block.h"

#define HUD_SLOT_COUNT 6

/* Forward declarations to avoid circular includes. */
struct Inventory;
struct RaycastHit;

void    hud_build(const struct Inventory* inv, float sw, float sh);
/* Emits world-space outline geometry for the targeted block. No-op if hit
 * is null or hit->hit is false. The outline pipeline is added in Task 9;
 * for now this is a stub that does nothing. */
void    hud_build_target(const struct RaycastHit* hit);
BlockID hud_selected_block(const struct Inventory* inv);

#endif
