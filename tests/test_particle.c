#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/particle.h"

/* Helper: count particles whose life has already expired (should be none in
 * the live [0,count) window after an update — the pool must compact them out). */
static int count_dead(const ParticleSystem *ps) {
    int dead = 0;
    for (int i = 0; i < ps->count; i++)
        if (ps->p[i].life <= 0.0f)
            dead++;
    return dead;
}

/* Init zeroes the count and seeds the rng. */
static void test_init(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 12345u);
    assert(ps.count == 0);
    printf("PASS: init\n");
}

/* Emitting increases count, but never past PARTICLE_MAX. */
static void test_emit_increases_count(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 1u);
    int before = ps.count;
    particle_emit_block_break(&ps, 1.0f, 2.0f, 3.0f, 0.5f, 0.4f, 0.3f);
    assert(ps.count > before);
    assert(ps.count <= PARTICLE_MAX);
    printf("PASS: emit_increases_count\n");
}

/* All three emitters spawn at least one particle and stay in bounds. */
static void test_all_emitters_spawn(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 7u);
    particle_emit_block_break(&ps, 0, 0, 0, 1, 1, 1);
    assert(ps.count > 0 && ps.count <= PARTICLE_MAX);

    particle_system_init(&ps, 7u);
    particle_emit_explosion(&ps, 0, 0, 0);
    assert(ps.count > 0 && ps.count <= PARTICLE_MAX);

    particle_system_init(&ps, 7u);
    particle_emit_splash(&ps, 0, 0, 0);
    assert(ps.count > 0 && ps.count <= PARTICLE_MAX);
    printf("PASS: all_emitters_spawn\n");
}

/* Explosion is a bigger burst than a single block break. */
static void test_explosion_bigger_than_break(void) {
    ParticleSystem a, b;
    particle_system_init(&a, 3u);
    particle_system_init(&b, 3u);
    particle_emit_block_break(&a, 0, 0, 0, 1, 1, 1);
    particle_emit_explosion(&b, 0, 0, 0);
    assert(b.count > a.count);
    printf("PASS: explosion_bigger_than_break\n");
}

/* Emitted particles carry the requested position and the block tint. */
static void test_block_break_tint_and_pos(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 99u);
    particle_emit_block_break(&ps, 10.0f, 20.0f, 30.0f, 0.2f, 0.6f, 0.9f);
    assert(ps.count > 0);
    for (int i = 0; i < ps.count; i++) {
        /* spawned near the requested origin (debris jitter is small) */
        assert(ps.p[i].x > 9.0f && ps.p[i].x < 11.0f);
        assert(ps.p[i].y > 19.0f && ps.p[i].y < 21.0f);
        assert(ps.p[i].z > 29.0f && ps.p[i].z < 31.0f);
        /* tinted by the block color */
        assert(ps.p[i].r == 0.2f);
        assert(ps.p[i].g == 0.6f);
        assert(ps.p[i].b == 0.9f);
        assert(ps.p[i].life > 0.0f);
        assert(ps.p[i].max_life > 0.0f);
        assert(ps.p[i].size > 0.0f);
    }
    printf("PASS: block_break_tint_and_pos\n");
}

/* Update integrates position by velocity and applies gravity (vy decreases). */
static void test_update_moves_and_gravity(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 0u);
    /* Inject one deterministic particle by hand. */
    ps.count = 1;
    ps.p[0] = (Particle){ .x = 0, .y = 0, .z = 0,
                          .vx = 2.0f, .vy = 5.0f, .vz = -1.0f,
                          .life = 10.0f, .max_life = 10.0f,
                          .size = 0.1f, .r = 1, .g = 1, .b = 1 };
    float vy0 = ps.p[0].vy;
    float dt = 0.5f;
    particle_update(&ps, dt);
    assert(ps.count == 1);
    /* moved along velocity */
    assert(ps.p[0].x > 0.9f && ps.p[0].x < 1.1f);   /* ~2.0*0.5 = 1.0 */
    assert(ps.p[0].z < -0.4f && ps.p[0].z > -0.6f); /* ~-1.0*0.5 = -0.5 */
    /* gravity pulled vy down */
    assert(ps.p[0].vy < vy0);
    /* life decremented by dt */
    assert(ps.p[0].life > 9.49f && ps.p[0].life < 9.51f);
    printf("PASS: update_moves_and_gravity\n");
}

