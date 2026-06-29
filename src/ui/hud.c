#include "hud.h"
#include "ui.h"
#include "../inventory.h"
#include "../crafting.h"
#include "../item.h"
#include "../net.h"        /* ContainerStatePacket + CONTAINER_NET_* tags */
#include "../smelting.h"   /* SMELT_TICKS_PER_ITEM for the cook arrow */
#include "../survival.h"   /* SURVIVAL_MAX_HEALTH for the low-health threshold */
#include <stdio.h>
#include <math.h>

#define SLOT_SIZE   40
#define SLOT_GAP     4
#define SLOT_BORDER  4
#define CROSSHAIR_W 16
#define CROSSHAIR_T  2
#define CROSSHAIR_GAP 4   /* centre gap so the crosshair frames the target */

/* Menu button geometry (pixels). Shared by layout + draw. */
#define BTN_W       260.0f
#define BTN_H        48.0f
#define BTN_GAP      16.0f
#define MENU_TOP_OFF 40.0f   /* first button below vertical centre */

/* Inventory grid geometry. */
#define INV_SLOT     54.0f
#define INV_SLOT_GAP  8.0f

/* Static assert for the cross-header invariant introduced in Task 1's fix. */
_Static_assert(HUD_SLOT_COUNT == INVENTORY_SLOTS,
    "HUD slot count must match Inventory slot count");

/* Latest server-reported survival stats. Latched by hud_set_survival because
 * hud_build's signature is owned by the (non-editable) renderer caller and
 * can't grow new parameters. Default to full so the bars look right before the
 * first PKT_PLAYER_HEALTH arrives. */
static int g_hud_food = 20;
static int g_hud_air  = 20;

void hud_set_survival(int food, int air)
{
    g_hud_food = food;
    g_hud_air  = air;
}

/* Hurt flash: a brief red vignette at the screen edges when the local player
 * takes damage. hud_trigger_hurt_flash arms it to full; hud_tick(dt) decays it
 * toward 0 each frame (driven from main.c's loop) so it fades over
 * ~HUD_HURT_FLASH_SEC. g_hurt_flash is the remaining 0..1 intensity. */
#define HUD_HURT_FLASH_SEC 0.35f
static float g_hurt_flash = 0.0f;

/* Free-running clock (seconds) advanced by hud_tick. Drives the persistent
 * low-health pulse so it breathes independently of the transient hurt flash. */
static float g_hud_time = 0.0f;

/* Latched "player eyes submerged" flag. Set from main.c each frame (deferred
 * one-liner; hud_build's signature can't grow an in_water param). When true,
 * hud_build paints a blue full-screen tint. */
static bool g_hud_underwater = false;

void hud_trigger_hurt_flash(void)
{
    g_hurt_flash = 1.0f;
}

void hud_set_underwater(bool underwater)
{
    g_hud_underwater = underwater;
}

void hud_tick(float dt)
{
    if (dt > 0.0f) g_hud_time += dt;
    if (g_hurt_flash > 0.0f) {
        g_hurt_flash -= dt / HUD_HURT_FLASH_SEC;
        if (g_hurt_flash < 0.0f) g_hurt_flash = 0.0f;
    }
}

/* Pure: low-health overlay intensity (0 above threshold, ramps to 1 at 0 HP). */
float hud_low_health_intensity(int health, int max_health)
{
    if (max_health <= 0) return 0.0f;
    if (health < 0) health = 0;
    float frac = (float)health / (float)max_health;   /* 0..1 health fraction */
    if (frac >= HUD_LOW_HEALTH_FRAC) return 0.0f;
    /* Linear ramp: 0 at the threshold, 1 at 0 HP. */
    float t = 1.0f - (frac / HUD_LOW_HEALTH_FRAC);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* Pure: raised-cosine pulse in [0,1] at HUD_LOW_HEALTH_PULSE_HZ. */
float hud_pulse_factor(float t)
{
    float w = 6.28318530717958647692f * HUD_LOW_HEALTH_PULSE_HZ * t;
    return 0.5f - 0.5f * cosf(w);
}

/* Eat progress: 0 = not eating, (0,1] = fraction of the eat hold elapsed. The
 * client latches it each frame while the eat key is held; hud_build draws a
 * small progress nibble above the hotbar. */
static float g_eat_progress = 0.0f;

void hud_set_eat_progress(float p)
{
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    g_eat_progress = p;
}

/* Latest server-reported armour total (points). Drawn as a 10-icon bar above
 * the hearts; each icon represents 2 points (matching the heart granularity). */
static int g_hud_armor_points = 0;

void hud_set_armor(const ItemId* worn, int points)
{
    (void)worn;   /* the bar is driven by the points total */
    g_hud_armor_points = points < 0 ? 0 : points;
}

/* Latched container snapshot for the GAME_CONTAINER screen. hud_build's
 * signature is owned by the renderer caller and can't grow a parameter, so
 * main.c latches the latest c->container here each frame (same pattern as the
 * survival/armour latches). NULL = no container to draw. */
static ContainerStatePacket g_hud_container;
static bool                 g_hud_container_valid = false;

void hud_set_container(const void* con_v)
{
    const ContainerStatePacket* con = (const ContainerStatePacket*)con_v;
    if (con) { g_hud_container = *con; g_hud_container_valid = true; }
    else     { g_hud_container_valid = false; }
}

/* ------------------------------------------------------------------ */
/*  Game-UI state machine (pure)                                       */
/* ------------------------------------------------------------------ */

static GameUiState g_screen = GAME_MAIN_MENU;

void        hud_set_screen(GameUiState s) { g_screen = s; }
GameUiState hud_get_screen(void)          { return g_screen; }

/* Main-menu sub-page (root buttons vs. the Load-World list) + the latched list
 * of world names to show. Names are copied so the caller's buffer can change. */
static MenuPage g_menu_page = MENU_PAGE_ROOT;
void     hud_set_menu_page(MenuPage p) { g_menu_page = p; }
MenuPage hud_get_menu_page(void)       { return g_menu_page; }

#define HUD_WORLD_LIST_MAX 64
static char g_world_names[HUD_WORLD_LIST_MAX][64];
static int  g_world_count = 0;

void hud_set_world_list(const char* const* names, int count)
{
    if (count < 0) count = 0;
    if (count > HUD_WORLD_LIST_MAX) count = HUD_WORLD_LIST_MAX;
    for (int i = 0; i < count; i++)
        snprintf(g_world_names[i], sizeof(g_world_names[i]), "%s",
                 names[i] ? names[i] : "");
    g_world_count = count;
}

GameUiState game_ui_toggle_pause(GameUiState s)
{
    switch (s) {
        case GAME_PLAYING:   return GAME_PAUSED;
        case GAME_PAUSED:    return GAME_PLAYING;
        case GAME_INVENTORY: return GAME_PLAYING;  /* esc closes inventory */
        case GAME_OPTIONS:   return GAME_PAUSED;   /* esc backs out of options */
        default:             return s;             /* main menu: no-op */
    }
}

GameUiState game_ui_toggle_inventory(GameUiState s)
{
    switch (s) {
        case GAME_PLAYING:   return GAME_INVENTORY;
        case GAME_INVENTORY: return GAME_PLAYING;
        default:             return s;             /* ignored in menu/pause */
    }
}

bool game_ui_cursor_free(GameUiState s)  { return s != GAME_PLAYING; }
bool game_ui_world_active(GameUiState s) { return s == GAME_PLAYING; }

/* ------------------------------------------------------------------ */
/*  Options / settings: latched audio state + pure slider math         */
/* ------------------------------------------------------------------ */

static float g_opt_volume = 0.6f;   /* mirrors audio default; main.c keeps in sync */
static bool  g_opt_muted  = false;
static bool  g_opt_music  = true;

void hud_set_audio_state(float volume, bool muted, bool music)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    g_opt_volume = volume;
    g_opt_muted  = muted;
    g_opt_music  = music;
}

