#include "particle.h"

/* Gravity pulling vy down each second (world units / s^2). Tuned for snappy
 * debris that arcs and settles quickly rather than physically realistic 9.8. */
#define PARTICLE_GRAVITY 18.0f

/* splitmix32: tiny, well-mixed, fully pure PRNG step. Mirrors loot.c so the
 * whole project shares one deterministic randomness style. */
uint32_t particle_rng_next(uint32_t *state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

/* Uniform float in [0,1). */
static float rng_unit(uint32_t *state)
{
    /* Use the top 24 bits for a clean [0,1) mantissa-sized value. */
    return (float)(particle_rng_next(state) >> 8) * (1.0f / 16777216.0f);
}

/* Uniform float in [lo, hi). */
static float rng_range(uint32_t *state, float lo, float hi)
{
    return lo + (hi - lo) * rng_unit(state);
}

void particle_system_init(ParticleSystem *ps, uint32_t seed)
{
    if (!ps)
        return;
    ps->count = 0;
    /* Avoid a zero state degenerating into a long low-entropy run. */
    ps->rng = seed ? seed : 0x1u;
}

/* Push one particle if there's room; drop it (return 0) when the pool is full.
 * Never writes past PARTICLE_MAX. */
static int particle_push(ParticleSystem *ps, const Particle *src)
{
    if (ps->count >= PARTICLE_MAX)
        return 0;
    ps->p[ps->count++] = *src;
    return 1;
}

void particle_update(ParticleSystem *ps, float dt)
{
    if (!ps || dt <= 0.0f)
        return;

    /* Integrate + age in place, compacting survivors toward the front. `w` is
     * the write cursor; `i` the read cursor. Dead particles are simply skipped
     * (not copied), which removes them in O(n) with no leftover slots. */
    int w = 0;
    for (int i = 0; i < ps->count; i++) {
        Particle pt = ps->p[i];

        pt.vy -= PARTICLE_GRAVITY * dt;
        pt.x += pt.vx * dt;
        pt.y += pt.vy * dt;
        pt.z += pt.vz * dt;
        pt.life -= dt;

        if (pt.life > 0.0f)
            ps->p[w++] = pt;
    }
    ps->count = w;
}

void particle_emit_block_break(ParticleSystem *ps, float x, float y, float z,
                               float r, float g, float b)
{
    if (!ps)
        return;

    const int burst = 10;
    for (int i = 0; i < burst; i++) {
        Particle pt;
        /* Small spawn jitter around the block face. */
        pt.x = x + rng_range(&ps->rng, -0.2f, 0.2f);
        pt.y = y + rng_range(&ps->rng, -0.2f, 0.2f);
        pt.z = z + rng_range(&ps->rng, -0.2f, 0.2f);
        /* Outward in the horizontal plane, biased upward. */
        pt.vx = rng_range(&ps->rng, -2.0f, 2.0f);
        pt.vy = rng_range(&ps->rng, 1.0f, 4.0f);
        pt.vz = rng_range(&ps->rng, -2.0f, 2.0f);
        pt.max_life = rng_range(&ps->rng, 0.4f, 0.8f);
        pt.life = pt.max_life;
        pt.size = rng_range(&ps->rng, 0.06f, 0.12f);
        pt.r = r;
        pt.g = g;
        pt.b = b;
        if (!particle_push(ps, &pt))
            break; /* pool full: drop the rest of this burst */
    }
}

void particle_emit_explosion(ParticleSystem *ps, float x, float y, float z)
{
    if (!ps)
        return;

    const int burst = 48;
    for (int i = 0; i < burst; i++) {
        Particle pt;
        pt.x = x + rng_range(&ps->rng, -0.3f, 0.3f);
        pt.y = y + rng_range(&ps->rng, -0.3f, 0.3f);
        pt.z = z + rng_range(&ps->rng, -0.3f, 0.3f);
        /* Strong radial blast in all directions. */
        pt.vx = rng_range(&ps->rng, -6.0f, 6.0f);
        pt.vy = rng_range(&ps->rng, -2.0f, 8.0f);
        pt.vz = rng_range(&ps->rng, -6.0f, 6.0f);
        pt.max_life = rng_range(&ps->rng, 0.8f, 1.6f);
        pt.life = pt.max_life;
        pt.size = rng_range(&ps->rng, 0.1f, 0.25f);
        /* Smoke/fire tint: warm, slightly randomized. */
        pt.r = rng_range(&ps->rng, 0.6f, 1.0f);
        pt.g = rng_range(&ps->rng, 0.3f, 0.6f);
        pt.b = rng_range(&ps->rng, 0.05f, 0.2f);
        if (!particle_push(ps, &pt))
            break;
    }
}

void particle_emit_splash(ParticleSystem *ps, float x, float y, float z)
{
    if (!ps)
        return;

    const int burst = 16;
    for (int i = 0; i < burst; i++) {
        Particle pt;
        pt.x = x + rng_range(&ps->rng, -0.15f, 0.15f);
        pt.y = y + rng_range(&ps->rng, 0.0f, 0.1f);
        pt.z = z + rng_range(&ps->rng, -0.15f, 0.15f);
        /* Mostly upward spray with a little horizontal spread. */
        pt.vx = rng_range(&ps->rng, -1.5f, 1.5f);
        pt.vy = rng_range(&ps->rng, 3.0f, 6.0f);
        pt.vz = rng_range(&ps->rng, -1.5f, 1.5f);
        pt.max_life = rng_range(&ps->rng, 0.5f, 1.0f);
        pt.life = pt.max_life;
        pt.size = rng_range(&ps->rng, 0.04f, 0.09f);
        /* Watery blue tint. */
        pt.r = rng_range(&ps->rng, 0.2f, 0.4f);
        pt.g = rng_range(&ps->rng, 0.4f, 0.6f);
        pt.b = rng_range(&ps->rng, 0.8f, 1.0f);
        if (!particle_push(ps, &pt))
            break;
    }
}

/* Per-call droplet count at full intensity. Kept small so a few hundred frames
 * of rain plus block-break bursts never saturate PARTICLE_MAX; combined with the
 * short lifetime below this holds the live rain count well under the cap. */
#define PARTICLE_RAIN_MAX_PER_CALL 12
/* Horizontal half-extent of the spawn box around the player (world units). */
#define PARTICLE_RAIN_RADIUS 8.0f
/* Height above the player center where droplets start. */
#define PARTICLE_RAIN_HEIGHT 10.0f

void particle_emit_rain(ParticleSystem *ps, float cx, float cy, float cz,
                        float intensity)
{
    if (!ps || intensity <= 0.0f)
        return;
    if (intensity > 1.0f)
        intensity = 1.0f;

    /* Scale the batch by intensity; at least one droplet for any positive
     * intensity so light rain is still visible. Round to nearest. */
    int count = (int)(PARTICLE_RAIN_MAX_PER_CALL * intensity + 0.5f);
    if (count < 1)
        count = 1;

    for (int i = 0; i < count; i++) {
        Particle pt;
        /* Random offset in a box centered on the player, spawned overhead. */
        pt.x = cx + rng_range(&ps->rng, -PARTICLE_RAIN_RADIUS, PARTICLE_RAIN_RADIUS);
        pt.y = cy + rng_range(&ps->rng, PARTICLE_RAIN_HEIGHT * 0.5f,
                                        PARTICLE_RAIN_HEIGHT);
        pt.z = cz + rng_range(&ps->rng, -PARTICLE_RAIN_RADIUS, PARTICLE_RAIN_RADIUS);
        /* Fast downward streak with a faint horizontal drift. Gravity will
         * accelerate it further during update. */
        pt.vx = rng_range(&ps->rng, -0.4f, 0.4f);
        pt.vy = rng_range(&ps->rng, -16.0f, -12.0f);
        pt.vz = rng_range(&ps->rng, -0.4f, 0.4f);
        /* Short life so droplets clear the player's vicinity and don't pile up. */
        pt.max_life = rng_range(&ps->rng, 0.5f, 0.9f);
        pt.life = pt.max_life;
        /* Thin streaks. */
        pt.size = rng_range(&ps->rng, 0.03f, 0.06f);
        /* Bluish-grey rain. */
        pt.r = rng_range(&ps->rng, 0.35f, 0.5f);
        pt.g = rng_range(&ps->rng, 0.45f, 0.6f);
        pt.b = rng_range(&ps->rng, 0.75f, 0.95f);
        if (!particle_push(ps, &pt))
            break; /* pool full: drop the rest (block-break bursts win) */
    }
}
