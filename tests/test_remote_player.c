#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/remote_player.h"

#define TWO_PI_F  6.28318530f
#define HALF_PI_F 1.57079632f
#define EPSILON   1e-4f

static int feq(float a, float b) { return fabsf(a - b) < EPSILON; }

/* Build a fully-populated 2-snapshot player at a given render_time.
 * positions[0] = (x0,y0,z0)@t0, positions[1] = (x1,y1,z1)@t1. */
static RemotePlayer make_player(
    float x0, float y0, float z0, float yaw0, float pitch0, double t0,
    float x1, float y1, float z1, float yaw1, float pitch1, double t1,
    double render_time)
{
    RemotePlayer p;
    memset(&p, 0, sizeof(p));
    p.active              = true;
    p.snapshot_count      = 2;
    p.positions[0][0]     = x0; p.positions[0][1] = y0; p.positions[0][2] = z0;
    p.positions[1][0]     = x1; p.positions[1][1] = y1; p.positions[1][2] = z1;
    p.yaws[0]             = yaw0;   p.yaws[1]    = yaw1;
    p.pitches[0]          = pitch0; p.pitches[1] = pitch1;
    p.snapshot_times[0]   = t0;
    p.snapshot_times[1]   = t1;
    p.render_time         = render_time;
    return p;
}

/* --- Guard: snapshot_count < 2 ---
 * Should return positions[1] directly, not interpolate from the zero-filled [0] slot.
 * FAILS with old code: old code lerps from positions[0]=(0,0,0), giving ~4.995 not 5.0. */
static void test_guard_snapshot_count_lt2(void)
{
    RemotePlayer p;
    memset(&p, 0, sizeof(p));
    p.active              = true;
    p.snapshot_count      = 1;
    p.positions[1][0]     = 5.0f;
    p.positions[1][1]     = 10.0f;
    p.positions[1][2]     = 15.0f;
    p.yaws[1]             = 1.0f;
    p.pitches[1]          = 0.5f;
    p.snapshot_times[1]   = 100.0;
    p.render_time         = 99.9;  /* just before snapshot, ensures t < 1 in old code */

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    assert(feq(pos[0], 5.0f));
    assert(feq(pos[1], 10.0f));
    assert(feq(pos[2], 15.0f));
    assert(feq(yaw,    1.0f));
    assert(feq(pitch,  0.5f));
    printf("PASS: test_guard_snapshot_count_lt2\n");
}

/* --- Guard: dt_snap == 0 (regression) ---
 * Both old and new code return positions[1] here; test preserves that. */
static void test_guard_dt_snap_zero(void)
{
    RemotePlayer p = make_player(
        0,0,0, 0,0, 5.0,
        10,0,0, 1,0, 5.0,  /* identical timestamps */
        5.5);

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    assert(feq(pos[0], 10.0f));
    assert(feq(pos[1],  0.0f));
    assert(feq(pos[2],  0.0f));
    assert(feq(yaw, 1.0f));
    assert(feq(pitch, 0.0f));
    printf("PASS: test_guard_dt_snap_zero\n");
}

/* --- Interpolation midpoint (regression) ---
 * Basic lerp at t=0.5 must still work after the rewrite. */
static void test_interpolate_midpoint(void)
{
    RemotePlayer p = make_player(
        0,0,0, 0,0, 0.0,
        10,0,0, 1,0, 1.0,
        0.5);

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    assert(feq(pos[0], 5.0f));
    assert(feq(yaw,    0.5f));
    assert(feq(pos[1], 0.0f));
    assert(feq(pos[2], 0.0f));
    assert(feq(pitch, 0.0f));
    printf("PASS: test_interpolate_midpoint\n");
}

/* --- Yaw wrap in interpolation path ---
 * Yaw crosses the 0/2π boundary; short-way-round must be taken.
 * yaw0 ≈ 2π-0.2 (≈349°), yaw1 = 0.2 (≈11°): short path is +0.4 rad CCW.
 * FAILS with old code: no wrap → gives ~π (completely wrong direction). */
static void test_interpolate_yaw_wrap(void)
{
    float yaw0 = TWO_PI_F - 0.2f;
    float yaw1 = 0.2f;

    RemotePlayer p = make_player(
        0,0,0, yaw0, 0, 0.0,
        0,0,0, yaw1, 0, 1.0,
        0.5);

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    /* dyaw wrapped = +0.4; at t=0.5: yaw0 + 0.2 */
    float expected = yaw0 + 0.2f;
    assert(feq(yaw, expected));
    printf("PASS: test_interpolate_yaw_wrap\n");
}

/* --- Extrapolation: basic ---
 * render_time = 1.5s; snapshots at 0s and 1s; player moving at 10 m/s in X.
 * excess = 0.5s → out_pos[0] = 10 + 10*0.5 = 15.
 * FAILS with old code: clamps t=1.0, returns positions[1]=(10,0,0). */
static void test_extrapolation_basic(void)
{
    RemotePlayer p = make_player(
        0,0,0, 0,0, 0.0,
        10,0,0, 0,0, 1.0,
        1.5);

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    assert(feq(pos[0], 15.0f));
    assert(feq(pos[1],  0.0f));
    assert(feq(pos[2],  0.0f));
    printf("PASS: test_extrapolation_basic\n");
}

/* --- Extrapolation: 2-second cap ---
 * render_time = 6.0s; excess = 5.0s → capped at 2.0s.
 * out_pos[0] = 10 + 10*2.0 = 30 (not 60).
 * FAILS with old code: returns positions[1]=(10,0,0). */
static void test_extrapolation_cap(void)
{
    RemotePlayer p = make_player(
        0,0,0, 0,0, 0.0,
        10,0,0, 0,0, 1.0,
        6.0);

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    assert(feq(pos[0], 30.0f));
    printf("PASS: test_extrapolation_cap\n");
}

/* --- Extrapolation: pitch clamp ---
 * pitches[0]=0, pitches[1]=HALF_PI/2 (45°); excess=2.0s after cap.
 * pit_vel = HALF_PI/2; unclamped = HALF_PI/2 + HALF_PI/2*2 = 3*HALF_PI/2 > HALF_PI.
 * clamped result = HALF_PI.
 * FAILS with old code: returns pitches[1]=HALF_PI/2, not HALF_PI. */
static void test_extrapolation_pitch_clamp(void)
{
    float p1_pitch = HALF_PI_F / 2.0f;

    RemotePlayer p = make_player(
        0,0,0, 0, 0.0f,    0.0,
        0,0,0, 0, p1_pitch, 1.0,
        3.0);  /* excess = 2.0 after cap */

    vec3 pos; float yaw, pitch;
    remote_player_interpolate(&p, 0.0f, pos, &yaw, &pitch);

    assert(feq(pitch, HALF_PI_F));
    printf("PASS: test_extrapolation_pitch_clamp\n");
}

int main(void)
{
    test_guard_snapshot_count_lt2();
    test_guard_dt_snap_zero();
    test_interpolate_midpoint();
    test_interpolate_yaw_wrap();
    test_extrapolation_basic();
    test_extrapolation_cap();
    test_extrapolation_pitch_clamp();
    printf("All remote_player tests passed.\n");
    return 0;
}