float hud_get_volume(void) { return g_opt_volume; }
bool  hud_get_muted(void)  { return g_opt_muted;  }
bool  hud_get_music(void)  { return g_opt_music;  }

bool hud_toggle_muted(void) { g_opt_muted = !g_opt_muted; return g_opt_muted; }
bool hud_toggle_music(void) { g_opt_music = !g_opt_music; return g_opt_music; }

/* Options-screen geometry. The volume slider sits at row 0 of the menu stack,
 * the mute/music toggles below it, then Back. */
#define OPT_SLIDER_W  BTN_W
#define OPT_SLIDER_H  18.0f

HudRect hud_volume_slider_rect(float sw, float sh)
{
    /* Centred horizontally, aligned with the first menu-button row but using a
     * thin slider track. */
    HudRect row = hud_menu_button_rect(0, sw, sh);
    float y = row.y + (row.h - OPT_SLIDER_H) * 0.5f;
    return (HudRect){ (sw - OPT_SLIDER_W) * 0.5f, y, OPT_SLIDER_W, OPT_SLIDER_H };
}

float hud_slider_value_from_x(HudRect track, float px)
{
    if (track.w <= 0.0f) return 0.0f;
    float v = (px - track.x) / track.w;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

float hud_slider_x_from_value(HudRect track, float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return track.x + track.w * v;
}

/* ------------------------------------------------------------------ */
/*  Layout helpers (pure)                                              */
/* ------------------------------------------------------------------ */

HudRect hud_menu_button_rect(int index, float sw, float sh)
{
    float x = (sw - BTN_W) * 0.5f;
    float y = sh * 0.5f + MENU_TOP_OFF + (float)index * (BTN_H + BTN_GAP);
    return (HudRect){ x, y, BTN_W, BTN_H };
}

HudRect hud_inventory_slot_rect(int i, float sw, float sh)
{
    int n = HUD_SLOT_COUNT;
    float total_w = n * INV_SLOT + (n - 1) * INV_SLOT_GAP;
    float x0 = (sw - total_w) * 0.5f;
    float y0 = sh * 0.5f - INV_SLOT * 0.5f;
    float x  = x0 + (float)i * (INV_SLOT + INV_SLOT_GAP);
    return (HudRect){ x, y0, INV_SLOT, INV_SLOT };
}

/* In-world hotbar geometry: a centred row anchored to the bottom edge. */
HudRect hud_hotbar_slot_rect(int i, float sw, float sh)
{
    int n = HUD_SLOT_COUNT;
    float total_w = n * SLOT_SIZE + (n - 1) * SLOT_GAP;
    float x0 = (sw - total_w) * 0.5f;
    float y0 = sh - SLOT_SIZE - 12.0f;
    float x  = x0 + (float)i * (SLOT_SIZE + SLOT_GAP);
    return (HudRect){ x, y0, (float)SLOT_SIZE, (float)SLOT_SIZE };
}

float hud_bar_fill(int value, int max)
{
    if (max <= 0)     return 0.0f;
    if (value <= 0)   return 0.0f;
    if (value >= max) return 1.0f;
    return (float)value / (float)max;
}

/* Crafting panel geometry. Rows sit below the inventory slot row. */
#define CRAFT_ROW_W   320.0f
#define CRAFT_ROW_H    30.0f
#define CRAFT_ROW_GAP   6.0f
#define CRAFT_TOP_OFF  70.0f   /* first row below the slot row's centre line */

HudRect hud_craft_row_rect(int i, float sw, float sh)
{
    float x = (sw - CRAFT_ROW_W) * 0.5f;
    float y0 = sh * 0.5f + CRAFT_TOP_OFF;
    float y  = y0 + (float)i * (CRAFT_ROW_H + CRAFT_ROW_GAP);
    return (HudRect){ x, y, CRAFT_ROW_W, CRAFT_ROW_H };
}

bool hud_rect_contains(HudRect r, float px, float py)
{
    return px >= r.x && px < r.x + r.w &&
           py >= r.y && py < r.y + r.h;
}

/* ------------------------------------------------------------------ */
/*  Container screen layout (pure)                                      */
/* ------------------------------------------------------------------ */

/* Slot size for the container grids; reuse the inventory-screen slot metrics so
 * the icons/counts draw identically. */
#define CON_SLOT      INV_SLOT
#define CON_SLOT_GAP  INV_SLOT_GAP

/* The container's own slots (chest grid / furnace cluster) occupy the upper
 * region; the player inventory row is drawn this far below the screen centre. */
#define CON_INV_ROW_OFF  90.0f

HudRect hud_container_inv_slot_rect(int i, float sw, float sh)
{
    int n = HUD_SLOT_COUNT;
    float total_w = n * CON_SLOT + (n - 1) * CON_SLOT_GAP;
    float x0 = (sw - total_w) * 0.5f;
    float y0 = sh * 0.5f + CON_INV_ROW_OFF;
    float x  = x0 + (float)i * (CON_SLOT + CON_SLOT_GAP);
    return (HudRect){ x, y0, CON_SLOT, CON_SLOT };
}

HudRect hud_chest_slot_rect(int i, float sw, float sh)
{
    if (i < 0) i = 0;
    int col = i % HUD_CHEST_COLS;
    int row = i / HUD_CHEST_COLS;
    int cols = HUD_CHEST_COLS;
    float total_w = cols * CON_SLOT + (cols - 1) * CON_SLOT_GAP;
    float x0 = (sw - total_w) * 0.5f;
    /* Three rows centred a little above the player inventory row. */
    float grid_h = 3 * CON_SLOT + 2 * CON_SLOT_GAP;
    float y0 = sh * 0.5f + CON_INV_ROW_OFF - 24.0f - grid_h;
    float x = x0 + (float)col * (CON_SLOT + CON_SLOT_GAP);
    float y = y0 + (float)row * (CON_SLOT + CON_SLOT_GAP);
    return (HudRect){ x, y, CON_SLOT, CON_SLOT };
}

HudRect hud_furnace_slot_rect(int i, float sw, float sh)
{
    /* Input over fuel on the left; output to the right, vertically between them.
     * The whole cluster is centred horizontally and sits above the player
     * inventory row. */
    float gap_v = 28.0f;          /* vertical gap input->fuel (room for arrow) */
    float gap_h = 120.0f;         /* horizontal gap input column -> output */
    float cluster_w = CON_SLOT + gap_h + CON_SLOT;
    float left_x = (sw - cluster_w) * 0.5f;
    float right_x = left_x + CON_SLOT + gap_h;
    float top_y = sh * 0.5f + CON_INV_ROW_OFF - 24.0f
                  - (2 * CON_SLOT + gap_v);
    switch (i) {
        case HUD_FURNACE_SLOT_INPUT:
            return (HudRect){ left_x, top_y, CON_SLOT, CON_SLOT };
        case HUD_FURNACE_SLOT_FUEL:
            return (HudRect){ left_x, top_y + CON_SLOT + gap_v, CON_SLOT, CON_SLOT };
        case HUD_FURNACE_SLOT_OUTPUT:
        default:
            return (HudRect){ right_x, top_y + (CON_SLOT + gap_v) * 0.5f,
                              CON_SLOT, CON_SLOT };
    }
}

float hud_furnace_progress_fill(int cook_progress, int ticks_per_item)
{
    return hud_bar_fill(cook_progress, ticks_per_item);
}

/* ------------------------------------------------------------------ */
/*  Screen drawing (overlays)                                          */
/* ------------------------------------------------------------------ */

/* A styled button: dark panel, lighter border, brighter when hovered. The
 * clickable region is registered by main.c (it owns the cursor); here we only
 * read ui_hovered_element() so the visuals track the cursor. */
static void draw_button(int id, HudRect r, const char* label)
{
    bool hot = (ui_hovered_element() == id);
    vec4 border_hot = {1.0f, 1.0f, 1.0f, 1.0f},  border_cold = {0.55f, 0.55f, 0.60f, 0.9f};
    vec4 fill_hot   = {0.30f, 0.32f, 0.38f, 0.95f}, fill_cold = {0.16f, 0.17f, 0.20f, 0.92f};
    float* border = hot ? border_hot : border_cold;
    float* fill   = hot ? fill_hot   : fill_cold;
    vec4 text   = {1.0f, 1.0f, 1.0f, 1.0f};

    ui_rect(r.x, r.y, r.w, r.h, border);
    ui_rect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, fill);

    float fs = 22.0f;
    float tw = ui_text_width(label, fs);
    ui_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - fs) * 0.5f + fs * 0.78f,
            fs, label, text);
}

