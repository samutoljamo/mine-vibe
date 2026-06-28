/* Stub for hud_set_survival so test_client (which links client.c) need not
 * pull in the full HUD/UI/renderer stack. The unit tests never exercise the
 * HUD path; client.c only references the symbol from its PKT_PLAYER_HEALTH
 * handler. */
void hud_set_survival(int food, int air)
{
    (void)food;
    (void)air;
}

/* Stub for hud_set_armor — client.c references it from its PKT_ARMOR handler. */
#include <stdint.h>
void hud_set_armor(const uint16_t* worn, int points)
{
    (void)worn;
    (void)points;
}
