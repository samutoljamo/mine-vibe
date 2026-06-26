#include "player.h"   /* JUMP_VEL (used by MOB_JUMP_SPEED) */
#include "mob.h"
#include <math.h>
#include <string.h>

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
    m->health = MOB_HEALTH;
    return m;
}

uint16_t mob_acquire_target(const Mob* m, const MobTargetInfo* players, int count) {
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
