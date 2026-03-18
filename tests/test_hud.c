#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "hud.h"

static void test_hud_init_defaults(void) {
    HUD hud;
    hud_init(&hud);
    assert(hud.selected_slot == 0);
    /* all 6 slots populated with non-AIR blocks */
    for (int i = 0; i < HUD_SLOT_COUNT; i++)
        assert(hud.slot_blocks[i] != BLOCK_AIR);
    printf("PASS: test_hud_init_defaults\n");
}

static void test_hud_selected_block(void) {
    HUD hud;
    hud_init(&hud);
    assert(hud_selected_block(&hud) == hud.slot_blocks[0]);
    hud.selected_slot = 3;
    assert(hud_selected_block(&hud) == hud.slot_blocks[3]);
    printf("PASS: test_hud_selected_block\n");
}

static void test_hud_build_vertex_count(void) {
    HUD hud;
    hud_init(&hud);
    HudVertex verts[HUD_MAX_VERTS];
    uint32_t idx[HUD_MAX_INDICES];
    uint32_t vc = hud_build(&hud, 1280.0f, 720.0f, verts, idx);
    assert(vc > 0);
    assert(vc <= HUD_MAX_VERTS);
    printf("PASS: test_hud_build_vertex_count\n");
}

static void test_hud_crosshair_near_center(void) {
    HUD hud;
    hud_init(&hud);
    HudVertex verts[HUD_MAX_VERTS];
    uint32_t idx[HUD_MAX_INDICES];
    hud_build(&hud, 1280.0f, 720.0f, verts, idx);
    /* Crosshair is first 8 vertices; all within ~14px of NDC origin.
     * 14px / 640px = 0.022 NDC units horizontally. Use 0.03 for safety. */
    for (int i = 0; i < 8; i++) {
        assert(fabsf(verts[i].x) < 0.03f);
        assert(fabsf(verts[i].y) < 0.03f);
    }
    printf("PASS: test_hud_crosshair_near_center\n");
}

static void test_hud_selected_border_is_white(void) {
    HUD hud;
    hud_init(&hud);
    hud.selected_slot = 0;
    HudVertex verts[HUD_MAX_VERTS];
    uint32_t idx[HUD_MAX_INDICES];
    uint32_t vc = hud_build(&hud, 1280.0f, 720.0f, verts, idx);
    /* At least one vertex must be pure white (selected slot border) */
    int found = 0;
    for (uint32_t i = 0; i < vc; i++) {
        if (verts[i].r > 0.99f && verts[i].g > 0.99f &&
            verts[i].b > 0.99f && verts[i].a > 0.99f) {
            found = 1; break;
        }
    }
    assert(found);
    printf("PASS: test_hud_selected_border_is_white\n");
}

int main(void) {
    test_hud_init_defaults();
    test_hud_selected_block();
    test_hud_build_vertex_count();
    test_hud_crosshair_near_center();
    test_hud_selected_border_is_white();
    printf("All HUD tests passed.\n");
    return 0;
}
