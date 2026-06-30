#ifndef REMOTE_PLAYER_H
#define REMOTE_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#define REMOTE_PLAYER_MAX    32
#define REMOTE_PLAYER_DELAY  0.025  /* seconds of interpolation lag */

typedef struct {
    uint8_t  player_id;
    bool     active;
    vec3     positions[2];      /* [0]=older [1]=newer */
    float    yaws[2], pitches[2];
    double   snapshot_times[2];
    uint8_t  snapshot_count;    /* 0, 1, or 2 */
    double   render_time;

    /* Walk-cycle animation state (client-side, driven from interpolated
     * motion — see player_anim.h). walk_phase advances with horizontal
     * movement; walk_speed is the smoothed horizontal speed used to scale the
     * swing amplitude and decay it to the rest pose when stopped. */
    float    walk_phase;
    float    walk_speed;
} RemotePlayer;

typedef struct {
    RemotePlayer players[REMOTE_PLAYER_MAX];
} RemotePlayerSet;

void remote_player_set_init(RemotePlayerSet* s);

/* Push a new snapshot (position + orientation + receive timestamp) */
void remote_player_push_snapshot(RemotePlayerSet* s,
                                  uint8_t player_id,
                                  float x, float y, float z,
                                  float yaw, float pitch,
                                  double recv_time);

/* Remove a player (on disconnect) */
void remote_player_remove(RemotePlayerSet* s, uint8_t player_id);

/* Advance render_time by dt and fill out interpolated state.
 * out_pos/yaw/pitch: interpolated values for each active+ready player. */
void remote_player_interpolate(RemotePlayer* p, float dt,
                                 vec3 out_pos, float* out_yaw, float* out_pitch);

/* Advance the player's walk-cycle animation by dt. Derives horizontal speed
 * from the two most recent snapshots (delta / interval) — robust against the
 * per-frame interpolation jitter — smooths it, and advances the walk phase.
 * Fills out_phase/out_speed for feeding player_anim_walk(). A stationary
 * player decays to speed 0 (rigid rest pose). */
void remote_player_update_anim(RemotePlayer* p, float dt,
                               float* out_phase, float* out_speed);

/* Get a player by id */
RemotePlayer* remote_player_set_get(RemotePlayerSet* s, uint8_t player_id);

#endif /* REMOTE_PLAYER_H */
