#include "mob_model.h"

/* Pure per-type mob box-model data. See mob_model.h for the coordinate and
 * scale conventions. All geometry is authored in normalized, player-sized
 * space with the feet center at the origin; the renderer scales it per type. */

/* ── Representative two-tone palettes (mirror mob_render.c's primary/secondary).
 * tint() picks per-box; the renderer's MobRenderDef takes precedence at draw. */
#define RGB(r,g,b) {(r),(g),(b)}

/* Humanoid: head 0.5³ at ~1.5 up, torso 0.5×0.75×0.25, arms/legs 0.25 thick.
 * Proportions copied from player_model.c's build_player_mesh().
 * `prim`/`sec` are the upper/lower tones for the type. */
static MobModel make_humanoid(const float prim[3], const float sec[3]) {
    MobModel m = {0};
    /* Head: full size 0.5×0.5×0.5, center y=1.50 */
    m.boxes[m.count++] = (MobBox){ 0.0f, 1.50f, 0.0f,  0.5f, 0.5f, 0.5f,
                                   {prim[0],prim[1],prim[2]}, MOB_PART_HEAD };
    /* Torso: 0.5×0.75×0.25, center y=0.875 */
    m.boxes[m.count++] = (MobBox){ 0.0f, 0.875f, 0.0f, 0.5f, 0.75f, 0.25f,
                                   {prim[0],prim[1],prim[2]}, MOB_PART_TORSO };
    /* Right arm: 0.25×0.75×0.25, center (+0.375, 0.875, 0) */
    m.boxes[m.count++] = (MobBox){ 0.375f, 0.875f, 0.0f, 0.25f, 0.75f, 0.25f,
                                   {prim[0],prim[1],prim[2]}, MOB_PART_ARM };
    /* Left arm: center (-0.375, 0.875, 0) */
    m.boxes[m.count++] = (MobBox){ -0.375f, 0.875f, 0.0f, 0.25f, 0.75f, 0.25f,
                                   {prim[0],prim[1],prim[2]}, MOB_PART_ARM };
    /* Right leg: 0.25×0.75×0.25, center (+0.125, 0.25, 0) */
    m.boxes[m.count++] = (MobBox){ 0.125f, 0.25f, 0.0f, 0.25f, 0.75f, 0.25f,
                                   {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    /* Left leg: center (-0.125, 0.25, 0) */
    m.boxes[m.count++] = (MobBox){ -0.125f, 0.25f, 0.0f, 0.25f, 0.75f, 0.25f,
                                   {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    return m;
}

/* ── Static model table, built once at first query. ─────────────────────── */

static MobModel g_models[MOB_TYPE_COUNT];
static int      g_built = 0;

static void build_creeper(MobModel* m) {
    *m = (MobModel){0};
    const float prim[3] = RGB(0.36f, 0.78f, 0.30f);  /* bright green */
    const float sec[3]  = RGB(0.20f, 0.50f, 0.18f);  /* darker green */
    /* Stubby legs: 4 short boxes, height 0.30, near the corners. */
    const float ly = 0.15f, lh = 0.30f, ls = 0.25f;
    const float lx = 0.18f, lz = 0.18f;
    m->boxes[m->count++] = (MobBox){  lx, ly,  lz, ls, lh, ls, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){ -lx, ly,  lz, ls, lh, ls, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){  lx, ly, -lz, ls, lh, ls, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){ -lx, ly, -lz, ls, lh, ls, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    /* Tall torso: 0.5×1.05×0.35, sitting on top of the legs (y 0.30..1.35). */
    m->boxes[m->count++] = (MobBox){ 0.0f, 0.825f, 0.0f, 0.5f, 1.05f, 0.35f,
                                     {prim[0],prim[1],prim[2]}, MOB_PART_TORSO };
    /* Head: 0.5³ on top, center y=1.60. */
    m->boxes[m->count++] = (MobBox){ 0.0f, 1.60f, 0.0f, 0.5f, 0.5f, 0.5f,
                                     {prim[0],prim[1],prim[2]}, MOB_PART_HEAD };
}

/* Quadruped builder shared by pig and cow. Torso is a long low horizontal box;
 * 4 legs at the corners; a head with a snout at the +Z (front) end. `horns`
 * adds two small horn boxes (cow). */
static void build_quadruped(MobModel* m,
                            float body_w, float body_h, float body_d,
                            float body_cy,
                            float leg_h, float leg_s,
                            float head_w, float head_h, float head_d,
                            const float prim[3], const float sec[3],
                            int horns) {
    *m = (MobModel){0};
    /* Torso: long along Z (front-back), low and broad. */
    m->boxes[m->count++] = (MobBox){ 0.0f, body_cy, 0.0f, body_w, body_h, body_d,
                                     {prim[0],prim[1],prim[2]}, MOB_PART_TORSO };
    /* 4 legs at the torso's footprint corners. */
    float lx = body_w * 0.5f - leg_s * 0.5f;
    float lz = body_d * 0.5f - leg_s * 0.5f;
    float ly = leg_h * 0.5f;
    m->boxes[m->count++] = (MobBox){  lx, ly,  lz, leg_s, leg_h, leg_s, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){ -lx, ly,  lz, leg_s, leg_h, leg_s, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){  lx, ly, -lz, leg_s, leg_h, leg_s, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){ -lx, ly, -lz, leg_s, leg_h, leg_s, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    /* Head: at the front (+Z), centered roughly at the top of the torso. */
    float head_cz = body_d * 0.5f + head_d * 0.5f;
    float head_cy = body_cy + body_h * 0.25f;
    m->boxes[m->count++] = (MobBox){ 0.0f, head_cy, head_cz, head_w, head_h, head_d,
                                     {prim[0],prim[1],prim[2]}, MOB_PART_HEAD };
    /* Snout: a small box jutting further forward from the head. */
    float snout_w = head_w * 0.5f, snout_h = head_h * 0.45f, snout_d = head_d * 0.4f;
    m->boxes[m->count++] = (MobBox){ 0.0f, head_cy - head_h * 0.15f,
                                     head_cz + head_d * 0.5f + snout_d * 0.5f,
                                     snout_w, snout_h, snout_d,
                                     {sec[0],sec[1],sec[2]}, MOB_PART_SNOUT };
    if (horns) {
        float hw = 0.08f, hh = 0.12f, hd = 0.08f;
        float hx = head_w * 0.35f;
        float hy = head_cy + head_h * 0.5f + hh * 0.5f;
        m->boxes[m->count++] = (MobBox){  hx, hy, head_cz, hw, hh, hd, {sec[0],sec[1],sec[2]}, MOB_PART_HORN };
        m->boxes[m->count++] = (MobBox){ -hx, hy, head_cz, hw, hh, hd, {sec[0],sec[1],sec[2]}, MOB_PART_HORN };
    }
}

static void build_chicken(MobModel* m) {
    *m = (MobModel){0};
    const float prim[3] = RGB(0.94f, 0.94f, 0.92f);  /* white feathers */
    const float sec[3]  = RGB(0.92f, 0.72f, 0.20f);  /* yellow beak/feet */
    /* Small plump torso: center y=0.35. */
    float body_w = 0.30f, body_h = 0.35f, body_d = 0.30f, body_cy = 0.40f;
    m->boxes[m->count++] = (MobBox){ 0.0f, body_cy, 0.0f, body_w, body_h, body_d,
                                     {prim[0],prim[1],prim[2]}, MOB_PART_TORSO };
    /* 2 thin legs. */
    float leg_h = 0.22f, leg_s = 0.06f, ly = leg_h * 0.5f, lx = 0.08f;
    m->boxes[m->count++] = (MobBox){  lx, ly, 0.0f, leg_s, leg_h, leg_s, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    m->boxes[m->count++] = (MobBox){ -lx, ly, 0.0f, leg_s, leg_h, leg_s, {sec[0],sec[1],sec[2]}, MOB_PART_LEG };
    /* Head on top, toward the front (+Z). */
    float head_s = 0.22f;
    float head_cz = body_d * 0.5f - head_s * 0.2f;
    float head_cy = body_cy + body_h * 0.5f + head_s * 0.5f;
    m->boxes[m->count++] = (MobBox){ 0.0f, head_cy, head_cz, head_s, head_s, head_s,
                                     {prim[0],prim[1],prim[2]}, MOB_PART_HEAD };
    /* Beak jutting forward from the head. */
    float beak_w = 0.08f, beak_h = 0.06f, beak_d = 0.10f;
    m->boxes[m->count++] = (MobBox){ 0.0f, head_cy, head_cz + head_s * 0.5f + beak_d * 0.5f,
                                     beak_w, beak_h, beak_d, {sec[0],sec[1],sec[2]}, MOB_PART_BEAK };
    /* 2 little wings on the sides of the torso. */
    float wing_w = 0.05f, wing_h = 0.22f, wing_d = 0.20f;
    float wx = body_w * 0.5f + wing_w * 0.5f;
    m->boxes[m->count++] = (MobBox){  wx, body_cy, 0.0f, wing_w, wing_h, wing_d, {prim[0],prim[1],prim[2]}, MOB_PART_WING };
    m->boxes[m->count++] = (MobBox){ -wx, body_cy, 0.0f, wing_w, wing_h, wing_d, {prim[0],prim[1],prim[2]}, MOB_PART_WING };
}

static void build_all(void) {
    const float zombie_p[3]   = RGB(0.40f, 0.62f, 0.32f);
    const float zombie_s[3]   = RGB(0.16f, 0.34f, 0.40f);
    const float skel_p[3]     = RGB(0.92f, 0.92f, 0.88f);
    const float skel_s[3]     = RGB(0.66f, 0.66f, 0.62f);

    g_models[MOB_ZOMBIE]   = make_humanoid(zombie_p, zombie_s);
    g_models[MOB_SKELETON] = make_humanoid(skel_p, skel_s);
    build_creeper(&g_models[MOB_CREEPER]);

    const float pig_p[3] = RGB(0.92f, 0.66f, 0.68f);
    const float pig_s[3] = RGB(0.78f, 0.48f, 0.52f);
    /* Pig: long low torso. */
    build_quadruped(&g_models[MOB_PIG],
                    /*body*/ 0.55f, 0.45f, 0.85f, /*cy*/ 0.50f,
                    /*leg*/ 0.28f, 0.18f,
                    /*head*/ 0.40f, 0.40f, 0.35f,
                    pig_p, pig_s, /*horns*/ 0);

    const float cow_p[3] = RGB(0.42f, 0.28f, 0.18f);
    const float cow_s[3] = RGB(0.90f, 0.88f, 0.84f);
    /* Cow: bigger and broader than the pig, with horns. */
    build_quadruped(&g_models[MOB_COW],
                    /*body*/ 0.65f, 0.60f, 1.00f, /*cy*/ 0.70f,
                    /*leg*/ 0.40f, 0.22f,
                    /*head*/ 0.45f, 0.45f, 0.40f,
                    cow_p, cow_s, /*horns*/ 1);

    build_chicken(&g_models[MOB_CHICKEN]);

    g_built = 1;
}

const MobModel* mob_model_for(MobType type) {
    if (!g_built) build_all();
    if ((unsigned)type >= (unsigned)MOB_TYPE_COUNT)
        return &g_models[MOB_ZOMBIE];
    return &g_models[type];
}
