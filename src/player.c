#include "player.h"
#include <math.h>
#include <GLFW/glfw3.h>

#define PLAYER_EYE_H  1.62f
#define FLY_SPEED      20.0f

void player_init(Player* player, vec3 start_pos)
{
    camera_init(&player->camera);
    glm_vec3_copy(start_pos, player->position);
    glm_vec3_zero(player->velocity);
    glm_vec3_zero(player->eye_pos);
    player->mode            = MODE_FREE;
    player->on_ground       = false;
    player->in_water        = false;
    player->sprinting       = false;
    player->noclip          = true;
    player->prev_space      = false;
    player->prev_v          = false;
    player->last_space_time = 0.0f;
    player->accumulator     = 0.0f;
}

void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    (void)world;

    vec3 front;
    camera_get_front(&player->camera, front);

    /* Horizontal forward (XZ only) — matches old camera behavior */
    vec3 forward = { front[0], 0.0f, front[2] };
    glm_vec3_normalize(forward);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(forward, up, right);
    glm_vec3_normalize(right);

    float vel = FLY_SPEED * dt;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        player->position[0] += forward[0] * vel;
        player->position[2] += forward[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        player->position[0] -= forward[0] * vel;
        player->position[2] -= forward[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        player->position[0] -= right[0] * vel;
        player->position[2] -= right[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        player->position[0] += right[0] * vel;
        player->position[2] += right[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        player->position[1] += vel;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        player->position[1] -= vel;
    }

    /* Update eye position */
    glm_vec3_copy(player->position, player->eye_pos);
    player->eye_pos[1] += PLAYER_EYE_H;
}
