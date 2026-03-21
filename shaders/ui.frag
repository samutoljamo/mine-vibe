#version 450

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    /* Solid quads: UV = white pixel → atlas.r = 1.0 → color passes through.
     * Glyphs: UV = glyph region → atlas.r = glyph alpha. */
    out_color = vec4(frag_color.rgb, frag_color.a * texture(atlas, frag_uv).r);
}
