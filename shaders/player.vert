#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_face_idx;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4  view;
    mat4  proj;
    vec4  sun_direction;
    vec4  sun_color;
    float ambient;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(location = 0) out vec2  frag_uv;
layout(location = 1) out float frag_light;

const vec3 NORMALS[6] = vec3[6](
    vec3( 1, 0, 0),
    vec3(-1, 0, 0),
    vec3( 0, 1, 0),
    vec3( 0,-1, 0),
    vec3( 0, 0, 1),
    vec3( 0, 0,-1)
);

void main() {
    vec4 world_pos  = pc.model * vec4(in_pos, 1.0);
    gl_Position     = ubo.proj * ubo.view * world_pos;

    vec3 world_normal = normalize(mat3(pc.model) * NORMALS[in_face_idx]);
    float ndotl = max(dot(world_normal, -ubo.sun_direction.xyz), 0.0);
    frag_light  = ubo.ambient + (1.0 - ubo.ambient) * ndotl * ubo.sun_color.r;

    frag_uv = in_uv;
}
