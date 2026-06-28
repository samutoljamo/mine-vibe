#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/survival.h"

static int fclose_eq(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* ---- fall damage ---- */
static void test_fall_damage(void) {
    /* Within the grace = no damage. */
    assert(survival_fall_damage(0.0f) == 0);
    assert(survival_fall_damage(SURVIVAL_FALL_SAFE) == 0);
    assert(survival_fall_damage(SURVIVAL_FALL_SAFE - 0.5f) == 0);
    /* 1 hp per block beyond grace (rounded). */
    assert(survival_fall_damage(SURVIVAL_FALL_SAFE + 1.0f) == 1);
    assert(survival_fall_damage(SURVIVAL_FALL_SAFE + 5.0f) == 5);
    /* A killing fall. */
    assert(survival_fall_damage(SURVIVAL_FALL_SAFE + 20.0f) == 20);
    /* Monotonic non-decreasing in distance. */
    int prev = 0;
    for (float d = 0; d < 60.0f; d += 0.25f) {
        int hp = survival_fall_damage(d);
        assert(hp >= prev || d <= SURVIVAL_FALL_SAFE);
        prev = hp;
    }
    printf("PASS: fall_damage\n");
}

/* ---- exhaustion / hunger decay ---- */
static void test_exhaustion_move(void) {
    assert(fclose_eq(survival_exhaustion_move(0.0f, false), 0.0f));
    assert(fclose_eq(survival_exhaustion_move(-1.0f, false), 0.0f));
    /* Sprinting costs strictly more than walking the same distance. */
    assert(survival_exhaustion_move(10.0f, true)
           > survival_exhaustion_move(10.0f, false));
    assert(fclose_eq(survival_exhaustion_move(10.0f, false),
                     10.0f * SURVIVAL_EXH_WALK_PER_M));
    printf("PASS: exhaustion_move\n");
}

static void test_apply_exhaustion(void) {
    float food = 20.0f, sat = 5.0f, exh = 0.0f;
    /* Below one threshold: nothing drains, exhaustion accumulates. */
    survival_apply_exhaustion(&food, &sat, &exh, SURVIVAL_EXHAUSTION_PER_POINT - 0.1f);
    assert(fclose_eq(food, 20.0f) && fclose_eq(sat, 5.0f));
    assert(exh > 0.0f && exh < SURVIVAL_EXHAUSTION_PER_POINT);

    /* One full threshold: drains saturation first, not food. */
    survival_apply_exhaustion(&food, &sat, &exh, 0.1f);
    assert(fclose_eq(food, 20.0f) && fclose_eq(sat, 4.0f));

    /* Drain all saturation, then food starts dropping. */
    food = 20.0f; sat = 0.0f; exh = 0.0f;
    survival_apply_exhaustion(&food, &sat, &exh, SURVIVAL_EXHAUSTION_PER_POINT * 3.0f);
    assert(fclose_eq(food, 17.0f));

    /* Food never goes below zero. */
    food = 1.0f; sat = 0.0f; exh = 0.0f;
    survival_apply_exhaustion(&food, &sat, &exh, SURVIVAL_EXHAUSTION_PER_POINT * 10.0f);
    assert(food >= 0.0f && fclose_eq(food, 0.0f));
    printf("PASS: apply_exhaustion\n");
}

/* ---- eating ---- */
static void test_eat(void) {
    float food = 10.0f, sat = 2.0f;
    bool ate = survival_eat(&food, &sat, 4.0f, 2.4f);
    assert(ate && fclose_eq(food, 14.0f) && fclose_eq(sat, 4.4f));

    /* Saturation can never exceed food (capped at food after eating). */
    food = 19.0f; sat = 0.0f;
    survival_eat(&food, &sat, 4.0f, 20.0f);
    assert(fclose_eq(food, 20.0f));
    assert(sat <= food + 1e-4f);
    assert(fclose_eq(sat, 20.0f));

    /* Eating while full is a no-op. */
    food = (float)SURVIVAL_MAX_FOOD; sat = 5.0f;
    assert(survival_eat(&food, &sat, 4.0f, 2.0f) == false);
    assert(fclose_eq(food, (float)SURVIVAL_MAX_FOOD));
    printf("PASS: eat\n");
}

/* ---- eating rule (server-authoritative wiring helpers) ---- */
static void test_can_eat(void) {
    const float MAX = (float)SURVIVAL_MAX_FOOD;
    /* Food + hunger below max -> may eat. */
    assert(survival_can_eat(true, 0.0f, MAX));
    assert(survival_can_eat(true, MAX - 1.0f, MAX));
    /* Full hunger -> refuse even a food item (don't waste it). */
    assert(survival_can_eat(true, MAX, MAX) == false);
    assert(survival_can_eat(true, MAX + 5.0f, MAX) == false);
    /* Non-food item -> never eatable, regardless of hunger. */
    assert(survival_can_eat(false, 0.0f, MAX) == false);
    assert(survival_can_eat(false, MAX, MAX) == false);
    printf("PASS: can_eat\n");
}

static void test_apply_food(void) {
    SurvivalState s;
    /* Partial restore from a hungry state. */
    survival_init(&s);
    s.food = 5.0f; s.saturation = 0.0f;
    assert(survival_apply_food(&s, 6) == true);
    assert(fclose_eq(s.food, 11.0f));
    assert(s.saturation <= s.food + 1e-4f);

    /* Clamp at the max: a big restore tops out at SURVIVAL_MAX_FOOD. */
    survival_init(&s);
    s.food = 18.0f; s.saturation = 0.0f;
    assert(survival_apply_food(&s, 20) == true);
    assert(fclose_eq(s.food, (float)SURVIVAL_MAX_FOOD));
    assert(s.saturation <= s.food + 1e-4f);

    /* Already full -> no-op, returns false. */
    survival_init(&s);
    s.food = (float)SURVIVAL_MAX_FOOD; s.saturation = 5.0f;
    assert(survival_apply_food(&s, 8) == false);
    assert(fclose_eq(s.food, (float)SURVIVAL_MAX_FOOD));

    /* Non-positive restore (e.g. non-food item slipped through) -> no-op. */
    survival_init(&s);
    s.food = 5.0f;
    assert(survival_apply_food(&s, 0) == false);
    assert(fclose_eq(s.food, 5.0f));
    printf("PASS: apply_food\n");
}

/* ---- regen ---- */
static void test_regen(void) {
    float timer = 0.0f, exh = 0.0f;
    /* No regen below the food threshold. */
    int h = survival_regen_step(SURVIVAL_REGEN_FOOD_THRESHOLD - 1, 10, &timer, &exh, 100.0f);
    assert(h == 0);
    /* No regen at full health. */
    timer = 0.0f;
    h = survival_regen_step(20.0f, SURVIVAL_MAX_HEALTH, &timer, &exh, 100.0f);
    assert(h == 0);
    /* Heals 1 hp per interval when well-fed and hurt. */
    timer = 0.0f; exh = 0.0f;
    h = survival_regen_step(20.0f, 10, &timer, &exh, SURVIVAL_REGEN_INTERVAL_SEC);
    assert(h == 1);
    assert(exh > 0.0f);   /* healing costs exhaustion */
    /* Several intervals at once heal multiple hp but never past max. */
    timer = 0.0f; exh = 0.0f;
    h = survival_regen_step(20.0f, SURVIVAL_MAX_HEALTH - 2,
                            &timer, &exh, SURVIVAL_REGEN_INTERVAL_SEC * 10.0f);
    assert(h == 2);
    printf("PASS: regen\n");
}

/* ---- starvation ---- */
static void test_starve(void) {
    float timer = 0.0f;
    /* No starvation while fed. */
    assert(survival_starve_step(5.0f, 20, &timer, 100.0f) == 0);
    /* Starves 1 hp per interval at food 0. */
    timer = 0.0f;
    assert(survival_starve_step(0.0f, 20, &timer, SURVIVAL_STARVE_INTERVAL_SEC) == 1);
    /* Never drops below the floor. */
    timer = 0.0f;
    int dmg = survival_starve_step(0.0f, SURVIVAL_STARVE_FLOOR,
                                   &timer, SURVIVAL_STARVE_INTERVAL_SEC * 10.0f);
    assert(dmg == 0);
    printf("PASS: starve\n");
}

/* ---- drowning ---- */
static void test_drown(void) {
    float air = SURVIVAL_MAX_AIR_SEC, dt_timer = 0.0f;
    /* Not submerged: air refills, no damage. */
    air = 1.0f; dt_timer = 0.5f;
    assert(survival_drown_step(false, &air, &dt_timer, 0.1f) == 0);
    assert(fclose_eq(air, SURVIVAL_MAX_AIR_SEC) && fclose_eq(dt_timer, 0.0f));

    /* Submerged: air drains, no damage while air remains. */
    air = SURVIVAL_MAX_AIR_SEC; dt_timer = 0.0f;
    assert(survival_drown_step(true, &air, &dt_timer, 1.0f) == 0);
    assert(air < SURVIVAL_MAX_AIR_SEC);

    /* Out of air + a full interval underwater: damage. */
    air = 0.0f; dt_timer = 0.0f;
    int dmg = survival_drown_step(true, &air, &dt_timer, SURVIVAL_DROWN_INTERVAL_SEC);
    assert(dmg == SURVIVAL_DROWN_DAMAGE);
    printf("PASS: drown\n");
}

/* ---- lava ---- */
static void test_lava(void) {
    float timer = 0.0f;
    assert(survival_lava_step(false, &timer, 100.0f) == 0);
    assert(fclose_eq(timer, 0.0f));
    timer = 0.0f;
    assert(survival_lava_step(true, &timer, SURVIVAL_LAVA_INTERVAL_SEC) == SURVIVAL_LAVA_DAMAGE);
    /* Leaving lava clears the timer. */
    timer = 0.3f;
    assert(survival_lava_step(false, &timer, 0.1f) == 0);
    assert(fclose_eq(timer, 0.0f));
    printf("PASS: lava\n");
}

/* ---- init sanity ---- */
static void test_init(void) {
    SurvivalState s;
    survival_init(&s);
    assert(fclose_eq(s.food, (float)SURVIVAL_MAX_FOOD));
    assert(fclose_eq(s.air, SURVIVAL_MAX_AIR_SEC));
    assert(s.saturation <= s.food);
    printf("PASS: init\n");
}

int main(void) {
    test_fall_damage();
    test_exhaustion_move();
    test_apply_exhaustion();
    test_eat();
    test_can_eat();
    test_apply_food();
    test_regen();
    test_starve();
    test_drown();
    test_lava();
    test_init();
    printf("All survival tests passed.\n");
    return 0;
}
