#include "camera_third.h"
#include <math.h>

/* Slight vertical lift for the third-person eye so the camera looks down a
 * touch on the player rather than sitting dead level with the head. */
#define CAMERA_THIRD_LIFT 0.4f

void camera_third_person(const vec3 player_eye, float yaw, float pitch,
                         CameraMode mode, float distance,
                         vec3 out_eye, vec3 out_look_dir)
{
    /* Forward vector — identical convention to camera_get_front(). */
    vec3 forward = {
        cosf(pitch) * cosf(yaw),
        sinf(pitch),
        cosf(pitch) * sinf(yaw),
    };
    glm_vec3_normalize(forward);

    if (mode == CAMERA_FIRST_PERSON) {
        glm_vec3_copy((float*)player_eye, out_eye);
        glm_vec3_copy(forward, out_look_dir);
        return;
    }

    if (mode == CAMERA_THIRD_PERSON_FRONT) {
        /* Eye ahead of the player, looking back toward the player. */
        out_eye[0] = player_eye[0] + forward[0] * distance;
        out_eye[1] = player_eye[1] + forward[1] * distance + CAMERA_THIRD_LIFT;
        out_eye[2] = player_eye[2] + forward[2] * distance;
        out_look_dir[0] = -forward[0];
        out_look_dir[1] = -forward[1];
        out_look_dir[2] = -forward[2];
        glm_vec3_normalize(out_look_dir);
        return;
    }

    /* CAMERA_THIRD_PERSON_BACK (default for any other value): eye behind the
     * player along the look direction, still facing forward. */
    out_eye[0] = player_eye[0] - forward[0] * distance;
    out_eye[1] = player_eye[1] - forward[1] * distance + CAMERA_THIRD_LIFT;
    out_eye[2] = player_eye[2] - forward[2] * distance;
    glm_vec3_copy(forward, out_look_dir);
}
