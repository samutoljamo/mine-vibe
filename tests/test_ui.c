#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <volk.h>
#include "../src/ui/ui.h"
#include "../src/ui/hud.h"

static void test_font_bake_succeeds(void)
{
    bool ok = ui_font_bake();
    assert(ok && "font bake must succeed");
    printf("PASS: test_font_bake_succeeds\n");
}

static void test_text_width_empty(void)
{
    ui_font_bake();
    float w = ui_text_width("", 20.0f);
    assert(w == 0.0f);
    printf("PASS: test_text_width_empty\n");
}

static void test_text_width_positive(void)
{
    ui_font_bake();
    float w = ui_text_width("Hello", 20.0f);
    assert(w > 0.0f && "non-empty string must have positive width");
    printf("PASS: test_text_width_positive\n");
}

static void test_text_width_scales(void)
{
    ui_font_bake();
    float w20 = ui_text_width("ABC", 20.0f);
    float w40 = ui_text_width("ABC", 40.0f);
    /* 40px text should be approximately 2x the width of 20px text */
    assert(fabsf(w40 - 2.0f * w20) < 2.0f && "text width must scale with size");
    printf("PASS: test_text_width_scales\n");
}

static void test_text_width_longer_is_wider(void)
{
    ui_font_bake();
    float w1 = ui_text_width("A", 20.0f);
    float w3 = ui_text_width("AAA", 20.0f);
    assert(w3 > w1 && "longer string must be wider");
    printf("PASS: test_text_width_longer_is_wider\n");
}

/* ------------------------------------------------------------------ */
/*  Geometry capacity guard (pure)                                     */
/* ------------------------------------------------------------------ */

static void test_geom_fits_within_capacity(void)
{
    /* Empty buffers: a single quad (4 verts, 6 indices) fits. */
    assert(ui_geom_fits(0, 4, 0, 6) && "quad fits in empty buffers");
    /* Exactly filling the buffers is allowed (boundary inclusive). */
    assert(ui_geom_fits(UI_MAX_VERTS - 4, 4, UI_MAX_INDICES - 6, 6) &&
           "appending up to the exact capacity must fit");
    printf("PASS: test_geom_fits_within_capacity\n");
}

static void test_geom_fits_rejects_overflow(void)
{
    /* One vertex too many. */
    assert(!ui_geom_fits(UI_MAX_VERTS - 3, 4, 0, 6) &&
           "vertex overflow must be rejected");
    /* One index too many (vertices fine). */
    assert(!ui_geom_fits(0, 4, UI_MAX_INDICES - 5, 6) &&
           "index overflow must be rejected");
    /* Already full buffers reject any append. */
    assert(!ui_geom_fits(UI_MAX_VERTS, 4, UI_MAX_INDICES, 6) &&
           "full buffers reject further appends");
    printf("PASS: test_geom_fits_rejects_overflow\n");
}

/* ------------------------------------------------------------------ */
/*  Input handler tests                                                */
/* ------------------------------------------------------------------ */

static void test_hittest_inside(void)
{
    ui_input_begin();
    ui_add_element(7, 100.0f, 100.0f, 50.0f, 50.0f);
    /* cursor inside, not clicked */
    ui_handle_mouse(120.0f, 120.0f, false);
    assert(ui_hovered_element() == 7 && "cursor inside element must hover it");
    assert(ui_clicked_element() == -1 && "no click means no clicked element");
    printf("PASS: test_hittest_inside\n");
}

static void test_hittest_outside(void)
{
    ui_input_begin();
    ui_add_element(7, 100.0f, 100.0f, 50.0f, 50.0f);
    ui_handle_mouse(10.0f, 10.0f, false);
    assert(ui_hovered_element() == -1 && "cursor outside any element must not hover");
    printf("PASS: test_hittest_outside\n");
}

static void test_hittest_edges(void)
{
    ui_input_begin();
    ui_add_element(3, 100.0f, 100.0f, 50.0f, 50.0f);
    /* top-left corner is inside (inclusive), bottom-right edge is exclusive */
    ui_handle_mouse(100.0f, 100.0f, false);
    assert(ui_hovered_element() == 3 && "top-left corner is inside");
    ui_handle_mouse(150.0f, 150.0f, false);
    assert(ui_hovered_element() == -1 && "bottom-right edge is exclusive");
    printf("PASS: test_hittest_edges\n");
}

