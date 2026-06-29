#include "mob_render.h"
#include "mob_model.h"

/* Per-type render table. Dimensions are absolute (blocks); the renderer scales
 * the shared box model by these relative to MOB_RENDER_BASE_*. The two tones
 * paint the upper (primary) and lower (secondary) halves of the model.
 *
 * The zombie row must stay first / matched to MOB_ZOMBIE so unknown types can
 * fall back to it. */
static const MobRenderDef MOB_RENDER[MOB_TYPE_COUNT] = {
    /* MOB_ZOMBIE: stocky humanoid, sickly green over dark teal (the original). */
    [MOB_ZOMBIE] = {
        .half_w = 0.30f, .height = 1.80f, .depth = 0.30f,
        .primary   = {0.40f, 0.62f, 0.32f},  /* sickly green */
        .secondary = {0.16f, 0.34f, 0.40f},  /* dark teal */
    },
    /* MOB_SKELETON: thin and slightly shorter, bone white over light grey. */
    [MOB_SKELETON] = {
        .half_w = 0.22f, .height = 1.74f, .depth = 0.22f,
        .primary   = {0.92f, 0.92f, 0.88f},  /* bone white */
        .secondary = {0.66f, 0.66f, 0.62f},  /* light grey */
    },
    /* MOB_CREEPER: taller, blocky, bright green over a darker green underside. */
    [MOB_CREEPER] = {
        .half_w = 0.32f, .height = 1.98f, .depth = 0.32f,
        .primary   = {0.36f, 0.78f, 0.30f},  /* bright creeper green */
        .secondary = {0.20f, 0.50f, 0.18f},  /* darker green */
    },
    /* MOB_PIG: low and broad, pink body with a darker snout/under tone. */
    [MOB_PIG] = {
        .half_w = 0.45f, .height = 0.90f, .depth = 0.34f,
        .primary   = {0.92f, 0.66f, 0.68f},  /* pig pink */
        .secondary = {0.78f, 0.48f, 0.52f},  /* darker snout pink */
    },
    /* MOB_COW: bigger and broader, brown body with white patches/legs. */
    [MOB_COW] = {
        .half_w = 0.48f, .height = 1.30f, .depth = 0.40f,
        .primary   = {0.42f, 0.28f, 0.18f},  /* hide brown */
        .secondary = {0.90f, 0.88f, 0.84f},  /* white patches */
    },
    /* MOB_CHICKEN: the smallest mob, white body with a yellow beak/feet tone. */
    [MOB_CHICKEN] = {
        .half_w = 0.20f, .height = 0.70f, .depth = 0.20f,
        .primary   = {0.94f, 0.94f, 0.92f},  /* white feathers */
        .secondary = {0.92f, 0.72f, 0.20f},  /* yellow beak/feet */
    },
};

MobRenderDef mob_render_def(MobType type) {
    if ((unsigned)type >= (unsigned)MOB_TYPE_COUNT)
        return MOB_RENDER[MOB_ZOMBIE];
    return MOB_RENDER[type];
}

bool mob_part_is_upper_tone(MobPartRole role) {
    /* Legs take the lower/secondary tone; everything else (head, torso, arms,
     * snout, beak, wings, horns) takes the upper/primary tone. */
    return role != MOB_PART_LEG;
}

void mob_model_fit_scale(MobType type, float out_scale[3]) {
    const MobModel* m = mob_model_for(type);
    float lo[3] = {  1e9f,  1e9f,  1e9f };
    float hi[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < m->count; i++) {
        const MobBox* b = &m->boxes[i];
        float c[3] = { b->cx, b->cy, b->cz };
        float h[3] = { b->w * 0.5f, b->h * 0.5f, b->d * 0.5f };
        for (int a = 0; a < 3; a++) {
            if (c[a] - h[a] < lo[a]) lo[a] = c[a] - h[a];
            if (c[a] + h[a] > hi[a]) hi[a] = c[a] + h[a];
        }
    }
    MobRenderDef def = mob_render_def(type);
    float target[3] = { def.half_w * 2.0f, def.height, def.depth * 2.0f };
    for (int a = 0; a < 3; a++) {
        float ext = hi[a] - lo[a];
        out_scale[a] = (ext > 1e-4f) ? target[a] / ext : 1.0f;
    }
}
