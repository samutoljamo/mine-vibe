#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include <stddef.h>
#include "../block.h"
#include "../item.h"

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
    /* Per-frame counters surfaced from the renderer, copied in by the caller.
     * culled_chunks: candidate chunk meshes rejected by the frustum test;
     * total_chunks = visible + culled (the candidate set this frame). */
    uint32_t visible_chunks;
    uint32_t culled_chunks;
    uint32_t total_chunks;
    uint32_t draw_calls;
} PerfStats;

/* Reset all samples and counters. */
static inline void perf_stats_reset(PerfStats* p)
{
    p->count = 0;
    p->head  = 0;
    p->visible_chunks = 0;
    p->culled_chunks  = 0;
    p->total_chunks   = 0;
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
    GAME_OPTIONS,
    GAME_CONTAINER,   /* an open furnace/chest screen (driven by container_open) */
} GameUiState;

/* Stable element ids for the data-driven hit-test API. Menu buttons and
 * inventory slots share one id space; slot ids start at HUD_ID_SLOT0. */
enum {
    HUD_ID_NONE = -1,
    HUD_ID_PLAY = 1000,   /* main menu: Play (legacy; superseded by New/Load) */
    HUD_ID_QUIT,          /* main menu / pause: Quit */
    HUD_ID_RESUME,        /* pause: Resume     */
    HUD_ID_CLOSE,         /* inventory: close  */
    HUD_ID_NEW_WORLD,     /* main menu: New World   */
    HUD_ID_LOAD_WORLD,    /* main menu: Load World (opens the world list) */
    HUD_ID_BACK,          /* world list: back to main menu */
    HUD_ID_SAVE,          /* pause: Save            */
    HUD_ID_SAVE_QUIT,     /* pause: Save & Quit     */
    HUD_ID_OPTIONS,       /* main menu / pause: Options */
    HUD_ID_VOL_SLIDER,    /* options: master volume slider track */
    HUD_ID_MUTE,          /* options: mute toggle      */
    HUD_ID_MUSIC,         /* options: music on/off toggle */
    HUD_ID_SLOT0 = 2000,  /* inventory/hotbar slots: HUD_ID_SLOT0 + i */
    HUD_ID_CRAFT0 = 3000, /* inventory crafting-panel rows: HUD_ID_CRAFT0 + i */
    HUD_ID_WORLD0 = 4000, /* load-world list rows: HUD_ID_WORLD0 + i */
    HUD_ID_CON0  = 5000,  /* container screen: container slots, HUD_ID_CON0 + i
                           * (chest 0..26 / furnace 0=input 1=fuel 2=output) */
    HUD_ID_CONINV0 = 6000,/* container screen: player-inventory slots,
                           * HUD_ID_CONINV0 + i */
};

/* The main menu has an extra "Load World" sub-screen. It isn't a GameUiState
 * (those gate cursor/world-input); instead hud.c latches a small page flag so
 * the main-menu draw path renders either the buttons or the world list. */
typedef enum {
    MENU_PAGE_ROOT = 0,   /* New World / Load World / Quit */
    MENU_PAGE_LOAD,       /* clickable list of saved worlds + Back */
} MenuPage;

void     hud_set_menu_page(MenuPage p);
MenuPage hud_get_menu_page(void);

/* Latch the names shown on the Load-World page. `names` is borrowed only for
 * the duration of the call (copied internally). Pass count<=0 to clear. */
void hud_set_world_list(const char* const* names, int count);

/* Max crafting recipe rows the inventory panel lays out / hit-tests. Kept here
 * so main.c (registration) and hud.c (drawing) agree without including
 * crafting.h. The visible list is clamped to the affordable recipes. */
#define HUD_CRAFT_ROWS 12

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

/* In-world hotbar slot rect for slot `i` (0..HUD_SLOT_COUNT-1), anchored to the
 * bottom-centre of the screen. Pure; shared by the draw path so the icon, count
 * and selection highlight all share one geometry. */
HudRect hud_hotbar_slot_rect(int i, float sw, float sh);

/* Clamped fill fraction [0,1] of a bar/indicator given a current `value` and a
 * positive `max`. value<=0 -> 0, value>=max -> 1, otherwise value/max. A
 * non-positive `max` yields 0 (degenerate bar, never negative/NaN). Pure;
 * drives the health/hunger/armour fill widths so the math is unit-testable. */
float hud_bar_fill(int value, int max);

/* Crafting-panel row rect for the `i`-th listed (affordable) recipe in the
 * inventory screen. Rows stack vertically below the slot grid. Pure; shared by
 * main.c (hit-test registration) and hud.c (drawing) so clicks line up. */
HudRect hud_craft_row_rect(int i, float sw, float sh);

/* Point-in-rect test (inclusive top-left, exclusive bottom-right). Pure. */
bool hud_rect_contains(HudRect r, float px, float py);

/* ------------------------------------------------------------------ */
/*  Container screen (furnace / chest) layout + drawing                 */
/* ------------------------------------------------------------------ */