/* Full-screen dim used behind modal overlays. */
static void draw_dim(float sw, float sh, float a)
{
    vec4 dim = {0.0f, 0.0f, 0.0f, a};
    ui_rect(0, 0, sw, sh, dim);
}

static void draw_title(const char* title, float sw, float sh)
{
    float fs = 64.0f;
    float tw = ui_text_width(title, fs);
    vec4 shadow = {0.0f, 0.0f, 0.0f, 0.6f};
    vec4 fg     = {0.95f, 0.85f, 0.35f, 1.0f};
    float x = (sw - tw) * 0.5f;
    float y = sh * 0.30f;
    ui_text(x + 3, y + 3, fs, title, shadow);
    ui_text(x,     y,     fs, title, fg);
}

static void draw_centered_label(const char* s, float fs, float sw, float y, vec4 col)
{
    float tw = ui_text_width(s, fs);
    ui_text((sw - tw) * 0.5f, y, fs, s, col);
}

static void hud_draw_main_menu(float sw, float sh)
{
    vec4 bg = {0.06f, 0.07f, 0.10f, 1.0f};
    ui_rect(0, 0, sw, sh, bg);
    draw_title("MINECRAFT", sw, sh);

    if (g_menu_page == MENU_PAGE_LOAD) {
        /* World list: one clickable row per saved world, then Back. */
        if (g_world_count == 0) {
            vec4 dim = {0.7f, 0.7f, 0.7f, 0.85f};
            draw_centered_label("No saved worlds yet", 20.0f, sw,
                                sh * 0.5f + MENU_TOP_OFF, dim);
            draw_button(HUD_ID_BACK, hud_menu_button_rect(1, sw, sh), "Back");
        } else {
            for (int i = 0; i < g_world_count; i++)
                draw_button(HUD_ID_WORLD0 + i,
                            hud_menu_button_rect(i, sw, sh), g_world_names[i]);
            draw_button(HUD_ID_BACK,
                        hud_menu_button_rect(g_world_count, sw, sh), "Back");
        }
        vec4 hint = {0.7f, 0.7f, 0.7f, 0.8f};
        draw_centered_label("Click a world to load it", 18.0f, sw,
                            sh - 40.0f, hint);
        return;
    }

    /* Root page. */
    draw_button(HUD_ID_NEW_WORLD,  hud_menu_button_rect(0, sw, sh), "New World");
    draw_button(HUD_ID_LOAD_WORLD, hud_menu_button_rect(1, sw, sh), "Load World");
    draw_button(HUD_ID_QUIT,       hud_menu_button_rect(2, sw, sh), "Quit");
    vec4 hint = {0.7f, 0.7f, 0.7f, 0.8f};
    draw_centered_label("New World to generate, Load World to continue", 18.0f,
                        sw, sh - 40.0f, hint);
}

