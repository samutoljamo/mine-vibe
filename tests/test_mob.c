#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/player.h"   /* JUMP_VEL, needed by mob.h tunables */
#include "../src/mob.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static void test_client_apply_and_deactivate(void) {
    ClientMobSet s; client_mob_set_init(&s);
    ClientMobSnapshot a[1] = {{ .id = 5, .type = 0, .x = 0, .y = 64, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, a, 1, 100.0);
    /* present again, moved */
    ClientMobSnapshot b[1] = {{ .id = 5, .type = 0, .x = 10, .y = 64, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, b, 1, 100.05);
    int found = -1;
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].active && s.mobs[i].id == 5) found = i;
    assert(found >= 0 && s.mobs[found].snapshot_count == 2);
    /* absent → deactivated */
    client_mob_set_apply(&s, NULL, 0, 100.10);
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].id == 5) assert(!s.mobs[i].active);
    printf("PASS: client_apply_and_deactivate\n");
}

static void test_client_interpolate_midpoint(void) {
    ClientMobSet s; client_mob_set_init(&s);
    ClientMobSnapshot a[1] = {{ .id = 1, .x = 0,  .y = 0, .z = 0, .yaw = 0, .health = 20 }};
    ClientMobSnapshot b[1] = {{ .id = 1, .x = 10, .y = 0, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, a, 1, 0.0);
    client_mob_set_apply(&s, b, 1, 1.0);
    ClientMob* m = NULL;
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].active && s.mobs[i].id == 1) m = &s.mobs[i];
    assert(m); m->render_time = 0.5;
    vec3 pos; float yaw;
    client_mob_interpolate(m, 0.0f, pos, &yaw);
    assert(feq(pos[0], 5.0f));
    printf("PASS: client_interpolate_midpoint\n");
}

static void test_mob_ray_hit(void) {
    ClientMobSet s; client_mob_set_init(&s);
    /* Place an active, render-ready mob at feet (5,0,0). */
    ClientMobSnapshot a[1] = {{ .id = 3, .x = 5, .y = 0, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, a, 1, 0.0);
    client_mob_set_apply(&s, a, 1, 1.0);   /* 2 snapshots so it's render-ready */
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].id == 3) s.mobs[i].render_time = 1.0;

    float t = 0.0f;
    /* Ray from origin (0, 1, 0) pointing +X at chest height hits the mob. */
    uint16_t id = mob_ray_hit(&s, (vec3){0,1,0}, (vec3){1,0,0}, 10.0f, &t);
    assert(id == 3);
    assert(t > 4.0f && t < 5.2f);   /* AABB front face near x≈4.7 */

    /* Ray pointing away misses. */
    id = mob_ray_hit(&s, (vec3){0,1,0}, (vec3){-1,0,0}, 10.0f, &t);
    assert(id == 0);
    printf("PASS: mob_ray_hit\n");
}

/* ---- Mob variety: per-type stats + AI helpers ---- */

static void test_per_type_stats(void) {
    MobStats z = mob_stats(MOB_ZOMBIE);
    MobStats k = mob_stats(MOB_SKELETON);
    MobStats c = mob_stats(MOB_CREEPER);

    /* Zombie keeps the legacy melee numbers. */
    assert(z.health == MOB_HEALTH);
    assert(z.attack_damage == MOB_ATTACK_DAMAGE);
    assert(feq(z.attack_range, MOB_ATTACK_RANGE));
    assert(z.ranged == false);
    assert(z.explodes == false);

    /* Skeleton is a ranged attacker, distinct stats, fragile-ish. */
    assert(k.ranged == true);
    assert(k.explodes == false);
    assert(k.attack_range > MOB_ATTACK_RANGE);   /* shoots from afar */
    assert(k.health > 0);

    /* Creeper explodes; big damage, short range, dies on detonation. */
    assert(c.explodes == true);
    assert(c.ranged == false);
    assert(c.attack_damage > z.attack_damage);
    assert(c.health > 0);

    /* Spawn uses per-type health, not a hardcoded constant. */
    MobSet s; mob_set_init(&s);
    Mob* m = mob_set_spawn(&s, MOB_CREEPER, (vec3){0,64,0});
    assert(m && m->health == c.health);
    Mob* sk = mob_set_spawn(&s, MOB_SKELETON, (vec3){1,64,0});
    assert(sk && sk->health == k.health);
    printf("PASS: per_type_stats\n");
}

