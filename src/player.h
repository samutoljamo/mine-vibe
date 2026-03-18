#ifndef PLAYER_H
#define PLAYER_H

#include "camera.h"
#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct GLFWwindow GLFWwindow;
typedef struct World World;

/* Physics constants — shared with server-side simulation */
#define PHYSICS_DT          (1.0f / 60.0f)
#define PLAYER_SPRINT_SPEED 5.6f

typedef enum PlayerMode {
    MODE_FREE,
    MODE_WALKING,
} PlayerMode;

typedef struct Player {
    Camera      camera;
    vec3        position;       /* feet position in world space */
    vec3        velocity;
    vec3        eye_pos;        /* computed: position + eye offset */
    PlayerMode  mode;
    bool        on_ground;
    bool        in_water;
    bool        sprinting;
    bool        noclip;         /* free mode: collision toggle */
    bool        prev_space;     /* edge detection */
    bool        prev_v;         /* edge detection */
    float       last_space_time;
    float       accumulator;    /* fixed-timestep dt accumulator */
    /* Agent mode input — set by main.c before player_update each frame */
    bool        agent_mode;
    float       agent_forward;  /* [-1, 1]; nonzero = key held */
    float       agent_right;    /* [-1, 1]; nonzero = key held */
    bool        agent_jump;     /* edge-triggered: set true for one frame */
    bool        agent_sprint;
} Player;

void player_init(Player* player, vec3 start_pos);
void player_update(Player* player, GLFWwindow* window, World* world, float dt);

#endif
