#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <cglm/cglm.h>
#include "../src/frustum.h"

/* Build a view-projection matrix for a camera at the origin looking down -Z
 * (right-handed, the convention cglm's glm_lookat / glm_perspective use), then
 * extract a Frustum from it. All geometry below is expressed in that camera's
 * world space so the expected inside/outside results are obvious by hand. */
static void make_frustum(Frustum* f)
{
    mat4 proj, view, vp;
    /* 60-degree vertical FOV, 16:9, near 0.1, far 100. */
    glm_perspective(glm_rad(60.0f), 16.0f / 9.0f, 0.1f, 100.0f, proj);
    /* Eye at origin, looking toward -Z, up = +Y. */
    glm_lookat((vec3){0.0f, 0.0f, 0.0f},
               (vec3){0.0f, 0.0f, -1.0f},
               (vec3){0.0f, 1.0f, 0.0f}, view);
    glm_mat4_mul(proj, view, vp);
    frustum_extract(vp, f);
}

/* A small box squarely in front of the camera (down -Z), well within the far
 * plane, must pass the test. */
static void test_box_inside_passes(void)
{
    Frustum f;
    make_frustum(&f);
    vec3 min = { -1.0f, -1.0f, -20.0f };
    vec3 max = {  1.0f,  1.0f, -18.0f };
    assert(frustum_test_aabb(&f, min, max) == true);
    printf("PASS: box_inside_passes\n");
}

/* A box squarely BEHIND the camera (positive Z) is outside the near/far volume
 * and must be culled. The near plane gives an unambiguous reject here. */
static void test_box_behind_fails(void)
{
    Frustum f;
    make_frustum(&f);
    vec3 min = { -1.0f, -1.0f, 18.0f };
    vec3 max = {  1.0f,  1.0f, 20.0f };
    assert(frustum_test_aabb(&f, min, max) == false);
    printf("PASS: box_behind_fails\n");
}

/* A box far off to the side, beyond the left frustum plane at a modest depth,
 * must be culled. At z=-10 the horizontal half-extent of the frustum is small
 * compared to x=1000, so this is well outside. */
static void test_box_far_to_side_fails(void)
{
    Frustum f;
    make_frustum(&f);
    vec3 min = { 1000.0f, -1.0f, -11.0f };
    vec3 max = { 1002.0f,  1.0f,  -9.0f };
    assert(frustum_test_aabb(&f, min, max) == false);
    printf("PASS: box_far_to_side_fails\n");
}

/* A box beyond the far plane must be culled. */
static void test_box_beyond_far_fails(void)
{
    Frustum f;
    make_frustum(&f);
    vec3 min = { -1.0f, -1.0f, -200.0f };
    vec3 max = {  1.0f,  1.0f, -150.0f };
    assert(frustum_test_aabb(&f, min, max) == false);
    printf("PASS: box_beyond_far_fails\n");
}

/* A box that straddles the near plane (part in front, part behind the camera)
 * intersects the frustum and must pass. */
static void test_box_straddling_passes(void)
{
    Frustum f;
    make_frustum(&f);
    vec3 min = { -1.0f, -1.0f, -5.0f };  /* in front  */
    vec3 max = {  1.0f,  1.0f,  5.0f };  /* behind    */
    assert(frustum_test_aabb(&f, min, max) == true);
    printf("PASS: box_straddling_passes\n");
}

/* A huge box enclosing the whole frustum (camera sits inside it) must pass:
 * the frustum is fully contained, so the AABB intersects it. */
static void test_box_enclosing_passes(void)
{
    Frustum f;
    make_frustum(&f);
    vec3 min = { -1000.0f, -1000.0f, -1000.0f };
    vec3 max = {  1000.0f,  1000.0f,  1000.0f };
    assert(frustum_test_aabb(&f, min, max) == true);
    printf("PASS: box_enclosing_passes\n");
}

/* frustum_extract must produce six normalizable, finite planes. */
static void test_extract_produces_six_planes(void)
{
    Frustum f;
    make_frustum(&f);
    for (int i = 0; i < 6; i++) {
        float nx = f.planes[i][0], ny = f.planes[i][1], nz = f.planes[i][2];
        float len2 = nx * nx + ny * ny + nz * nz;
        /* A real clipping plane has a non-degenerate normal. */
        assert(len2 > 1e-6f);
    }
    printf("PASS: extract_produces_six_planes\n");
}

int main(void)
{
    test_extract_produces_six_planes();
    test_box_inside_passes();
    test_box_behind_fails();
    test_box_far_to_side_fails();
    test_box_beyond_far_fails();
    test_box_straddling_passes();
    test_box_enclosing_passes();
    printf("ALL FRUSTUM TESTS PASSED\n");
    return 0;
}