static void test_mob_max_health(void) {
    /* All types have a positive max-health pool. */
    for (int t = 0; t < MOB_TYPE_COUNT; t++)
        assert(mob_max_health((MobType)t) > 0);

    /* Matches the per-type stat table (single source of truth). */
    for (int t = 0; t < MOB_TYPE_COUNT; t++)
        assert(mob_max_health((MobType)t) == mob_stats((MobType)t).health);

    /* Hostiles are tankier than every passive farm animal. */
    int min_hostile = mob_max_health(MOB_ZOMBIE);
    if (mob_max_health(MOB_SKELETON) < min_hostile) min_hostile = mob_max_health(MOB_SKELETON);
    if (mob_max_health(MOB_CREEPER)  < min_hostile) min_hostile = mob_max_health(MOB_CREEPER);
    int max_passive = mob_max_health(MOB_PIG);
    if (mob_max_health(MOB_COW)     > max_passive) max_passive = mob_max_health(MOB_COW);
    if (mob_max_health(MOB_CHICKEN) > max_passive) max_passive = mob_max_health(MOB_CHICKEN);
    assert(min_hostile > max_passive);

    /* Chicken is the frailest mob. */
    for (int t = 0; t < MOB_TYPE_COUNT; t++)
        if (t != MOB_CHICKEN)
            assert(mob_max_health(MOB_CHICKEN) <= mob_max_health((MobType)t));

    /* A freshly spawned mob starts at full (max) health. */
    MobSet s; mob_set_init(&s);
    Mob* z = mob_set_spawn(&s, MOB_ZOMBIE, (vec3){0,64,0});
    assert(z && z->health == mob_max_health(MOB_ZOMBIE));
    Mob* ch = mob_set_spawn(&s, MOB_CHICKEN, (vec3){1,64,0});
    assert(ch && ch->health == mob_max_health(MOB_CHICKEN));
    printf("PASS: mob_max_health\n");
}

static void test_skeleton_retreat_and_shoot(void) {
    /* Too close -> wants to back off. */
    assert(skeleton_wants_to_retreat(SKELETON_RETREAT_RANGE - 0.5f) == true);
    /* At/beyond the comfortable standoff -> hold ground. */
    assert(skeleton_wants_to_retreat(SKELETON_RETREAT_RANGE + 0.5f) == false);

    /* In shoot band: between retreat distance and max shoot range. */
    assert(skeleton_in_shoot_range(SKELETON_SHOOT_RANGE - 1.0f) == true);
    /* Out of range -> can't shoot. */
    assert(skeleton_in_shoot_range(SKELETON_SHOOT_RANGE + 1.0f) == false);
    /* Point-blank is still "in range" (it can shoot while retreating). */
    assert(skeleton_in_shoot_range(0.5f) == true);
    printf("PASS: skeleton_retreat_and_shoot\n");
}

static void test_creeper_detonation(void) {
    /* Out of detonation radius: never blows up regardless of fuse. */
    assert(creeper_should_detonate(CREEPER_DETONATE_RANGE + 1.0f, 0.0f) == false);
    /* In range but fuse still burning. */
    assert(creeper_should_detonate(CREEPER_DETONATE_RANGE - 0.5f, 0.5f) == false);
    /* In range and fuse elapsed -> boom. */
    assert(creeper_should_detonate(CREEPER_DETONATE_RANGE - 0.5f, 0.0f) == true);
    assert(creeper_should_detonate(CREEPER_DETONATE_RANGE - 0.5f, -0.1f) == true);

    /* Fuse should arm only when the creeper is close enough to start hissing. */
    assert(creeper_should_arm_fuse(CREEPER_FUSE_RANGE - 0.5f) == true);
    assert(creeper_should_arm_fuse(CREEPER_FUSE_RANGE + 0.5f) == false);
    printf("PASS: creeper_detonation\n");
}

/* ---- Passive mobs (pig/cow/chicken): classification, stats, wander, flee ---- */

static void test_passive_classification(void) {
    /* Hostiles are not passive. */
    assert(mob_is_passive(MOB_ZOMBIE)   == false);
    assert(mob_is_passive(MOB_SKELETON) == false);
    assert(mob_is_passive(MOB_CREEPER)  == false);
    /* Farm animals are passive. */
    assert(mob_is_passive(MOB_PIG)     == true);
    assert(mob_is_passive(MOB_COW)     == true);
    assert(mob_is_passive(MOB_CHICKEN) == true);
    printf("PASS: passive_classification\n");
}

