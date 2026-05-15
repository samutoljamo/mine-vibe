#version 450

layout(location = 0) in vec2  frag_uv;
layout(location = 1) in float frag_light;
layout(location = 2) in float frag_ao;
layout(location = 3) in float frag_view_z;

layout(set = 0, binding = 1) uniform sampler2D tex_atlas;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 sun_direction;
    vec4 sun_color;
    float ambient;
    float underwater;
} ubo;

layout(location = 0) out vec4 out_color;

const float MIN_BRIGHT = 0.08;

/* Underwater appearance: cool tint applied close to camera, fading to a deep
 * water color at distance. Tuned for visible-but-tinted near, opaque-blue at
 * ~32 blocks out. */
const vec3  WATER_TINT_NEAR = vec3(0.45, 0.65, 0.85);
const vec3  WATER_COLOR_FAR = vec3(0.06, 0.18, 0.40);
const float WATER_FOG_RANGE = 32.0;

void main() {
    vec4 tex_color = texture(tex_atlas, frag_uv);
    if (tex_color.a < 0.5) discard;

    float sky       = max(frag_light, MIN_BRIGHT);
    float ao_factor = 0.4 + 0.6 * frag_ao;
    vec3  lit       = tex_color.rgb * sky * ubo.sun_color.rgb * ao_factor;

    if (ubo.underwater > 0.5) {
        float fog = clamp(frag_view_z / WATER_FOG_RANGE, 0.0, 1.0);
        lit = mix(lit * WATER_TINT_NEAR, WATER_COLOR_FAR, fog);
    }

    out_color = vec4(lit, 1.0);
}
