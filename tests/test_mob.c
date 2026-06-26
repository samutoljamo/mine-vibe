#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/player.h"   /* JUMP_VEL, needed by mob.h tunables */
#include "../src/mob.h"

#define EPS 1e-4f
static int feq(float a, float b) { return fabsf(a - b) < EPS; }

static void test_spawn_assigns_unique_ids(void) {
    MobSet s; mob_set_init(&s);
    Mob* a = mob_set_spawn(&s, MOB_ZOMBIE, (vec3){0,64,0});
    Mob* b = mob_set_spawn(&s, MOB_ZOMBIE, (vec3){1,64,0});
    assert(a && b);
    assert(a->id != 0 && b->id != 0 && a->id != b->id);
    assert(a->health == MOB_HEALTH);
    assert(mob_set_get(&s, a->id) == a);
    mob_set_remove(&s, a->id);
    assert(mob_set_get(&s, a->id) == NULL);
    printf("PASS: spawn_assigns_unique_ids\n");
}

static void test_acquire_target_nearest_in_range(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    MobTargetInfo players[2] = {
        { .player_id = 1 }, { .player_id = 2 },
    };
    glm_vec3_copy((vec3){5,64,0},  players[0].position);  /* dist 5 */
    glm_vec3_copy((vec3){3,64,0},  players[1].position);  /* dist 3 (nearest) */
    assert(mob_acquire_target(&m, players, 2) == 2);
    printf("PASS: acquire_target_nearest_in_range\n");
}

static void test_acquire_target_out_of_range(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    MobTargetInfo p = { .player_id = 1 };
    glm_vec3_copy((vec3){100,64,0}, p.position);  /* beyond aggro */
    assert(mob_acquire_target(&m, &p, 1) == 0);
    printf("PASS: acquire_target_out_of_range\n");
}

static void test_acquire_target_hysteresis(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    m.target_player = 1;                       /* already chasing player 1 */
    MobTargetInfo p = { .player_id = 1 };
    glm_vec3_copy((vec3){20,64,0}, p.position); /* between aggro(16) and deaggro(24) */
    assert(mob_acquire_target(&m, &p, 1) == 1); /* retained */
    glm_vec3_copy((vec3){30,64,0}, p.position); /* beyond deaggro */
    assert(mob_acquire_target(&m, &p, 1) == 0); /* dropped */
    printf("PASS: acquire_target_hysteresis\n");
}

static void test_steer_points_at_target(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    float vx, vz, yaw;
    mob_steer(&m, (vec3){10,64,0}, &vx, &vz, &yaw);  /* +X */
    assert(feq(vx, MOB_SPEED));
    assert(feq(vz, 0.0f));
    assert(feq(yaw, atan2f(0.0f, 10.0f)));           /* = 0 */
    mob_steer(&m, (vec3){0,64,10}, &vx, &vz, &yaw);  /* +Z */
    assert(feq(vz, MOB_SPEED));
    assert(feq(yaw, atan2f(10.0f, 0.0f)));           /* = PI/2 */
    printf("PASS: steer_points_at_target\n");
}

static void test_combat_apply_and_death(void) {
    int16_t h = 20;
    assert(mob_combat_apply(&h, 5) == false); assert(h == 15);
    assert(mob_combat_apply(&h, 15) == true); assert(h <= 0);
    /* further hits on a dead entity still report dead but don't underflow weirdly */
    assert(mob_combat_apply(&h, 5) == false);  /* already dead -> not a fresh kill */
    printf("PASS: combat_apply_and_death\n");
}

int main(void) {
    test_spawn_assigns_unique_ids();
    test_acquire_target_nearest_in_range();
    test_acquire_target_out_of_range();
    test_acquire_target_hysteresis();
    test_steer_points_at_target();
    test_combat_apply_and_death();
    printf("All mob tests passed.\n");
    return 0;
}
