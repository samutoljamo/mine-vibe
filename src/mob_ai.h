#ifndef MOB_AI_H
#define MOB_AI_H

#include <stdbool.h>
#include <stdint.h>
#include "mob.h"     /* MobType (read-only; this module never mutates mob.{c,h}) */
#include "item.h"    /* ItemId, ITEM_* */

/* ------------------------------------------------------------------ */
/*  Passive mob spawning + loot tables — all pure (no Vulkan/net/      */
/*  globals/RNG). Server-side helpers that the authoritative loop in   */
/*  server.c consults; everything here is a deterministic function of  */
/*  its arguments so it is fully unit-testable (see tests/test_mob_ai).*/
/* ------------------------------------------------------------------ */

/* ---- Passive (farm animal) spawning tunables ---- */
#define PASSIVE_SPAWN_INTERVAL  8.0f    /* seconds between passive spawn attempts */
#define PASSIVE_CAP             10      /* max live passive mobs kept near anchor */
#define PASSIVE_HERD_MIN        2       /* smallest herd burst                    */
#define PASSIVE_HERD_MAX        4       /* largest herd burst                     */
#define PASSIVE_SPAWN_MIN       16.0f   /* min ring distance from the anchor      */
#define PASSIVE_SPAWN_MAX       40.0f   /* max ring distance from the anchor      */
#define PASSIVE_MIN_PLAYER_DIST 12.0f   /* never spawn this close to any player   */
#define PASSIVE_HERD_SPREAD     3.0f    /* herd cluster radius around the seed     */

/* The passive types that may be herd-spawned, in a stable order. */
#define PASSIVE_TYPE_COUNT 3
extern const MobType MOB_PASSIVE_TYPES[PASSIVE_TYPE_COUNT];

/* True if `type` is a passive farm animal eligible for daytime herd spawning.
 * Pure wrapper over the mob classification (pig/cow/chicken). */
bool mob_ai_is_passive_spawn_type(MobType type);

/* Passive-spawn eligibility. Returns true only when ALL hold:
 *   - it is daytime (`is_day` — caller derives from the day/night clock),
 *   - the candidate column has a valid grass surface (`on_grass`),
 *   - the live passive-mob count leaves room for a herd (`live_passive`
 *     plus PASSIVE_HERD_MIN must not exceed PASSIVE_CAP),
 *   - the nearest player is at least PASSIVE_MIN_PLAYER_DIST away
 *     (`nearest_player_dist`; pass a large value / FLT_MAX when no players).
 * Pure. */
bool mob_ai_passive_spawn_ok(bool is_day, bool on_grass,
                             int live_passive, float nearest_player_dist);

/* How many animals to spawn in one herd burst, given how many passive mobs are
 * already live and a caller-supplied pseudo-random roll `rng` (any value; only
 * its low bits are used). Result is clamped to [PASSIVE_HERD_MIN,
 * PASSIVE_HERD_MAX] and never exceeds the remaining room under PASSIVE_CAP.
 * Returns 0 if there is no room for even the minimum herd. Pure. */
int mob_ai_herd_size(int live_passive, uint32_t rng);

/* Pick the herd's species from a pseudo-random roll. Pure. */
MobType mob_ai_herd_type(uint32_t rng);

/* ------------------------------------------------------------------ */
/*  Loot tables                                                         */
/* ------------------------------------------------------------------ */

/* A single loot stack awarded to a mob's killer. */
typedef struct {
    ItemId  item;
    uint8_t count;
} MobLootDrop;

/* Max distinct stacks any single mob can drop. */
#define MOB_LOOT_MAX 2

/* Fill `out` (capacity MOB_LOOT_MAX) with the loot a `type` mob drops to its
 * killer, returning the number of stacks written (0..MOB_LOOT_MAX).
 *
 * Only ItemIds that EXIST in the current item space are emitted; drops whose
 * intended item is not yet modelled (pork/beef/chicken meat, feather, rotten
 * flesh, bone, arrow, gunpowder) are simply omitted, so the table stays valid
 * as those items are added later. Today only cow -> ITEM_LEATHER exists.
 * Pure. */
int mob_loot(MobType type, MobLootDrop out[MOB_LOOT_MAX]);

#endif /* MOB_AI_H */
