#version 450

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4  view;
    mat4  proj;
    vec4  sun_direction;
    vec4  sun_color;
    float ambient;
} ubo;

layout(location = 0) in vec3 pos;

void main() {
    gl_Position = ubo.proj * ubo.view * vec4(pos, 1.0);
}
