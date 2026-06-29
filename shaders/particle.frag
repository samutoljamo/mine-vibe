#version 450

/* Particle billboard fragment shader: vertex color with a soft round falloff
 * (so square quads read as little puffs/sparks) and life-faded alpha. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_quad;

layout(location = 0) out vec4 out_color;

void main() {
    float d = dot(v_quad, v_quad);     /* squared distance from quad center */
    if (d > 1.0)
        discard;                       /* clip to a disc */
    float falloff = 1.0 - d;           /* fade toward the rim */
    out_color = vec4(v_color.rgb, v_color.a * falloff);
}