/* Particles die after max_life and are compacted out; no dead left in [0,count). */
static void test_death_and_compaction(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 5u);
    particle_emit_explosion(&ps, 0, 0, 0);
    int spawned = ps.count;
    assert(spawned > 0);

    /* Step long enough to outlive every particle. */
    for (int step = 0; step < 1000 && ps.count > 0; step++)
        particle_update(&ps, 0.1f);

    assert(ps.count == 0);
    assert(count_dead(&ps) == 0);
    printf("PASS: death_and_compaction (spawned %d, all died)\n", spawned);
}

/* Survivors stay live and dead are removed midway through their lifetimes. */
static void test_partial_compaction(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 0u);
    ps.count = 3;
    for (int i = 0; i < 3; i++)
        ps.p[i] = (Particle){ .life = (float)(i + 1), .max_life = (float)(i + 1),
                              .size = 0.1f };
    /* kill the middle-lived one (i=0 has life 1.0) by stepping 1.0s */
    particle_update(&ps, 1.0f);
    /* p[0] life 1.0 -> 0.0 -> dead; p[1] 2->1, p[2] 3->2 survive */
    assert(ps.count == 2);
    assert(count_dead(&ps) == 0);
    for (int i = 0; i < ps.count; i++)
        assert(ps.p[i].life > 0.0f);
    printf("PASS: partial_compaction\n");
}

/* Repeated large bursts must never overflow the pool. */
static void test_no_overflow(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 42u);
    for (int i = 0; i < 10000; i++) {
        particle_emit_explosion(&ps, (float)i, 0, 0);
        particle_emit_block_break(&ps, 0, (float)i, 0, 1, 1, 1);
        particle_emit_splash(&ps, 0, 0, (float)i);
        assert(ps.count >= 0);
        assert(ps.count <= PARTICLE_MAX);
    }
    /* Pool should be saturated, not overflowed. */
    assert(ps.count == PARTICLE_MAX);
    printf("PASS: no_overflow (count saturated at %d)\n", ps.count);
}

/* When full, new emissions are dropped — count must not move past the cap and
 * no out-of-bounds write happened (guarded by ASan/valgrind too). */
static void test_drop_when_full(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 1u);
    while (ps.count < PARTICLE_MAX)
        particle_emit_block_break(&ps, 0, 0, 0, 1, 1, 1);
    int full = ps.count;
    assert(full == PARTICLE_MAX);
    particle_emit_explosion(&ps, 0, 0, 0);
    assert(ps.count == full);
    printf("PASS: drop_when_full\n");
}

/* Deterministic for a fixed seed + identical call sequence. */
static void test_determinism(void) {
    ParticleSystem a, b;
    particle_system_init(&a, 0xBEEFu);
    particle_system_init(&b, 0xBEEFu);
    for (int i = 0; i < 20; i++) {
        particle_emit_block_break(&a, 1, 2, 3, 0.5f, 0.5f, 0.5f);
        particle_emit_block_break(&b, 1, 2, 3, 0.5f, 0.5f, 0.5f);
        particle_emit_splash(&a, 0, 0, 0);
        particle_emit_splash(&b, 0, 0, 0);
        particle_update(&a, 0.05f);
        particle_update(&b, 0.05f);
    }
    assert(a.count == b.count);
    assert(a.rng == b.rng);
    assert(memcmp(a.p, b.p, sizeof(Particle) * (size_t)a.count) == 0);
    printf("PASS: determinism\n");
}

int main(void) {
    test_init();
    test_emit_increases_count();
    test_all_emitters_spawn();
    test_explosion_bigger_than_break();
    test_block_break_tint_and_pos();
    test_update_moves_and_gravity();
    test_death_and_compaction();
    test_partial_compaction();
    test_no_overflow();
    test_drop_when_full();
    test_determinism();
    printf("ALL PARTICLE TESTS PASSED\n");
    return 0;
}
