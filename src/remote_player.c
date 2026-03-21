#include "remote_player.h"
#include <math.h>
#include <string.h>

#define TWO_PI_F  6.28318530f
#define HALF_PI_F 1.57079632f

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

    double dt_snap = p->snapshot_times[1] - p->snapshot_times[0];

    /* Guard: need two snapshots and a positive interval to derive velocity */
    if (p->snapshot_count < 2 || dt_snap <= 0.0) {
        out_pos[0] = p->positions[1][0];
        out_pos[1] = p->positions[1][1];
        out_pos[2] = p->positions[1][2];
        *out_yaw   = p->yaws[1];
        *out_pitch = p->pitches[1];
        return;
    }

    if (p->render_time <= p->snapshot_times[1]) {
        /* Interpolation path */
        double t = (p->render_time - p->snapshot_times[0]) / dt_snap;
        if (t > 1.0) t = 1.0;
        if (t < 0.0) t = 0.0;
        float tf = (float)t;

        out_pos[0] = p->positions[0][0] + tf * (p->positions[1][0] - p->positions[0][0]);
        out_pos[1] = p->positions[0][1] + tf * (p->positions[1][1] - p->positions[0][1]);
        out_pos[2] = p->positions[0][2] + tf * (p->positions[1][2] - p->positions[0][2]);

        /* Wrap yaw delta to [-π, π] to take the short way round */
        float dyaw = p->yaws[1] - p->yaws[0];
        dyaw -= TWO_PI_F * roundf(dyaw / TWO_PI_F);
        *out_yaw   = p->yaws[0] + tf * dyaw;
        *out_pitch = p->pitches[0] + tf * (p->pitches[1] - p->pitches[0]);
    } else {
        /* Extrapolation path: render_time has overshot the latest snapshot */
        double excess = p->render_time - p->snapshot_times[1];
        if (excess > 2.0) excess = 2.0;  /* 2-second cap */
        float fexcess = (float)excess;

        /* Velocity from last two snapshots (float differences, double division) */
        float vel_x = (float)((p->positions[1][0] - p->positions[0][0]) / dt_snap);
        float vel_y = (float)((p->positions[1][1] - p->positions[0][1]) / dt_snap);
        float vel_z = (float)((p->positions[1][2] - p->positions[0][2]) / dt_snap);

        float dyaw = p->yaws[1] - p->yaws[0];
        dyaw -= TWO_PI_F * roundf(dyaw / TWO_PI_F);
        float yaw_vel = (float)(dyaw / dt_snap);

        float pit_vel = (float)((p->pitches[1] - p->pitches[0]) / dt_snap);

        out_pos[0] = p->positions[1][0] + vel_x * fexcess;
        out_pos[1] = p->positions[1][1] + vel_y * fexcess;
        out_pos[2] = p->positions[1][2] + vel_z * fexcess;

        /* Normalize yaw after accumulation to keep it in [-π, π] */
        float yv = p->yaws[1] + yaw_vel * fexcess;
        yv -= TWO_PI_F * roundf(yv / TWO_PI_F);
        *out_yaw = yv;

        /* Clamp pitch — interpolated values stay in range naturally; extrapolated may not */
        float pv = p->pitches[1] + pit_vel * fexcess;
        if (pv >  HALF_PI_F) pv =  HALF_PI_F;
        if (pv < -HALF_PI_F) pv = -HALF_PI_F;
        *out_pitch = pv;
    }
}

RemotePlayer* remote_player_set_get(RemotePlayerSet* s, uint8_t pid)
{
    for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
        if (s->players[i].active && s->players[i].player_id == pid)
            return &s->players[i];
    }
    return NULL;
}
