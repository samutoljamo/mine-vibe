#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/player.h"      /* JUMP_VEL, needed by mob.h tunables */
#include "../src/mob.h"
#include "../src/mob_render.h"
#include "../src/mob_model.h"

static int rgb_in_range(const float c[3]) {
    for (int i = 0; i < 3; i++)
        if (c[i] < 0.0f || c[i] > 1.0f) return 0;
    return 1;
}

static int rgb_eq(const float a[3], const float b[3]) {
    for (int i = 0; i < 3; i++)
        if (fabsf(a[i] - b[i]) > 1e-6f) return 0;
    return 1;
}

static int dims_eq(const MobRenderDef* a, const MobRenderDef* b) {
    return fabsf(a->half_w - b->half_w) < 1e-6f
        && fabsf(a->height - b->height) < 1e-6f
        && fabsf(a->depth  - b->depth ) < 1e-6f;
}

static void test_all_types_sane(void) {
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        MobRenderDef d = mob_render_def((MobType)t);
        /* Non-zero, positive body dims. */
        assert(d.half_w > 0.0f && d.half_w < 2.0f);
        assert(d.height > 0.0f && d.height < 4.0f);
        assert(d.depth  > 0.0f && d.depth  < 2.0f);
        /* Colors in [0,1]. */
        assert(rgb_in_range(d.primary));
        assert(rgb_in_range(d.secondary));
        /* Two-tone: primary and secondary must actually differ. */
        assert(!rgb_eq(d.primary, d.secondary));
    }
    printf("PASS: all_types_sane\n");
}

static void test_each_type_differs_from_zombie(void) {
    MobRenderDef z = mob_render_def(MOB_ZOMBIE);
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        if (t == MOB_ZOMBIE) continue;
        MobRenderDef d = mob_render_def((MobType)t);
        /* Each type is visually distinct from the zombie: either the body
         * proportions differ or at least one of the two tones differs. */
        int differs = !dims_eq(&d, &z)
                   || !rgb_eq(d.primary, z.primary)
                   || !rgb_eq(d.secondary, z.secondary);
        assert(differs);
    }
    printf("PASS: each_type_differs_from_zombie\n");
}

static void test_known_silhouettes(void) {
    /* Creeper is taller than the zombie. */
    assert(mob_render_def(MOB_CREEPER).height > mob_render_def(MOB_ZOMBIE).height);
    /* Skeleton is thinner than the zombie. */
    assert(mob_render_def(MOB_SKELETON).half_w < mob_render_def(MOB_ZOMBIE).half_w);
    /* Chicken is the smallest mob. */
    MobRenderDef chick = mob_render_def(MOB_CHICKEN);
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        if (t == MOB_CHICKEN) continue;
        assert(chick.height <= mob_render_def((MobType)t).height);
    }
    printf("PASS: known_silhouettes\n");
}

static void test_unknown_type_falls_back_to_zombie(void) {
    MobRenderDef z = mob_render_def(MOB_ZOMBIE);
    MobRenderDef u = mob_render_def((MobType)999);
    assert(dims_eq(&u, &z));
    assert(rgb_eq(u.primary, z.primary));
    assert(rgb_eq(u.secondary, z.secondary));
    printf("PASS: unknown_type_falls_back_to_zombie\n");
}

static void test_part_tone_routing(void) {
    /* Legs are the only part routed to the lower/secondary tone. */
    assert(mob_part_is_upper_tone(MOB_PART_HEAD));
    assert(mob_part_is_upper_tone(MOB_PART_TORSO));
    assert(mob_part_is_upper_tone(MOB_PART_ARM));
    assert(mob_part_is_upper_tone(MOB_PART_SNOUT));
    assert(mob_part_is_upper_tone(MOB_PART_BEAK));
    assert(mob_part_is_upper_tone(MOB_PART_WING));
    assert(mob_part_is_upper_tone(MOB_PART_HORN));
    assert(!mob_part_is_upper_tone(MOB_PART_LEG));
    printf("PASS: part_tone_routing\n");
}

static void test_fit_scale(void) {
    /* For every type, fitting the normalized model to its silhouette yields
     * positive, finite scales, and reproduces the target dimensions when
     * applied to the model's measured extent. */
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        float s[3];
        mob_model_fit_scale((MobType)t, s);
        for (int a = 0; a < 3; a++) {
            assert(s[a] > 0.0f);
            assert(isfinite(s[a]));
        }
        /* Apply scale to the model's Y extent → should equal target height. */
        const MobModel* m = mob_model_for((MobType)t);
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < m->count; i++) {
            float c = m->boxes[i].cy, h = m->boxes[i].h * 0.5f;
            if (c - h < lo) lo = c - h;
            if (c + h > hi) hi = c + h;
        }
        float scaled_h = (hi - lo) * s[1];
        assert(fabsf(scaled_h - mob_render_def((MobType)t).height) < 1e-3f);
    }
    printf("PASS: fit_scale\n");
}

int main(void) {
    test_all_types_sane();
    test_each_type_differs_from_zombie();
    test_known_silhouettes();
    test_unknown_type_falls_back_to_zombie();
    test_part_tone_routing();
    test_fit_scale();
    printf("ALL MOB_RENDER TESTS PASSED\n");
    return 0;
}