static void test_topmost_on_overlap(void)
{
    ui_input_begin();
    ui_add_element(1, 100.0f, 100.0f, 100.0f, 100.0f);
    ui_add_element(2, 120.0f, 120.0f, 100.0f, 100.0f); /* added later = on top */
    ui_handle_mouse(130.0f, 130.0f, false); /* inside both */
    assert(ui_hovered_element() == 2 && "later-added element wins on overlap");
    printf("PASS: test_topmost_on_overlap\n");
}

static void test_click_dispatch(void)
{
    ui_input_begin();
    ui_add_element(42, 0.0f, 0.0f, 200.0f, 200.0f);
    ui_handle_mouse(50.0f, 50.0f, true);
    assert(ui_hovered_element() == 42 && "clicked element is also hovered");
    assert(ui_clicked_element() == 42 && "click inside element sets clicked id");
    printf("PASS: test_click_dispatch\n");
}

static void test_click_empty_space_noop(void)
{
    ui_input_begin();
    ui_add_element(42, 0.0f, 0.0f, 50.0f, 50.0f);
    ui_handle_mouse(500.0f, 500.0f, true); /* click outside */
    assert(ui_hovered_element() == -1 && "no element hovered in empty space");
    assert(ui_clicked_element() == -1 && "click in empty space dispatches nothing");
    printf("PASS: test_click_empty_space_noop\n");
}

static void test_input_begin_resets(void)
{
    ui_input_begin();
    ui_add_element(9, 0.0f, 0.0f, 100.0f, 100.0f);
    ui_handle_mouse(10.0f, 10.0f, true);
    assert(ui_clicked_element() == 9);
    /* new frame clears registered elements and state */
    ui_input_begin();
    ui_handle_mouse(10.0f, 10.0f, true);
    assert(ui_hovered_element() == -1 && "elements cleared after input_begin");
    assert(ui_clicked_element() == -1 && "clicked state cleared after input_begin");
    printf("PASS: test_input_begin_resets\n");
}

static int g_cb_id = -1;
static void* g_cb_user = NULL;
static void click_cb(int id, void* user) { g_cb_id = id; g_cb_user = user; }

static void test_click_callback(void)
{
    int marker = 0;
    g_cb_id = -1; g_cb_user = NULL;
    ui_input_begin();
    ui_add_element_cb(5, 0.0f, 0.0f, 100.0f, 100.0f, click_cb, &marker);
    /* hover only: callback must not fire */
    ui_handle_mouse(10.0f, 10.0f, false);
    assert(g_cb_id == -1 && "callback must not fire on hover");
    /* click: callback fires with id and user pointer */
    ui_handle_mouse(10.0f, 10.0f, true);
    assert(g_cb_id == 5 && "click invokes callback with element id");
    assert(g_cb_user == &marker && "callback receives user pointer");
    printf("PASS: test_click_callback\n");
}

/* ------------------------------------------------------------------ */
/*  PerfStats — pure rolling-average frametime -> FPS helper           */
/* ------------------------------------------------------------------ */

static void test_perf_empty_is_zero(void)
{
    PerfStats p;
    perf_stats_reset(&p);
    assert(perf_stats_avg_frametime_ms(&p) == 0.0f);
    assert(perf_stats_avg_fps(&p) == 0.0f);
    assert(p.visible_chunks == 0 && p.draw_calls == 0);
    printf("PASS: test_perf_empty_is_zero\n");
}

static void test_perf_constant_60fps(void)
{
    PerfStats p;
    perf_stats_reset(&p);
    /* Feed a steady 16.6667 ms/frame -> exactly 60 FPS. */
    for (int i = 0; i < 10; i++) perf_stats_push(&p, 1.0f / 60.0f);
    assert(fabsf(perf_stats_avg_frametime_ms(&p) - (1000.0f / 60.0f)) < 0.01f);
    assert(fabsf(perf_stats_avg_fps(&p) - 60.0f) < 0.05f);
    printf("PASS: test_perf_constant_60fps\n");
}

