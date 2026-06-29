#ifndef PARTICLE_H
#define PARTICLE_H

#include <stdint.h>

/* Pure CPU particle simulation core.
 *
 * A fixed-capacity pool of short-lived debris/spray particles used for visual
 * effects: block-break debris, explosion bursts, water splashes. This module is
 * PURE — no Vulkan/GLFW, no rand()/time(). All randomness comes from a seeded
 * splitmix32 PRNG carried inside the system, so a given (seed + call sequence)
 * is fully deterministic and unit-testable.
 *
 * Lifecycle:
 *   particle_system_init(&ps, seed);          // empty pool, seeded rng
 *   particle_emit_*(&ps, ...);                // spawn a burst (clamped to cap)
 *   particle_update(&ps, dt);                 // integrate + age + compact
 *
 * GPU rendering integration (uploading the live pool to a vertex/instance
 * buffer and drawing it) is intentionally DEFERRED to a follow-up ticket; this
 * round is the pure simulation only.
 */

#define PARTICLE_MAX 1024

typedef struct {
    float x, y, z;       /* world position                     */
    float vx, vy, vz;    /* velocity (world units / second)    */
    float life;          /* remaining lifetime in seconds      */
    float max_life;      /* lifetime at spawn (for fade ratios)*/
    float size;          /* render size in world units         */
    float r, g, b;       /* tint color, 0..1                   */
} Particle;

typedef struct {
    Particle p[PARTICLE_MAX]; /* live particles occupy [0, count) */
    int      count;           /* number of live particles         */
    uint32_t rng;             /* splitmix32 state                 */
} ParticleSystem;

/* Reset the pool to empty and seed the PRNG. */
void particle_system_init(ParticleSystem *ps, uint32_t seed);

/* Pure PRNG step (splitmix32): advances *state and returns a uint32. */
uint32_t particle_rng_next(uint32_t *state);

/* Advance the simulation by dt seconds: integrate position by velocity, apply
 * gravity to vy, decrement life, then compact out any dead (life <= 0)
 * particles so [0, count) holds only live ones. Stable; never leaks slots. */
void particle_update(ParticleSystem *ps, float dt);

/* Spawn a small burst of debris at (x,y,z) with outward+upward velocities and a
 * short life, tinted by the given block color. Clamped to PARTICLE_MAX (extra
 * spawns are dropped when the pool is full). */
void particle_emit_block_break(ParticleSystem *ps, float x, float y, float z,
                               float r, float g, float b);

/* Spawn a larger radial burst at (x,y,z) with longer life (smoke/fire tint). */
void particle_emit_explosion(ParticleSystem *ps, float x, float y, float z);

/* Spawn an upward water-droplet spray at (x,y,z). */
void particle_emit_splash(ParticleSystem *ps, float x, float y, float z);

/* Spawn a small batch of fast DOWNWARD-falling rain streaks in a box centered
 * horizontally on (cx,cz) and starting ABOVE cy, so weather is visible around
 * the player. `intensity` in [0,1] scales the per-call droplet count (0 spawns
 * none). Droplets are thin, bluish and short-lived (they fall fast and expire
 * before piling up); the count is capped so the pool keeps headroom for other
 * effects, and any excess is dropped when the pool is full. Deterministic given
 * the system rng. */
void particle_emit_rain(ParticleSystem *ps, float cx, float cy, float cz,
                        float intensity);

#endif /* PARTICLE_H */
