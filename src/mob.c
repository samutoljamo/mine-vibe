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