static void test_perf_averages_samples(void)
{
    PerfStats p;
    perf_stats_reset(&p);
    /* 10ms and 30ms -> 20ms average -> 50 FPS. */
    perf_stats_push(&p, 0.010f);
    perf_stats_push(&p, 0.030f);
    assert(fabsf(perf_stats_avg_frametime_ms(&p) - 20.0f) < 0.01f);
    assert(fabsf(perf_stats_avg_fps(&p) - 50.0f) < 0.05f);
    printf("PASS: test_perf_averages_samples\n");
}

static void test_perf_rejects_nonpositive(void)
{
    PerfStats p;
    perf_stats_reset(&p);
    perf_stats_push(&p, 0.0f);    /* ignored (no div-by-zero) */
    perf_stats_push(&p, -1.0f);   /* ignored */
    assert(p.count == 0);
    assert(perf_stats_avg_fps(&p) == 0.0f);
    perf_stats_push(&p, 0.020f);  /* now one valid sample */
    assert(p.count == 1);
    assert(fabsf(perf_stats_avg_fps(&p) - 50.0f) < 0.05f);
    printf("PASS: test_perf_rejects_nonpositive\n");
}

static void test_perf_ring_caps_count(void)
{
    PerfStats p;
    perf_stats_reset(&p);
    /* Overfill the ring; count must saturate at PERF_SAMPLE_COUNT and the
     * average must reflect only the most-recent window. */
    for (int i = 0; i < PERF_SAMPLE_COUNT + 50; i++)
        perf_stats_push(&p, 1.0f / 30.0f);   /* 33.33 ms -> 30 FPS */
    assert(p.count == PERF_SAMPLE_COUNT);
    assert(fabsf(perf_stats_avg_fps(&p) - 30.0f) < 0.05f);
    printf("PASS: test_perf_ring_caps_count\n");
}

/* ------------------------------------------------------------------ */
/*  Game-UI state machine (pure)                                       */
/* ------------------------------------------------------------------ */

static void test_pause_toggle(void)
{
    assert(game_ui_toggle_pause(GAME_PLAYING) == GAME_PAUSED);
    assert(game_ui_toggle_pause(GAME_PAUSED)  == GAME_PLAYING);
    /* Esc closes the inventory back to playing. */
    assert(game_ui_toggle_pause(GAME_INVENTORY) == GAME_PLAYING);
    /* Esc is a no-op in the main menu. */
    assert(game_ui_toggle_pause(GAME_MAIN_MENU) == GAME_MAIN_MENU);
    printf("PASS: test_pause_toggle\n");
}

static void test_inventory_toggle(void)
{
    assert(game_ui_toggle_inventory(GAME_PLAYING)   == GAME_INVENTORY);
    assert(game_ui_toggle_inventory(GAME_INVENTORY) == GAME_PLAYING);
    /* No-op outside the world. */
    assert(game_ui_toggle_inventory(GAME_MAIN_MENU) == GAME_MAIN_MENU);
    assert(game_ui_toggle_inventory(GAME_PAUSED)    == GAME_PAUSED);
    printf("PASS: test_inventory_toggle\n");
}

static void test_cursor_and_world_active(void)
{
    /* Cursor is captured only while playing; free in every menu/overlay. */
    assert(game_ui_cursor_free(GAME_PLAYING)   == false);
    assert(game_ui_cursor_free(GAME_PAUSED)    == true);
    assert(game_ui_cursor_free(GAME_INVENTORY) == true);
    assert(game_ui_cursor_free(GAME_MAIN_MENU) == true);
    /* World simulation runs only while playing. */
    assert(game_ui_world_active(GAME_PLAYING)   == true);
    assert(game_ui_world_active(GAME_PAUSED)    == false);
    assert(game_ui_world_active(GAME_INVENTORY) == false);
    assert(game_ui_world_active(GAME_MAIN_MENU) == false);
    /* An open container is a free-cursor overlay that freezes the world. */
    assert(game_ui_cursor_free(GAME_CONTAINER)  == true);
    assert(game_ui_world_active(GAME_CONTAINER) == false);
    printf("PASS: test_cursor_and_world_active\n");
}

