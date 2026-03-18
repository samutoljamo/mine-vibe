#include "player.h"
#include "physics.h"
#include <math.h>
#include <GLFW/glfw3.h>

#define PLAYER_HALF_W    0.3f
#define PLAYER_HEIGHT    1.8f
#define PLAYER_EYE_H     1.62f
#define FLY_SPEED        20.0f
#define WALK_SPEED       4.3f
#define SPRINT_SPEED     5.6f
#define SWIM_SPEED       2.0f
#define GRAVITY          25.2f
#define JUMP_VEL         7.95f
#define TERMINAL_VEL     78.4f
#define WATER_SINK       2.0f
#define SWIM_UP_VEL      4.0f
#define DRAG_GROUND      0.85f
#define DRAG_AIR         0.98f
#define DRAG_WATER       0.80f
#define WATER_Y_DRAG     0.80f
#define PHYSICS_DT       (1.0f / 60.0f)
#define DOUBLETAP_WINDOW 0.3f

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
    player->last_space_time = -1.0f;
    player->accumulator     = 0.0f;
    player->agent_mode      = false;
    player->agent_forward   = 0.0f;
    player->agent_right     = 0.0f;
    player->agent_jump      = false;
    player->agent_sprint    = false;
}

/* ------------------------------------------------------------------ */
/*  Free mode tick                                                     */
/* ------------------------------------------------------------------ */

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
    float analog_scale = 1.0f;

    if (player->agent_mode) {
        float fwd = player->agent_forward;
        float rgt = player->agent_right;
        if (fwd != 0.0f) {
            float s = fwd > 0.0f ? 1.0f : -1.0f;
            dir[0] += front[0] * s;
            dir[1] += front[1] * s;
            dir[2] += front[2] * s;
            has_input = true;
        }
        if (rgt != 0.0f) {
            float s = rgt > 0.0f ? 1.0f : -1.0f;
            dir[0] += right[0] * s;
            dir[2] += right[2] * s;
            has_input = true;
        }
        if (player->agent_jump) {
            dir[1] += 1.0f;
            has_input = true;
        }
        float fwd_abs = fabsf(fwd);
        float rgt_abs = fabsf(rgt);
        analog_scale = fwd_abs > rgt_abs ? fwd_abs : rgt_abs;
        if (player->agent_jump || analog_scale > 1.0f) analog_scale = 1.0f;
    } else {
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
    }

    if (has_input) {
        float len = glm_vec3_norm(dir);
        if (len > 0.0001f)
            glm_vec3_scale(dir, FLY_SPEED * analog_scale / len, player->velocity);
        else
            glm_vec3_zero(player->velocity);
    } else {
        glm_vec3_zero(player->velocity);
    }

    if (player->noclip) {
        player->position[0] += player->velocity[0] * PHYSICS_DT;
        player->position[1] += player->velocity[1] * PHYSICS_DT;
        player->position[2] += player->velocity[2] * PHYSICS_DT;
    } else {
        physics_move(player->position, player->velocity,
                     PLAYER_HALF_W, PLAYER_HEIGHT, PHYSICS_DT, world);
    }
}

/* ------------------------------------------------------------------ */
/*  Walking mode tick                                                  */
/* ------------------------------------------------------------------ */

