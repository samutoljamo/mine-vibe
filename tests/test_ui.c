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

int main(void)
{
    test_font_bake_succeeds();
    test_text_width_empty();
    test_text_width_positive();
    test_text_width_scales();
    test_text_width_longer_is_wider();
    test_perf_empty_is_zero();
    test_perf_constant_60fps();
    test_perf_averages_samples();
    test_perf_rejects_nonpositive();
    test_perf_ring_caps_count();
    printf("All ui tests passed.\n");
    return 0;
}