static void hud_draw_pause(float sw, float sh)
{
    draw_dim(sw, sh, 0.55f);
    draw_title("PAUSED", sw, sh);
    draw_button(HUD_ID_RESUME,    hud_menu_button_rect(0, sw, sh), "Resume");
    draw_button(HUD_ID_OPTIONS,   hud_menu_button_rect(1, sw, sh), "Options");
    draw_button(HUD_ID_SAVE,      hud_menu_button_rect(2, sw, sh), "Save");
    draw_button(HUD_ID_SAVE_QUIT, hud_menu_button_rect(3, sw, sh), "Save & Quit");
    draw_button(HUD_ID_QUIT,      hud_menu_button_rect(4, sw, sh), "Quit");
    vec4 hint = {0.7f, 0.7f, 0.7f, 0.8f};
    draw_centered_label("Esc to resume", 18.0f, sw, sh - 40.0f, hint);
}

/* A labelled on/off toggle styled like a button. `on` drives the fill colour
 * and the trailing ON/OFF text. The clickable region is registered by main.c. */
static void draw_toggle(int id, HudRect r, const char* label, bool on)
{
    bool hot = (ui_hovered_element() == id);
    vec4 border = {0.55f, 0.55f, 0.60f, 0.9f};
    vec4 fill_on  = {0.20f, 0.40f, 0.24f, 0.95f};
    vec4 fill_off = {0.16f, 0.17f, 0.20f, 0.92f};
    vec4 fill_hot = {0.30f, 0.32f, 0.38f, 0.95f};
    vec4 text   = {1.0f, 1.0f, 1.0f, 1.0f};
    float* f = hot ? fill_hot : (on ? fill_on : fill_off);

    ui_rect(r.x, r.y, r.w, r.h, border);
    ui_rect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, f);

    float fs = 20.0f;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %s", label, on ? "ON" : "OFF");
    float tw = ui_text_width(buf, fs);
    ui_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - fs) * 0.5f + fs * 0.78f,
            fs, buf, text);
}

static void hud_draw_options(float sw, float sh)
{
    draw_dim(sw, sh, 0.55f);
    draw_title("OPTIONS", sw, sh);

    /* Master-volume slider (row 0). */
    HudRect track = hud_volume_slider_rect(sw, sh);
    vec4 track_bg = {0.16f, 0.17f, 0.20f, 0.95f};
    vec4 track_bd = {0.55f, 0.55f, 0.60f, 0.9f};
    vec4 fillc    = {0.35f, 0.55f, 0.85f, 1.0f};
    vec4 knobc    = {1.0f, 1.0f, 1.0f, 1.0f};
    vec4 text     = {1.0f, 1.0f, 1.0f, 1.0f};

    float v = hud_get_volume();
    ui_rect(track.x - 2, track.y - 2, track.w + 4, track.h + 4, track_bd);
    ui_rect(track.x, track.y, track.w, track.h, track_bg);
    ui_rect(track.x, track.y, track.w * v, track.h, fillc);
    float kx = hud_slider_x_from_value(track, v);
    ui_rect(kx - 4, track.y - 3, 8, track.h + 6, knobc);

    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "Volume: %d%%", (int)(v * 100.0f + 0.5f));
    ui_text(track.x, track.y - 10.0f, 18.0f, vbuf, text);

    /* Mute + music toggles (rows 1 and 2), then Back (row 3). */
    draw_toggle(HUD_ID_MUTE,  hud_menu_button_rect(1, sw, sh), "Mute",  hud_get_muted());
    draw_toggle(HUD_ID_MUSIC, hud_menu_button_rect(2, sw, sh), "Music", hud_get_music());
    draw_button(HUD_ID_BACK,  hud_menu_button_rect(3, sw, sh), "Back");

    vec4 hint = {0.7f, 0.7f, 0.7f, 0.8f};
    draw_centered_label("Esc or Back to return", 18.0f, sw, sh - 40.0f, hint);
}

