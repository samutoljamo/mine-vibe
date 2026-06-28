#include "survival.h"
#include <math.h>

void survival_init(SurvivalState* s)
{
    s->food         = (float)SURVIVAL_MAX_FOOD;
    s->saturation   = 5.0f;                       /* vanilla starting reserve */
    s->exhaustion   = 0.0f;
    s->air          = SURVIVAL_MAX_AIR_SEC;
    s->regen_timer  = 0.0f;
    s->starve_timer = 0.0f;
    s->drown_timer  = 0.0f;
    s->lava_timer   = 0.0f;
}

int survival_fall_damage(float distance)
{
    if (distance <= SURVIVAL_FALL_SAFE) return 0;
    float dmg = distance - SURVIVAL_FALL_SAFE;
    /* Round to nearest hp; clamp to >= 0. */
    int hp = (int)floorf(dmg + 0.5f);
    return hp < 0 ? 0 : hp;
}

float survival_exhaustion_move(float dist, bool sprinting)
{
    if (dist <= 0.0f) return 0.0f;
    return dist * (sprinting ? SURVIVAL_EXH_SPRINT_PER_M
                             : SURVIVAL_EXH_WALK_PER_M);
}

float survival_exhaustion_jump(bool sprinting)
{
    return sprinting ? SURVIVAL_EXH_SPRINT_JUMP : SURVIVAL_EXH_JUMP;
}

void survival_apply_exhaustion(float* food, float* saturation,
                               float* exhaustion, float added)
{
    *exhaustion += added;
    /* Drain one point of saturation (then food) per full unit threshold. */
    while (*exhaustion >= SURVIVAL_EXHAUSTION_PER_POINT) {
        *exhaustion -= SURVIVAL_EXHAUSTION_PER_POINT;
        if (*saturation > 0.0f) {
            *saturation -= 1.0f;
            if (*saturation < 0.0f) *saturation = 0.0f;
        } else if (*food > 0.0f) {
            *food -= 1.0f;
            if (*food < 0.0f) *food = 0.0f;
        }
    }
}

bool survival_eat(float* food, float* saturation,
                  float restore_food, float restore_sat)
{
    if (*food >= (float)SURVIVAL_MAX_FOOD) return false;   /* already full */
    *food += restore_food;
    if (*food > (float)SURVIVAL_MAX_FOOD) *food = (float)SURVIVAL_MAX_FOOD;
    *saturation += restore_sat;
    if (*saturation > *food) *saturation = *food;          /* sat capped at food */
    if (*saturation < 0.0f) *saturation = 0.0f;
    return true;
}

bool survival_can_eat(bool is_food, float food, float max_food)
{
    return is_food && food < max_food;
}

bool survival_apply_food(SurvivalState* s, int hunger_restore)
{
    if (hunger_restore <= 0) return false;
    /* Grant saturation equal to the hunger restored; survival_eat clamps food to
     * the max and caps saturation at the new food level. */
    return survival_eat(&s->food, &s->saturation,
                        (float)hunger_restore, (float)hunger_restore);
}

int survival_regen_step(float food, int health, float* regen_timer,
                        float* exhaustion, float dt)
{
    if (food < (float)SURVIVAL_REGEN_FOOD_THRESHOLD
        || health >= SURVIVAL_MAX_HEALTH) {
        *regen_timer = 0.0f;
        return 0;
    }
    *regen_timer += dt;
    int healed = 0;
    while (*regen_timer >= SURVIVAL_REGEN_INTERVAL_SEC
           && health + healed < SURVIVAL_MAX_HEALTH) {
        *regen_timer -= SURVIVAL_REGEN_INTERVAL_SEC;
        healed++;
        *exhaustion += SURVIVAL_REGEN_EXHAUSTION;
    }
    return healed;
}

int survival_regen_tick(SurvivalState* s, int health, float dt)
{
    int heal = survival_regen_step(s->food, health, &s->regen_timer,
                                   &s->exhaustion, dt);
    /* Settle the exhaustion the heal just charged into saturation/food so the
     * cost is visible immediately: saturation drains first, then food. The
     * `added` is 0 here because survival_regen_step already bumped exhaustion;
     * passing 0 just runs the threshold-draining loop on the existing value. */
    survival_apply_exhaustion(&s->food, &s->saturation, &s->exhaustion, 0.0f);
    return heal;
}

int survival_starve_step(float food, int health, float* starve_timer,
                         float dt)
{
    if (food > 0.0f || health <= SURVIVAL_STARVE_FLOOR) {
        *starve_timer = 0.0f;
        return 0;
    }
    *starve_timer += dt;
    int dmg = 0;
    while (*starve_timer >= SURVIVAL_STARVE_INTERVAL_SEC
           && health - dmg > SURVIVAL_STARVE_FLOOR) {
        *starve_timer -= SURVIVAL_STARVE_INTERVAL_SEC;
        dmg++;
    }
    return dmg;
}

int survival_drown_step(bool head_submerged, float* air, float* drown_timer,
                        float dt)
{
    if (!head_submerged) {
        *air = SURVIVAL_MAX_AIR_SEC;
        *drown_timer = 0.0f;
        return 0;
    }
    if (*air > 0.0f) {
        *air -= dt;
        if (*air < 0.0f) *air = 0.0f;
        return 0;
    }
    *drown_timer += dt;
    int dmg = 0;
    while (*drown_timer >= SURVIVAL_DROWN_INTERVAL_SEC) {
        *drown_timer -= SURVIVAL_DROWN_INTERVAL_SEC;
        dmg += SURVIVAL_DROWN_DAMAGE;
    }
    return dmg;
}

int survival_lava_step(bool touching_lava, float* lava_timer, float dt)
{
    if (!touching_lava) {
        *lava_timer = 0.0f;
        return 0;
    }
    *lava_timer += dt;
    int dmg = 0;
    while (*lava_timer >= SURVIVAL_LAVA_INTERVAL_SEC) {
        *lava_timer -= SURVIVAL_LAVA_INTERVAL_SEC;
        dmg += SURVIVAL_LAVA_DAMAGE;
    }
    return dmg;
}
