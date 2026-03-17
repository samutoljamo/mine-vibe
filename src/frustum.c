#include "frustum.h"

void frustum_extract(mat4 vp, Frustum* f)
{
    glm_frustum_planes(vp, f->planes);
}

bool frustum_test_aabb(const Frustum* f, vec3 min, vec3 max)
{
    vec3 box[2];
    glm_vec3_copy(min, box[0]);
    glm_vec3_copy(max, box[1]);
    return glm_aabb_frustum(box, (vec4*)f->planes);
}
