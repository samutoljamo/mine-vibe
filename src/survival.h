#ifndef SURVIVAL_H
#define SURVIVAL_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Survival mechanics — PURE decision/math core.                      */
/*                                                                     */
/*  Everything in this translation unit is a pure function of its      */
/*  arguments (no globals, no I/O, no time/random). The server owns    */
/*  the state and calls these each tick; tests exercise them directly. */
/* ------------------------------------------------------------------ */

/* Hunger model (mirrors vanilla Minecraft closely but simplified):
 *   - food: 0..SURVIVAL_MAX_FOOD ("drumsticks", 20 = full)
 *   - saturation: hidden reserve, 0..food. Drains before food does.
 *   - exhaustion: accumulates with activity; every SURVIVAL_EXHAUSTION_PER_POINT
 *     units consumed drains 1 saturation (or 1 food once saturation hits 0).
 * Health regenerates while food is high; starvation hurts while food is 0. */
#define SURVIVAL_MAX_FOOD              20
#define SURVIVAL_MAX_HEALTH           20    /* must match PLAYER_MAX_HEALTH */

#define SURVIVAL_EXHAUSTION_PER_POINT  4.0f /* exhaustion to drain 1 food/sat */

/* Exhaustion added per unit of horizontal distance walked / sprinted /
 * jumped. Tuned so ordinary play slowly empties the bar. */
#define SURVIVAL_EXH_WALK_PER_M       0.01f
#define SURVIVAL_EXH_SPRINT_PER_M     0.10f
#define SURVIVAL_EXH_JUMP             0.05f
#define SURVIVAL_EXH_SPRINT_JUMP      0.20f
/* Passive trickle so an idle player still eventually gets hungry. */
#define SURVIVAL_EXH_IDLE_PER_SEC     0.005f

/* Regen: at/above this food level, heal 1 hp every REGEN_INTERVAL seconds
 * (and burn a little saturation/exhaustion doing so). */
#define SURVIVAL_REGEN_FOOD_THRESHOLD 18
#define SURVIVAL_REGEN_INTERVAL_SEC   4.0f
#define SURVIVAL_REGEN_EXHAUSTION     6.0f  /* exhaustion cost per hp healed */

/* Starvation: at food 0, lose 1 hp every STARVE_INTERVAL seconds, but never
 * below STARVE_FLOOR (vanilla never starves to death on easy/normal). */
#define SURVIVAL_STARVE_INTERVAL_SEC  4.0f
#define SURVIVAL_STARVE_FLOOR         0

/* Drowning / oxygen: air ticks count DOWN while the head is submerged.
 * When air hits 0, take damage every DROWN_INTERVAL seconds. Out of water,
 * air refills instantly (handled by caller resetting to MAX). */
#define SURVIVAL_MAX_AIR_SEC          10.0f
#define SURVIVAL_DROWN_INTERVAL_SEC   1.0f
#define SURVIVAL_DROWN_DAMAGE         2

/* Lava contact damage per LAVA_INTERVAL while touching lava. */
#define SURVIVAL_LAVA_INTERVAL_SEC    0.5f
#define SURVIVAL_LAVA_DAMAGE          4

/* Fall damage: 1 hp per block beyond a SURVIVAL_FALL_SAFE-block grace,
 * matching vanilla (3-block grace, then 1 hp/block). */
#define SURVIVAL_FALL_SAFE            3.0f

/* Respawn grace period after death — no environmental/mob damage. */
#define SURVIVAL_RESPAWN_GRACE_SEC    3.0f

/* ------------------------------------------------------------------ */
/*  Per-player survival state (server-owned).                          */
/* ------------------------------------------------------------------ */
typedef struct {
    float food;          /* 0..SURVIVAL_MAX_FOOD                       */
    float saturation;    /* 0..food                                    */
    float exhaustion;    /* 0..SURVIVAL_EXHAUSTION_PER_POINT (wraps)   */
    float air;           /* seconds of breath remaining, 0..MAX_AIR    */
    float regen_timer;   /* seconds accumulated toward next heal       */
    float starve_timer;  /* seconds accumulated toward next starve hit */
    float drown_timer;   /* seconds accumulated toward next drown hit  */
    float lava_timer;    /* seconds accumulated toward next lava hit   */
} SurvivalState;