static void test_menu_button_layout(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect b0 = hud_menu_button_rect(0, sw, sh);
    HudRect b1 = hud_menu_button_rect(1, sw, sh);
    /* Buttons are centred horizontally and don't overlap vertically. */
    assert(fabsf((b0.x + b0.w * 0.5f) - sw * 0.5f) < 0.5f);
    assert(b1.y > b0.y + b0.h && "second button sits below the first with a gap");
    assert(b0.w > 0.0f && b0.h > 0.0f);
    /* The centre of button 0 hit-tests inside button 0's rect. */
    assert(hud_rect_contains(b0, b0.x + b0.w * 0.5f, b0.y + b0.h * 0.5f));
    /* ...and not inside button 1. */
    assert(!hud_rect_contains(b1, b0.x + b0.w * 0.5f, b0.y + b0.h * 0.5f));
    printf("PASS: test_menu_button_layout\n");
}

static void test_inventory_slot_layout(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect prev = hud_inventory_slot_rect(0, sw, sh);
    for (int i = 1; i < HUD_SLOT_COUNT; i++) {
        HudRect r = hud_inventory_slot_rect(i, sw, sh);
        assert(r.x > prev.x && "slots are laid out left to right");
        assert(r.x >= prev.x + prev.w && "slots do not overlap horizontally");
        assert(fabsf(r.y - prev.y) < 0.001f && "slots share a row");
        prev = r;
    }
    /* The whole row is horizontally centred. */
    HudRect first = hud_inventory_slot_rect(0, sw, sh);
    HudRect last  = hud_inventory_slot_rect(HUD_SLOT_COUNT - 1, sw, sh);
    float row_mid = (first.x + (last.x + last.w)) * 0.5f;
    assert(fabsf(row_mid - sw * 0.5f) < 0.5f);
    printf("PASS: test_inventory_slot_layout\n");
}

static void test_hotbar_slot_layout(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect prev = hud_hotbar_slot_rect(0, sw, sh);
    assert(prev.w > 0.0f && prev.h > 0.0f);
    for (int i = 1; i < HUD_SLOT_COUNT; i++) {
        HudRect r = hud_hotbar_slot_rect(i, sw, sh);
        assert(r.x > prev.x && "hotbar slots run left to right");
        assert(r.x >= prev.x + prev.w && "hotbar slots do not overlap");
        assert(fabsf(r.y - prev.y) < 0.001f && "hotbar slots share a row");
        prev = r;
    }
    /* Row is horizontally centred. */
    HudRect first = hud_hotbar_slot_rect(0, sw, sh);
    HudRect last  = hud_hotbar_slot_rect(HUD_SLOT_COUNT - 1, sw, sh);
    float row_mid = (first.x + (last.x + last.w)) * 0.5f;
    assert(fabsf(row_mid - sw * 0.5f) < 0.5f);
    /* Anchored near the bottom of the screen. */
    assert(first.y + first.h < sh && "hotbar fits on screen");
    assert(first.y > sh * 0.5f && "hotbar sits in the lower half");
    /* The centre of slot 0 hit-tests inside slot 0 and not slot 1. */
    HudRect s1 = hud_hotbar_slot_rect(1, sw, sh);
    assert(hud_rect_contains(first, first.x + first.w * 0.5f, first.y + first.h * 0.5f));
    assert(!hud_rect_contains(s1, first.x + first.w * 0.5f, first.y + first.h * 0.5f));
    printf("PASS: test_hotbar_slot_layout\n");
}

static void test_bar_fill_fraction(void)
{
    /* Endpoints. */
    assert(hud_bar_fill(0, 20)  == 0.0f);
    assert(hud_bar_fill(20, 20) == 1.0f);
    /* Midpoint. */
    assert(fabsf(hud_bar_fill(10, 20) - 0.5f) < 0.001f);
    assert(fabsf(hud_bar_fill(5, 20)  - 0.25f) < 0.001f);
    /* Clamps below 0 and above 1. */
    assert(hud_bar_fill(-5, 20) == 0.0f && "negative value clamps to 0");
    assert(hud_bar_fill(40, 20) == 1.0f && "over-max value clamps to 1");
    /* Degenerate max never produces NaN / negative. */
    assert(hud_bar_fill(5, 0)  == 0.0f && "zero max yields empty bar");
    assert(hud_bar_fill(5, -3) == 0.0f && "negative max yields empty bar");
    /* Monotonic non-decreasing in value. */
    float prev = -1.0f;
    for (int v = 0; v <= 20; v++) {
        float f = hud_bar_fill(v, 20);
        assert(f >= prev && "bar fill is monotonic in value");
        prev = f;
    }
    printf("PASS: test_bar_fill_fraction\n");
}

