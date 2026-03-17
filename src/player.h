#ifndef PLAYER_H
#define PLAYER_H

#include "camera.h"
#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct GLFWwindow GLFWwindow;
typedef struct World World;

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
} Player;

void player_init(Player* player, vec3 start_pos);
void player_update(Player* player, GLFWwindow* window, World* world, float dt);

#endif
