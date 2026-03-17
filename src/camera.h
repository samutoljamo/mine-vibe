#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>

typedef struct Camera {
    vec3  position;
    float yaw;       /* radians */
    float pitch;     /* radians */
    float speed;
    float sensitivity;
    float fov;
    float last_x, last_y;
    bool  first_mouse;
} Camera;

void camera_init(Camera* cam, vec3 start_pos);
void camera_process_mouse(Camera* cam, double xpos, double ypos);
void camera_process_keyboard(Camera* cam, GLFWwindow* window, float dt);
void camera_get_view(Camera* cam, mat4 out);
void camera_get_proj(Camera* cam, float aspect, mat4 out);
void camera_get_front(Camera* cam, vec3 out);

#endif
