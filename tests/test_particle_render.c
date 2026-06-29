#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/particle.h"
#include "../src/particle_render.h"

static int approx(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* Alpha: full opacity early in life, fades to 0 at death, 0 for dead/degenerate. */
static void test_alpha_fade(void) {
    assert(approx(particle_render_alpha(1.0f, 1.0f), 1.0f));   /* fresh */
    assert(approx(particle_render_alpha(0.8f, 1.0f), 1.0f));   /* still in hold */
    assert(approx(particle_render_alpha(0.0f, 1.0f), 0.0f));   /* dead */
    assert(particle_render_alpha(0.3f, 1.0f) < 1.0f);          /* fading */
    assert(particle_render_alpha(0.3f, 1.0f) > 0.0f);
    assert(approx(particle_render_alpha(1.0f, 0.0f), 0.0f));   /* max_life 0 */
    /* Monotonic non-increasing as life drops. */
    assert(particle_render_alpha(0.5f, 1.0f) >= particle_render_alpha(0.2f, 1.0f));
    printf("PASS: alpha_fade\n");
}

/* Build writes 6 verts/particle, 9 floats each, returns the vertex count. */
static void test_build_vertex_count(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 42u);
    particle_emit_block_break(&ps, 0, 0, 0, 0.5f, 0.5f, 0.5f);
    int n = ps.count;
    assert(n > 0);

    float right[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
    static float out[1024 * 6 * 9];
    size_t verts = particle_render_build(&ps, right, up, out);
    assert(verts == (size_t)n * PARTICLE_RENDER_VERTS_PER);
    printf("PASS: build_vertex_count\n");
}

/* A single particle at the origin with unit size: corners land at +-half along
 * the camera basis, and quad coords span [-1,1]. */
static void test_build_geometry(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 1u);
    /* Hand-place one deterministic particle. */
    ps.count = 1;
    ps.p[0] = (Particle){ .x = 0, .y = 0, .z = 0, .size = 2.0f,
                          .life = 1.0f, .max_life = 1.0f,
                          .r = 1, .g = 0, .b = 0 };
    float right[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
    float out[6 * 9];
    size_t verts = particle_render_build(&ps, right, up, out);
    assert(verts == 6);

    /* half = size*0.5 = 1.0; with right=+x, up=+y, every corner is at
     * (+-1, +-1, 0). Check all 6 verts lie on that square and quad coords match. */
    for (int k = 0; k < 6; k++) {
        const float* v = out + k * 9;
        assert(approx(fabsf(v[0]), 1.0f));   /* x = +-1 */
        assert(approx(fabsf(v[1]), 1.0f));   /* y = +-1 */
        assert(approx(v[2], 0.0f));          /* z = 0   */
        assert(approx(v[3], 1.0f));          /* r */
        assert(approx(v[6], 1.0f));          /* alpha (fresh) */
        assert(approx(fabsf(v[7]), 1.0f) && approx(fabsf(v[8]), 1.0f)); /* quad */
        /* world offset must equal quad coord scaled by half along the basis. */
        assert(approx(v[0], v[7]));          /* x == quad_u * 1.0 */
        assert(approx(v[1], v[8]));          /* y == quad_v * 1.0 */
    }
    printf("PASS: build_geometry\n");
}

/* Empty pool / NULL args produce no vertices and never crash. */
static void test_build_guards(void) {
    ParticleSystem ps;
    particle_system_init(&ps, 9u);     /* count 0 */
    float right[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
    float out[16];
    assert(particle_render_build(&ps, right, up, out) == 0);
    assert(particle_render_build(NULL, right, up, out) == 0);
    assert(particle_render_build(&ps, NULL, up, out) == 0);
    assert(particle_render_build(&ps, right, up, NULL) == 0);
    printf("PASS: build_guards\n");
}

int main(void) {
    test_alpha_fade();
    test_build_vertex_count();
    test_build_geometry();
    test_build_guards();
    printf("ALL PASS: particle_render\n");
    return 0;
}
