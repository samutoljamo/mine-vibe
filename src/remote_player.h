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

/* Get a player by id */
RemotePlayer* remote_player_set_get(RemotePlayerSet* s, uint8_t player_id);

#endif /* REMOTE_PLAYER_H */
