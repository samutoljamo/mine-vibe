#ifndef PLAYER_ANIM_H
#define PLAYER_ANIM_H

/* ─────────────────────────────────────────────────────────────────────────
 * Per-limb animation framework (PURE — no Vulkan/GLFW).
 *
 * The humanoid/mob mesh is baked as a set of boxes, each box tagged with an
 * AnimPart. At draw time each part may carry a single pitch angle (rotation
 * about the model-space X axis) and, for the head, an extra yaw (about Y).
 * Each part rotates about its own PIVOT (joint) in model space — shoulder for
 * arms, hip for legs, neck for head — so a limb swings about its joint rather
 * than the model origin.
 *
 * The same transform runs on the GPU (shaders/player.vert). This module is the
 * CPU mirror: it defines the part set, the pivots, and the exact vertex
 * transform, so the math can be unit-tested and so callers (walk cycle, attack
 * swing) can fill angles with a clean, testable API. The shader MUST match
 * player_anim_pivot() and player_anim_transform_vertex() exactly.
 *
 * CRITICAL invariant: all-zero angles MUST leave every vertex unchanged, so the
 * default pose renders identically to the old rigid mesh (no regression).
 * ───────────────────────────────────────────────────────────────────────── */

/* Animatable body parts. Index space shared by the vertex `part` attribute,
 * PlayerRenderState.limb_angle[], the pivot table, and the shader. */
typedef enum {
    ANIM_PART_HEAD = 0,
    ANIM_PART_TORSO,    /* rigid: pivot/angle unused, kept for index symmetry */
    ANIM_PART_ARM_R,
    ANIM_PART_ARM_L,
    ANIM_PART_LEG_R,
    ANIM_PART_LEG_L,
    ANIM_PART_COUNT
} AnimPart;

/* Per-part joint pivot in model space (feet-center origin, matching
 * player_model.c). Writes pivot[0..2] = {x,y,z}. Out-of-range parts -> origin.
 * MUST match the PIVOTS[] table in shaders/player.vert. */
void player_anim_pivot(int part, float pivot[3]);

/* Transform a single vertex for the given part by `pitch` (radians about the
 * part's pivot, X axis) and `head_yaw` (radians about Y, applied only to the
 * head). With pitch == 0 and head_yaw == 0 the output equals the input exactly.
 * MUST match the per-vertex rotation in shaders/player.vert. */
void player_anim_transform_vertex(int part, float pitch, float head_yaw,
                                  const float in[3], float out[3]);

/* Fill a limb_angle[ANIM_PART_COUNT] array (pitch, radians) from a walk phase.
 * Arms and legs swing in anti-phase (left vs right) with amplitude scaled by
 * `speed` (0 = standing still -> all zeros). `phase` is in radians. Head/torso
 * are left at 0. Pure: deterministic in (phase, speed). */
void player_anim_walk(float limb_angle[ANIM_PART_COUNT], float phase, float speed);

#endif /* PLAYER_ANIM_H */
