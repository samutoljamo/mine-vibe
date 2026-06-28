#include "mob_ai.h"

/* Stable passive-type list: pig, cow, chicken. */
const MobType MOB_PASSIVE_TYPES[PASSIVE_TYPE_COUNT] = {
    MOB_PIG, MOB_COW, MOB_CHICKEN,
};

/* Classify directly from the enum so this module stays independent of mob.c
 * (it is compiled into tests without it). Mirrors mob_is_passive(). */
bool mob_ai_is_passive_spawn_type(MobType type) {
    return type == MOB_PIG || type == MOB_COW || type == MOB_CHICKEN;
}

bool mob_ai_passive_spawn_ok(bool is_day, bool on_grass,
                             int live_passive, float nearest_player_dist) {
    if (!is_day)   return false;
    if (!on_grass) return false;
    /* Need room for at least the smallest herd under the cap. */
    if (live_passive + PASSIVE_HERD_MIN > PASSIVE_CAP) return false;
    /* Keep herds away from players. */
    if (nearest_player_dist < PASSIVE_MIN_PLAYER_DIST) return false;
    return true;
}

int mob_ai_herd_size(int live_passive, uint32_t rng) {
    int room = PASSIVE_CAP - live_passive;
    if (room <= 0) return 0;

    int span = PASSIVE_HERD_MAX - PASSIVE_HERD_MIN + 1;   /* inclusive range */
    int n = PASSIVE_HERD_MIN + (int)(rng % (uint32_t)span);
    if (n > room) n = room;
    if (n < 0)    n = 0;
    return n;
}

MobType mob_ai_herd_type(uint32_t rng) {
    return MOB_PASSIVE_TYPES[rng % (uint32_t)PASSIVE_TYPE_COUNT];
}

int mob_loot(MobType type, MobLootDrop out[MOB_LOOT_MAX]) {
    int n = 0;
    switch (type) {
    case MOB_PIG:
        out[n].item = ITEM_RAW_PORK;    out[n].count = 1; n++;
        break;
    case MOB_COW:
        out[n].item = ITEM_RAW_BEEF;    out[n].count = 1; n++;
        out[n].item = ITEM_LEATHER;     out[n].count = 1; n++;
        break;
    case MOB_CHICKEN:
        out[n].item = ITEM_RAW_CHICKEN; out[n].count = 1; n++;
        out[n].item = ITEM_FEATHER;     out[n].count = 1; n++;
        break;
    case MOB_ZOMBIE:
        out[n].item = ITEM_ROTTEN_FLESH; out[n].count = 1; n++;
        break;
    case MOB_SKELETON:
        out[n].item = ITEM_BONE;        out[n].count = 1; n++;
        out[n].item = ITEM_ARROW;       out[n].count = 1; n++;
        break;
    case MOB_CREEPER:
        out[n].item = ITEM_GUNPOWDER;   out[n].count = 1; n++;
        break;
    default:
        break;
    }
    return n;
}
