#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_face_idx;
layout(location = 3) in uint in_part;     // AnimPart this vertex belongs to (limb id)

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4  view;
    mat4  proj;
    vec4  sun_direction;
    vec4  sun_color;
    float ambient;
} ubo;

// Must match src/player_model.c push layout and pipeline push range (124 B):
// mat4 model (64) + vec4 tint (16) + vec4 tint2 (16) + float limb_angle[6] (24)
// + float head_yaw (4). limb_angle[part] is a pitch (radians, about the part's
// pivot X axis); head_yaw is an extra yaw (radians, about Y) for the head only.
layout(push_constant) uniform PushConstants {
    mat4  model;
    vec4  tint;   // a = 0: player skin; a > 0: mob, rgb = primary (upper) colour
    vec4  tint2;  // mob only: rgb = secondary (lower) colour
    float limb_angle[6];
    float head_yaw;
} pc;

layout(location = 0) out vec2  frag_uv;
layout(location = 1) out float frag_light;

const vec3 NORMALS[6] = vec3[6](
    vec3( 1, 0, 0),
    vec3(-1, 0, 0),
    vec3( 0, 1, 0),
    vec3( 0,-1, 0),
    vec3( 0, 0, 1),
    vec3( 0, 0,-1)
);

// Per-part joint pivots in model space (feet-center origin). MUST match
// PIVOTS[] in src/player_anim.c. Index order = AnimPart:
// 0 HEAD, 1 TORSO, 2 ARM_R, 3 ARM_L, 4 LEG_R, 5 LEG_L.
const vec3 PIVOTS[6] = vec3[6](
    vec3( 0.0,   1.25,  0.0),
    vec3( 0.0,   0.0,   0.0),
    vec3( 0.375, 1.25,  0.0),
    vec3(-0.375, 1.25,  0.0),
    vec3( 0.125, 0.625, 0.0),
    vec3(-0.125, 0.625, 0.0)
);

// Rotation about X (pitch). Identity at angle 0.
mat3 rot_x(float a) {
    float c = cos(a), s = sin(a);
    return mat3(1.0, 0.0, 0.0,
                0.0, c,   s,
                0.0, -s,  c);
}
// Rotation about Y (yaw). Identity at angle 0.
mat3 rot_y(float a) {
    float c = cos(a), s = sin(a);
    return mat3(c, 0.0, -s,
                0.0, 1.0, 0.0,
                s, 0.0, c);
}

void main() {
    uint part = in_part;
    vec3  pivot = PIVOTS[part];
    float pitch = pc.limb_angle[part];

    // Per-limb joint rotation about the part's pivot. With all-zero angles this
    // is the identity, so the pose is byte-identical to the old rigid mesh.
    mat3 limb = rot_x(pitch);
    if (part == 0u)              // head also yaws about Y
        limb = limb * rot_y(pc.head_yaw);

    vec3 local_pos    = pivot + limb * (in_pos - pivot);
    vec3 local_normal = limb * NORMALS[in_face_idx];

    vec4 world_pos = pc.model * vec4(local_pos, 1.0);
    gl_Position    = ubo.proj * ubo.view * world_pos;

    vec3 world_normal = normalize(mat3(pc.model) * local_normal);
    float ndotl = max(dot(world_normal, -ubo.sun_direction.xyz), 0.0);
    frag_light  = ubo.ambient + (1.0 - ubo.ambient) * ndotl * ubo.sun_color.r;

    frag_uv = in_uv;
}
