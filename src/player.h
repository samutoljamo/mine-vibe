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
#define PLAYER_SNEAK_SPEED  1.3f
#define PLAYER_SNEAK_EYE_DIP 0.3f   /* world units; matches MC's ~5/16 dip */
#define GRAVITY             25.2f   /* m/s^2, applied per tick as v -= GRAVITY*dt */
#define JUMP_VEL            7.95f   /* upward impulse; peak ≈ v^2/2g ≈ 1.25 blocks */
#define TERMINAL_VEL        78.4f   /* fall-speed clamp */

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
    bool        crouching;      /* walking mode: shift held — slows movement,
                                   dips eye, and prevents stepping off block edges */
    bool        noclip;         /* free mode: collision toggle */
    bool        prev_space;     /* edge detection (mode double-tap) */
    bool        jump_space_prev;/* edge detection (jump impulse) — last frame's
                                   space state, so the on-ground jump only fires
                                   on the up->down transition, not while held */
    bool        prev_v;         /* edge detection */
    float       last_space_time;
    float       accumulator;    /* fixed-timestep dt accumulator */
    /* Agent mode input — set by main.c before player_update each frame */
    bool        agent_mode;
    float       agent_forward;  /* [-1, 1]; nonzero = key held */
    float       agent_right;    /* [-1, 1]; nonzero = key held */
    bool        agent_jump;     /* edge-triggered: set true for one frame */
    bool        agent_sprint;
    bool        agent_crouch;
} Player;

void player_init(Player* player, vec3 start_pos);
void player_update(Player* player, GLFWwindow* window, World* world, float dt);

/* Pure edge-trigger for the on-ground jump impulse. Returns true only on the
 * rising edge of the jump key (space pressed this frame but not the previous)
 * while grounded and out of water. Holding the key does NOT re-fire — the
 * player must release and press again. Swimming/water ascent is handled
 * separately (continuous), so in_water suppresses the on-ground impulse here. */
bool jump_should_fire(bool space_now, bool space_prev, bool on_ground,
                      bool in_water);

#endif