static void hud_draw_inventory(const Inventory* inv, float sw, float sh)
{
    draw_dim(sw, sh, 0.45f);

    /* Backing panel sized around the slot row. */
    int n = HUD_SLOT_COUNT;
    float total_w = n * INV_SLOT + (n - 1) * INV_SLOT_GAP;
    float px = (sw - total_w) * 0.5f - 20.0f;
    float py = sh * 0.5f - INV_SLOT * 0.5f - 56.0f;
    float pw = total_w + 40.0f;
    float ph = INV_SLOT + 96.0f;
    vec4 panel_border = {0.55f, 0.55f, 0.60f, 0.95f};
    vec4 panel_fill   = {0.12f, 0.13f, 0.16f, 0.96f};
    ui_rect(px, py, pw, ph, panel_border);
    ui_rect(px + 3, py + 3, pw - 6, ph - 6, panel_fill);

    vec4 title = {0.95f, 0.95f, 0.95f, 1.0f};
    ui_text(px + 16, py + 28, 24.0f, "Inventory", title);

    vec4 slot_fill   = {0.20f, 0.20f, 0.22f, 1.0f};
    vec4 slot_sel    = {1.0f,  1.0f,  1.0f,  1.0f};
    vec4 slot_unsel  = {0.40f, 0.40f, 0.42f, 1.0f};
    vec4 slot_hot    = {1.0f,  0.95f, 0.55f, 1.0f};
    vec4 text_white  = {1.0f,  1.0f,  1.0f,  1.0f};

    for (int i = 0; i < n; i++) {
        HudRect r = hud_inventory_slot_rect(i, sw, sh);
        int id = HUD_ID_SLOT0 + i;
        bool hot = (ui_hovered_element() == id);
        bool sel = (inv && i == inv->selected);
        vec4* border = hot ? &slot_hot : (sel ? &slot_sel : &slot_unsel);
        ui_rect(r.x, r.y, r.w, r.h, *border);
        ui_rect(r.x + SLOT_BORDER, r.y + SLOT_BORDER,
                r.w - 2 * SLOT_BORDER, r.h - 2 * SLOT_BORDER, slot_fill);

        if (!inv) continue;
        const InventorySlot* s = &inv->slots[i];
        if (s->count == 0) continue;
        ui_block_icon(s->item, r.x + 8, r.y + 8, r.w - 16);
        if (s->count > 1) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", s->count);
            float tw = ui_text_width(buf, 14.0f);
            ui_text(r.x + r.w - tw - 4, r.y + r.h - 6, 14.0f, buf, text_white);
        }
    }

    /* --- Crafting panel: list the recipes the player can currently afford ---
     * The row order matches crafting_affordable() so the click target main.c
     * registers (HUD_ID_CRAFT0 + i -> that recipe index) lines up with the row
     * drawn here. */
    {
        ItemCounts counts;
        crafting_counts_from_inventory(inv, &counts);
        int idx[HUD_CRAFT_ROWS];
        int navail = crafting_affordable(&counts, idx, HUD_CRAFT_ROWS);

        vec4 ctitle = {0.95f, 0.95f, 0.95f, 1.0f};
        HudRect first = hud_craft_row_rect(0, sw, sh);
        ui_text(first.x, first.y - 12.0f, 20.0f, "Crafting", ctitle);

        if (navail == 0) {
            vec4 dim = {0.6f, 0.6f, 0.62f, 0.9f};
            ui_text(first.x, first.y + 16.0f, 15.0f,
                    "Gather materials to craft", dim);
        }

        vec4 row_fill = {0.18f, 0.19f, 0.22f, 0.96f};
        vec4 row_hot  = {0.30f, 0.32f, 0.40f, 0.98f};
        vec4 row_bord = {0.50f, 0.50f, 0.56f, 0.9f};
        vec4 row_text = {1.0f, 1.0f, 1.0f, 1.0f};
        vec4 out_text = {0.80f, 0.95f, 0.80f, 1.0f};

        for (int i = 0; i < navail; i++) {
            const Recipe* r = crafting_recipe(idx[i]);
            if (!r) continue;
            HudRect rr = hud_craft_row_rect(i, sw, sh);
            bool hot = (ui_hovered_element() == HUD_ID_CRAFT0 + i);
            ui_rect(rr.x, rr.y, rr.w, rr.h, row_bord);
            ui_rect(rr.x + 2, rr.y + 2, rr.w - 4, rr.h - 4,
                    hot ? row_hot : row_fill);

            /* Output icon on the left, recipe name, then "xN" yield. */
            ui_block_icon(r->output.item, rr.x + 4, rr.y + 3, rr.h - 6);
            ui_text(rr.x + rr.h + 4, rr.y + rr.h - 9, 16.0f, r->name, row_text);

            char yld[16];
            snprintf(yld, sizeof(yld), "x%d", r->output.count);
            float tw = ui_text_width(yld, 16.0f);
            ui_text(rr.x + rr.w - tw - 8, rr.y + rr.h - 9, 16.0f, yld, out_text);
        }
    }

    vec4 hint = {0.7f, 0.7f, 0.7f, 0.85f};
    draw_centered_label("E or Esc to close", 16.0f, sw, sh - 36.0f, hint);
}

/* ------------------------------------------------------------------ */
/*  Container screen drawing (furnace / chest)                          */
/* ------------------------------------------------------------------ */

/* Draw one item slot: border (hot when hovered), dark fill, and — if non-empty
 * — the item icon + count. Mirrors the inventory-screen slot visuals so the
 * container reuses the exact same icon/count path. */
static void hud_draw_slot(HudRect r, int id, ItemId item, int count)
{
    vec4 slot_fill  = {0.20f, 0.20f, 0.22f, 1.0f};
    vec4 slot_unsel = {0.40f, 0.40f, 0.42f, 1.0f};
    vec4 slot_hot   = {1.0f,  0.95f, 0.55f, 1.0f};
    vec4 text_white = {1.0f,  1.0f,  1.0f,  1.0f};

    bool hot = (ui_hovered_element() == id);
    vec4* border = hot ? &slot_hot : &slot_unsel;
    ui_rect(r.x, r.y, r.w, r.h, *border);
    ui_rect(r.x + SLOT_BORDER, r.y + SLOT_BORDER,
            r.w - 2 * SLOT_BORDER, r.h - 2 * SLOT_BORDER, slot_fill);

    if (count <= 0) return;
    ui_block_icon(item, r.x + 8, r.y + 8, r.w - 16);
    if (count > 1) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", count);
        float tw = ui_text_width(buf, 14.0f);
        ui_text(r.x + r.w - tw - 4, r.y + r.h - 6, 14.0f, buf, text_white);
    }
}

/* The player-inventory row shown at the bottom of the container screen. */
static void hud_draw_container_inventory(const Inventory* inv, float sw, float sh)
{
    for (int i = 0; i < HUD_SLOT_COUNT; i++) {
        HudRect r = hud_container_inv_slot_rect(i, sw, sh);
        const InventorySlot* s = inv ? &inv->slots[i] : NULL;
        hud_draw_slot(r, HUD_ID_CONINV0 + i,
                      s ? s->item : BLOCK_AIR, s ? (int)s->count : 0);
    }
}

