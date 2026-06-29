#ifndef LOOT_H
#define LOOT_H

#include <stdint.h>
#include "item.h"

/* Pure, deterministic weighted loot-table model.
 *
 * A loot table is an array of weighted entries. Rolling the table picks one
 * entry with probability proportional to its weight, then a stack count
 * uniformly in [min_count, max_count]. All randomness is *injected* by the
 * caller (a uint32 roll, or a seed fed to the bundled pure PRNG) so results
 * are fully reproducible and unit-testable. No rand()/time().
 *
 * Intended use: dungeon chest fills now, mob drops later. This round is the
 * standalone pure model only; integration lives in later tickets.
 *
 * Properties relied on by callers and verified by tests:
 *   - Pure & deterministic: identical args always yield the same result.
 *   - loot_select_index: roll 0 picks entry 0; a roll just below the running
 *     cumulative weight of entry k picks entry k; rolls >= total wrap modulo
 *     total so any uint32 is valid.
 *   - loot_roll: returned item is one of the table's items and count is in
 *     [min_count, max_count] for the chosen entry.
 */

typedef struct {
    ItemId  item;       /* what drops */
    uint8_t min_count;  /* inclusive lower bound on stack size (>= 1) */
    uint8_t max_count;  /* inclusive upper bound on stack size       */
    uint32_t weight;    /* selection weight; larger = more likely    */
} LootEntry;

typedef struct {
    const LootEntry *entries; /* array of entries (not owned)        */
    int              count;   /* number of entries                   */
} LootTable;

typedef struct {
    ItemId item;   /* selected item */
    int    count;  /* selected stack count in [min_count, max_count] */
} LootDrop;

/* Sum of all entry weights. Returns 0 for an empty/NULL table. */
uint32_t loot_total_weight(const LootTable *t);

/* Pure PRNG step (splitmix32). Advances *state and returns a uint32. Use it
 * to derive successive rolls from a single seed. Pure: same state -> same out. */
uint32_t loot_rng_next(uint32_t *state);

/* Select an entry index by weight. `roll` is any uint32; it is reduced modulo
 * the total weight, then mapped to the entry whose cumulative weight interval
 * contains it (roll 0 -> entry 0). Returns -1 for an empty/zero-weight table. */
int loot_select_index(const LootTable *t, uint32_t roll);

/* Roll the table with an injected uint32 `rng`. Selects an entry by weight and
 * a count uniformly in [min_count, max_count] derived from the same rng (via a
 * splitmix32 step so the two draws are decorrelated). Deterministic in `rng`.
 * Returns {item=0, count=0} for an empty/zero-weight table. */
LootDrop loot_roll(const LootTable *t, uint32_t rng);

/* Example tables (defined in loot.c). */
extern const LootTable LOOT_DUNGEON_CHEST;

#endif /* LOOT_H */
