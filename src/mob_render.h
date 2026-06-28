#ifndef MOB_RENDER_H
#define MOB_RENDER_H

#include "mob.h"   /* MobType */

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

#endif /* MOB_RENDER_H */
