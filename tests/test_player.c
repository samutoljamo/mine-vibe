#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/player.h"
#include "../src/gameplay.h"
#include "../src/block.h"

/*
 * Walking-mode ticks call physics_move(... World*) -> world_get_block. We stub
 * a flat stone world at y<64 so the player has a floor to stand on. The pointer
 * is never dereferenced (we ignore it), so any non-NULL value works; tests pass
 * a sentinel. This is a real (non-mock) block source: it answers honest queries.
 */
BlockID world_get_block(struct World* w, int x, int y, int z) {
    (void)w; (void)x; (void)z;
    return (y < 64) ? BLOCK_STONE : BLOCK_AIR;
}
static struct World* const FLOOR_WORLD = (struct World*)1;

/*
 * Player state-transition tests. We drive the player through its public API in
 * AGENT MODE, which bypasses all GLFW input reads (player_update only touches
 * glfwGetKey/glfwGetTime on the non-agent path), so window may be NULL.
 *
 * FREE mode with noclip moves position by velocity*dt directly without any
 * world block queries, so world may be NULL too — this exercises the real
 * input->velocity->integration pipeline with no Vulkan/GLFW/world machinery.
 */

static void test_init_defaults(void) {
    Player p;
    vec3 start = { 1.0f, 2.0f, 3.0f };
    player_init(&p, start);
    assert(p.position[0] == 1.0f && p.position[1] == 2.0f && p.position[2] == 3.0f);
    assert(p.velocity[0] == 0.0f && p.velocity[1] == 0.0f && p.velocity[2] == 0.0f);
    assert(p.mode == MODE_FREE);
    assert(p.noclip == true);
    assert(p.on_ground == false);
    assert(p.in_water == false);
    assert(p.agent_mode == false);
    printf("PASS: init_defaults\n");
}

/* Eye position = feet + PLAYER_EYE_H; crouching dips it by PLAYER_SNEAK_EYE_DIP.
 * player_update recomputes eye_pos every frame. */
static void test_eye_position_and_crouch_dip(void) {
    Player p;
    vec3 start = { 0.0f, 64.0f, 0.0f };
    player_init(&p, start);
    p.agent_mode = true;
    /* No input: position unchanged, eye computed. */
    player_update(&p, NULL, NULL, 1.0f / 60.0f);
    assert(fabsf(p.eye_pos[1] - (64.0f + PLAYER_EYE_H)) < 1e-4f);

    /* Switch to walking + crouch and re-run against the stub floor world: eye
     * dips by PLAYER_SNEAK_EYE_DIP. Crouch is set via the agent crouch flag
     * (consumed by tick_walking). Feet start at y=64 resting on the floor. */
    Player pw;
    vec3 stand = { 0.0f, 64.0f, 0.0f };
    player_init(&pw, stand);
    pw.agent_mode = true;
    pw.mode = MODE_WALKING;
    pw.agent_crouch = true;
    player_update(&pw, NULL, FLOOR_WORLD, 1.0f / 60.0f);
    float expect = pw.position[1] + PLAYER_EYE_H - PLAYER_SNEAK_EYE_DIP;
    assert(fabsf(pw.eye_pos[1] - expect) < 1e-4f);
    printf("PASS: eye_position_and_crouch_dip\n");
}

/* FREE mode + noclip: agent forward input drives the player along the camera
 * front vector; with no input velocity is zeroed. No world access on this path. */
static void test_free_noclip_movement(void) {
    Player p;
    vec3 start = { 0.0f, 64.0f, 0.0f };
    player_init(&p, start);
    p.agent_mode = true;
    assert(p.mode == MODE_FREE && p.noclip);

    /* Camera default yaw/pitch points somewhere; capture front by moving and
     * measuring the displacement direction. Drive forward for several frames. */
    vec3 before; glm_vec3_copy(p.position, before);
    p.agent_forward = 1.0f;
    /* Run enough real time to accumulate multiple physics ticks. */
    for (int i = 0; i < 30; i++)
        player_update(&p, NULL, NULL, 1.0f / 60.0f);

    float moved = glm_vec3_distance(before, p.position);
    assert(moved > 0.1f);   /* actually translated */

    /* Releasing input zeroes velocity; position then holds steady. */
    p.agent_forward = 0.0f;
    player_update(&p, NULL, NULL, 1.0f / 60.0f);
    assert(p.velocity[0] == 0.0f && p.velocity[1] == 0.0f && p.velocity[2] == 0.0f);
    vec3 settled; glm_vec3_copy(p.position, settled);
    player_update(&p, NULL, NULL, 1.0f / 60.0f);
    assert(glm_vec3_distance(settled, p.position) < 1e-4f);
    printf("PASS: free_noclip_movement\n");
}

