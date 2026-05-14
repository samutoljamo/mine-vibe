#version 450

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(0.0, 0.0, 0.0, 0.6);   /* semi-transparent black outline */
}