static void tick_walking(Player* player, GLFWwindow* window, World* world)
{
    /* 1. Compute desired movement direction on XZ plane */
    vec3 front;
    camera_get_front(&player->camera, front);

    vec3 forward = { front[0], 0.0f, front[2] };
    glm_vec3_normalize(forward);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(forward, up, right);
    glm_vec3_normalize(right);

    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;
    float analog_scale = 1.0f;

    if (player->agent_mode) {
        float fwd = player->agent_forward;
        float rgt = player->agent_right;
        if (fwd != 0.0f) {
            float s = fwd > 0.0f ? 1.0f : -1.0f;
            dir[0] += forward[0] * s;
            dir[2] += forward[2] * s;
            has_input = true;
        }
        if (rgt != 0.0f) {
            float s = rgt > 0.0f ? 1.0f : -1.0f;
            dir[0] += right[0] * s;
            dir[2] += right[2] * s;
            has_input = true;
        }
        player->sprinting = has_input && player->agent_sprint;
        float fwd_abs = fabsf(fwd);
        float rgt_abs = fabsf(rgt);
        analog_scale = fwd_abs > rgt_abs ? fwd_abs : rgt_abs;
        if (analog_scale > 1.0f) analog_scale = 1.0f;
    } else {
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            dir[0] += forward[0]; dir[2] += forward[2];
            has_input = true;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            dir[0] -= forward[0]; dir[2] -= forward[2];
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

        /* 2. Sprint check */
        player->sprinting = has_input
            && (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
            && (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);
    }

    /* Normalize direction */
    float len = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (len > 0.0001f) {
        dir[0] /= len;
        dir[2] /= len;
    }

    float speed = WALK_SPEED;
    if (player->sprinting) speed = SPRINT_SPEED;
    if (player->in_water)  speed = SWIM_SPEED;

    /* Snap-to-speed: set horizontal velocity directly */
    if (has_input) {
        player->velocity[0] = dir[0] * speed * analog_scale;
        player->velocity[2] = dir[2] * speed * analog_scale;
    }

    /* 3. Gravity / water physics */
    if (!player->in_water) {
        if (!player->on_ground)
            player->velocity[1] -= GRAVITY * PHYSICS_DT;
    } else {
        /* Damp vertical velocity (prevents carrying fall speed into water) */
        player->velocity[1] *= WATER_Y_DRAG;
        player->velocity[1] -= WATER_SINK * PHYSICS_DT;
        bool swim_up = player->agent_mode
                       ? player->agent_jump
                       : (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
        if (swim_up)
            player->velocity[1] = SWIM_UP_VEL;
    }

    /* 4. Terminal velocity */
    if (player->velocity[1] < -TERMINAL_VEL)
        player->velocity[1] = -TERMINAL_VEL;

    /* 5. Jump */
    bool do_jump = player->agent_mode
                   ? player->agent_jump
                   : (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
    if (do_jump && player->on_ground && !player->in_water) {
        player->velocity[1] = JUMP_VEL;
    }

    /* 6. Collision */
    PhysicsResult result = physics_move(player->position, player->velocity,
                                        PLAYER_HALF_W, PLAYER_HEIGHT,
                                        PHYSICS_DT, world);
    player->on_ground = result.on_ground;
    player->in_water  = result.in_water;

    /* 7. Drag (only when no input) */
    if (!has_input) {
        float drag = DRAG_AIR;
        if (player->on_ground) drag = DRAG_GROUND;
        if (player->in_water)  drag = DRAG_WATER;
        player->velocity[0] *= drag;
        player->velocity[2] *= drag;
    }
}

/* ------------------------------------------------------------------ */
/*  Player update (called once per frame)                              */
/* ------------------------------------------------------------------ */

void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    if (!player->agent_mode) {
        /* Edge detection */
        bool space_held = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        bool space_pressed = space_held && !player->prev_space;
        player->prev_space = space_held;

        bool v_held = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
        bool v_pressed = v_held && !player->prev_v;
        player->prev_v = v_held;

        /* Mode switching: double-tap space */
        if (space_pressed) {
            float now = (float)glfwGetTime();
            if (player->last_space_time >= 0.0f
                && (now - player->last_space_time) < DOUBLETAP_WINDOW) {
                if (player->mode == MODE_WALKING) {
                    player->mode = MODE_FREE;
                    player->noclip = true;
                } else {
                    player->mode = MODE_WALKING;
                }
                glm_vec3_zero(player->velocity);
                player->on_ground = false;
                player->in_water  = false;
                player->last_space_time = -1.0f;
            } else {
                player->last_space_time = now;
            }
        }

        /* Noclip toggle (V key, free mode only) */
        if (v_pressed && player->mode == MODE_FREE)
            player->noclip = !player->noclip;
    }

    /* Fixed timestep physics */
    player->accumulator += dt;
    if (player->accumulator > 0.05f)
        player->accumulator = 0.05f;

    while (player->accumulator >= PHYSICS_DT) {
        if (player->mode == MODE_FREE)
            tick_free(player, window, world);
        else
            tick_walking(player, window, world);
        player->accumulator -= PHYSICS_DT;
    }

    /* Update eye position */
    glm_vec3_copy(player->position, player->eye_pos);
    player->eye_pos[1] += PLAYER_EYE_H;
}
