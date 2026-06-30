#include "player_anim.h"
#include <math.h>

/* Joint pivots in model space (feet-center origin), matching player_model.c box
 * layout: shoulders at the top of the torso (y=1.25), hips at the top of the
 * legs (y=0.625), neck at the bottom of the head (y=1.25). X picks the
 * left/right side so the limb swings about its own joint. Torso is rigid.
 * KEEP IN SYNC with PIVOTS[] in shaders/player.vert. */
static const float PIVOTS[ANIM_PART_COUNT][3] = {
    [ANIM_PART_HEAD]  = {  0.0f,   1.25f, 0.0f },
    [ANIM_PART_TORSO] = {  0.0f,   0.0f,  0.0f },
    [ANIM_PART_ARM_R] = {  0.375f, 1.25f, 0.0f },
    [ANIM_PART_ARM_L] = { -0.375f, 1.25f, 0.0f },
    [ANIM_PART_LEG_R] = {  0.125f, 0.625f, 0.0f },
    [ANIM_PART_LEG_L] = { -0.125f, 0.625f, 0.0f },
};

void player_anim_pivot(int part, float pivot[3])
{
    if (part < 0 || part >= ANIM_PART_COUNT) {
        pivot[0] = pivot[1] = pivot[2] = 0.0f;
        return;
    }
    pivot[0] = PIVOTS[part][0];
    pivot[1] = PIVOTS[part][1];
    pivot[2] = PIVOTS[part][2];
}

void player_anim_transform_vertex(int part, float pitch, float head_yaw,
                                  const float in[3], float out[3])
{
    float pivot[3];
    player_anim_pivot(part, pivot);

    /* Work in pivot-local space. */
    float x = in[0] - pivot[0];
    float y = in[1] - pivot[1];
    float z = in[2] - pivot[2];

    /* Pitch: rotate about X axis (swing forward/back). */
    if (pitch != 0.0f) {
        float c = cosf(pitch), s = sinf(pitch);
        float ny = c * y - s * z;
        float nz = s * y + c * z;
        y = ny;
        z = nz;
    }

    /* Head yaw: rotate about Y axis (look left/right). Head part only. */
    if (part == ANIM_PART_HEAD && head_yaw != 0.0f) {
        float c = cosf(head_yaw), s = sinf(head_yaw);
        float nx = c * x + s * z;
        float nz = -s * x + c * z;
        x = nx;
        z = nz;
    }

    out[0] = x + pivot[0];
    out[1] = y + pivot[1];
    out[2] = z + pivot[2];
}

void player_anim_walk(float limb_angle[ANIM_PART_COUNT], float phase, float speed)
{
    for (int i = 0; i < ANIM_PART_COUNT; i++)
        limb_angle[i] = 0.0f;

    /* Standing still: no swing. */
    if (speed <= 0.0f)
        return;

    /* Peak swing ~35deg at full speed, clamped. */
    float amp = 0.6f * speed;
    if (amp > 0.9f) amp = 0.9f;

    float s = sinf(phase) * amp;
    /* Arms and legs swing in anti-phase; left limb opposes right limb. */
    limb_angle[ANIM_PART_ARM_R] =  s;
    limb_angle[ANIM_PART_ARM_L] = -s;
    limb_angle[ANIM_PART_LEG_R] = -s;
    limb_angle[ANIM_PART_LEG_L] =  s;
}

/* Cadence: radians of phase per second per unit speed. At a normal walk
 * (~4 m/s) this gives a brisk but readable step frequency. */
#define PLAYER_ANIM_PHASE_RATE 2.0f
/* A tiny floor below which an entity is treated as standing still (m/s). */
#define PLAYER_ANIM_MOVE_EPS   0.05f
/* Smoothing time constant for the locomotion speed ramp (seconds). */
#define PLAYER_ANIM_SPEED_TAU  0.12f
#define PLAYER_ANIM_TWO_PI     6.2831853071795864769f

float player_anim_walk_phase_advance(float phase, float speed, float dt)
{
    if (dt <= 0.0f)
        return phase;
    /* Standing still: freeze the phase (no continued cycling at rest). */
    if (speed <= PLAYER_ANIM_MOVE_EPS)
        return phase;

    phase += (PLAYER_ANIM_PHASE_RATE * speed) * dt;

    /* Wrap into [0, 2π) without an unbounded loop for huge dt*speed. */
    if (phase >= PLAYER_ANIM_TWO_PI || phase < 0.0f) {
        phase = fmodf(phase, PLAYER_ANIM_TWO_PI);
        if (phase < 0.0f)
            phase += PLAYER_ANIM_TWO_PI;
    }
    return phase;
}

float player_anim_speed_smooth(float current, float target, float dt)
{
    if (dt <= 0.0f)
        return current;
    if (target < 0.0f)
        target = 0.0f;
    /* Exponential approach toward target; framerate-independent. */
    float a = 1.0f - expf(-dt / PLAYER_ANIM_SPEED_TAU);
    if (a > 1.0f) a = 1.0f;
    float next = current + (target - current) * a;
    /* Snap to exact 0 once we are within the move floor of rest so a stopped
     * entity returns to the all-zero (rigid) pose rather than asymptoting. */
    if (target == 0.0f && next < PLAYER_ANIM_MOVE_EPS)
        next = 0.0f;
    return next;
}
