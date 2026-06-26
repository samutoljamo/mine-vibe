#version 450

layout(location = 0) in vec2  frag_uv;
layout(location = 1) in float frag_light;

layout(set = 0, binding = 1) uniform sampler2D tex_skin;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 tint;   // rgb = override colour, a = blend strength (0 = unmodified skin)
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(tex_skin, frag_uv);
    if (tex_color.a < 0.5) discard;   // keep the skin's cutout shape

    vec3 base;
    if (pc.tint.a > 0.0) {
        // Mob: a two-tone zombie instead of the player skin. The model's UVs put
        // the head in the top half of the skin (v < 0.5) and the torso/limbs in
        // the bottom half, so split on frag_uv.y: green head/face, dark-teal body.
        vec3 skin  = vec3(0.40, 0.62, 0.32);   // sickly green
        vec3 cloth = vec3(0.16, 0.34, 0.40);   // dark teal — reads against grass
        base = (frag_uv.y < 0.5) ? skin : cloth;
    } else {
        base = tex_color.rgb;                  // remote player: real skin
    }
    out_color = vec4(base * frag_light, 1.0);
}
