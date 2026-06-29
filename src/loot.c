#include "loot.h"

/* splitmix32: a tiny, well-mixed, fully pure PRNG step. Given a state word it
 * advances it and returns a uniformly-distributed uint32. Used to (a) generate
 * successive rolls from one seed and (b) decorrelate the entry-pick from the
 * count-pick inside a single loot_roll. */
uint32_t loot_rng_next(uint32_t *state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

uint32_t loot_total_weight(const LootTable *t)
{
    if (!t || t->count <= 0 || !t->entries)
        return 0;
    uint32_t total = 0;
    for (int i = 0; i < t->count; i++)
        total += t->entries[i].weight;
    return total;
}

int loot_select_index(const LootTable *t, uint32_t roll)
{
    uint32_t total = loot_total_weight(t);
    if (total == 0)
        return -1;

    /* Reduce any uint32 into [0, total) so every roll is valid. */
    uint32_t r = roll % total;

    uint32_t cumulative = 0;
    for (int i = 0; i < t->count; i++) {
        cumulative += t->entries[i].weight;
        if (r < cumulative)
            return i;
    }
    /* Unreachable when total > 0, but keep the compiler and callers safe. */
    return t->count - 1;
}

LootDrop loot_roll(const LootTable *t, uint32_t rng)
{
    LootDrop drop = { 0, 0 };

    int idx = loot_select_index(t, rng);
    if (idx < 0)
        return drop;

    const LootEntry *e = &t->entries[idx];

    /* Derive an independent draw for the count so it does not correlate with
     * the entry selection above. */
    uint32_t cstate = rng;
    uint32_t cr = loot_rng_next(&cstate);

    int span = (int)e->max_count - (int)e->min_count + 1; /* >= 1 */
    int count = (int)e->min_count + (int)(cr % (uint32_t)span);

    drop.item  = e->item;
    drop.count = count;
    return drop;
}

/* ------------------------------------------------------------------ */
/*  Example tables                                                      */
/* ------------------------------------------------------------------ */

/* A classic dungeon chest: mostly mob-drop materials and ammo, with a rarer
 * iron payoff. Uses only ItemIds that exist in item.h. Weights set rarity:
 * common arrows/bones, rarer gunpowder, scarce iron ingots. */
static const LootEntry DUNGEON_CHEST_ENTRIES[] = {
    { ITEM_ARROW,     4, 12, 30 },
    { ITEM_BONE,      1,  4, 25 },
    { ITEM_GUNPOWDER, 1,  3, 12 },
    { ITEM_IRON_INGOT,1,  2,  4 },
};

const LootTable LOOT_DUNGEON_CHEST = {
    DUNGEON_CHEST_ENTRIES,
    (int)(sizeof DUNGEON_CHEST_ENTRIES / sizeof DUNGEON_CHEST_ENTRIES[0]),
};
