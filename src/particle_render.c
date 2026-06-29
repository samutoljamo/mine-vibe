#include "particle_render.h"

float particle_render_alpha(float life, float max_life)
{
    if (max_life <= 0.0f || life <= 0.0f)
        return 0.0f;
    float ratio = life / max_life;          /* 1 at spawn -> 0 at death */
    if (ratio > 1.0f) ratio = 1.0f;
    /* Hold full opacity for the first ~60% of life, then linearly fade. */
    const float fade_start = 0.6f;
    if (ratio >= fade_start)
        return 1.0f;
    return ratio / fade_start;
}

size_t particle_render_build(const ParticleSystem* ps,
                             const float cam_right[3],
                             const float cam_up[3],
                             float* out)
{
    if (!ps || !cam_right || !cam_up || !out || ps->count <= 0)
        return 0;

    /* The 4 quad corners as (right_scale, up_scale, quad_u, quad_v) and the two
     * triangles (0,1,2)(0,2,3) referencing them. */
    static const float corner[4][4] = {
        { -1.0f, -1.0f, -1.0f, -1.0f },
        {  1.0f, -1.0f,  1.0f, -1.0f },
        {  1.0f,  1.0f,  1.0f,  1.0f },
        { -1.0f,  1.0f, -1.0f,  1.0f },
    };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };

    size_t v = 0;
    float* w = out;
    for (int i = 0; i < ps->count; i++) {
        const Particle* p = &ps->p[i];
        float a = particle_render_alpha(p->life, p->max_life);
        float half = p->size * 0.5f;
        for (int k = 0; k < 6; k++) {
            const float* c = corner[tri[k]];
            float rs = c[0] * half, us = c[1] * half;
            /* world pos = center + right*rs + up*us */
            w[0] = p->x + cam_right[0] * rs + cam_up[0] * us;
            w[1] = p->y + cam_right[1] * rs + cam_up[1] * us;
            w[2] = p->z + cam_right[2] * rs + cam_up[2] * us;
            w[3] = p->r; w[4] = p->g; w[5] = p->b; w[6] = a;
            w[7] = c[2]; w[8] = c[3];
            w += PARTICLE_RENDER_VERT_FLOATS;
            v++;
        }
    }
    return v;
}
