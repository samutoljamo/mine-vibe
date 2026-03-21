#include <assert.h>
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

/* hud_build is now a stub that calls ui_rect/ui_text (Task 6 will restore geometry).
 * Verify it at least compiles and does not crash. */
static void test_hud_build_runs(void) {
    HUD hud;
    hud_init(&hud);
    hud_build(&hud, 1280.0f, 720.0f);
    printf("PASS: test_hud_build_runs\n");
}

int main(void) {
    test_hud_init_defaults();
    test_hud_selected_block();
    test_hud_build_runs();
    printf("All HUD tests passed.\n");
    return 0;
}