static void test_low_health_intensity(void)
{
    /* At/above the threshold (25% of 20 = 5 HP) there is no low-health pulse. */
    assert(hud_low_health_intensity(20, 20) == 0.0f && "full health => 0");
    assert(hud_low_health_intensity(10, 20) == 0.0f && "half health => 0");
    assert(hud_low_health_intensity(6, 20)  == 0.0f && "just above threshold => 0");
    assert(hud_low_health_intensity(5, 20)  == 0.0f && "at threshold => 0");
    /* Below the threshold it ramps up as health drops toward 0. */
    float i4 = hud_low_health_intensity(4, 20);
    float i2 = hud_low_health_intensity(2, 20);
    float i1 = hud_low_health_intensity(1, 20);
    assert(i4 > 0.0f && i4 < 1.0f && "below threshold ramps in (0,1)");
    assert(i2 > i4 && "intensity increases as health drops");
    assert(i1 > i2 && "intensity increases as health drops");
    /* At/below 0 HP it saturates at 1. */
    assert(hud_low_health_intensity(0, 20)  == 1.0f && "0 HP => full intensity");
    assert(hud_low_health_intensity(-5, 20) == 1.0f && "negative clamps to 1");
    /* Degenerate max never produces NaN / out-of-range. */
    assert(hud_low_health_intensity(5, 0)  == 0.0f && "zero max => 0");
    assert(hud_low_health_intensity(5, -3) == 0.0f && "negative max => 0");
    /* Monotonic non-increasing in health. */
    float prev = 2.0f;
    for (int h = 0; h <= 20; h++) {
        float v = hud_low_health_intensity(h, 20);
        assert(v >= 0.0f && v <= 1.0f && "intensity stays in [0,1]");
        assert(v <= prev && "intensity is non-increasing as health rises");
        prev = v;
    }
    printf("PASS: test_low_health_intensity\n");
}

static void test_pulse_factor(void)
{
    /* Pulse oscillates in [0,1] regardless of time. */
    for (float t = 0.0f; t < 10.0f; t += 0.123f) {
        float p = hud_pulse_factor(t);
        assert(p >= 0.0f && p <= 1.0f && "pulse stays in [0,1]");
    }
    /* Periodicity: the pulse repeats over its period (2*pi/freq). */
    float a = hud_pulse_factor(0.0f);
    float b = hud_pulse_factor((float)(2.0 * 3.14159265358979 / HUD_LOW_HEALTH_PULSE_HZ));
    assert(fabsf(a - b) < 0.01f && "pulse is periodic");
    printf("PASS: test_pulse_factor\n");
}

static void test_swing_phase(void)
{
    /* Phase is elapsed/duration clamped to [0,1]. */
    assert(hud_swing_phase(0.0f, 0.25f) == 0.0f && "start => 0");
    assert(fabsf(hud_swing_phase(0.125f, 0.25f) - 0.5f) < 1e-5f && "mid => 0.5");
    assert(hud_swing_phase(0.25f, 0.25f) == 1.0f && "end => 1");
    assert(hud_swing_phase(1.0f, 0.25f) == 1.0f && "past end clamps to 1");
    assert(hud_swing_phase(-0.5f, 0.25f) == 0.0f && "negative clamps to 0");
    /* Degenerate duration never NaN/out-of-range. */
    assert(hud_swing_phase(0.1f, 0.0f) == 1.0f && "zero duration => fully done");
    assert(hud_swing_phase(0.1f, -1.0f) == 1.0f && "negative duration => fully done");
    printf("PASS: test_swing_phase\n");
}

static void test_swing_arc(void)
{
    /* The arc is 0 at both ends (rest), peaks at the middle of the swing. */
    assert(fabsf(hud_swing_arc(0.0f)) < 1e-5f && "arc starts at rest");
    assert(fabsf(hud_swing_arc(1.0f)) < 1e-5f && "arc ends at rest");
    float peak = hud_swing_arc(0.5f);
    assert(peak > 0.9f && peak <= 1.0f && "arc peaks near 1 mid-swing");
    /* Stays in [0,1] across the whole sweep and rises then falls. */
    float prev = -1.0f;
    bool rose = false, fell = false;
    for (float p = 0.0f; p <= 1.0f + 1e-4f; p += 0.05f) {
        float a = hud_swing_arc(p);
        assert(a >= -1e-5f && a <= 1.0f + 1e-5f && "arc stays in [0,1]");
        if (prev >= 0.0f) {
            if (a > prev + 1e-4f) rose = true;
            if (a < prev - 1e-4f) fell = true;
        }
        prev = a;
    }
    assert(rose && fell && "arc rises then falls (an arc, not monotone)");
    printf("PASS: test_swing_arc\n");
}

