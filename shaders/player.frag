#version 450

layout(location = 0) in vec2  frag_uv;
layout(location = 1) in float frag_light;

layout(set = 0, binding = 1) uniform sampler2D tex_skin;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(tex_skin, frag_uv);
    if (tex_color.a < 0.5) discard;
    out_color = vec4(tex_color.rgb * frag_light, 1.0);
}
