#include "player.h"   /* JUMP_VEL (used by MOB_JUMP_SPEED) */
#include "mob.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mob_set_init(MobSet* s) { memset(s, 0, sizeof(*s)); }

Mob* mob_set_get(MobSet* s, uint16_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < MOB_MAX; i++)
        if (s->mobs[i].active && s->mobs[i].id == id) return &s->mobs[i];
    return NULL;
}

void mob_set_remove(MobSet* s, uint16_t id) {
    Mob* m = mob_set_get(s, id);
    if (m) m->active = false;
}

Mob* mob_set_spawn(MobSet* s, MobType type, vec3 pos) {
    int slot = -1;
    for (int i = 0; i < MOB_MAX; i++)
        if (!s->mobs[i].active) { slot = i; break; }
    if (slot < 0) return NULL;

    /* Smallest unused nonzero id (≤ MOB_MAX active, so this terminates fast). */
    uint16_t id = 0;
    for (uint32_t cand = 1; cand <= 0xFFFF; cand++) {
        bool used = false;
        for (int i = 0; i < MOB_MAX; i++)
            if (s->mobs[i].active && s->mobs[i].id == (uint16_t)cand) { used = true; break; }
        if (!used) { id = (uint16_t)cand; break; }
    }
    if (id == 0) return NULL;

    Mob* m = &s->mobs[slot];
    memset(m, 0, sizeof(*m));
    m->id     = id;
    m->active = true;
    m->type   = type;
    glm_vec3_copy(pos, m->position);
    m->health = mob_stats(type).health;
    return m;
}

/* ---- Mob variety: per-type stats + AI helpers (pure) ---- */

MobStats mob_stats(MobType type) {
    switch (type) {
    case MOB_SKELETON:
        return (MobStats){
            .health = SKELETON_HEALTH,
            .speed  = MOB_SPEED,
            .attack_range    = SKELETON_SHOOT_RANGE,
            .attack_damage   = SKELETON_ATTACK_DAMAGE,
            .attack_interval = SKELETON_ATTACK_INTERVAL,
            .ranged   = true,
            .explodes = false,
            .passive  = false,
        };
    case MOB_CREEPER:
        return (MobStats){
            .health = CREEPER_HEALTH,
            .speed  = MOB_SPEED,
            .attack_range    = CREEPER_DETONATE_RANGE,
            .attack_damage   = CREEPER_BLAST_DAMAGE,
            .attack_interval = 0.0f,
            .ranged   = false,
            .explodes = true,
            .passive  = false,
        };
    /* Passive farm animals: wander, never attack. */
    case MOB_PIG:
        return (MobStats){
            .health = PIG_HEALTH, .speed = PASSIVE_SPEED, .passive = true,
        };
    case MOB_COW:
        return (MobStats){
            .health = COW_HEALTH, .speed = PASSIVE_SPEED, .passive = true,
        };
    case MOB_CHICKEN:
        return (MobStats){
            .health = CHICKEN_HEALTH, .speed = PASSIVE_SPEED, .passive = true,
        };
    case MOB_ZOMBIE:
    default:
        return (MobStats){
            .health = MOB_HEALTH,
            .speed  = MOB_SPEED,
            .attack_range    = MOB_ATTACK_RANGE,
            .attack_damage   = MOB_ATTACK_DAMAGE,
            .attack_interval = MOB_ATTACK_INTERVAL,
            .ranged   = false,
            .explodes = false,
            .passive  = false,
        };
    }
}

int mob_max_health(MobType type) {
    return mob_stats(type).health;
}

bool mob_is_passive(MobType type) {
    return mob_stats(type).passive;
}

/* Deterministic, smooth, bounded wander heading. Mixes the mob id into the
 * phase/frequency so each animal drifts independently, and folds the result
 * into [-PI, PI]. Pure: a function of (id, time) only — no global RNG/clock. */
float mob_wander_step(uint16_t mob_id, float time_s) {
    /* Per-mob phase + two slightly different frequencies → an aperiodic-looking
     * but fully deterministic drift. Scaled by PI so the sum lands in range
     * before folding, then folded to guarantee [-PI, PI]. */
    float phase = (float)mob_id * 1.61803399f;          /* golden-ratio spread */
    float a = sinf(time_s * 0.37f + phase);
    float b = sinf(time_s * 0.13f + phase * 2.0f);
    float h = (a + b) * 0.5f * (float)M_PI;             /* in [-PI, PI] */
    /* Fold defensively in case of fp rounding at the extremes. */
    while (h >  (float)M_PI) h -= 2.0f * (float)M_PI;
    while (h < -(float)M_PI) h += 2.0f * (float)M_PI;
    return h;
}