void survival_init(SurvivalState* s);

/* ------------------------------------------------------------------ */
/*  Pure functions                                                     */
/* ------------------------------------------------------------------ */

/* Fall damage in hp for a fall of `distance` blocks. 0 within the grace. */
int survival_fall_damage(float distance);

/* Exhaustion produced by `dist` metres of horizontal travel this tick.
 * `sprinting` selects the higher sprint cost. */
float survival_exhaustion_move(float dist, bool sprinting);

/* Exhaustion produced by a single jump. */
float survival_exhaustion_jump(bool sprinting);

/* Apply `added` exhaustion to (food, saturation, exhaustion), draining
 * saturation first then food. Mutates the three values in place. Pure given
 * its outputs are a deterministic function of inputs. */
void survival_apply_exhaustion(float* food, float* saturation,
                               float* exhaustion, float added);

/* Eat: returns the new food/saturation after consuming a food item worth
 * `restore_food` hunger and `restore_sat` saturation. Caps at the max and
 * keeps saturation <= food. Returns true if anything was consumed (i.e. the
 * player was not already full). */
bool survival_eat(float* food, float* saturation,
                  float restore_food, float restore_sat);

/* Eating rule (server-authoritative): may a held item be eaten right now?
 * True iff the item is a food item AND the player's food is below the max
 * (so we never waste a food item at full hunger). Pure predicate. */
bool survival_can_eat(bool is_food, float food, float max_food);

/* Apply a food item worth `hunger_restore` hunger points to `s`, clamping food
 * to SURVIVAL_MAX_FOOD and topping saturation up to (but not past) the new food
 * level. Returns true if anything was consumed (false when already full or the
 * restore value is non-positive). Thin wrapper over survival_eat that grants
 * saturation equal to the hunger restored (a simple model; cooked > raw because
 * cooked foods carry a higher hunger_restore). */
bool survival_apply_food(SurvivalState* s, int hunger_restore);

/* Regeneration step. Given current food/health and the accumulated
 * regen_timer (seconds), advance by `dt`. Returns hp to ADD this tick (0 or
 * more) and updates *regen_timer and *exhaustion accordingly. Heals only when
 * food >= threshold and health < max. */
int survival_regen_step(float food, int health, float* regen_timer,
                        float* exhaustion, float dt);

/* Convenience whole-state regen tick. Runs survival_regen_step against the
 * SurvivalState's own fields, and immediately settles the exhaustion the heal
 * cost into saturation/food (so each healed hp visibly burns the hidden
 * saturation reserve first, then food — Minecraft-style). Returns hp to ADD
 * this tick (clamped so it never exceeds SURVIVAL_MAX_HEALTH given `health`).
 * Pure: a deterministic function of *s and its inputs. */
int survival_regen_tick(SurvivalState* s, int health, float dt);

/* Starvation step. At food <= 0 and health > STARVE_FLOOR, returns hp to
 * SUBTRACT this tick (0 or more) and advances *starve_timer. */
int survival_starve_step(float food, int health, float* starve_timer,
                         float dt);

/* Drowning step. If `head_submerged`, decrement *air by dt; once air <= 0,
 * accrue *drown_timer and return damage when an interval elapses. If not
 * submerged, refill air to MAX and reset the drown timer. Returns hp to
 * subtract this tick. */
int survival_drown_step(bool head_submerged, float* air, float* drown_timer,
                        float dt);

/* Lava step. If `touching_lava`, accrue *lava_timer and return damage on each
 * interval; otherwise reset the timer. Returns hp to subtract this tick. */
int survival_lava_step(bool touching_lava, float* lava_timer, float dt);

#endif /* SURVIVAL_H */