static void test_passive_stats_non_hostile(void) {
    MobType ts[3] = { MOB_PIG, MOB_COW, MOB_CHICKEN };
    for (int i = 0; i < 3; i++) {
        MobStats st = mob_stats(ts[i]);
        assert(st.passive == true);
        assert(st.health > 0);
        assert(st.speed > 0.0f);
        /* Passive => no attacking at all. */
        assert(st.attack_damage == 0);
        assert(st.ranged   == false);
        assert(st.explodes == false);
    }
    /* Hostiles report passive == false. */
    assert(mob_stats(MOB_ZOMBIE).passive   == false);
    assert(mob_stats(MOB_SKELETON).passive == false);
    assert(mob_stats(MOB_CREEPER).passive  == false);

    /* Spawn uses per-type health for passive mobs too. */
    MobSet s; mob_set_init(&s);
    Mob* cow = mob_set_spawn(&s, MOB_COW, (vec3){0,64,0});
    assert(cow && cow->health == mob_stats(MOB_COW).health);
    printf("PASS: passive_stats_non_hostile\n");
}

static void test_passive_never_targets(void) {
    /* A passive mob standing right next to a player must not acquire it. */
    Mob m; memset(&m, 0, sizeof(m));
    m.type = MOB_PIG;
    glm_vec3_copy((vec3){0,64,0}, m.position);
    MobTargetInfo p = { .player_id = 7 };
    glm_vec3_copy((vec3){1,64,0}, p.position);  /* point-blank */
    assert(mob_acquire_target(&m, &p, 1) == 0);

    /* Even with a pre-set target (defensive), passive drops it. */
    m.target_player = 7;
    assert(mob_acquire_target(&m, &p, 1) == 0);

    /* Sanity: a hostile in the same spot DOES target. */
    Mob z; memset(&z, 0, sizeof(z));
    z.type = MOB_ZOMBIE;
    glm_vec3_copy((vec3){0,64,0}, z.position);
    assert(mob_acquire_target(&z, &p, 1) == 7);
    printf("PASS: passive_never_targets\n");
}

static void test_wander_determinism_and_bounds(void) {
    /* Same (id, time) => same heading; pure, no globals. */
    float a = mob_wander_step(3, 12.0f);
    float b = mob_wander_step(3, 12.0f);
    assert(feq(a, b));

    /* Heading is a bounded angle in [-PI, PI]. */
    for (uint16_t id = 1; id <= 20; id++) {
        for (int k = 0; k < 16; k++) {
            float t = (float)k * 2.137f;
            float h = mob_wander_step(id, t);
            assert(h >= -(float)M_PI - EPS && h <= (float)M_PI + EPS);
        }
    }

    /* Heading varies by id and over time (not a constant). */
    assert(!feq(mob_wander_step(1, 5.0f), mob_wander_step(2, 5.0f)) ||
           !feq(mob_wander_step(1, 5.0f), mob_wander_step(3, 5.0f)));
    assert(!feq(mob_wander_step(4, 0.0f), mob_wander_step(4, 50.0f)));
    printf("PASS: wander_determinism_and_bounds\n");
}

static void test_flee_on_hit(void) {
    /* Just hit (panic timer fresh) => fleeing. */
    assert(mob_flees_when_hit(MOB_PIG, MOB_PANIC_TIME) == true);
    assert(mob_flees_when_hit(MOB_COW, 0.5f)           == true);
    /* Panic elapsed => calm again. */
    assert(mob_flees_when_hit(MOB_CHICKEN, 0.0f)  == false);
    assert(mob_flees_when_hit(MOB_PIG, -0.1f)     == false);
    /* Hostiles don't "flee" via this helper (they fight back). */
    assert(mob_flees_when_hit(MOB_ZOMBIE, MOB_PANIC_TIME) == false);
    printf("PASS: flee_on_hit\n");
}

int main(void) {
    test_spawn_assigns_unique_ids();
    test_acquire_target_nearest_in_range();
    test_acquire_target_out_of_range();
    test_acquire_target_hysteresis();
    test_steer_points_at_target();
    test_combat_apply_and_death();
    test_client_apply_and_deactivate();
    test_client_interpolate_midpoint();
    test_mob_ray_hit();
    test_per_type_stats();
    test_mob_max_health();
    test_skeleton_retreat_and_shoot();
    test_creeper_detonation();
    test_passive_classification();
    test_passive_stats_non_hostile();
    test_passive_never_targets();
    test_wander_determinism_and_bounds();
    test_flee_on_hit();
    printf("All mob tests passed.\n");
    return 0;
}
