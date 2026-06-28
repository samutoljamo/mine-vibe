#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_normal;
layout(location = 3) in uint in_ao;
layout(location = 4) in uint in_light;
layout(location = 5) in uint in_tile;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 sun_direction;
    vec4 sun_color;
    float ambient;
    float underwater;
} ubo;

layout(push_constant) uniform PushConstants {
    vec4 chunk_offset;
} pc;

layout(location = 0) out vec2       frag_uv;     /* repeat-space (tiled) or atlas UV */
layout(location = 1) out float      frag_light;
layout(location = 2) out float      frag_ao;
layout(location = 3) out float      frag_view_z;
layout(location = 4) out flat uint  frag_tile;   /* 255 = use frag_uv verbatim */

void main() {
    vec3 world_pos = in_pos + pc.chunk_offset.xyz;
    vec4 view_pos  = ubo.view * vec4(world_pos, 1.0);
    gl_Position    = ubo.proj * view_pos;

    frag_uv     = in_uv;
    frag_light  = float(in_light) / 15.0;
    frag_ao     = float(in_ao) / 3.0;
    frag_view_z = -view_pos.z;   /* positive distance in front of camera */
    frag_tile   = in_tile;
}
