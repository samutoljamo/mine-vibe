#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct Camera {
    float yaw;       /* radians */
    float pitch;     /* radians */
    float sensitivity;
    float fov;
    float last_x, last_y;
    bool  first_mouse;
} Camera;

void camera_init(Camera* cam);
void camera_process_mouse(Camera* cam, double xpos, double ypos);
void camera_get_view(Camera* cam, vec3 eye_pos, mat4 out);
void camera_get_proj(Camera* cam, float aspect, mat4 out);
void camera_get_front(Camera* cam, vec3 out);

#endif
