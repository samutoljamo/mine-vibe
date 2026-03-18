#include "remote_player.h"
#include <string.h>

void remote_player_set_init(RemotePlayerSet* s)
{
    memset(s, 0, sizeof(*s));
}

void remote_player_push_snapshot(RemotePlayerSet* s, uint8_t pid,
                                  float x, float y, float z,
                                  float yaw, float pitch,
                                  double recv_time)
{
    /* Find or allocate slot */
    RemotePlayer* p = NULL;
    RemotePlayer* free_slot = NULL;
    for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
        if (s->players[i].active && s->players[i].player_id == pid) {
            p = &s->players[i]; break;
        }
        if (!s->players[i].active && !free_slot)
            free_slot = &s->players[i];
    }
    if (!p) {
        if (!free_slot) return; /* no space */
        p = free_slot;
        memset(p, 0, sizeof(*p));
        p->active    = true;
        p->player_id = pid;
    }

    /* Shift ring buffer */
    if (p->snapshot_count > 0) {
        glm_vec3_copy(p->positions[1], p->positions[0]);
        p->yaws[0]           = p->yaws[1];
        p->pitches[0]        = p->pitches[1];
        p->snapshot_times[0] = p->snapshot_times[1];
    }
    p->positions[1][0] = x;
    p->positions[1][1] = y;
    p->positions[1][2] = z;
    p->yaws[1]           = yaw;
    p->pitches[1]        = pitch;
    p->snapshot_times[1] = recv_time;

    if (p->snapshot_count < 2) {
        p->snapshot_count++;
        if (p->snapshot_count == 1)
            p->render_time = recv_time - REMOTE_PLAYER_DELAY;
    }
}

void remote_player_remove(RemotePlayerSet* s, uint8_t pid)
{
    for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
        if (s->players[i].active && s->players[i].player_id == pid) {
            s->players[i].active = false;
            return;
        }
    }
}

void remote_player_interpolate(RemotePlayer* p, float dt,
                                 vec3 out_pos, float* out_yaw, float* out_pitch)
{
    p->render_time += dt;
    double t0 = p->snapshot_times[0];
    double t1 = p->snapshot_times[1];
    float  t  = (t1 > t0) ? (float)((p->render_time - t0) / (t1 - t0)) : 1.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;

    out_pos[0] = p->positions[0][0] + t * (p->positions[1][0] - p->positions[0][0]);
    out_pos[1] = p->positions[0][1] + t * (p->positions[1][1] - p->positions[0][1]);
    out_pos[2] = p->positions[0][2] + t * (p->positions[1][2] - p->positions[0][2]);
    *out_yaw   = p->yaws[0]   + t * (p->yaws[1]   - p->yaws[0]);
    *out_pitch = p->pitches[0] + t * (p->pitches[1] - p->pitches[0]);
}

RemotePlayer* remote_player_set_get(RemotePlayerSet* s, uint8_t pid)
{
    for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
        if (s->players[i].active && s->players[i].player_id == pid)
            return &s->players[i];
    }
    return NULL;
}
