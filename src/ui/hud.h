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

/* ------------------------------------------------------------------ */
/*  Game-UI state machine (pure, testable)                             */
/* ------------------------------------------------------------------ */
/*
 * High-level screen the client is showing. The renderer always calls
 * hud_build(); hud_build dispatches on the latched screen (set from main.c
 * via hud_set_screen) so the main menu / pause / inventory overlays compose
 * on top of (or instead of) the in-world HUD without the renderer needing to
 * know about any of them. Transition + cursor-capture logic lives in pure
 * helpers below so it can be unit-tested with no Vulkan.
 */
typedef enum {
    GAME_MAIN_MENU = 0,
    GAME_PLAYING,
    GAME_PAUSED,
    GAME_INVENTORY,
} GameUiState;

/* Stable element ids for the data-driven hit-test API. Menu buttons and
 * inventory slots share one id space; slot ids start at HUD_ID_SLOT0. */
enum {
    HUD_ID_NONE = -1,
    HUD_ID_PLAY = 1000,   /* main menu: Play   */
    HUD_ID_QUIT,          /* main menu / pause: Quit */
    HUD_ID_RESUME,        /* pause: Resume     */
    HUD_ID_CLOSE,         /* inventory: close  */
    HUD_ID_SLOT0 = 2000,  /* inventory/hotbar slots: HUD_ID_SLOT0 + i */
};

/* Axis-aligned rectangle in screen pixels (top-left origin). */
typedef struct { float x, y, w, h; } HudRect;

/* Esc behaviour: from PLAYING -> PAUSED, from PAUSED -> PLAYING. From
 * INVENTORY esc returns to PLAYING. From MAIN_MENU esc is a no-op (stays).
 * Pure; returns the next state. */
GameUiState game_ui_toggle_pause(GameUiState s);

/* Inventory key (E) behaviour: PLAYING <-> INVENTORY. No-op in menu/pause.
 * Pure; returns the next state. */
GameUiState game_ui_toggle_inventory(GameUiState s);

/* True when the mouse cursor should be free (visible) for this state — i.e.
 * any menu/overlay. False means the cursor is captured for mouselook
 * (PLAYING). Pure. */
bool game_ui_cursor_free(GameUiState s);

/* True when in-world input (movement, mouselook, block break/place) should be
 * processed. Only PLAYING. Pure. */
bool game_ui_world_active(GameUiState s);

/* Layout helpers — pure, shared by main.c (hit-test registration) and
 * hud.c (drawing) so the clickable region always matches the drawn button.
 * Buttons are stacked vertically and centred horizontally.
 *
 * `index` is the 0-based button position from the top of the stack. */
HudRect hud_menu_button_rect(int index, float sw, float sh);

/* Inventory slot rect for slot `i` (0..HUD_SLOT_COUNT-1) in the inventory
 * screen's grid. */
HudRect hud_inventory_slot_rect(int i, float sw, float sh);

/* Point-in-rect test (inclusive top-left, exclusive bottom-right). Pure. */
bool hud_rect_contains(HudRect r, float px, float py);

/* Latch the screen to draw. Called from main.c each frame (or on change). */
void    hud_set_screen(GameUiState s);
GameUiState hud_get_screen(void);

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
