#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/player_anim.h"

#define EPS 1e-5f

static int close3(const float a[3], const float b[3]) {
    return fabsf(a[0]-b[0]) < EPS && fabsf(a[1]-b[1]) < EPS && fabsf(a[2]-b[2]) < EPS;
}

/* CRITICAL: all-zero angles must leave every vertex EXACTLY unchanged, for
 * every part — this is the no-regression guarantee (identity == old pose). */
static void test_identity_is_no_op(void) {
    float samples[][3] = {
        { 0.0f, 0.0f, 0.0f }, { 0.25f, 1.75f, 0.125f }, { -0.5f, 0.5f, -0.125f },
        { 0.375f, 0.875f, 0.0f }, { -0.125f, 0.0f, 0.1f },
    };
    for (int p = 0; p < ANIM_PART_COUNT; p++) {
        for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
            float out[3];
            player_anim_transform_vertex(p, 0.0f, 0.0f, samples[i], out);
            assert(close3(samples[i], out));
        }
    }
    printf("PASS: identity is no-op\n");
}

/* A point exactly at the pivot is a fixed point of any rotation. */
static void test_pivot_is_fixed(void) {
    for (int p = 0; p < ANIM_PART_COUNT; p++) {
        float pivot[3];
        player_anim_pivot(p, pivot);
        float out[3];
        player_anim_transform_vertex(p, 0.7f, 0.4f, pivot, out);
        assert(close3(pivot, out));
    }
    printf("PASS: pivot is fixed point\n");
}

/* Pitch rotates about the part's pivot on the X axis: a point directly below
 * the leg hip swings forward (+Z) for a negative pitch. Verify radius is
 * preserved and X is untouched. */
static void test_pitch_about_pivot(void) {
    float pivot[3];
    player_anim_pivot(ANIM_PART_LEG_R, pivot);
    float in[3] = { pivot[0], pivot[1] - 0.5f, pivot[2] }; /* 0.5 below hip */
    float out[3];
    player_anim_transform_vertex(ANIM_PART_LEG_R, 0.5f, 0.0f, in, out);

    /* X unchanged by an X-axis rotation. */
    assert(fabsf(out[0] - in[0]) < EPS);
    /* Distance from pivot preserved. */
    float r_in  = sqrtf(powf(in[1]-pivot[1],2) + powf(in[2]-pivot[2],2));
    float r_out = sqrtf(powf(out[1]-pivot[1],2) + powf(out[2]-pivot[2],2));
    assert(fabsf(r_in - r_out) < EPS);
    /* Actually moved. */
    assert(fabsf(out[2] - in[2]) > EPS);
    printf("PASS: pitch rotates about pivot\n");
}

/* Head yaw rotates only the head, about Y; non-head parts ignore head_yaw. */
static void test_head_yaw(void) {
    float in[3] = { 0.2f, 1.5f, 0.25f };
    float head[3], arm[3];
    player_anim_transform_vertex(ANIM_PART_HEAD,  0.0f, 0.6f, in, head);
    player_anim_transform_vertex(ANIM_PART_ARM_R, 0.0f, 0.6f, in, arm);
    assert(!close3(in, head));   /* head yawed */
    assert(close3(in, arm));     /* arm ignores head_yaw with zero pitch */

    /* Y unchanged by a Y-axis rotation. */
    assert(fabsf(head[1] - in[1]) < EPS);
    printf("PASS: head yaw is head-only, about Y\n");
}

/* Walk: standing still -> all zero; moving -> arms/legs anti-phase, head/torso
 * untouched; deterministic. */
static void test_walk(void) {
    float a[ANIM_PART_COUNT], b[ANIM_PART_COUNT];

    player_anim_walk(a, 1.0f, 0.0f);  /* speed 0 */
    for (int i = 0; i < ANIM_PART_COUNT; i++) assert(a[i] == 0.0f);

    player_anim_walk(a, 1.3f, 1.0f);
    player_anim_walk(b, 1.3f, 1.0f);
    for (int i = 0; i < ANIM_PART_COUNT; i++) assert(a[i] == b[i]); /* deterministic */

    assert(a[ANIM_PART_HEAD]  == 0.0f);
    assert(a[ANIM_PART_TORSO] == 0.0f);
    /* Anti-phase: left arm opposes right arm; same-side arm opposes same-side leg. */
    assert(fabsf(a[ANIM_PART_ARM_R] + a[ANIM_PART_ARM_L]) < EPS);
    assert(fabsf(a[ANIM_PART_LEG_R] + a[ANIM_PART_LEG_L]) < EPS);
    assert(fabsf(a[ANIM_PART_ARM_R] + a[ANIM_PART_LEG_R]) < EPS);
    /* Actually swinging. */
    assert(fabsf(a[ANIM_PART_ARM_R]) > EPS);
    printf("PASS: walk cycle\n");
}

int main(void) {
    test_identity_is_no_op();
    test_pivot_is_fixed();
    test_pitch_about_pivot();
    test_head_yaw();
    test_walk();
    printf("All player_anim tests passed.\n");
    return 0;
}
