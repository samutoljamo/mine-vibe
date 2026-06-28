#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/player.h"      /* JUMP_VEL, needed by mob.h tunables */
#include "../src/mob.h"
#include "../src/mob_render.h"

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

int main(void) {
    test_all_types_sane();
    test_each_type_differs_from_zombie();
    test_known_silhouettes();
    test_unknown_type_falls_back_to_zombie();
    printf("ALL MOB_RENDER TESTS PASSED\n");
    return 0;
}
