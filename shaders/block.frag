#version 450

layout(location = 0) in vec2       frag_uv;
layout(location = 1) in float      frag_light;
layout(location = 2) in float      frag_ao;
layout(location = 3) in float      frag_view_z;
layout(location = 4) in flat uint  frag_tile;

layout(set = 0, binding = 1) uniform sampler2D tex_atlas;

/* Atlas: 16x16 grid of 16px tiles in a 256px texture. Mirror of the C-side
 * TILE_UV / HALF_TEXEL inset so tiled (greedy-merged) quads sample exactly
 * the same in-tile UV window as the legacy single-tile quads. */
const float TILE_UV    = 1.0 / 16.0;
const float HALF_TEXEL = 1.5 / 256.0;

/* Wrap a repeat-space coordinate (range [0..W]x[0..H]) back into a single
 * atlas tile, using textureGrad so mip selection follows the *unwrapped*
 * coordinate — this avoids a high-mip seam where fract() jumps 1->0. */
vec4 sample_tiled(uint tile, vec2 repeat_uv) {
    vec2 tile_origin = vec2(float(tile % 16u), float(tile / 16u)) * TILE_UV;
    float span = TILE_UV - 2.0 * HALF_TEXEL;
    vec2 local = fract(repeat_uv);
    vec2 atlas_uv = tile_origin + HALF_TEXEL + local * span;
    /* Derivatives of the continuous (pre-fract) atlas coordinate. */
    vec2 ddx = dFdx(repeat_uv) * span;
    vec2 ddy = dFdy(repeat_uv) * span;
    return textureGrad(tex_atlas, atlas_uv, ddx, ddy);
}

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
    vec4 tex_color = (frag_tile == 255u)
        ? texture(tex_atlas, frag_uv)        /* legacy single-tile quad */
        : sample_tiled(frag_tile, frag_uv);  /* greedy-merged, tile-repeat UV */
    /* Threshold 0.1 (not 0.5): anisotropic filtering at oblique view
     * angles averages many samples along the projected pixel footprint;
     * at high LODs those samples can cross atlas tile boundaries into
     * empty cells (alpha 0) and collapse the averaged alpha. The old
     * 0.5 cutoff then discarded whole fragments of opaque blocks like
     * water, producing visible "see-through" gaps. The lower threshold
     * still cleanly cuts leaves and other intentionally-transparent
     * tiles (transparent texels have alpha 0). */
    if (tex_color.a < 0.1) discard;

    float sky       = max(frag_light, MIN_BRIGHT);
    float ao_factor = 0.4 + 0.6 * frag_ao;
    vec3  lit       = tex_color.rgb * sky * ubo.sun_color.rgb * ao_factor;

    if (ubo.underwater > 0.0) {
        float fog = clamp(frag_view_z / WATER_FOG_RANGE, 0.0, 1.0);
        vec3 uw = mix(lit * WATER_TINT_NEAR, WATER_COLOR_FAR, fog);
        lit = mix(lit, uw, ubo.underwater);
    }

    out_color = vec4(lit, 1.0);
}
