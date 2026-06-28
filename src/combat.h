#ifndef COMBAT_H
#define COMBAT_H

#include "item.h"

/* ------------------------------------------------------------------ */
/*  Combat — PURE weapon-damage / attack-cooldown / armour math.       */
/*                                                                     */
/*  Everything here is a pure function of its arguments (no globals,    */
/*  no I/O, no time/random). The server owns the state and calls these  */
/*  to resolve a melee swing; tests exercise them directly. No Vulkan / */
/*  GLFW / net dependencies.                                            */
/* ------------------------------------------------------------------ */

/* Bare-hand ("fist") melee damage, in hit-points. The floor of the weapon
 * damage scale: any real weapon does at least this much. */
#define COMBAT_FIST_DAMAGE        1.0f

/* Default swing cooldown (seconds) for an empty hand / non-weapon item. */
#define COMBAT_FIST_COOLDOWN      0.25f

/* ------------------------------------------------------------------ */
/*  Weapon damage                                                      */
/* ------------------------------------------------------------------ */

/* Melee damage dealt by attacking while holding `weapon`, in hit-points.
 *
 * Pure lookup over a data-driven table. Ordering, for a given material tier:
 *   sword > axe > pickaxe/shovel > fist.
 * Higher material tiers (wood < stone < iron) deal more damage within a kind.
 * Blocks, materials, food, armour and any non-weapon item fall back to the
 * bare-hand baseline (COMBAT_FIST_DAMAGE). The table is keyed on the item's
 * (tool-kind, material), so adding new sword tiers / materials needs only the
 * matching ItemDef — no change here. */
float weapon_damage(ItemId weapon);

/* ------------------------------------------------------------------ */
/*  Attack cooldown                                                    */
/* ------------------------------------------------------------------ */

/* Minimum time (seconds) between successive melee swings with `weapon`.
 *
 * Pure lookup. Heavier/stronger weapons swing a touch slower than the bare
 * hand; swords are the dedicated combat weapon and recover fastest among the
 * weapons. Non-weapon items use COMBAT_FIST_COOLDOWN. Always > 0. */
float attack_cooldown(ItemId weapon);

/* ------------------------------------------------------------------ */
/*  Armour damage reduction                                           */
/* ------------------------------------------------------------------ */

/* Final damage (hit-points) a player takes from a `raw_damage` blow while
 * wearing the given equipment set of ARMOR_SLOT_COUNT item ids.
 *
 * Minecraft-style: sums the armour points across the worn pieces (empty /
 * non-armour slots contribute 0, total clamped to ARMOR_MAX_POINTS), then
 * reduces the incoming damage by 4% per point, capped at ARMOR_CAP_PERCENT
 * (80%). Returns at least 0, and at least 1 when raw_damage > 0 (armour never
 * makes a real blow harmless). Pure; thin convenience wrapper over
 * armor_points_total + damage_after_armor. */
int armor_damage_reduction(const ItemId armor[ARMOR_SLOT_COUNT], int raw_damage);

#endif /* COMBAT_H */
