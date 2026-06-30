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
