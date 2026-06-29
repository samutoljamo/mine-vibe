#version 450

/* Particle billboard vertex shader.
 *
 * Each particle is expanded CPU-side (renderer_frame.c) into two triangles
 * (6 vertices) whose corners are already offset along the camera's right/up
 * basis in WORLD space, so this shader only has to project them. Per-vertex
 * RGBA carries the tint with alpha already faded by remaining life, plus a
 * local quad coordinate used by the fragment shader for a soft round falloff. */

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4  view;
    mat4  proj;
    vec4  sun_direction;
    vec4  sun_color;
    float ambient;
} ubo;

layout(location = 0) in vec3 pos;     /* world-space billboard corner          */
layout(location = 1) in vec4 color;   /* rgba, alpha pre-faded by life ratio   */
layout(location = 2) in vec2 quad;    /* corner in [-1,1]^2 for the round mask  */

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_quad;

void main() {
    v_color = color;
    v_quad  = quad;
    gl_Position = ubo.proj * ubo.view * vec4(pos, 1.0);
}
