#include "camera.h"
#include <math.h>

void camera_init(Camera* cam, vec3 start_pos)
{
    glm_vec3_copy(start_pos, cam->position);
    cam->yaw         = -(float)GLM_PI_2;  /* -PI/2 => looking along -Z */
    cam->pitch       = 0.0f;
    cam->speed       = 20.0f;
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

    /* Clamp pitch to avoid gimbal lock */
    float limit = (float)GLM_PI_2 - 0.01f;
    if (cam->pitch >  limit) cam->pitch =  limit;
    if (cam->pitch < -limit) cam->pitch = -limit;
}

void camera_process_keyboard(Camera* cam, GLFWwindow* window, float dt)
{
    float velocity = cam->speed * dt;

    vec3 front;
    camera_get_front(cam, front);

    /* Horizontal forward (project onto XZ plane) */
    vec3 forward = { front[0], 0.0f, front[2] };
    glm_vec3_normalize(forward);

    /* Right vector */
    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(forward, up, right);
    glm_vec3_normalize(right);

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        cam->position[0] += forward[0] * velocity;
        cam->position[1] += forward[1] * velocity;
        cam->position[2] += forward[2] * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        cam->position[0] -= forward[0] * velocity;
        cam->position[1] -= forward[1] * velocity;
        cam->position[2] -= forward[2] * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        cam->position[0] -= right[0] * velocity;
        cam->position[1] -= right[1] * velocity;
        cam->position[2] -= right[2] * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        cam->position[0] += right[0] * velocity;
        cam->position[1] += right[1] * velocity;
        cam->position[2] += right[2] * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        cam->position[1] += velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        cam->position[1] -= velocity;
    }
}

void camera_get_view(Camera* cam, mat4 out)
{
    vec3 front;
    camera_get_front(cam, front);

    vec3 center;
    glm_vec3_add(cam->position, front, center);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    glm_lookat(cam->position, center, up, out);
}

void camera_get_proj(Camera* cam, float aspect, mat4 out)
{
    glm_perspective(cam->fov, aspect, 1.0f, 2000.0f, out);
    /* Flip Y for Vulkan (Vulkan Y-down, OpenGL Y-up) */
    out[1][1] *= -1.0f;
}