static void test_chest_slot_layout(void)
{
    float sw = 1280.0f, sh = 720.0f;
    /* 27 slots in a 3x9 grid: left-to-right within a row, rows stacked down. */
    for (int row = 0; row < 3; row++) {
        HudRect prev = hud_chest_slot_rect(row * HUD_CHEST_COLS, sw, sh);
        assert(prev.w > 0.0f && prev.h > 0.0f);
        for (int col = 1; col < HUD_CHEST_COLS; col++) {
            HudRect r = hud_chest_slot_rect(row * HUD_CHEST_COLS + col, sw, sh);
            assert(r.x > prev.x && "chest slots run left to right");
            assert(r.x >= prev.x + prev.w && "chest slots do not overlap");
            assert(fabsf(r.y - prev.y) < 0.001f && "chest row shares a y");
            prev = r;
        }
    }
    /* Row 1 sits below row 0. */
    HudRect r0 = hud_chest_slot_rect(0, sw, sh);
    HudRect r1 = hud_chest_slot_rect(HUD_CHEST_COLS, sw, sh);
    assert(r1.y >= r0.y + r0.h && "chest rows stack downward");
    /* The grid is horizontally centred. */
    HudRect first = hud_chest_slot_rect(0, sw, sh);
    HudRect last  = hud_chest_slot_rect(HUD_CHEST_COLS - 1, sw, sh);
    float mid = (first.x + (last.x + last.w)) * 0.5f;
    assert(fabsf(mid - sw * 0.5f) < 0.5f);
    /* Hit-test: centre of slot 0 is inside slot 0, not slot 1. */
    HudRect s1 = hud_chest_slot_rect(1, sw, sh);
    assert(hud_rect_contains(first, first.x + first.w * 0.5f, first.y + first.h * 0.5f));
    assert(!hud_rect_contains(s1, first.x + first.w * 0.5f, first.y + first.h * 0.5f));
    printf("PASS: test_chest_slot_layout\n");
}

static void test_furnace_slot_layout(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect in  = hud_furnace_slot_rect(HUD_FURNACE_SLOT_INPUT,  sw, sh);
    HudRect fu  = hud_furnace_slot_rect(HUD_FURNACE_SLOT_FUEL,   sw, sh);
    HudRect out = hud_furnace_slot_rect(HUD_FURNACE_SLOT_OUTPUT, sw, sh);
    assert(in.w > 0.0f && fu.w > 0.0f && out.w > 0.0f);
    /* Fuel sits below input (same column). */
    assert(fu.y > in.y + in.h * 0.5f && "fuel is below input");
    assert(fabsf(fu.x - in.x) < 0.001f && "input + fuel share a column");
    /* Output is to the right of the input/fuel column. */
    assert(out.x > in.x + in.w && "output is right of the input column");
    /* The three slots are distinct, non-overlapping hit targets. */
    assert(!hud_rect_contains(fu,  in.x + in.w * 0.5f, in.y + in.h * 0.5f));
    assert(!hud_rect_contains(out, in.x + in.w * 0.5f, in.y + in.h * 0.5f));
    printf("PASS: test_furnace_slot_layout\n");
}

static void test_furnace_progress_fill(void)
{
    /* Endpoints + midpoint of the cook arrow. */
    assert(hud_furnace_progress_fill(0, 200)   == 0.0f);
    assert(hud_furnace_progress_fill(200, 200) == 1.0f);
    assert(fabsf(hud_furnace_progress_fill(100, 200) - 0.5f) < 0.001f);
    /* Clamps. */
    assert(hud_furnace_progress_fill(-10, 200) == 0.0f);
    assert(hud_furnace_progress_fill(400, 200) == 1.0f);
    /* Degenerate budget never yields NaN/negative. */
    assert(hud_furnace_progress_fill(50, 0)  == 0.0f);
    assert(hud_furnace_progress_fill(50, -7) == 0.0f);
    printf("PASS: test_furnace_progress_fill\n");
}

