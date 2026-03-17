#include "camera.h"
#include <math.h>

void camera_init(Camera* cam)
{
    cam->yaw         = -(float)GLM_PI_2;
    cam->pitch       = 0.0f;
    cam->sensitivity = 0.002f;
    cam->fov         = glm_rad(70.0f);
    cam->first_mouse = true;
    cam->last_x      = 0.0f;
    cam->last_y      = 0.0f;
}

void camera_get_front(Camera* cam, vec3 out)
{
    out[0] = cosf(cam->pitch) * cosf(cam->yaw);
    out[1] = sinf(cam->pitch);
    out[2] = cosf(cam->pitch) * sinf(cam->yaw);
    glm_vec3_normalize(out);
}

void camera_process_mouse(Camera* cam, double xpos, double ypos)
{
    float xf = (float)xpos;
    float yf = (float)ypos;

    if (cam->first_mouse) {
        cam->last_x = xf;
        cam->last_y = yf;
        cam->first_mouse = false;
        return;
    }

    float dx = xf - cam->last_x;
    float dy = yf - cam->last_y;
    cam->last_x = xf;
    cam->last_y = yf;

    cam->yaw   += dx * cam->sensitivity;
    cam->pitch -= dy * cam->sensitivity;

    float limit = (float)GLM_PI_2 - 0.01f;
    if (cam->pitch >  limit) cam->pitch =  limit;
    if (cam->pitch < -limit) cam->pitch = -limit;
}

void camera_get_view(Camera* cam, vec3 eye_pos, mat4 out)
{
    vec3 front;
    camera_get_front(cam, front);

    vec3 center;
    glm_vec3_add(eye_pos, front, center);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    glm_lookat(eye_pos, center, up, out);
}

void camera_get_proj(Camera* cam, float aspect, mat4 out)
{
    glm_perspective(cam->fov, aspect, 0.1f, 1000.0f, out);
    out[1][1] *= -1.0f;
}