bool mob_flees_when_hit(MobType type, float panic_timer) {
    return mob_is_passive(type) && panic_timer > 0.0f;
}

bool skeleton_wants_to_retreat(float dist) {
    return dist < SKELETON_RETREAT_RANGE;
}

bool skeleton_in_shoot_range(float dist) {
    return dist <= SKELETON_SHOOT_RANGE;
}

bool creeper_should_arm_fuse(float dist) {
    return dist <= CREEPER_FUSE_RANGE;
}

bool creeper_should_detonate(float dist, float fuse_timer) {
    return dist <= CREEPER_DETONATE_RANGE && fuse_timer <= 0.0f;
}

uint16_t mob_acquire_target(const Mob* m, const MobTargetInfo* players, int count) {
    /* Passive farm animals never acquire (or retain) a target. */
    if (mob_is_passive(m->type)) return 0;
    /* Keep current target while within deaggro range (hysteresis). */
    if (m->target_player != 0) {
        for (int i = 0; i < count; i++) {
            if (players[i].player_id == m->target_player) {
                float d = glm_vec3_distance((float*)players[i].position,
                                            (float*)m->position);
                if (d <= MOB_DEAGGRO_RANGE) return m->target_player;
                break;
            }
        }
    }
    /* Otherwise acquire nearest within aggro range. */
    uint16_t best = 0;
    float best_d = MOB_AGGRO_RANGE;
    for (int i = 0; i < count; i++) {
        float d = glm_vec3_distance((float*)players[i].position, (float*)m->position);
        if (d <= best_d) { best_d = d; best = players[i].player_id; }
    }
    return best;
}

void mob_steer(const Mob* m, vec3 target_pos,
               float* out_vx, float* out_vz, float* out_yaw) {
    float dx = target_pos[0] - m->position[0];
    float dz = target_pos[2] - m->position[2];
    float len = sqrtf(dx*dx + dz*dz);
    if (len < 1e-4f) { *out_vx = 0.0f; *out_vz = 0.0f; *out_yaw = m->yaw; return; }
    *out_vx  = dx / len * MOB_SPEED;
    *out_vz  = dz / len * MOB_SPEED;
    *out_yaw = atan2f(dz, dx);   /* matches camera_get_front XZ convention */
}

bool mob_combat_apply(int16_t* health, int dmg) {
    bool was_alive = (*health > 0);
    *health = (int16_t)(*health - dmg);
    return was_alive && (*health <= 0);
}

/* ---- Ray-AABB helpers ---- */

/* Slab ray-AABB; returns entry t>=0 or -1 on miss. dir need not be normalized
 * (t is in units of |dir|; callers pass a unit dir so t is world distance). */
static float ray_aabb(vec3 o, vec3 d, vec3 lo, vec3 hi) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; a++) {
        if (fabsf(d[a]) < 1e-8f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return -1.0f;
        } else {
            float inv = 1.0f / d[a];
            float t1 = (lo[a] - o[a]) * inv;
            float t2 = (hi[a] - o[a]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return -1.0f;
        }
    }
    return tmin;
}

uint16_t mob_ray_hit(const ClientMobSet* s, vec3 origin, vec3 dir,
                     float max_dist, float* out_t) {
    uint16_t best = 0;
    float best_t = max_dist;
    for (int i = 0; i < MOB_MAX; i++) {
        const ClientMob* m = &s->mobs[i];
        if (!m->active || m->snapshot_count < 2) continue;
        /* Use the latest snapshot position as the AABB anchor (feet). */
        vec3 lo = { m->positions[1][0] - MOB_HALF_W, m->positions[1][1],
                    m->positions[1][2] - MOB_HALF_W };
        vec3 hi = { m->positions[1][0] + MOB_HALF_W, m->positions[1][1] + MOB_HEIGHT,
                    m->positions[1][2] + MOB_HALF_W };
        float t = ray_aabb(origin, dir, lo, hi);
        if (t >= 0.0f && t < best_t) { best_t = t; best = m->id; }
    }
    if (best && out_t) *out_t = best_t;
    return best;
}

/* ---- Client-side interpolated mob ---- */

#define MOB_TWO_PI_F 6.28318530f

