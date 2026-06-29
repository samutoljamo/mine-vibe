#ifndef MOB_RENDER_H
#define MOB_RENDER_H

#include "mob.h"        /* MobType */
#include "mob_model.h"  /* MobPartRole */
#include <stdbool.h>

/* Per-type render description for a mob.
 *
 * The renderer draws every mob with the shared two-tone box model
 * (player_model.c). This table makes each mob type visually distinct without a
 * dedicated mesh: the body box is scaled per axis (half_w / height / depth,
 * relative to the player model's default 0.3 half-width × 1.8 height × 0.3
 * depth), and the fragment shader paints the upper half with `primary` and the
 * lower half with `secondary`.
 *
 * Pure lookup, no globals. Adding a mob = one row in mob_render.c. */
typedef struct {
    float half_w;        /* horizontal half-extent, blocks (X) */
    float height;        /* total standing height, blocks (Y) */
    float depth;         /* horizontal half-extent, blocks (Z) */
    float primary[3];    /* upper/main two-tone colour, RGB in [0,1] */
    float secondary[3];  /* lower/accent two-tone colour, RGB in [0,1] */
} MobRenderDef;

/* The dimensions the shared player/zombie box model is built at. The renderer
 * scales each mob's drawn box by (half_w/MOB_RENDER_BASE_HALF_W, etc.). */
#define MOB_RENDER_BASE_HALF_W  0.3f
#define MOB_RENDER_BASE_HEIGHT  1.8f
#define MOB_RENDER_BASE_DEPTH   0.3f

/* Render description for a mob type. Unknown/out-of-range types fall back to
 * the zombie profile. */
MobRenderDef mob_render_def(MobType type);

/* Pure two-tone routing for the mesh baker (0xm). The player pipeline paints a
 * mob box with the primary push-constant colour where the sampled skin UV has
 * v < 0.5 and the secondary colour where v >= 0.5. So the baker must place each
 * box's faces in the correct skin half. Returns true if a part should take the
 * UPPER (primary) tone, false if it should take the LOWER (secondary) tone.
 * Legs map to the secondary tone (mirrors mob_model.c, where legs use `sec`);
 * every other part maps to the primary tone. Pure — unit-tested. */
bool mob_part_is_upper_tone(MobPartRole role);

/* Per-axis draw scale that fits a type's normalized box-model (mob_model_for)
 * into its target silhouette (mob_render_def: full width 2*half_w, full height
 * `height`, full depth 2*depth). Because each per-type mesh has its OWN
 * normalized extents (a chicken model is short, a creeper tall), the renderer
 * can't divide by a single shared base height; this computes scale =
 * target_size / model_extent per axis. Degenerate (zero-extent) axes yield 1.0.
 * Pure — unit-tested. */
void mob_model_fit_scale(MobType type, float out_scale[3]);

#endif /* MOB_RENDER_H */