static void test_container_inv_slot_layout(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect prev = hud_container_inv_slot_rect(0, sw, sh);
    assert(prev.w > 0.0f && prev.h > 0.0f);
    for (int i = 1; i < HUD_SLOT_COUNT; i++) {
        HudRect r = hud_container_inv_slot_rect(i, sw, sh);
        assert(r.x > prev.x && "player-inv slots run left to right");
        assert(r.x >= prev.x + prev.w && "player-inv slots do not overlap");
        assert(fabsf(r.y - prev.y) < 0.001f && "player-inv slots share a row");
        prev = r;
    }
    /* The player inventory row sits below the chest grid. */
    HudRect chest_last = hud_chest_slot_rect(HUD_CHEST_SLOTS - 1, sw, sh);
    HudRect inv0 = hud_container_inv_slot_rect(0, sw, sh);
    assert(inv0.y >= chest_last.y + chest_last.h
           && "player inventory is below the chest grid");
    printf("PASS: test_container_inv_slot_layout\n");
}

static void test_rect_contains_edges(void)
{
    HudRect r = { 10.0f, 20.0f, 30.0f, 40.0f };
    assert(hud_rect_contains(r, 10.0f, 20.0f) && "top-left inclusive");
    assert(!hud_rect_contains(r, 40.0f, 20.0f) && "right edge exclusive");
    assert(!hud_rect_contains(r, 10.0f, 60.0f) && "bottom edge exclusive");
    assert(hud_rect_contains(r, 25.0f, 40.0f) && "interior point inside");
    assert(!hud_rect_contains(r, 9.0f, 40.0f) && "left of rect outside");
    printf("PASS: test_rect_contains_edges\n");
}

/* ------------------------------------------------------------------ */
/*  Options / settings screen (pure)                                   */
/* ------------------------------------------------------------------ */

static void test_options_state_is_a_menu(void)
{
    /* The options screen is a menu: cursor free, world frozen. */
    assert(game_ui_cursor_free(GAME_OPTIONS)  == true);
    assert(game_ui_world_active(GAME_OPTIONS) == false);
    /* Esc backs out of options to the pause overlay. */
    assert(game_ui_toggle_pause(GAME_OPTIONS) == GAME_PAUSED);
    /* E (inventory) is a no-op while in options. */
    assert(game_ui_toggle_inventory(GAME_OPTIONS) == GAME_OPTIONS);
    printf("PASS: test_options_state_is_a_menu\n");
}

static void test_volume_slider_value_mapping(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect t = hud_volume_slider_rect(sw, sh);
    assert(t.w > 0.0f && t.h > 0.0f);
    /* Clicking the left edge maps to 0, the right edge to 1, the centre to ~0.5. */
    assert(fabsf(hud_slider_value_from_x(t, t.x)            - 0.0f) < 0.001f);
    assert(fabsf(hud_slider_value_from_x(t, t.x + t.w)      - 1.0f) < 0.001f);
    assert(fabsf(hud_slider_value_from_x(t, t.x + t.w*0.5f) - 0.5f) < 0.001f);
    /* Out-of-range cursor positions clamp to [0,1]. */
    assert(hud_slider_value_from_x(t, t.x - 100.0f) == 0.0f);
    assert(hud_slider_value_from_x(t, t.x + t.w + 100.0f) == 1.0f);
    /* Monotonic: moving right never decreases the value. */
    float prev = -1.0f;
    for (int i = 0; i <= 10; i++) {
        float v = hud_slider_value_from_x(t, t.x + t.w * (i / 10.0f));
        assert(v >= prev && "slider value is monotonic in x");
        prev = v;
    }
    printf("PASS: test_volume_slider_value_mapping\n");
}

static void test_volume_slider_value_x_roundtrip(void)
{
    float sw = 1280.0f, sh = 720.0f;
    HudRect t = hud_volume_slider_rect(sw, sh);
    /* x_from_value puts the knob inside the track and round-trips the value. */
    for (int i = 0; i <= 10; i++) {
        float v = i / 10.0f;
        float x = hud_slider_x_from_value(t, v);
        assert(x >= t.x - 0.001f && x <= t.x + t.w + 0.001f);
        assert(fabsf(hud_slider_value_from_x(t, x) - v) < 0.001f);
    }
    printf("PASS: test_volume_slider_value_x_roundtrip\n");
}

