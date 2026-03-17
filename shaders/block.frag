#version 450

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in float frag_light;
layout(location = 2) in float frag_ao;

layout(set = 0, binding = 1) uniform sampler2D tex_atlas;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(tex_atlas, frag_uv);
    if (tex_color.a < 0.5) discard;

    float ao_factor = 0.4 + 0.6 * frag_ao;
    out_color = vec4(tex_color.rgb * frag_light * ao_factor, 1.0);
}
