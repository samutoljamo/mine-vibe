#ifndef CAMERA_THIRD_H
#define CAMERA_THIRD_H

#include <cglm/cglm.h>

/* Camera modes cycled by F5. FIRST_PERSON is the default (eye == player eye).
 * The two third-person modes orbit the camera away from the player along the
 * look direction so the local body is visible. */
typedef enum {
    CAMERA_FIRST_PERSON       = 0,
    CAMERA_THIRD_PERSON_BACK  = 1,
    CAMERA_THIRD_PERSON_FRONT = 2,
    CAMERA_MODE_COUNT         = 3
} CameraMode;

/* Advance to the next camera mode (wraps FRONT -> FIRST_PERSON). */
static inline CameraMode camera_mode_next(CameraMode m) {
    return (CameraMode)(((int)m + 1) % CAMERA_MODE_COUNT);
}

/* PURE: compute the camera eye position and unit look direction for a given
 * mode. The look direction is the forward vector built from yaw/pitch (same
 * convention as camera_get_front).
 *
 *   FIRST_PERSON       : out_eye == player_eye, out_look_dir == forward.
 *   THIRD_PERSON_BACK  : out_eye sits `distance` BEHIND the player along the
 *                        look direction (slightly raised), still looking
 *                        forward (toward / past the player).
 *   THIRD_PERSON_FRONT : out_eye sits `distance` AHEAD of the player, looking
 *                        back toward the player (look dir negated).
 *
 * No collision/terrain awareness — a plain ray.
 */
void camera_third_person(const vec3 player_eye, float yaw, float pitch,
                         CameraMode mode, float distance,
                         vec3 out_eye, vec3 out_look_dir);

#endif
