#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include <stddef.h>
#include "../block.h"

#define HUD_SLOT_COUNT 6

/* ------------------------------------------------------------------ */
/*  Performance stats (pure, testable)                                 */
/* ------------------------------------------------------------------ */

/* Number of recent frametime samples kept for the rolling average. */
#define PERF_SAMPLE_COUNT 120

/* Rolling ring buffer of frametimes (seconds). Pure data + pure helpers
 * below — no Vulkan, no globals — so the FPS/frametime math is unit-testable
 * in isolation (see tests/test_ui.c). */
typedef struct PerfStats {
    float    samples[PERF_SAMPLE_COUNT]; /* frametimes in seconds */
    int      count;                      /* valid samples (<= PERF_SAMPLE_COUNT) */
    int      head;                       /* next write index (ring) */
    /* Per-frame counters surfaced from the renderer, copied in by the caller. */
    uint32_t visible_chunks;
    uint32_t draw_calls;
} PerfStats;

/* Reset all samples and counters. */
static inline void perf_stats_reset(PerfStats* p)
{
    p->count = 0;
    p->head  = 0;
    p->visible_chunks = 0;
    p->draw_calls     = 0;
    for (int i = 0; i < PERF_SAMPLE_COUNT; i++) p->samples[i] = 0.0f;
}

/* Push one frametime sample (seconds). Non-positive samples are ignored so a
 * stalled/zero-dt frame can't poison the average or divide-by-zero the FPS. */
static inline void perf_stats_push(PerfStats* p, float dt_seconds)
{
    if (!(dt_seconds > 0.0f)) return;   /* also rejects NaN */
    p->samples[p->head] = dt_seconds;
    p->head = (p->head + 1) % PERF_SAMPLE_COUNT;
    if (p->count < PERF_SAMPLE_COUNT) p->count++;
}

/* Rolling-average frametime in milliseconds. 0 if no samples yet. */
static inline float perf_stats_avg_frametime_ms(const PerfStats* p)
{
    if (p->count <= 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < p->count; i++) sum += p->samples[i];
    return (sum / (float)p->count) * 1000.0f;
}

/* Rolling-average FPS derived from the average frametime. 0 if no samples. */
static inline float perf_stats_avg_fps(const PerfStats* p)
{
    float ms = perf_stats_avg_frametime_ms(p);
    if (ms <= 0.0f) return 0.0f;
    return 1000.0f / ms;
}

/* Forward declarations to avoid circular includes. */
struct Inventory;

void    hud_build(const struct Inventory* inv, int player_health, float sw, float sh);
BlockID hud_selected_block(const struct Inventory* inv);

/* Draw the performance overlay (top-left): rolling FPS + frametime, visible
 * chunk count and draw-call count. Pure UI emission; safe to call between
 * ui_frame_begin / ui_frame_end. */
void    hud_draw_stats(const PerfStats* p, float sw, float sh);

/* Latch the latest server-reported survival stats so hud_build (whose
 * signature is fixed by its renderer caller) can draw the hunger + oxygen
 * bars. food: 0..20, air: 0..20. The client calls this from the
 * PKT_PLAYER_HEALTH handler. */
void    hud_set_survival(int food, int air);

#endif
