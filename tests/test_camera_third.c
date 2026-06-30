#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/camera_third.h"

#define EPS 1e-4f

static int feq(float a, float b) { return fabsf(a - b) < EPS; }

/* First person: eye is the player eye, look dir is the forward vector. */
static void test_first_person_is_identity(void) {
    vec3 eye = { 5.0f, 70.0f, -3.0f };
    float yaw = 0.0f, pitch = 0.0f;   /* forward = +X */
    vec3 out_eye, out_dir;
    camera_third_person(eye, yaw, pitch, CAMERA_FIRST_PERSON, 3.0f, out_eye, out_dir);
    assert(feq(out_eye[0], eye[0]) && feq(out_eye[1], eye[1]) && feq(out_eye[2], eye[2]));
    assert(feq(out_dir[0], 1.0f) && feq(out_dir[1], 0.0f) && feq(out_dir[2], 0.0f));
    printf("PASS: first_person_is_identity\n");
}

/* Back mode: eye is behind the player along forward; still looks forward. */
static void test_back_is_behind(void) {
    vec3 eye = { 0.0f, 70.0f, 0.0f };
    float yaw = 0.0f, pitch = 0.0f;   /* forward = +X */
    float dist = 4.0f;
    vec3 out_eye, out_dir;
    camera_third_person(eye, yaw, pitch, CAMERA_THIRD_PERSON_BACK, dist, out_eye, out_dir);
    /* Behind +X forward means smaller X. */
    assert(out_eye[0] < eye[0]);
    assert(feq(out_eye[0], -dist));
    assert(feq(out_eye[2], 0.0f));
    assert(out_eye[1] > eye[1]);          /* slight upward lift */
    /* Look direction unchanged (toward the player and past it). */
    assert(feq(out_dir[0], 1.0f) && feq(out_dir[1], 0.0f) && feq(out_dir[2], 0.0f));
    printf("PASS: back_is_behind\n");
}

/* Front mode: eye is ahead of the player; looks back toward the player. */
static void test_front_is_ahead(void) {
    vec3 eye = { 0.0f, 70.0f, 0.0f };
    float yaw = 0.0f, pitch = 0.0f;   /* forward = +X */
    float dist = 4.0f;
    vec3 out_eye, out_dir;
    camera_third_person(eye, yaw, pitch, CAMERA_THIRD_PERSON_FRONT, dist, out_eye, out_dir);
    assert(out_eye[0] > eye[0]);
    assert(feq(out_eye[0], dist));
    assert(out_eye[1] > eye[1]);          /* slight upward lift */
    /* Looks back: -X. */
    assert(feq(out_dir[0], -1.0f) && feq(out_dir[1], 0.0f) && feq(out_dir[2], 0.0f));
    printf("PASS: front_is_ahead\n");
}

/* Out look dir is always unit length (back/front/first). */
static void test_look_dir_is_unit(void) {
    vec3 eye = { 1.0f, 2.0f, 3.0f };
    float yaws[]   = { 0.0f, 1.3f, -2.1f, 3.0f };
    float pitches[]= { 0.0f, 0.5f, -0.7f, 1.0f };
    CameraMode modes[] = { CAMERA_FIRST_PERSON, CAMERA_THIRD_PERSON_BACK, CAMERA_THIRD_PERSON_FRONT };
    for (int m = 0; m < 3; m++)
        for (int i = 0; i < 4; i++) {
            vec3 out_eye, out_dir;
            camera_third_person(eye, yaws[i], pitches[i], modes[m], 3.5f, out_eye, out_dir);
            float len = sqrtf(out_dir[0]*out_dir[0] + out_dir[1]*out_dir[1] + out_dir[2]*out_dir[2]);
            assert(feq(len, 1.0f));
        }
    printf("PASS: look_dir_is_unit\n");
}

/* Eye offset magnitude (ignoring the vertical lift) equals distance, in the
 * direction sign matching the mode. */
static void test_offset_distance(void) {
    vec3 eye = { 10.0f, 64.0f, -5.0f };
    float yaw = 0.7f, pitch = 0.3f;
    float dist = 3.0f;
    vec3 fwd = { cosf(pitch)*cosf(yaw), sinf(pitch), cosf(pitch)*sinf(yaw) };
    /* normalize */
    float fl = sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
    fwd[0]/=fl; fwd[1]/=fl; fwd[2]/=fl;

    vec3 be, bd, fe, fd;
    camera_third_person(eye, yaw, pitch, CAMERA_THIRD_PERSON_BACK, dist, be, bd);
    camera_third_person(eye, yaw, pitch, CAMERA_THIRD_PERSON_FRONT, dist, fe, fd);
    /* horizontal-only offset (XZ) should equal dist*forward_xz */
    assert(feq(be[0], eye[0] - fwd[0]*dist));
    assert(feq(be[2], eye[2] - fwd[2]*dist));
    assert(feq(fe[0], eye[0] + fwd[0]*dist));
    assert(feq(fe[2], eye[2] + fwd[2]*dist));
    printf("PASS: offset_distance\n");
}

int main(void) {
    test_first_person_is_identity();
    test_back_is_behind();
    test_front_is_ahead();
    test_look_dir_is_unit();
    test_offset_distance();
    printf("ALL camera_third tests passed\n");
    return 0;
}
