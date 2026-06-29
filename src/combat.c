#include "combat.h"

/* ------------------------------------------------------------------ */
/*  Combat math (pure).                                                 */
/*                                                                     */
/*  weapon_damage / attack_cooldown are data-driven lookups keyed on    */
/*  the held item's (tool-kind, material). Damage is a per-kind base    */
/*  plus a per-material-tier bonus, so the ordering                     */
/*    sword > axe > pickaxe/shovel > fist                               */
/*  and "higher tier hits harder within a kind" both fall out of the    */
/*  two tables below. Adding a new sword tier (or a whole new weapon    */
/*  kind / material) is a table edit — no control-flow changes.         */
/* ------------------------------------------------------------------ */

/* Per-material-tier damage bonus (added on top of the kind base when the item
 * is a tool/weapon). Strictly increasing wood < stone < iron so a higher tier
 * always hits harder for a given kind. MATERIAL_NONE is the fist/non-tool
 * baseline and contributes nothing. Indexed by ToolMaterial. */
static float material_damage_bonus(ToolMaterial m) {
    switch (m) {
        case MATERIAL_WOOD:    return 1.0f;
        case MATERIAL_STONE:   return 2.0f;
        case MATERIAL_IRON:    return 3.0f;
        case MATERIAL_DIAMOND: return 4.0f;
        default:               return 0.0f;   /* MATERIAL_NONE */
    }
}

/* Per-kind base melee damage. Swords are the dedicated weapon and sit above
 * the improvised tools; the axe is the best improvised weapon, ahead of the
 * pickaxe/shovel. TOOL_NONE never reaches here (handled as the fist baseline).
 * The sword is the dedicated weapon and ranks above the axe; combined with the
 * per-material bonus this keeps sword > axe within and across tiers. */
static float kind_base_damage(ToolKind kind) {
    switch (kind) {
        case TOOL_SWORD:   return 4.0f;   /* dedicated weapon (> axe) */
        case TOOL_AXE:     return 3.0f;
        case TOOL_PICKAXE: return 2.0f;
        case TOOL_SHOVEL:  return 1.0f;
        default:           return 0.0f;   /* TOOL_NONE: fist baseline */
    }
}

float weapon_damage(ItemId weapon) {
    if (!item_is_tool(weapon))
        return COMBAT_FIST_DAMAGE;

    const ItemDef* def = item_get_def(weapon);
    float dmg = kind_base_damage(def->tool_kind)
              + material_damage_bonus(def->material);

    /* A weapon always beats bare fists, even a wooden shovel. */
    if (dmg < COMBAT_FIST_DAMAGE) dmg = COMBAT_FIST_DAMAGE;
    return dmg;
}

/* Per-kind swing cooldown (seconds). Weapons recover a touch slower than the
 * bare hand; the heavy axe is the slowest, swords (future) the fastest weapon.
 * Tuned so the ordering is axe > pickaxe ~ shovel > fist. */
static float kind_cooldown(ToolKind kind) {
    switch (kind) {
        case TOOL_SWORD:   return 0.45f;  /* fast dedicated weapon */
        case TOOL_AXE:     return 0.80f;
        case TOOL_PICKAXE: return 0.50f;
        case TOOL_SHOVEL:  return 0.50f;
        default:           return COMBAT_FIST_COOLDOWN;
    }
}

float attack_cooldown(ItemId weapon) {
    if (!item_is_tool(weapon))
        return COMBAT_FIST_COOLDOWN;
    return kind_cooldown(item_get_def(weapon)->tool_kind);
}

int armor_damage_reduction(const ItemId armor[ARMOR_SLOT_COUNT], int raw_damage) {
    int points = armor_points_total(armor);
    return damage_after_armor(raw_damage, points);
}

int armor_durability_loss(int damage) {
    if (damage <= 0) return 0;       /* no real blow => no wear */
    int loss = damage / 4;           /* floor(damage/4), vanilla rule */
    return loss < 1 ? 1 : loss;      /* a real blow always costs >= 1 */
}