static void test_options_audio_latch_and_toggle(void)
{
    /* The HUD latches the audio settings (it can't call audio.c directly) and
     * exposes pure toggles for main.c to drive the audio API with. */
    hud_set_audio_state(0.5f, false, true);
    assert(fabsf(hud_get_volume() - 0.5f) < 0.001f);
    assert(hud_get_muted() == false);
    assert(hud_get_music() == true);

    /* Toggles flip the latched flags and return the new value. */
    assert(hud_toggle_muted() == true);
    assert(hud_get_muted() == true);
    assert(hud_toggle_muted() == false);
    assert(hud_get_muted() == false);

    assert(hud_toggle_music() == false);
    assert(hud_get_music() == false);
    assert(hud_toggle_music() == true);
    assert(hud_get_music() == true);

    /* Volume latch clamps to [0,1]. */
    hud_set_audio_state(2.0f, true, false);
    assert(fabsf(hud_get_volume() - 1.0f) < 0.001f);
    assert(hud_get_muted() == true);
    assert(hud_get_music() == false);
    hud_set_audio_state(-1.0f, false, false);
    assert(hud_get_volume() == 0.0f);
    printf("PASS: test_options_audio_latch_and_toggle\n");
}

static void test_options_button_navigation(void)
{
    /* The pause menu has an Options button; clicking it should resolve to the
     * Options id via the shared hit-test API (mirrors how main.c registers
     * pause buttons against hud_menu_button_rect). */
    float sw = 1280.0f, sh = 720.0f;
    /* Pause layout: Resume, Options, Save, Save&Quit, Quit. Options is row 1. */
    HudRect opt = hud_menu_button_rect(1, sw, sh);
    ui_input_begin();
    ui_add_element(HUD_ID_OPTIONS, opt.x, opt.y, opt.w, opt.h);
    ui_handle_mouse(opt.x + opt.w * 0.5f, opt.y + opt.h * 0.5f, true);
    assert(ui_clicked_element() == HUD_ID_OPTIONS && "Options button click resolves");

    /* The options screen has a Back control; clicking it resolves to BACK. */
    HudRect back = hud_menu_button_rect(4, sw, sh);
    ui_input_begin();
    ui_add_element(HUD_ID_BACK, back.x, back.y, back.w, back.h);
    ui_handle_mouse(back.x + back.w * 0.5f, back.y + back.h * 0.5f, true);
    assert(ui_clicked_element() == HUD_ID_BACK && "Back button click resolves");
    printf("PASS: test_options_button_navigation\n");
}

int main(void)
{
    test_font_bake_succeeds();
    test_text_width_empty();
    test_text_width_positive();
    test_text_width_scales();
    test_text_width_longer_is_wider();
    test_geom_fits_within_capacity();
    test_geom_fits_rejects_overflow();
    test_hittest_inside();
    test_hittest_outside();
    test_hittest_edges();
    test_topmost_on_overlap();
    test_click_dispatch();
    test_click_empty_space_noop();
    test_input_begin_resets();
    test_click_callback();
    test_perf_empty_is_zero();
    test_perf_constant_60fps();
    test_perf_averages_samples();
    test_perf_rejects_nonpositive();
    test_perf_ring_caps_count();
    test_pause_toggle();
    test_inventory_toggle();
    test_cursor_and_world_active();
    test_menu_button_layout();
    test_inventory_slot_layout();
    test_hotbar_slot_layout();
    test_bar_fill_fraction();
    test_low_health_intensity();
    test_pulse_factor();
    test_swing_phase();
    test_swing_arc();
    test_chest_slot_layout();
    test_furnace_slot_layout();
    test_furnace_progress_fill();
    test_container_inv_slot_layout();
    test_rect_contains_edges();
    test_options_state_is_a_menu();
    test_volume_slider_value_mapping();
    test_volume_slider_value_x_roundtrip();
    test_options_audio_latch_and_toggle();
    test_options_button_navigation();
    printf("All ui tests passed.\n");
    return 0;
}
