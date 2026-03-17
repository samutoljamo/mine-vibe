#include "player.h"
#include "physics.h"
#include <math.h>
#include <GLFW/glfw3.h>

#define PLAYER_HALF_W  0.3f
#define PLAYER_HEIGHT  1.8f
#define PLAYER_EYE_H   1.62f
#define FLY_SPEED      20.0f
#define PHYSICS_DT     (1.0f / 60.0f)

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

static void tick_free(Player* player, GLFWwindow* window, World* world)
{
    vec3 front;
    camera_get_front(&player->camera, front);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(front, up, right);
    glm_vec3_normalize(right);

    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        dir[0] += front[0]; dir[1] += front[1]; dir[2] += front[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        dir[0] -= front[0]; dir[1] -= front[1]; dir[2] -= front[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        dir[0] -= right[0]; dir[2] -= right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        dir[0] += right[0]; dir[2] += right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        dir[1] += 1.0f;
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        dir[1] -= 1.0f;
        has_input = true;
    }

    /* Normalize and set velocity */
    if (has_input) {
        float len = glm_vec3_norm(dir);
        if (len > 0.0001f)
            glm_vec3_scale(dir, FLY_SPEED / len, player->velocity);
        else
            glm_vec3_zero(player->velocity);
    } else {
        glm_vec3_zero(player->velocity);
    }

    /* Move */
    if (player->noclip) {
        player->position[0] += player->velocity[0] * PHYSICS_DT;
        player->position[1] += player->velocity[1] * PHYSICS_DT;
        player->position[2] += player->velocity[2] * PHYSICS_DT;
    } else {
        physics_move(player->position, player->velocity,
                     PLAYER_HALF_W, PLAYER_HEIGHT, PHYSICS_DT, world);
    }
}

void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    /* Edge detection for V key (noclip toggle) */
    bool v_held = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
    bool v_pressed = v_held && !player->prev_v;
    player->prev_v = v_held;

    if (v_pressed && player->mode == MODE_FREE)
        player->noclip = !player->noclip;

    /* Fixed timestep */
    player->accumulator += dt;
    if (player->accumulator > 0.05f)
        player->accumulator = 0.05f;

    while (player->accumulator >= PHYSICS_DT) {
        tick_free(player, window, world);
        player->accumulator -= PHYSICS_DT;
    }

    /* Update eye position */
    glm_vec3_copy(player->position, player->eye_pos);
    player->eye_pos[1] += PLAYER_EYE_H;
}
