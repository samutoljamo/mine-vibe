#version 450

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    /* RGBA atlas:
     *   - White pixel region: sampled (1,1,1,1), tint passes through.
     *   - Glyphs: sampled (g,g,g,g), tint multiplies through (matches old R8 behaviour).
     *   - Block icons: sampled full RGBA, tint white passes full color through. */
    out_color = texture(atlas, frag_uv) * frag_color;
}