/* Forward declaration so the container drawer can take a (const Inventory*)
 * without hud.h depending on inventory.h (mirrors hud_build below). */
struct Inventory;

/* Furnace slot index ordering on the wire (matches the server's
 * sbe_transfer / CONTAINER_STATE field order). */
enum {
    HUD_FURNACE_SLOT_INPUT  = 0,
    HUD_FURNACE_SLOT_FUEL   = 1,
    HUD_FURNACE_SLOT_OUTPUT = 2,
    HUD_FURNACE_SLOT_COUNT  = 3,
};

/* Number of chest slots laid out (3 rows of 9). Must match
 * CONTAINER_NET_CHEST_SLOTS / CHEST_SLOTS. */
#define HUD_CHEST_SLOTS 27
#define HUD_CHEST_COLS   9

/* Chest slot `i` (0..HUD_CHEST_SLOTS-1) rect in the container screen's 3x9
 * grid (drawn above the player inventory). Pure. */
HudRect hud_chest_slot_rect(int i, float sw, float sh);

/* Furnace slot `i` (HUD_FURNACE_SLOT_*) rect: input (top) + fuel (below) on the
 * left, output on the right. Pure. */
HudRect hud_furnace_slot_rect(int i, float sw, float sh);

/* Player-inventory slot `i` (0..INVENTORY_SLOTS-1) rect as drawn at the bottom
 * of the container screen (one centred row). Pure. */
HudRect hud_container_inv_slot_rect(int i, float sw, float sh);

/* Clamped fraction [0,1] of the furnace cook arrow given the current
 * cook_progress in ticks and the per-item tick budget. Negative/zero budget or
 * progress yields 0; progress past the budget clamps to 1. Pure. */
float hud_furnace_progress_fill(int cook_progress, int ticks_per_item);

/* Draw the open container screen (furnace or chest) over the player inventory.
 * `con` is a const ContainerStatePacket* (net.h); typed as void* here so hud.h
 * doesn't pull in net.h. Must be non-NULL. Pure UI emission. */
void hud_draw_container(const void* con,
                        const struct Inventory* inv, float sw, float sh);

/* Latch the latest container snapshot (const ContainerStatePacket*, or NULL to
 * clear) so hud_build can draw the GAME_CONTAINER screen without a new
 * parameter. main.c calls this each frame from c->container. */
void hud_set_container(const void* con);

/* ------------------------------------------------------------------ */
/*  Options / settings screen (pure helpers + latched audio state)     */
/* ------------------------------------------------------------------ */

/* Track rect of the master-volume slider on the Options screen. Pure; shared by
 * main.c (hit-test + value math) and hud.c (drawing) so the clickable track
 * matches the drawn one. */
HudRect hud_volume_slider_rect(float sw, float sh);

/* Map a cursor x (screen pixels) within a slider `track` to a value in [0,1],
 * clamped at the ends. Pure. */
float hud_slider_value_from_x(HudRect track, float px);

/* Inverse of hud_slider_value_from_x: the x (screen pixels) of the knob centre
 * for a value in [0,1], clamped to the track. Pure. */
float hud_slider_x_from_value(HudRect track, float v);

/* Latch the current audio settings so the Options screen can draw them without
 * pulling in audio.c. `volume` is clamped to [0,1]. main.c keeps this in sync
 * with the audio API (it owns audio_*). */
void  hud_set_audio_state(float volume, bool muted, bool music);

float hud_get_volume(void);   /* latched master volume, [0,1] */
bool  hud_get_muted(void);    /* latched mute flag            */
bool  hud_get_music(void);    /* latched music-on flag        */

/* Pure toggles of the latched flags; return the new value. main.c calls these
 * and then mirrors the result into the audio API. */
bool  hud_toggle_muted(void);
bool  hud_toggle_music(void);

/* Latch the screen to draw. Called from main.c each frame (or on change). */
void    hud_set_screen(GameUiState s);
GameUiState hud_get_screen(void);

/* Latch the latest server-reported armour set so hud_build can draw the armour
 * bar above the hearts. `worn` is ARMOR_SLOT_COUNT item ids (BLOCK_AIR = empty);
 * `points` is the clamped total armour points. The client calls this from the
 * PKT_ARMOR handler. Passing NULL clears the bar. */
void    hud_set_armor(const ItemId* worn, int points);

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

/* Start a brief red screen-edge hurt flash that decays on its own over the next
 * ~0.35s. Called from the client's PKT_PLAYER_HEALTH handler whenever the local
 * player's health drops. Idempotent (re-arming just restarts the fade). */
void    hud_trigger_hurt_flash(void);

/* Advance time-based HUD effects (currently the hurt flash) by dt seconds.
 * Called once per frame from the main loop. */
void    hud_tick(float dt);

/* Latch the current eating progress (0 = not eating, (0,1] = fraction of the
 * eat hold elapsed) so hud_build can draw a progress nibble above the hotbar.
 * main.c updates this each frame while the eat key is held; set to 0 when not
 * eating. Clamped to [0,1]. */
void    hud_set_eat_progress(float p);

#endif
