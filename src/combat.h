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
/*  Knockback                                                          */
/* ------------------------------------------------------------------ */

/* Horizontal knockback speed (blocks/s) imparted to an entity by a melee hit,
 * directed away from the attacker. Tuned to shove the target back a readable
 * amount before it decays — noticeable but not launchy. */
#define COMBAT_KNOCKBACK_STRENGTH 6.0f

/* Small upward component (blocks/s) added on top of the horizontal shove so a
 * hit "pops" the target a touch off the ground (vanilla-ish feel). */
#define COMBAT_KNOCKBACK_LIFT     3.0f

/* Per-second exponential decay applied to the local player's residual knockback
 * velocity. The client applies player knockback as a short decaying positional
 * nudge (player_update overwrites the player's own horizontal velocity each
 * frame), so the nudge has to fade on its own. */
#define COMBAT_KNOCKBACK_DECAY    9.0f

/* ------------------------------------------------------------------ */
/*  Eating                                                            */
/* ------------------------------------------------------------------ */

/* How long (seconds) the eat key must be held before the food is consumed.
 * Eating is no longer instant: the client gates the PKT_EAT send on this hold,
 * plays the eat SFX while chewing, and shows a HUD progress nibble. The server
 * still authoritatively validates + applies the food. */
#define EAT_DURATION_SEC          1.2f

/* ------------------------------------------------------------------ */
/*  Knockback impulse (pure)                                          */
/* ------------------------------------------------------------------ */

/* Horizontal knockback impulse to push a target at (tx,tz) away from an attacker
 * at (ax,az): the unit vector from attacker -> target scaled by `strength`,
 * written to (*out_dx,*out_dz). Degenerate case (attacker and target at the same
 * XZ point) yields a zero impulse rather than a NaN. Pure; no globals/IO. */
void knockback_impulse(float ax, float az, float tx, float tz,
                       float strength, float* out_dx, float* out_dz);

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

/* ------------------------------------------------------------------ */
/*  Armour durability wear                                            */
/* ------------------------------------------------------------------ */

/* Durability points a single worn armour piece loses when its wearer takes a
 * blow of `damage` hit-points. Vanilla-style: floor(damage / 4), but at least
 * 1 on any real blow so even a tiny hit costs something. A zero (or negative)
 * damage event causes no wear — armour only degrades on a real hit. Pure. */
int armor_durability_loss(int damage);

#endif /* COMBAT_H */