void client_mob_set_init(ClientMobSet* s) { memset(s, 0, sizeof(*s)); }

static ClientMob* client_mob_find(ClientMobSet* s, uint16_t id) {
    for (int i = 0; i < MOB_MAX; i++)
        if (s->mobs[i].active && s->mobs[i].id == id) return &s->mobs[i];
    return NULL;
}

static void client_mob_push(ClientMob* m, const ClientMobSnapshot* snap, double recv_time) {
    if (m->snapshot_count > 0) {
        glm_vec3_copy(m->positions[1], m->positions[0]);
        m->yaws[0] = m->yaws[1];
        m->snapshot_times[0] = m->snapshot_times[1];
    }
    m->positions[1][0] = snap->x;
    m->positions[1][1] = snap->y;
    m->positions[1][2] = snap->z;
    m->yaws[1] = snap->yaw;
    m->snapshot_times[1] = recv_time;
    m->type = snap->type;
    m->health = snap->health;
    if (m->snapshot_count < 2) {
        m->snapshot_count++;
        if (m->snapshot_count == 1) m->render_time = recv_time - MOB_INTERP_DELAY;
    }
}

void client_mob_set_apply(ClientMobSet* s, const ClientMobSnapshot* snaps,
                          int count, double recv_time) {
    /* Mark all currently-active as unseen this update. */
    bool seen[MOB_MAX] = { false };
    for (int i = 0; i < count; i++) {
        ClientMob* m = client_mob_find(s, snaps[i].id);
        if (!m) {
            for (int j = 0; j < MOB_MAX; j++) if (!s->mobs[j].active) {
                memset(&s->mobs[j], 0, sizeof(s->mobs[j]));
                s->mobs[j].active = true;
                s->mobs[j].id = snaps[i].id;
                m = &s->mobs[j];
                break;
            }
            if (!m) continue; /* full */
        }
        client_mob_push(m, &snaps[i], recv_time);
        for (int j = 0; j < MOB_MAX; j++) if (&s->mobs[j] == m) { seen[j] = true; break; }
    }
    for (int j = 0; j < MOB_MAX; j++)
        if (s->mobs[j].active && !seen[j]) s->mobs[j].active = false;
}

void client_mob_interpolate(ClientMob* m, float dt, vec3 out_pos, float* out_yaw) {
    m->render_time += dt;
    double dt_snap = m->snapshot_times[1] - m->snapshot_times[0];
    if (m->snapshot_count < 2 || dt_snap <= 0.0) {
        glm_vec3_copy(m->positions[1], out_pos);
        *out_yaw = m->yaws[1];
        return;
    }
    if (m->render_time <= m->snapshot_times[1]) {
        double t = (m->render_time - m->snapshot_times[0]) / dt_snap;
        if (t > 1.0) t = 1.0; if (t < 0.0) t = 0.0;
        float tf = (float)t;
        out_pos[0] = m->positions[0][0] + tf * (m->positions[1][0] - m->positions[0][0]);
        out_pos[1] = m->positions[0][1] + tf * (m->positions[1][1] - m->positions[0][1]);
        out_pos[2] = m->positions[0][2] + tf * (m->positions[1][2] - m->positions[0][2]);
        float dyaw = m->yaws[1] - m->yaws[0];
        dyaw -= MOB_TWO_PI_F * roundf(dyaw / MOB_TWO_PI_F);
        *out_yaw = m->yaws[0] + tf * dyaw;
    } else {
        double excess = m->render_time - m->snapshot_times[1];
        if (excess > 2.0) excess = 2.0;
        float fe = (float)excess;
        float vx = (float)((m->positions[1][0] - m->positions[0][0]) / dt_snap);
        float vy = (float)((m->positions[1][1] - m->positions[0][1]) / dt_snap);
        float vz = (float)((m->positions[1][2] - m->positions[0][2]) / dt_snap);
        out_pos[0] = m->positions[1][0] + vx * fe;
        out_pos[1] = m->positions[1][1] + vy * fe;
        out_pos[2] = m->positions[1][2] + vz * fe;
        float dyaw = m->yaws[1] - m->yaws[0];
        dyaw -= MOB_TWO_PI_F * roundf(dyaw / MOB_TWO_PI_F);
        float yv = m->yaws[1] + (float)(dyaw / dt_snap) * fe;
        yv -= MOB_TWO_PI_F * roundf(yv / MOB_TWO_PI_F);
        *out_yaw = yv;
    }
}