void hud_draw_container(const void* con_v, const Inventory* inv,
                        float sw, float sh)
{
    const ContainerStatePacket* con = (const ContainerStatePacket*)con_v;
    if (!con) return;

    draw_dim(sw, sh, 0.45f);

    vec4 title       = {0.95f, 0.95f, 0.95f, 1.0f};
    vec4 label_dim   = {0.70f, 0.70f, 0.74f, 0.95f};

    if (con->type == CONTAINER_NET_CHEST) {
        HudRect g0 = hud_chest_slot_rect(0, sw, sh);
        ui_text(g0.x, g0.y - 12.0f, 24.0f, "Chest", title);
        for (int i = 0; i < HUD_CHEST_SLOTS; i++) {
            HudRect r = hud_chest_slot_rect(i, sw, sh);
            hud_draw_slot(r, HUD_ID_CON0 + i,
                          (ItemId)con->slots[i].item, (int)con->slots[i].count);
        }
    } else { /* furnace */
        HudRect in  = hud_furnace_slot_rect(HUD_FURNACE_SLOT_INPUT,  sw, sh);
        HudRect fu  = hud_furnace_slot_rect(HUD_FURNACE_SLOT_FUEL,   sw, sh);
        HudRect out = hud_furnace_slot_rect(HUD_FURNACE_SLOT_OUTPUT, sw, sh);

        ui_text(in.x, in.y - 16.0f, 24.0f, "Furnace", title);

        hud_draw_slot(in,  HUD_ID_CON0 + HUD_FURNACE_SLOT_INPUT,
                      (ItemId)con->f_input,  (int)con->f_input_count);
        hud_draw_slot(fu,  HUD_ID_CON0 + HUD_FURNACE_SLOT_FUEL,
                      (ItemId)con->f_fuel,   (int)con->f_fuel_count);
        hud_draw_slot(out, HUD_ID_CON0 + HUD_FURNACE_SLOT_OUTPUT,
                      (ItemId)con->f_output, (int)con->f_output_count);

        /* Cook-progress arrow between the input column and the output slot. */
        float arrow_x = in.x + in.w + 12.0f;
        float arrow_w = (out.x - 12.0f) - arrow_x;
        if (arrow_w < 8.0f) arrow_w = 8.0f;
        float arrow_y = out.y + out.h * 0.5f - 4.0f;
        vec4 track = {0.12f, 0.12f, 0.14f, 0.95f};
        vec4 fillc = {0.95f, 0.70f, 0.25f, 1.0f};
        ui_rect(arrow_x, arrow_y, arrow_w, 8.0f, track);
        float frac = hud_furnace_progress_fill((int)con->f_cook_progress,
                                               SMELT_TICKS_PER_ITEM);
        ui_rect(arrow_x, arrow_y, arrow_w * frac, 8.0f, fillc);

        /* Burn (fuel) indicator: a small flame bar under the fuel slot, lit
         * while the furnace is actively burning. */
        {
            float bx = fu.x, by = fu.y + fu.h + 4.0f;
            vec4 btrack = {0.12f, 0.12f, 0.14f, 0.95f};
            vec4 bfill  = {0.95f, 0.40f, 0.10f, 1.0f};
            ui_rect(bx, by, fu.w, 6.0f, btrack);
            if (con->f_burn_ticks_left > 0)
                ui_rect(bx, by, fu.w, 6.0f, bfill);
        }
    }

    /* Player inventory + a heading + hint, shared between both container types. */
    HudRect inv0 = hud_container_inv_slot_rect(0, sw, sh);
    ui_text(inv0.x, inv0.y - 10.0f, 18.0f, "Inventory", label_dim);
    hud_draw_container_inventory(inv, sw, sh);

    vec4 hint = {0.7f, 0.7f, 0.7f, 0.85f};
    draw_centered_label("Click a slot to move the stack  -  U or Esc to close",
                        16.0f, sw, sh - 36.0f, hint);
}

/* hud_build — accepts a NULL `inv`. The crosshair is always drawn (e.g., in
 * single-player / pre-connect runs before the first PKT_INVENTORY arrives);
 * the hotbar slots are only drawn when `inv` is non-NULL. */
/* Crosshair: four short ticks around a centre gap, each with a 1px dark
 * outline so it stays visible over bright and dark terrain alike. */
static void hud_draw_crosshair(float sw, float sh)
{
    vec4 fg     = {1.0f, 1.0f, 1.0f, 0.95f};
    vec4 shadow = {0.0f, 0.0f, 0.0f, 0.55f};
    float cx = sw * 0.5f, cy = sh * 0.5f;
    float g = CROSSHAIR_GAP, len = CROSSHAIR_W, t = CROSSHAIR_T;

    /* Each arm drawn as a shadow rect then the bright rect on top. */
    struct { float x, y, w, h; } arms[4] = {
        { cx + g,           cy - t * 0.5f, len, t },   /* right */
        { cx - g - len,     cy - t * 0.5f, len, t },   /* left  */
        { cx - t * 0.5f,    cy + g,        t,   len }, /* down  */
        { cx - t * 0.5f,    cy - g - len,  t,   len }, /* up    */
    };
    for (int i = 0; i < 4; i++)
        ui_rect(arms[i].x - 1, arms[i].y - 1, arms[i].w + 2, arms[i].h + 2, shadow);
    for (int i = 0; i < 4; i++)
        ui_rect(arms[i].x, arms[i].y, arms[i].w, arms[i].h, fg);
}

/* Red screen-edge vignette while a hurt flash is active. Four soft border bands
 * (top/bottom/left/right) whose alpha decays linearly from the latched start
 * over HUD_HURT_FLASH_SEC; once elapsed it draws nothing. Center stays clear so
 * play isn't obscured. */
static void hud_draw_hurt_flash(float sw, float sh)
{
    if (g_hurt_flash <= 0.0f) return;
    float a = g_hurt_flash * 0.45f;   /* peak alpha ~0.45 */
    vec4 red = { 0.75f, 0.05f, 0.05f, a };
    float band = sh * 0.18f;          /* edge band thickness */
    float bandx = sw * 0.13f;
    ui_rect(0,            0,            sw,    band, red);   /* top    */
    ui_rect(0,            sh - band,    sw,    band, red);   /* bottom */
    ui_rect(0,            0,            bandx, sh,   red);   /* left   */
    ui_rect(sw - bandx,   0,            bandx, sh,   red);   /* right  */
}

/* Persistent low-health vignette: a pulsing red screen-edge frame while health
 * is at/under the threshold. Distinct from the transient hurt flash — this one
 * stays up and breathes (via the hud_tick clock) the whole time the player is
 * low, and its strength scales as health drops toward 0. Center stays clear. */
static void hud_draw_low_health(int player_health, float sw, float sh)
{
    float intensity = hud_low_health_intensity(player_health, SURVIVAL_MAX_HEALTH);
    if (intensity <= 0.0f) return;
    /* Pulse between ~55% and 100% of the intensity-scaled peak so it never
     * fully vanishes while low (always a visible warning) but still breathes. */
    float pulse = 0.55f + 0.45f * hud_pulse_factor(g_hud_time);
    float a = intensity * pulse * 0.5f;   /* peak alpha ~0.5 at 0 HP */
    vec4 red = { 0.7f, 0.0f, 0.0f, a };
    float band  = sh * 0.20f;
    float bandx = sw * 0.15f;
    ui_rect(0,          0,         sw,    band, red);   /* top    */
    ui_rect(0,          sh - band, sw,    band, red);   /* bottom */
    ui_rect(0,          0,         bandx, sh,   red);   /* left   */
    ui_rect(sw - bandx, 0,         bandx, sh,   red);   /* right  */
}

/* Underwater tint: a flat blue overlay across the whole screen while the
 * player's eyes are submerged (latched via hud_set_underwater from main.c). */
static void hud_draw_underwater(float sw, float sh)
{
    if (!g_hud_underwater) return;
    vec4 blue = { 0.06f, 0.22f, 0.45f, 0.30f };
    ui_rect(0, 0, sw, sh, blue);
}

/* Small eating-progress nibble: a thin bar centred just above the hotbar that
 * fills as the eat hold completes. Drawn only while actively eating (progress in
 * (0,1)); the full bar disappears the moment the eat fires. */
