#version 450

layout(location = 0) in vec2  frag_uv;
layout(location = 1) in float frag_light;

layout(set = 0, binding = 1) uniform sampler2D tex_skin;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 tint;   // a = 0: player skin; a > 0: mob, rgb = primary (upper) colour
    vec4 tint2;  // mob only: rgb = secondary (lower) colour
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(tex_skin, frag_uv);
    if (tex_color.a < 0.5) discard;   // keep the skin's cutout shape

    vec3 base;
    if (pc.tint.a > 0.0) {
        // Mob: a two-tone box instead of the player skin. The model's UVs put
        // the head in the top half of the skin (v < 0.5) and the torso/limbs in
        // the bottom half, so split on frag_uv.y. Colours are supplied per mob
        // type via the push constants (primary = upper, secondary = lower).
        base = (frag_uv.y < 0.5) ? pc.tint.rgb : pc.tint2.rgb;
    } else {
        base = tex_color.rgb;                  // remote player: real skin
    }
    out_color = vec4(base * frag_light, 1.0);
}
