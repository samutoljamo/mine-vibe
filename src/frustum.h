#ifndef FRUSTUM_H
#define FRUSTUM_H

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct Frustum {
    vec4 planes[6];
} Frustum;

void frustum_extract(mat4 vp, Frustum* f);
bool frustum_test_aabb(const Frustum* f, vec3 min, vec3 max);

#endif