static void hud_draw_eat_progress(float sw, float sh)
{
    if (g_eat_progress <= 0.0f) return;
    float w = 120.0f, h = 6.0f;
    float x = (sw - w) * 0.5f;
    HudRect h0 = hud_hotbar_slot_rect(0, sw, sh);
    float y = h0.y - 18.0f;
    vec4 bg   = { 0.05f, 0.05f, 0.05f, 0.7f };
    vec4 fill = { 0.55f, 0.85f, 0.35f, 0.95f };   /* food green */
    ui_rect(x - 1, y - 1, w + 2, h + 2, bg);
    ui_rect(x, y, w * g_eat_progress, h, fill);
}

void hud_build(const Inventory* inv, int player_health, float sw, float sh)
{
    /* Screen dispatch: menus/overlays are driven by the latched game-UI state
     * (set from main.c). The renderer calls hud_build unconditionally, so this
     * is where the main menu / pause / inventory screens compose on top of (or
     * instead of) the in-world HUD. */
    GameUiState screen = hud_get_screen();

    if (screen == GAME_MAIN_MENU) {
        hud_draw_main_menu(sw, sh);
        return;
    }

    /* In-world HUD (crosshair + hotbar + bars) for PLAYING / PAUSED /
     * INVENTORY. The crosshair is hidden when a modal overlay is up so it
     * doesn't sit under the dim. */
    if (screen == GAME_PLAYING) {
        hud_draw_crosshair(sw, sh);
        /* Underwater tint sits behind the damage vignettes so red hurt cues
         * still read clearly through the blue while submerged. */
        hud_draw_underwater(sw, sh);
        hud_draw_low_health(player_health, sw, sh);
        hud_draw_hurt_flash(sw, sh);
        hud_draw_eat_progress(sw, sh);
    }

    if (inv) {

    /* Hotbar layout (geometry from the shared pure helper so the click/icon/
     * highlight all line up). */
    int n = HUD_SLOT_COUNT;
    HudRect h0 = hud_hotbar_slot_rect(0, sw, sh);
    HudRect hL = hud_hotbar_slot_rect(n - 1, sw, sh);
    float hy = h0.y;

    vec4 fill         = {0.15f, 0.15f, 0.15f, 0.75f};
    vec4 panel_bg     = {0.04f, 0.04f, 0.05f, 0.55f};
    vec4 border_sel   = {1.0f,  0.95f, 0.35f, 1.0f};   /* warm accent on selection */
    vec4 sel_glow     = {1.0f,  0.95f, 0.35f, 0.22f};  /* soft halo behind selected slot */
    vec4 border_unsel = {0.4f,  0.4f,  0.4f,  0.75f};
    vec4 text_white   = {1.0f,  1.0f,  1.0f,  1.0f};
    vec4 text_shadow  = {0.0f,  0.0f,  0.0f,  0.85f};

    /* Backing strip behind the whole row so slots read clearly over terrain. */
    {
        float pad = 4.0f;
        ui_rect(h0.x - pad, hy - pad,
                (hL.x + hL.w) - h0.x + 2 * pad, h0.h + 2 * pad, panel_bg);
    }

    for (int i = 0; i < n; i++) {
        HudRect r = hud_hotbar_slot_rect(i, sw, sh);
        bool sel = (i == inv->selected);

        /* Selected slot gets a soft halo behind it + a thicker bright border so
         * the active slot is unmistakable at a glance. */
        if (sel) {
            float g = 3.0f;
            ui_rect(r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g, sel_glow);
        }
        int bw = sel ? (SLOT_BORDER + 1) : SLOT_BORDER;
        vec4* border = sel ? &border_sel : &border_unsel;
        ui_rect(r.x, r.y, r.w, r.h, *border);
        ui_rect(r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw, fill);

        const InventorySlot* s = &inv->slots[i];
        if (s->count == 0) continue;

        /* Item icon (samples the atlas tile for the slot's ItemId — same path the
         * inventory screen uses), inset by 4 pixels. */
        ui_block_icon(s->item, r.x + 4, r.y + 4, r.w - 8);

        /* Count, only if > 1, bottom-right of slot (shadowed for legibility). */
        if (s->count > 1) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", s->count);
            float tw = ui_text_width(buf, 12.0f);
            float tx = r.x + r.w - tw - 3, ty = r.y + r.h - 14;
            ui_text(tx + 1, ty + 1, 12.0f, buf, text_shadow);
            ui_text(tx, ty, 12.0f, buf, text_white);
        }
    }

    /* Health hearts — 10 hearts = 20 hp, drawn above the hotbar. */
    if (player_health >= 0) {
        const float HSZ = 16.0f, HGAP = 2.0f;
        int hearts = 10;
        float total = hearts * HSZ + (hearts - 1) * HGAP;
        float hx0 = (sw - total) * 0.5f;
        float hy0 = sh - SLOT_SIZE - 12.0f - HSZ - 8.0f;
        vec4 full  = {0.85f, 0.10f, 0.10f, 1.0f};
        vec4 half  = {0.85f, 0.10f, 0.10f, 1.0f};
        vec4 empty = {0.20f, 0.20f, 0.20f, 0.6f};

        /* Armour bar — 10 icons = 20 points, drawn one row above the hearts,
         * left-aligned with them. Each icon = 2 armour points (half icon = 1).
         * Hidden when the player wears no armour. */
        if (g_hud_armor_points > 0) {
            int icons = 10;
            float ay0 = hy0 - HSZ - 4.0f;
            vec4 afull  = {0.78f, 0.80f, 0.88f, 1.0f};  /* steel grey */
            vec4 aempty = {0.20f, 0.20f, 0.20f, 0.6f};
            for (int i = 0; i < icons; i++) {
                float x = hx0 + i * (HSZ + HGAP);
                int p_here = g_hud_armor_points - i * 2;   /* 2 points per icon */
                ui_rect(x, ay0, HSZ, HSZ, aempty);
                if (p_here >= 2)      ui_rect(x, ay0, HSZ, HSZ, afull);
                else if (p_here == 1) ui_rect(x, ay0, HSZ * 0.5f, HSZ, afull);
            }
        }

        for (int i = 0; i < hearts; i++) {
            float x = hx0 + i * (HSZ + HGAP);
            int hp_here = player_health - i * 2;   /* 2 hp per heart */
            ui_rect(x, hy0, HSZ, HSZ, empty);
            if (hp_here >= 2)      ui_rect(x, hy0, HSZ, HSZ, full);
            else if (hp_here == 1) ui_rect(x, hy0, HSZ * 0.5f, HSZ, half);
        }
        (void)half;

        /* Continuous health gauge directly under the hearts: a dark track with a
         * red fill proportional to hp/20. Reinforces the per-heart readout with a
         * single clear bar (fill fraction from the pure hud_bar_fill helper). */
        {
            const float BAR_H = 3.0f, BAR_PAD = 2.0f;
            float by = hy0 + HSZ + BAR_PAD;
            vec4 track = {0.10f, 0.10f, 0.10f, 0.7f};
            vec4 fillh = {0.85f, 0.10f, 0.10f, 1.0f};
            float frac = hud_bar_fill(player_health, 20);
            ui_rect(hx0, by, total, BAR_H, track);
            ui_rect(hx0, by, total * frac, BAR_H, fillh);
        }

        /* Hunger bar — 10 drumsticks = 20 food, mirrored on the right edge of
         * the heart row, filling from the right inward (vanilla layout). */
        {
            int drum = 10;
            float total_d = drum * HSZ + (drum - 1) * HGAP;
            float dx0 = (sw + total) * 0.5f - total_d;   /* right of centre */
            if (dx0 < hx0 + total + HGAP) dx0 = hx0 + total + HGAP;
            vec4 dfull  = {0.55f, 0.35f, 0.10f, 1.0f};   /* drumstick brown */
            vec4 dhalf  = {0.55f, 0.35f, 0.10f, 1.0f};
            vec4 dempty = {0.20f, 0.20f, 0.20f, 0.6f};
            for (int i = 0; i < drum; i++) {
                /* Fill from the right: rightmost drumstick = first food. */
                float x = dx0 + (drum - 1 - i) * (HSZ + HGAP);
                int f_here = g_hud_food - i * 2;
                ui_rect(x, hy0, HSZ, HSZ, dempty);
                if (f_here >= 2)      ui_rect(x, hy0, HSZ, HSZ, dfull);
                else if (f_here == 1) ui_rect(x + HSZ * 0.5f, hy0, HSZ * 0.5f, HSZ, dhalf);
            }
            (void)dhalf;

            /* Continuous hunger gauge under the drumsticks, filling from the
             * right to mirror the icon row. Fraction from hud_bar_fill. */
            const float BAR_H = 3.0f, BAR_PAD = 2.0f;
            float by = hy0 + HSZ + BAR_PAD;
            vec4 track = {0.10f, 0.10f, 0.10f, 0.7f};
            vec4 fillf = {0.55f, 0.35f, 0.10f, 1.0f};
            float frac = hud_bar_fill(g_hud_food, 20);
            ui_rect(dx0, by, total_d, BAR_H, track);
            float fw = total_d * frac;
            ui_rect(dx0 + (total_d - fw), by, fw, BAR_H, fillf);
        }

        /* Oxygen bubbles — only while underwater (air < full). 10 bubbles = 20.
         * Drawn one row above the hunger bar, filling from the right. */
        if (g_hud_air < 20) {
            int bub = 10;
            float total_b = bub * HSZ + (bub - 1) * HGAP;
            float bx0 = (sw + total) * 0.5f - total_b;
            float by0 = hy0 - HSZ - 4.0f;
            vec4 bfull = {0.30f, 0.55f, 0.95f, 1.0f};
            for (int i = 0; i < bub; i++) {
                int a_here = g_hud_air - i * 2;
                if (a_here >= 1) {
                    float x = bx0 + (bub - 1 - i) * (HSZ + HGAP);
                    ui_rect(x, by0, HSZ, HSZ, bfull);
                }
            }
        }
    }
    }  /* end if (inv) */

    /* Modal overlays drawn last so they sit above the in-world HUD. */
    if (screen == GAME_PAUSED)
        hud_draw_pause(sw, sh);
    else if (screen == GAME_OPTIONS)
        hud_draw_options(sw, sh);
    else if (screen == GAME_INVENTORY)
        hud_draw_inventory(inv, sw, sh);
    else if (screen == GAME_CONTAINER && g_hud_container_valid)
        hud_draw_container(&g_hud_container, inv, sw, sh);
}