/* Free-mode agent jump adds +Y to the move direction, so the player rises. */
static void test_free_noclip_vertical(void) {
    Player p;
    vec3 start = { 0.0f, 64.0f, 0.0f };
    player_init(&p, start);
    p.agent_mode = true;
    p.agent_jump = true;   /* in free mode this contributes +Y to direction */
    float y0 = p.position[1];
    for (int i = 0; i < 30; i++)
        player_update(&p, NULL, NULL, 1.0f / 60.0f);
    assert(p.position[1] > y0);   /* ascended */
    printf("PASS: free_noclip_vertical\n");
}

/* Fixed-timestep accumulator: a single huge dt is clamped so the player can't
 * teleport an unbounded distance in one frame (accumulator capped at 0.05s =
 * 3 physics ticks). */
static void test_accumulator_clamp(void) {
    Player p;
    vec3 start = { 0.0f, 64.0f, 0.0f };
    player_init(&p, start);
    p.agent_mode = true;
    p.agent_forward = 1.0f;

    vec3 before; glm_vec3_copy(p.position, before);
    /* One absurd frame (10 seconds). Without the clamp this would integrate
     * ~600 ticks; with it, at most 3 ticks worth of movement. */
    player_update(&p, NULL, NULL, 10.0f);
    float moved = glm_vec3_distance(before, p.position);

    /* 3 ticks at FLY_SPEED 20 m/s * (1/60) = ~1.0 m max. Generous upper bound. */
    assert(moved < 2.0f);
    printf("PASS: accumulator_clamp  (moved=%.4f)\n", (double)moved);
}

/* Walking mode against the flat stub floor (top at y=64): a player dropped just
 * above the floor lands, becomes grounded, and stops falling — the real
 * tick_walking gravity + physics_move collision pipeline. */
static void test_walking_lands_on_floor(void) {
    Player p;
    vec3 start = { 0.0f, 66.0f, 0.0f };  /* 2 blocks above the floor top */
    player_init(&p, start);
    p.agent_mode = true;
    p.mode = MODE_WALKING;
    for (int i = 0; i < 120; i++)
        player_update(&p, NULL, FLOOR_WORLD, 1.0f / 60.0f);
    assert(p.on_ground);
    assert(fabsf(p.position[1] - 64.0f) < 1e-2f);  /* feet on floor top */
    assert(p.velocity[1] >= -1e-2f);               /* not still plummeting */
    printf("PASS: walking_lands_on_floor  (y=%.4f)\n", (double)p.position[1]);
}

/* Walking-mode jump: a grounded player given the jump input leaves the ground
 * (upward velocity imparted). */
static void test_walking_jump(void) {
    Player p;
    vec3 start = { 0.0f, 64.0f, 0.0f };  /* resting on floor */
    player_init(&p, start);
    p.agent_mode = true;
    p.mode = MODE_WALKING;
    /* One settle tick to register on_ground. */
    player_update(&p, NULL, FLOOR_WORLD, 1.0f / 60.0f);
    assert(p.on_ground);
    /* Now jump. */
    p.agent_jump = true;
    player_update(&p, NULL, FLOOR_WORLD, 1.0f / 60.0f);
    assert(p.position[1] > 64.0f);   /* lifted off */
    printf("PASS: walking_jump  (y=%.4f)\n", (double)p.position[1]);
}

int main(void) {
    test_init_defaults();
    test_eye_position_and_crouch_dip();
    test_free_noclip_movement();
    test_free_noclip_vertical();
    test_accumulator_clamp();
    test_walking_lands_on_floor();
    test_walking_jump();
    printf("ALL PLAYER TESTS PASSED\n");
    return 0;
}
