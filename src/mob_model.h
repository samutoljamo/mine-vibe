#ifndef MOB_MODEL_H
#define MOB_MODEL_H

#include "mob.h"   /* MobType */

/* ─────────────────────────────────────────────────────────────────────────
 * Per-type mob box-model data layer (PURE — no Vulkan/GLFW).
 *
 * This module defines, as plain data, the body shape of each mob type as a
 * small list of axis-aligned boxes. It is groundwork: a follow-up renderer
 * change (0xm) bakes these boxes into GPU meshes. Nothing here touches the
 * renderer.
 *
 * ── Coordinate convention ──────────────────────────────────────────────────
 * Right-handed, matching player_model.c:
 *   +X = the mob's right, +Y = up, +Z = forward (the facing direction).
 * The origin (0,0,0) is the mob's FEET CENTER (ground plane), so a box's
 * cy is its center height above the feet and the lowest box bottom sits at
 * y ≈ 0. A box is described by its CENTER (cx,cy,cz) and full SIZE (w,h,d)
 * along X/Y/Z; half-extents are size*0.5. This mirrors how player_model.c
 * lays out its head/torso/arm/leg boxes (e.g. head 0.5³ centered ~1.5 up,
 * legs reaching down to the feet).
 *
 * ── Scale convention ───────────────────────────────────────────────────────
 * Boxes are authored in a NORMALIZED, player-sized space (a humanoid stands
 * ~1.9 tall, like the player model). They are deliberately NOT pre-scaled to
 * each mob's collision box. The renderer already owns per-type silhouette
 * scaling via MobRenderDef (src/mob_render.c: half_w/height/depth), so the
 * baking step scales this normalized model to each mob's drawn size. To keep
 * the visual model from drifting wildly larger than the ~0.6-wide × 1.8-tall
 * mob hitbox (MOB_HALF_W / MOB_HEIGHT in mob.h), every model's combined
 * bounding box stays within a sane normalized envelope: feet at y≈0, height
 * ≤ ~2.0, and total horizontal/vertical extent < 4 blocks (asserted in tests).
 *
 * Mobs are currently two-tone tinted, so each box carries a simple RGB tint
 * (channels in [0,1]) plus a part role for the future skin/UV mapping. The
 * tints here are representative defaults (the renderer's MobRenderDef primary/
 * secondary tones take precedence at draw time).
 * ───────────────────────────────────────────────────────────────────────── */

/* What body part a box represents — lets the baking step pick a skin region
 * and lets callers/tests reason about anatomy (e.g. "4 legs, no arms"). */
typedef enum {
    MOB_PART_HEAD = 0,
    MOB_PART_TORSO,
    MOB_PART_ARM,
    MOB_PART_LEG,
    MOB_PART_SNOUT,   /* pig/cow muzzle */
    MOB_PART_BEAK,    /* chicken */
    MOB_PART_WING,    /* chicken */
    MOB_PART_HORN,    /* cow (optional) */
    MOB_PART_ROLE_COUNT
} MobPartRole;

/* A single axis-aligned box of a mob's body.
 *   (cx,cy,cz): box center, in feet-center/normalized space (see header).
 *   (w,h,d):    full size along X/Y/Z (positive); half-extents are size*0.5.
 *   tint:       representative RGB in [0,1].
 *   role:       which body part this box is. */
typedef struct {
    float       cx, cy, cz;
    float       w, h, d;
    float       tint[3];
    MobPartRole role;
} MobBox;

/* Upper bound on boxes per model (keeps models small and stack-friendly). */
#define MOB_MODEL_MAX_BOXES 12

/* A complete mob body: a fixed-capacity list of boxes plus a live count. */
typedef struct {
    MobBox boxes[MOB_MODEL_MAX_BOXES];
    int    count;
} MobModel;

/* Return the box-model for a mob type. Never NULL; out-of-range types fall
 * back to the zombie (humanoid) model, mirroring mob_render_def(). The
 * returned pointer is to static storage and is valid for the program lifetime. */
const MobModel* mob_model_for(MobType type);

#endif /* MOB_MODEL_H */