BlockID hud_selected_block(const Inventory* inv)
{
    /* The selected item is a block only when it's in the block id range; a
     * held tool isn't placeable, so report air. */
    return item_as_block(inv->slots[inv->selected].item);
}

/* Performance overlay — top-left stack of small text lines on a dark panel.
 * Uses the pure PerfStats helpers (see hud.h) for the FPS/frametime math. */
void hud_draw_stats(const PerfStats* p, float sw, float sh)
{
    (void)sw; (void)sh;
    if (!p) return;

    const float FS    = 16.0f;   /* font size  */
    const float LINE  = 18.0f;   /* line height */
    const float PAD   = 6.0f;
    const float X0    = 8.0f;
    const float Y0    = 8.0f;

    char l0[48], l1[48], l2[64], l3[48];
    snprintf(l0, sizeof(l0), "FPS %.0f  (%.2f ms)",
             perf_stats_avg_fps(p), perf_stats_avg_frametime_ms(p));
    snprintf(l1, sizeof(l1), "Chunks %u", p->visible_chunks);
    /* Frustum-cull effectiveness: how many candidate chunks were skipped. */
    float cull_pct = p->total_chunks
        ? (100.0f * (float)p->culled_chunks / (float)p->total_chunks) : 0.0f;
    snprintf(l2, sizeof(l2), "Culled %u/%u (%.0f%%)",
             p->culled_chunks, p->total_chunks, cull_pct);
    snprintf(l3, sizeof(l3), "Draws %u", p->draw_calls);

    const char* lines[4] = { l0, l1, l2, l3 };
    int nlines = 4;

    /* Panel sized to the widest line. */
    float maxw = 0.0f;
    for (int i = 0; i < nlines; i++) {
        float w = ui_text_width(lines[i], FS);
        if (w > maxw) maxw = w;
    }
    vec4 panel = {0.0f, 0.0f, 0.0f, 0.5f};
    ui_rect(X0 - PAD, Y0 - PAD,
            maxw + 2 * PAD, nlines * LINE + 2 * PAD - (LINE - FS), panel);

    vec4 fg = {1.0f, 1.0f, 0.4f, 1.0f};
    for (int i = 0; i < nlines; i++)
        ui_text(X0, Y0 + i * LINE, FS, lines[i], fg);
}
