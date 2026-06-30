#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"
#include "ui/ui.h"
#include "player.h"
#include "world.h"
#include "block_physics.h"
#include "chunk_mesh.h"
#include "worldgen.h"
#include "agent.h"
#include "ui/hud.h"
#include "net.h"
#include "net_thread.h"
#include "server.h"
#include "client.h"
#include "remote_player.h"
#include "player_model.h"
#include "particle.h"
#include "mob_render.h"
#include "platform_thread.h"
#include "raycast.h"
#include "physics.h"     /* physics_move — collision-resolve the knockback nudge */
#include "gameplay.h"
#include "inventory.h"
#include "crafting.h"
#include "combat.h"      /* EAT_DURATION_SEC + COMBAT_KNOCKBACK_DECAY */
#include "daynight.h"
#include "worldsave.h"
#include "audio.h"
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif
#include <time.h>

#define WORLD_SEED 420

static Player  g_player;
static Client* g_client = NULL;   /* set in main() so callbacks can reach it */
static RaycastHit g_target;       /* refreshed each frame for outline + click */
static uint16_t g_target_mob = 0; /* nearest mob under the crosshair, 0 = none */
/* Live client-side mob set; declared here (before the input callbacks) so the
 * attack handler can look up the struck mob's position for the hit spark. Set
 * in main() once the client is up. */
static ClientMobSet*    g_mobs = NULL;
static bool g_show_stats = false; /* perf overlay visibility; toggled with F3 */

/* Hold-to-eat state. Eating is no longer instant: while the eat key (F) is held
 * over a food item, g_eating accumulates toward EAT_DURATION_SEC. The SFX plays
 * at the start of the hold; PKT_EAT is sent once the hold completes (the server
 * still authoritatively validates + applies). g_eat_slot records which hotbar
 * slot the hold started on so swapping mid-chew restarts the eat. */
static bool  g_eating       = false;
static float g_eat_timer    = 0.0f;
static int   g_eat_slot     = -1;
/* Local knockback residual velocity (blocks/s), drained from PKT_KNOCKBACK and
 * applied as a short decaying positional nudge each frame (player_update
 * overwrites the player's own horizontal velocity, so it can't live there). */
static float g_kb_vx = 0.0f, g_kb_vy = 0.0f, g_kb_vz = 0.0f;

/* Crafting-panel row -> recipe-index map, rebuilt each frame the inventory
 * screen registers its hit-test rects so a click resolves to the right recipe.
 * g_craft_count rows are valid in g_craft_idx[]. */
static int g_craft_idx[HUD_CRAFT_ROWS];
static int g_craft_count = 0;

/* Block coordinates of the container the player is currently viewing. Latched
 * when a PKT_CONTAINER_OPEN is sent so subsequent slot-click actions + the
 * close packet target the same block even if the look-target moves. Valid only
 * while g_client->container_open. */
static int g_container_x, g_container_y, g_container_z;

/* Headless-harness position hold (fn8). The shared-world headless sim runs the
 * player's physics LOCALLY and reports it to the authoritative server, so any
 * residual horizontal velocity/inertia makes the reported position drift over
 * ticks — which races the server's reach validation and makes place/break/open
 * non-reproducible. When `g_harness_hold` is set, every sim tick pins the
 * player's XZ to the held position and zeros horizontal velocity AFTER the
 * physics step. Vertical motion is left to gravity, so a player tp'd onto solid
 * ground rests in place (reach-stable) while one tp'd into the air still falls
 * (fall-damage scenarios work). Movement verbs (move/jump) release the hold;
 * `tp` re-asserts it at the new position. */
static bool  g_harness_hold = true;
static float g_harness_hold_x, g_harness_hold_z;

/* Game-UI state machine. Starts at the main menu; Play enters PLAYING, Esc
 * toggles the pause overlay, E toggles the inventory screen. The cursor is
 * captured only in PLAYING (mouselook); freed in every menu/overlay. */
static GameUiState g_ui_state = GAME_MAIN_MENU;
static GLFWwindow* g_window = NULL;
static bool g_agent_mode_active = false;

/* Apply the cursor-capture mode that matches the current UI state. Captured
 * (disabled) for PLAYING mouselook; normal (visible) for menus/overlays. */
static void ui_apply_cursor(GLFWwindow* w)
{
    if (g_agent_mode_active) return;   /* agent mode never grabs the cursor */
    glfwSetInputMode(w, GLFW_CURSOR,
        game_ui_cursor_free(g_ui_state) ? GLFW_CURSOR_NORMAL
                                        : GLFW_CURSOR_DISABLED);
}

/* Switch UI state and reflect it in the cursor + the HUD's latched screen. */
static void ui_set_state(GameUiState s)
{
    /* Re-entering mouselook: discard the first delta so the camera doesn't
     * snap by the distance the free cursor travelled while a menu was open. */
    if (s == GAME_PLAYING && g_ui_state != GAME_PLAYING)
        g_player.camera.first_mouse = true;
    g_ui_state = s;
    hud_set_screen(s);
    if (g_window) ui_apply_cursor(g_window);
}

/* Timed-mining state. While the break button is held on a block we accumulate
 * elapsed seconds; the break packet is sent once it reaches the block's
 * block_break_time. Progress resets when the target cell changes or the button
 * releases. -1 cell coords mean "no block currently being mined". */
static float g_mining_progress = 0.0f;
static int   g_mining_x = INT32_MIN, g_mining_y = INT32_MIN, g_mining_z = INT32_MIN;

/* Footstep cadence (63k): accumulate horizontal ground-distance walked and emit
 * a footstep every FOOTSTEP_STRIDE metres. step_seq varies the per-step gain. */
static float    g_footstep_accum = 0.0f;
static uint64_t g_footstep_seq   = 0;

static void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    (void)w; (void)xoff;
    if (!game_ui_world_active(g_ui_state)) return;   /* hotbar scroll only in-world */
    int dir = (yoff > 0) ? -1 : 1;
    if (!g_client) return;
    g_client->inventory.selected =
        (g_client->inventory.selected + dir + INVENTORY_SLOTS) % INVENTORY_SLOTS;
}

static void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    (void)window;
    /* Only drive mouselook while playing; in menus/overlays the cursor is free
     * for hit-testing and must not yank the camera. */
    if (!game_ui_world_active(g_ui_state)) return;
    camera_process_mouse(&g_player.camera, xpos, ypos);
}

static void mouse_button_callback(GLFWwindow* w, int button, int action, int mods)
{
    (void)w; (void)mods;
    if (action != GLFW_PRESS) return;
    if (!game_ui_world_active(g_ui_state)) return;  /* menu clicks handled in loop */
    if (!g_client) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        /* Every left-click is an attack swing: play the held-item swing
         * animation + a quick whoosh immediately, regardless of whether it
         * connects. (a4s.3.7) */
        hud_trigger_swing();
        audio_play(SFX_SWING);

        if (!g_target.hit && !g_target_mob) return;  /* swung at nothing */

        /* Mob melee stays a single click. Block breaking is now timed: the
         * held-button progress is accumulated each frame in the main loop and
         * the PKT_BLOCK_BREAK is sent only when mining completes. */
        if (g_target_mob) {
            client_send_mob_attack(g_client, g_target_mob);
            /* Immediate attacker-side feedback on a landed melee hit: a sharp
             * connect "thwack" plus the mob's hurt grunt. The server stays
             * authoritative over damage/death; this is local feel only. */
            audio_play(SFX_HIT);
            audio_play(SFX_MOB_HURT);

            /* Hit spark: a small particle burst at the struck mob's position,
             * emitted into the client's own particle pool (the renderer already
             * uploads it each frame). Subtle attacker-side feedback only. */
            if (g_mobs) {
                for (int i = 0; i < MOB_MAX; i++) {
                    ClientMob* m = &g_mobs->mobs[i];
                    if (!m->active || m->id != g_target_mob) continue;
                    /* Latest snapshot position (same field mob_ray_hit tests),
                     * biased up toward chest height for the spark. */
                    float hx = m->positions[1][0];
                    float hy = m->positions[1][1] + 1.0f;
                    float hz = m->positions[1][2];
                    particle_emit_block_break(&g_client->particles,
                                              hx, hy, hz,
                                              0.95f, 0.85f, 0.30f);
                    break;
                }
            }
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (!g_target.hit) return;  /* nothing to place against */
        client_send_place(g_client,
                          g_target.x, g_target.y, g_target.z,
                          (uint8_t)g_target.face,
                          (uint8_t)g_client->inventory.selected);
    }
}

static void key_callback(GLFWwindow* window, int key, int scancode,
                          int action, int mods)
{
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        /* Esc closes an open container first (sending the close packet); the
         * main-loop reconcile then drops the GAME_CONTAINER screen. Otherwise it
         * toggles pause from in-world and closes the inventory; in the main menu
         * it does nothing (use the Quit button to leave). */
        if (g_client && g_client->container_open) {
            client_send_container_close(g_client, g_container_x,
                                        g_container_y, g_container_z);
        } else {
            ui_set_state(game_ui_toggle_pause(g_ui_state));
        }
    }
    if (key == GLFW_KEY_E && action == GLFW_PRESS) {
        /* E opens/closes the inventory screen (no-op in menu/pause). */
        ui_set_state(game_ui_toggle_inventory(g_ui_state));
    }
    /* F is now hold-to-eat: the PRESS arms an eat hold and the main loop drives
     * the timer (reading the held key) + sends PKT_EAT only once the hold
     * completes. The press itself just plays the eat SFX and starts the timer so
     * a quick tap does nothing. Actual consume is server-authoritative. */
    if (key == GLFW_KEY_F && action == GLFW_PRESS) {
        if (g_client && game_ui_world_active(g_ui_state)) {
            int slot = g_client->inventory.selected;
            ItemId held = g_client->inventory.slots[slot].item;
            if (g_client->inventory.slots[slot].count > 0 && item_is_food(held)) {
                g_eating    = true;
                g_eat_timer = 0.0f;
                g_eat_slot  = slot;
                audio_play(SFX_EAT);   /* chewing cue at the start of the hold */
            }
        }
    }
    if (key == GLFW_KEY_U && action == GLFW_PRESS) {
        /* U opens (or, if already open, closes) the container block-entity the
         * player is looking at — furnace/chest. Opening sends PKT_CONTAINER_OPEN
         * (the server marks us a viewer + streams CONTAINER_STATE into
         * g_client->container); the main-loop reconcile then raises the
         * GAME_CONTAINER screen. The server no-ops the open if the aimed cell
         * isn't a tracked container, so it is safe to send for any block. */
        if (g_client && g_client->container_open) {
            /* Close the container we have open (coords latched at open time). */
            client_send_container_close(g_client, g_container_x,
                                        g_container_y, g_container_z);
        } else if (g_client && game_ui_world_active(g_ui_state) && g_target.hit) {
            g_container_x = g_target.x;
            g_container_y = g_target.y;
            g_container_z = g_target.z;
            client_send_container_open(g_client, g_target.x,
                                       g_target.y, g_target.z);
        }
    }
    if (key == GLFW_KEY_F3 && action == GLFW_PRESS)
        g_show_stats = !g_show_stats;   /* toggle the perf overlay */
    if (key == GLFW_KEY_M && action == GLFW_PRESS) {
        /* M is a global mute hotkey. Flip the audio engine's mute and mirror the
         * new state into the HUD so the Options screen reflects it. */
        bool m = !audio_get_muted();
        audio_set_muted(m);
        hud_set_audio_state(audio_get_master_volume(), m, hud_get_music());
    }
}

static bool apply_agent_command(const AgentCommand *cmd, Player *player,
                                 Renderer *renderer,
                                 bool *dump_frame, char *dump_path)
{
    (void)renderer;
    switch (cmd->type) {
    case CMD_MOVE:
        player->agent_forward = cmd->move.forward;
        player->agent_right   = cmd->move.right;
        break;
    case CMD_LOOK: {
        float yaw_rad   = cmd->look.yaw   * (3.14159265f / 180.0f);
        float pitch_rad = cmd->look.pitch * (3.14159265f / 180.0f);
        player->camera.yaw   = yaw_rad;
        player->camera.pitch = pitch_rad;
        break;
    }
    case CMD_JUMP:
        player->agent_jump = true;
        break;
    case CMD_SPRINT:
        player->agent_sprint = (cmd->sprint.active != 0);
        break;
    case CMD_SELECT_SLOT: {
        int s = cmd->select_slot.slot;
        if (s >= 0 && s < INVENTORY_SLOTS && g_client)
            g_client->inventory.selected = s;
        break;
    }
    case CMD_MODE:
        player->mode = (cmd->mode.mode == 0) ? MODE_FREE : MODE_WALKING;
        glm_vec3_zero(player->velocity);
        player->on_ground = false;
        player->in_water  = false;
        break;
    case CMD_GET_STATE:
        break;
    case CMD_DUMP_FRAME:
        *dump_frame = true;
        strncpy(dump_path, cmd->dump_frame.path, 255);
        dump_path[255] = '\0';
        break;
    case CMD_QUIT:
        return false;
    }
    return true;
}

static RemotePlayerSet* g_remote_players = NULL;
static Player*          g_player_ptr     = NULL;
static vec3             g_spawn_pos      = {0, 0, 0};
static World*           g_net_world      = NULL;  /* remote client's network-fed world */

/* Server streamed us a chunk: insert its blocks into the local network-fed
 * world; world_update will light + mesh it. Runs on the main thread (client_poll
 * is called from the render loop), so touching the world is safe. */
static void on_chunk_data(int32_t cx, int32_t cz, const uint8_t* blocks,
                          size_t blocks_len, void* user) {
    (void)user;
    if (g_net_world) world_insert_network_chunk(g_net_world, cx, cz, blocks, blocks_len);
}

/* Server told us a chunk left range: evict it (frees its GPU mesh). */
static void on_chunk_unload(int32_t cx, int32_t cz, void* user) {
    (void)user;
    if (g_net_world) world_evict_chunk(g_net_world, cx, cz);
}

static void on_death(void* user) {
    (void)user;
    if (g_player_ptr) {
        glm_vec3_copy(g_spawn_pos, g_player_ptr->position);
        glm_vec3_zero(g_player_ptr->velocity);
        g_player_ptr->on_ground = false;
    }
}

static void on_mobs(const ClientMobSnapshot* mobs, int count, double recv_time, void* user)
{
    (void)user;
    if (g_mobs) client_mob_set_apply(g_mobs, mobs, count, recv_time);
}

static void on_snapshot(const ClientPlayerSnapshot* s, void* user)
{
    (void)user;
    if (g_remote_players)
        remote_player_push_snapshot(g_remote_players,
            s->player_id, s->x, s->y, s->z,
            s->yaw, s->pitch, s->recv_time);
}

static void on_player_leave(uint8_t pid, void* user)
{
    (void)user;
    if (g_remote_players)
        remote_player_remove(g_remote_players, pid);
}

typedef struct {
    uint16_t  port;
    int       max;
    int       seed;
    char      save_path[WORLDSAVE_PATH_MAX];
    Renderer* renderer;        /* host shared-world: server creates the world
                                * with this renderer; NULL -> headless server   */
    int       render_distance; /* client render distance for the shared world   */
    GameMode  gamemode;        /* world's mode; gates all player damage server-side */
} ServerArgs;
static void* server_thread_func(void* arg)
{
    ServerArgs* a = (ServerArgs*)arg;
    server_run_ex(a->port, a->max, a->seed, a->save_path,
                  a->renderer, a->render_distance, a->gamemode);
    free(a);
    return NULL;
}

/* Forward decl: headless test harness (no glfw/renderer/window). Defined below
 * main(); runs the integrated server + a client SIMULATION driven by agent JSON
 * commands with a deterministic fixed-dt `step`. */
static int run_headless_harness(uint16_t port);

int main(int argc, char *argv[])
{
    bool agent_mode   = false;
    bool headless_mode = false;
    bool server_mode  = false;
    bool host_mode    = false;
    bool client_mode  = false;
    const char* connect_ip = "127.0.0.1";
    uint16_t    port       = NET_DEFAULT_PORT;
    /* Headless visual capture: render screenshot_frame frames (to let nearby
     * chunks stream in + mesh), dump one PNG, then exit cleanly. Lets automated
     * tooling/CI catch rendering regressions in a single self-terminating run. */
    const char* screenshot_path  = NULL;
    int         screenshot_frame = 240;

    /* Graphics/perf settings, defaulting to values that run well on
     * integrated GPUs. Override via CLI flags below. */
    RenderSettings gfx = render_settings_default();

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--agent")  == 0) agent_mode  = true;
        else if (strcmp(argv[i], "--headless") == 0) headless_mode = true;
        else if (strcmp(argv[i], "--harness")  == 0) { agent_mode = true; headless_mode = true; }
        else if (strcmp(argv[i], "--server") == 0) server_mode = true;
        else if (strcmp(argv[i], "--host")   == 0) host_mode   = true;
        else if (strcmp(argv[i], "--client") == 0) {
            client_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                connect_ip = argv[++i];
        }
        else if (strcmp(argv[i], "--render-distance") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v < 1)  v = 1;
            if (v > 64) v = 64;   /* keep the loading expected-chunk math sane */
            gfx.render_distance = v;
        }
        else if (strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
            gfx.msaa = atoi(argv[++i]);   /* clamped to device caps at init */
        }
        else if (strcmp(argv[i], "--aniso") == 0 && i + 1 < argc) {
            gfx.aniso = atoi(argv[++i]);  /* clamped to device caps at init */
        }
        else if (strcmp(argv[i], "--render-scale") == 0 && i + 1 < argc) {
            /* 3D-scene resolution factor; clamped to 0.25..1.0 at renderer init.
             * 1.0 = full res (legacy path); below 1.0 downsamples the world and
             * upscales to the window — a fill-rate win on integrated GPUs. */
            gfx.render_scale = (float)atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--present") == 0 && i + 1 < argc) {
            const char* m = argv[++i]; /* falls back to FIFO at init if unsupported */
            if      (strcmp(m, "mailbox")   == 0) gfx.present = PRESENT_PREF_MAILBOX;
            else if (strcmp(m, "immediate") == 0) gfx.present = PRESENT_PREF_IMMEDIATE;
            else                                  gfx.present = PRESENT_PREF_FIFO;
        }
        else if (strcmp(argv[i], "--stats") == 0) {
            g_show_stats = true;          /* start with the perf overlay on */
        }
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                screenshot_frame = atoi(argv[++i]);  /* optional frame count */
        }
    }

    printf("Settings: render-distance=%d chunks, msaa=%dx, aniso=%dx, present=%s, "
           "render-scale=%.2f\n",
           gfx.render_distance, gfx.msaa, gfx.aniso,
           gfx.present == PRESENT_PREF_MAILBOX   ? "mailbox" :
           gfx.present == PRESENT_PREF_IMMEDIATE ? "immediate" : "fifo",
           (double)gfx.render_scale);

    /* Single-player is a loopback host: spawn an in-process server and
     * connect to it. Gameplay (inventory, block break/place) is server-
     * authoritative, so without a server there is no interaction. */
    if (!host_mode && !client_mode && !server_mode)
        host_mode = true;

    if (server_mode) {
        /* Dedicated server: legacy single hardcoded world (NULL -> world.dat). */
        server_run(port, SERVER_MAX_CLIENTS, WORLD_SEED, NULL);
        return 0;
    }

    /* Headless test harness (--agent --headless, or --harness): run the
     * integrated server + a client SIMULATION with NO glfwInit / renderer /
     * window. Returns BEFORE any GPU/display init below. */
    if (agent_mode && headless_mode) {
        return run_headless_harness(port);
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    g_window            = window;
    g_agent_mode_active = agent_mode;

    /* Agent mode is headless-driven: it never shows menus and always runs the
     * in-world simulation, so it starts in PLAYING with the cursor captured.
     * Interactive mode opens at the main menu with a free cursor. */
    ui_set_state(agent_mode ? GAME_PLAYING : GAME_MAIN_MENU);

    if (!agent_mode) {
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetKeyCallback(window, key_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
        ui_apply_cursor(window);   /* free cursor for the main menu */
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }

    Renderer renderer;
    if (!renderer_init(&renderer, window, gfx)) {
        fprintf(stderr, "Failed to init renderer\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    renderer_init_player_mesh(&renderer);

    /* ---- World selection (interactive host mode only) ------------------
     * Singleplayer/host now manages multiple worlds under saves/<name>/.
     * Before any server/world is created we run a lightweight menu loop that
     * draws the main menu over a black background (no world yet) and waits for
     * the player to either create a new world or load an existing one. The
     * chosen seed + overlay path are then fed to the server thread and to
     * world_create so client and server agree on the seed.
     *
     * --client (joins a remote server) and --agent (headless automation) skip
     * this and use the legacy hardcoded seed + world.dat path, so their flows
     * are unchanged. The pause menu's Save / Save & Quit talk to the server via
     * server_request_save()/server_request_stop(); to switch worlds mid-session
     * the player quits (clean restart) and relaunches. */
    int  session_seed = WORLD_SEED;
    char session_path[WORLDSAVE_PATH_MAX] = {0};   /* empty -> legacy world.dat */
    GameMode session_gamemode = GAMEMODE_SURVIVAL; /* per-world; survival default */
    bool want_quit = false;

    /* A valid (placeholder) player so the world-selection menu can build a view
     * matrix before the real seed-based spawn is computed below. */
    player_init(&g_player, (vec3){ 0, 80, 0 });
    g_player_ptr = &g_player;
    g_player.agent_mode = agent_mode;

    if (host_mode && !agent_mode) {
        /* New-world name/seed come from an incrementing counter seeded by the
         * existing world count, so no rand()/time() globals are needed. */
        WorldList wl;
        worldsave_scan(&wl);
        uint64_t new_counter = (uint64_t)wl.count;

        ui_set_state(GAME_MAIN_MENU);
        hud_set_menu_page(MENU_PAGE_ROOT);

        bool chosen = false;
        int prev_lmb = GLFW_RELEASE;
        while (!chosen && !glfwWindowShouldClose(window)) {
            glfwPollEvents();
            float sw = (float)renderer.swapchain.extent.width;
            float sh = (float)renderer.swapchain.extent.height;

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            int lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT);
            bool clicked = (lmb == GLFW_PRESS && prev_lmb == GLFW_RELEASE);
            prev_lmb = lmb;

            /* (Re)scan worlds and publish names for the Load page each frame so
             * a world created earlier this loop would show up. */
            WorldList list;
            worldsave_scan(&list);
            const char* names[WORLDSAVE_LIST_MAX];
            for (int i = 0; i < list.count; i++) names[i] = list.entries[i].name;
            hud_set_world_list(names, list.count);

            ui_input_begin();
            if (hud_get_menu_page() == MENU_PAGE_ROOT) {
                HudRect b0 = hud_menu_button_rect(0, sw, sh);
                HudRect b1 = hud_menu_button_rect(1, sw, sh);
                HudRect b2 = hud_menu_button_rect(2, sw, sh);
                ui_add_element(HUD_ID_NEW_WORLD,  b0.x, b0.y, b0.w, b0.h);
                ui_add_element(HUD_ID_LOAD_WORLD, b1.x, b1.y, b1.w, b1.h);
                ui_add_element(HUD_ID_QUIT,       b2.x, b2.y, b2.w, b2.h);
            } else {
                for (int i = 0; i < list.count; i++) {
                    HudRect r = hud_menu_button_rect(i, sw, sh);
                    ui_add_element(HUD_ID_WORLD0 + i, r.x, r.y, r.w, r.h);
                }
                int back_idx = list.count > 0 ? list.count : 1;
                HudRect rb = hud_menu_button_rect(back_idx, sw, sh);
                ui_add_element(HUD_ID_BACK, rb.x, rb.y, rb.w, rb.h);
            }
            ui_handle_mouse((float)mx, (float)my, clicked);
            int hit = ui_clicked_element();

            if (hit == HUD_ID_NEW_WORLD) {
                WorldMeta m;
                char nm[WORLDSAVE_NAME_MAX];
                worldsave_new_name(new_counter, nm, sizeof(nm));
                int32_t sd = worldsave_seed_from_counter(new_counter);
                worldsave_meta_init(&m, nm, sd, (int64_t)(new_counter + 1));
                worldsave_write_meta(&m);   /* creates saves/<name>/ + meta */
                session_seed = sd;
                worldsave_dat_path(m.name, session_path, sizeof(session_path));
                session_gamemode = m.gamemode;
                chosen = true;
            } else if (hit == HUD_ID_LOAD_WORLD) {
                hud_set_menu_page(MENU_PAGE_LOAD);
            } else if (hit == HUD_ID_BACK) {
                hud_set_menu_page(MENU_PAGE_ROOT);
            } else if (hit == HUD_ID_QUIT) {
                want_quit = true;
                chosen = true;
            } else if (hit >= HUD_ID_WORLD0
                       && hit < HUD_ID_WORLD0 + list.count) {
                WorldMeta* m = &list.entries[hit - HUD_ID_WORLD0];
                session_seed = m->seed;
                worldsave_dat_path(m->name, session_path, sizeof(session_path));
                session_gamemode = m->gamemode;
                /* Bump recency so it floats to the top next time. */
                m->last_played += 1;
                worldsave_write_meta(m);
                chosen = true;
            }

            mat4 view, proj;
            camera_get_view(&g_player.camera, g_player.eye_pos, view);
            float aspect = sw / sh;
            camera_get_proj(&g_player.camera, aspect, proj);
            vec3 sky = {0.06f, 0.07f, 0.10f};
            renderer_draw_frame(&renderer, NULL, 0, NULL, 0, view, proj,
                                (vec3){0,-1,0}, 1.0f, sky,
                                NULL, -1, NULL, 0.0f, false, NULL);
        }

        /* Player picked the world: enter PLAYING and reset the menu page so a
         * later Esc shows the in-game pause overlay, not the world list. */
        hud_set_menu_page(MENU_PAGE_ROOT);
        ui_set_state(GAME_PLAYING);
    } else {
        /* client / agent / dedicated: legacy single world. */
        session_seed = WORLD_SEED;
        session_path[0] = '\0';
    }

    if (want_quit) {
        renderer_cleanup(&renderer);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    PT_Thread server_thread = {0};
    if (host_mode) {
        ServerArgs* sargs = malloc(sizeof(ServerArgs));
        sargs->port = port;
        sargs->max  = SERVER_MAX_CLIENTS;
        sargs->seed = session_seed;
        snprintf(sargs->save_path, sizeof(sargs->save_path), "%s", session_path);
        /* Host shared-world: hand the server our renderer + render distance so it
         * creates the ONE authoritative world (with GPU meshing) that we then
         * render directly — no second seed-generated client copy. The server
         * thread will not drive world_update; this (render) thread does. */
        sargs->renderer        = &renderer;
        sargs->render_distance = gfx.render_distance;
        sargs->gamemode        = session_gamemode;
        pt_thread_create(&server_thread, server_thread_func, sargs);
        /* Give server 200ms to bind before client tries to connect */
        pt_sleep_ms(200);
    }

    int net_fd = -1;
    NetThread net_thread;
    Client client;
    RemotePlayerSet remote_players;
    ClientMobSet mob_set;
    bool networking = host_mode || client_mode;

    if (networking) {
        net_fd = net_socket_client();
        net_thread_start(&net_thread, net_fd);

        /* Resolve via getaddrinfo so --client accepts DNS hostnames and IP
         * literals, not just dotted-quad IPv4 (mine-vibe-9xu). */
        struct sockaddr_in srv_addr = {0};
        NetResolveResult rr = net_resolve(connect_ip, port, &srv_addr);
        if (rr == NET_RESOLVE_NO_IPV4) {
            fprintf(stderr, "[main] FATAL: '%s' resolved only to IPv6; the UDP "
                            "transport is currently IPv4-only.\n", connect_ip);
            return 1;
        } else if (rr != NET_RESOLVE_OK) {
            fprintf(stderr, "[main] FATAL: could not resolve server host '%s'\n",
                    connect_ip);
            return 1;
        }

        client_init(&client, &net_thread, &srv_addr);
        remote_player_set_init(&remote_players);
        client_mob_set_init(&mob_set);

        g_remote_players = &remote_players;
        g_client         = &client;
        g_mobs           = &mob_set;
        client_set_snapshot_cb(&client, on_snapshot, NULL);
        client_set_leave_cb(&client, on_player_leave, NULL);
        client_set_mobs_cb(&client, on_mobs, NULL);
        client_set_death_cb(&client, on_death, NULL);
        client_set_chunk_cb(&client, on_chunk_data, NULL);
        client_set_chunk_unload_cb(&client, on_chunk_unload, NULL);
        /* Host renders the server's world in-process (shared world) — tell the
         * server NOT to stream chunks to us. A true remote client receives the
         * streamed terrain. */
        client_set_shared_world(&client, host_mode);
        client_set_render_distance(&client, gfx.render_distance);
        client_connect(&client);
    }

    int spawn_y = worldgen_get_height(0, 0, session_seed) + 4;
    vec3 g_spawn = { 0, (float)spawn_y, 0 };
    player_init(&g_player, g_spawn);
    g_player_ptr = &g_player;
    glm_vec3_copy(g_spawn, g_spawn_pos);
    g_player.agent_mode = agent_mode;

    /* Host shared-world unification: instead of generating a SECOND world from
     * the seed, render the integrated server's authoritative world directly.
     * This eliminates the two-worlds divergence (validation gaps, late-joiner /
     * save-load mismatch) for singleplayer/host. The server created the world
     * with our renderer, so its chunk meshes upload to the GPU; we (the main /
     * render thread) own world_update on it. `world_is_borrowed` marks that the
     * server owns the lifetime, so we don't double-destroy it on shutdown.
     *
     * Remote client (--client) still generates locally from the seed until the
     * server-side chunk streaming push loop lands (see docs/plans/chunk-
     * streaming.md, remote mode is PARTIAL). */
    World* world = NULL;
    bool   world_is_borrowed = false;
    if (host_mode) {
        /* The server thread creates the world asynchronously; spin briefly for
         * it to be published. It is created early in server_run_ex, so this
         * resolves within a few ms in practice. */
        for (int tries = 0; tries < 2000 && !world; tries++) {
            world = server_get_world();
            if (!world) pt_sleep_ms(1);
        }
        if (!world) {
            fprintf(stderr, "[main] FATAL: server world never became available\n");
            return 1;
        }
        world_is_borrowed = true;
    } else {
        /* Remote client (--client): the world is fed exclusively by the server's
         * streamed chunks. Create it in network-fed mode so world_update never
         * regenerates terrain from the seed; chunks arrive via on_chunk_data and
         * are lit + meshed by the local pipeline. */
        world = world_create(&renderer, session_seed, gfx.render_distance);
        world_set_network_fed(world, true);
        g_net_world = world;
    }

    BlockPhysics physics;
    block_physics_init(&physics);
    if (agent_mode) agent_init();

    /* Loading threshold: 30% of circular render area */
    int rd = world_get_render_distance(world);
    int expected_chunks = 0;
    for (int dx = -rd; dx <= rd; dx++)
        for (int dz = -rd; dz <= rd; dz++)
            if (dx*dx + dz*dz <= rd*rd)
                expected_chunks++;
    int load_threshold = (int)(0.30f * (float)expected_chunks);
    if (load_threshold < 1) load_threshold = 1;

    vec3 sun_dir = { -0.5f, -0.8f, -0.3f };
    glm_vec3_normalize(sun_dir);

    /* Day/night clock source. When networking, the server is authoritative and
     * we read a smoothed estimate from the client. Singleplayer has no server,
     * so we advance a local wall-clock-driven counter ourselves (starting at
     * noon) — otherwise the sky would freeze. SERVER_TICK_RATE ticks/second
     * matches the server's clock rate so a day is still DAY_LENGTH_TICKS. */
    double sp_clock_start = glfwGetTime();

    /* Loading loop: run until 30% of chunks are meshed */
    {
        ChunkMesh* meshes = NULL;
        uint32_t   mesh_count = 0;
        char       title[128];

        while (!glfwWindowShouldClose(window)
               && mesh_count < (uint32_t)load_threshold)
        {
            glfwPollEvents();
            player_update(&g_player, window, world, 0.0f);
            /* Pump networking during loading so a remote client actually
             * receives streamed chunks (its world is network-fed and generates
             * nothing locally); also keeps the connection alive for the host. */
            if (networking) {
                client_send_position(&client,
                                     g_player.position[0], g_player.position[1],
                                     g_player.position[2],
                                     g_player.camera.yaw, g_player.camera.pitch);
                client_poll(&client);
                /* Apply any chunk inserts/evicts that the callbacks queued is
                 * immediate (callbacks mutate the world directly), so just run
                 * world_update to light+mesh them. */
            }
            world_update(world, &physics, g_player.position);
            world_get_meshes(world, &meshes, &mesh_count);

            mat4 view, proj;
            camera_get_view(&g_player.camera, g_player.eye_pos, view);
            float aspect = (float)renderer.swapchain.extent.width
                         / (float)renderer.swapchain.extent.height;
            camera_get_proj(&g_player.camera, aspect, proj);

            /* During loading the world is at noon (full daylight). */
            float ld_phase = daynight_phase01(DAY_LENGTH_TICKS / 4);
            float ld_bright = daynight_brightness(ld_phase);
            vec3  ld_sky;   daynight_sky_color(ld_phase, ld_sky);
            renderer_draw_frame(&renderer, meshes, mesh_count, NULL, 0, view, proj, sun_dir,
                                ld_bright, ld_sky,
                                networking ? &client.inventory : NULL,
                                -1,
                                NULL,
                                /* underwater factor */ 0.0f,
                                false, NULL);

            uint32_t pct = (uint32_t)(100.0f * (float)mesh_count
                                              / (float)load_threshold);
            if (pct > 100) pct = 100;
            snprintf(title, sizeof(title), "Minecraft | Loading... %u%%", pct);
            glfwSetWindowTitle(window, title);
        }
    }

    if (agent_mode) agent_emit_ready();

    double last_time = glfwGetTime();
    int frame_count = 0;
    double fps_timer = last_time;
    uint64_t tick = 0;
    uint64_t total_frames = 0;   /* monotonic; drives --screenshot timing */

    /* Rolling perf stats: frametime ring buffer drives the FPS/frametime
     * readout; the renderer mirrors visible-chunk and draw-call counts into it
     * each frame. Drawn as an overlay when g_show_stats, and printed to stdout
     * roughly once a second. */
    PerfStats perf;
    perf_stats_reset(&perf);
    double stats_print_timer = last_time;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        perf_stats_push(&perf, dt);   /* feed the rolling FPS/frametime average */
        hud_tick(dt);                 /* decay the hurt flash + other timed FX */

        glfwPollEvents();

        /* Reconcile the container screen with the server-authoritative open flag.
         * client_poll (run later in the frame) flips g_client->container_open and
         * refreshes g_client->container; raise the GAME_CONTAINER overlay while a
         * container is open and drop back to PLAYING once it closes. The screen
         * renders from the latched snapshot, so the furnace cook/burn bars
         * animate as fresh PKT_CONTAINER_STATE packets arrive. */
        if (g_client) {
            if (g_client->container_open) {
                hud_set_container(&g_client->container);
                if (g_ui_state == GAME_PLAYING)
                    ui_set_state(GAME_CONTAINER);
            } else {
                hud_set_container(NULL);
                if (g_ui_state == GAME_CONTAINER)
                    ui_set_state(GAME_PLAYING);
            }
        }

        bool dump_frame = false;
        char dump_path[256] = {0};
        /* Headless screenshot: once enough frames have rendered for nearby
         * chunks to stream in, dump one frame; exit happens after the draw. */
        if (screenshot_path && total_frames == (uint64_t)screenshot_frame) {
            dump_frame = true;
            strncpy(dump_path, screenshot_path, 255);
            dump_path[255] = '\0';
        }
        if (agent_mode) {
            AgentCommand cmd;
            g_player.agent_jump = false;  /* reset per-frame edge-triggered input */
            while (agent_pop_command(&cmd)) {
                if (!apply_agent_command(&cmd, &g_player, &renderer,
                                          &dump_frame, dump_path)) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                    break;
                }
            }
        }

        /* ---- Menu / overlay input ----
         * When not in PLAYING the cursor is free; register the current screen's
         * clickable rects with the UI hit-test API, resolve the cursor, and act
         * on edge-triggered left clicks. Done before the in-world simulation so
         * a state transition this frame takes effect immediately. */
        if (!game_ui_world_active(g_ui_state) && !agent_mode) {
            double mx, my;
            glfwGetCursorPos(window, &mx, &my);
            int lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT);
            static int prev_lmb = GLFW_RELEASE;
            bool clicked = (lmb == GLFW_PRESS && prev_lmb == GLFW_RELEASE);
            prev_lmb = lmb;

            float sw = (float)renderer.swapchain.extent.width;
            float sh = (float)renderer.swapchain.extent.height;

            ui_input_begin();
            if (g_ui_state == GAME_MAIN_MENU) {
                HudRect b0 = hud_menu_button_rect(0, sw, sh);
                HudRect b1 = hud_menu_button_rect(1, sw, sh);
                ui_add_element(HUD_ID_PLAY, b0.x, b0.y, b0.w, b0.h);
                ui_add_element(HUD_ID_QUIT, b1.x, b1.y, b1.w, b1.h);
            } else if (g_ui_state == GAME_PAUSED) {
                HudRect b0 = hud_menu_button_rect(0, sw, sh);
                HudRect b1 = hud_menu_button_rect(1, sw, sh);
                HudRect b2 = hud_menu_button_rect(2, sw, sh);
                HudRect b3 = hud_menu_button_rect(3, sw, sh);
                HudRect b4 = hud_menu_button_rect(4, sw, sh);
                ui_add_element(HUD_ID_RESUME,    b0.x, b0.y, b0.w, b0.h);
                ui_add_element(HUD_ID_OPTIONS,   b1.x, b1.y, b1.w, b1.h);
                ui_add_element(HUD_ID_SAVE,      b2.x, b2.y, b2.w, b2.h);
                ui_add_element(HUD_ID_SAVE_QUIT, b3.x, b3.y, b3.w, b3.h);
                ui_add_element(HUD_ID_QUIT,      b4.x, b4.y, b4.w, b4.h);
            } else if (g_ui_state == GAME_OPTIONS) {
                HudRect tr = hud_volume_slider_rect(sw, sh);
                /* Pad the slider hit area vertically so it's easy to grab. */
                ui_add_element(HUD_ID_VOL_SLIDER, tr.x, tr.y - 8.0f, tr.w, tr.h + 16.0f);
                HudRect b1 = hud_menu_button_rect(1, sw, sh);
                HudRect b2 = hud_menu_button_rect(2, sw, sh);
                HudRect b3 = hud_menu_button_rect(3, sw, sh);
                ui_add_element(HUD_ID_MUTE,  b1.x, b1.y, b1.w, b1.h);
                ui_add_element(HUD_ID_MUSIC, b2.x, b2.y, b2.w, b2.h);
                ui_add_element(HUD_ID_BACK,  b3.x, b3.y, b3.w, b3.h);
            } else if (g_ui_state == GAME_INVENTORY) {
                for (int i = 0; i < HUD_SLOT_COUNT; i++) {
                    HudRect r = hud_inventory_slot_rect(i, sw, sh);
                    ui_add_element(HUD_ID_SLOT0 + i, r.x, r.y, r.w, r.h);
                }
                /* Crafting rows: same affordable-recipe ordering hud.c draws,
                 * so HUD_ID_CRAFT0 + i maps to craft_idx[i]. */
                if (g_client) {
                    ItemCounts counts;
                    crafting_counts_from_inventory(&g_client->inventory, &counts);
                    int navail = crafting_affordable(&counts, g_craft_idx,
                                                     HUD_CRAFT_ROWS);
                    g_craft_count = navail;
                    for (int i = 0; i < navail; i++) {
                        HudRect r = hud_craft_row_rect(i, sw, sh);
                        ui_add_element(HUD_ID_CRAFT0 + i, r.x, r.y, r.w, r.h);
                    }
                } else {
                    g_craft_count = 0;
                }
            } else if (g_ui_state == GAME_CONTAINER && g_client
                       && g_client->container_open) {
                /* Register the open container's slots + the player-inventory row.
                 * Container slots: chest 0..26 (3x9) or furnace 0..2; mirrored by
                 * hud_draw_container so clicks line up with the drawn icons. */
                int cslots = (g_client->container.type == CONTAINER_NET_CHEST)
                             ? HUD_CHEST_SLOTS : HUD_FURNACE_SLOT_COUNT;
                for (int i = 0; i < cslots; i++) {
                    HudRect r = (g_client->container.type == CONTAINER_NET_CHEST)
                              ? hud_chest_slot_rect(i, sw, sh)
                              : hud_furnace_slot_rect(i, sw, sh);
                    ui_add_element(HUD_ID_CON0 + i, r.x, r.y, r.w, r.h);
                }
                for (int i = 0; i < HUD_SLOT_COUNT; i++) {
                    HudRect r = hud_container_inv_slot_rect(i, sw, sh);
                    ui_add_element(HUD_ID_CONINV0 + i, r.x, r.y, r.w, r.h);
                }
            }
            ui_handle_mouse((float)mx, (float)my, clicked);

            /* Volume slider: drag-while-held, not just edge clicks. While the
             * cursor hovers the slider track and the button is down, map x ->
             * volume continuously and push it to the audio engine. */
            if (g_ui_state == GAME_OPTIONS && lmb == GLFW_PRESS
                && ui_hovered_element() == HUD_ID_VOL_SLIDER) {
                HudRect tr = hud_volume_slider_rect(sw, sh);
                float v = hud_slider_value_from_x(tr, (float)mx);
                audio_set_master_volume(v);
                hud_set_audio_state(v, audio_get_muted(), hud_get_music());
            }

            int hit = ui_clicked_element();
            if (hit == HUD_ID_OPTIONS) {
                /* Sync the HUD's latched audio state from the engine before
                 * showing the screen so sliders/toggles render the truth. */
                hud_set_audio_state(audio_get_master_volume(),
                                    audio_get_muted(), hud_get_music());
                ui_set_state(GAME_OPTIONS);
            } else if (g_ui_state == GAME_OPTIONS && hit == HUD_ID_BACK) {
                ui_set_state(GAME_PAUSED);
            } else if (hit == HUD_ID_MUTE) {
                bool m = hud_toggle_muted();
                audio_set_muted(m);
            } else if (hit == HUD_ID_MUSIC) {
                bool on = hud_toggle_music();
                audio_set_music(on);
            } else if (hit == HUD_ID_PLAY || hit == HUD_ID_RESUME) {
                ui_set_state(GAME_PLAYING);
            } else if (hit == HUD_ID_SAVE) {
                /* Trigger a mid-game world flush on the server thread. */
                if (host_mode) server_request_save();
            } else if (hit == HUD_ID_SAVE_QUIT) {
                /* Flush, then fall through to a normal shutdown (the teardown
                 * below force-flushes again, so the save is guaranteed). */
                if (host_mode) server_request_save();
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else if (hit == HUD_ID_QUIT) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else if (hit >= HUD_ID_SLOT0 && hit < HUD_ID_SLOT0 + HUD_SLOT_COUNT) {
                /* Clicking an inventory slot: if it holds an armour piece,
                 * equip it (server-authoritative); otherwise just select it. */
                if (g_client) {
                    int slot = hit - HUD_ID_SLOT0;
                    const InventorySlot* is = &g_client->inventory.slots[slot];
                    if (is->count > 0 && item_is_armor(is->item))
                        client_send_equip(g_client, (uint8_t)slot);
                    else
                        g_client->inventory.selected = slot;
                }
            } else if (hit >= HUD_ID_CRAFT0
                       && hit < HUD_ID_CRAFT0 + g_craft_count) {
                /* Clicking a crafting row sends a server-authoritative craft
                 * request for the recipe that row maps to. */
                if (g_client) {
                    int row = hit - HUD_ID_CRAFT0;
                    client_send_craft(g_client, (uint16_t)g_craft_idx[row]);
                }
            } else if (g_client && g_client->container_open
                       && hit >= HUD_ID_CON0
                       && hit < HUD_ID_CON0 + HUD_CHEST_SLOTS) {
                /* Whole-stack move of a container slot into the inventory. The
                 * count is clamped at the slot's contents (server re-validates).
                 * v1: left-click moves the entire stack; no shift/half split. */
                int slot = hit - HUD_ID_CON0;
                int avail;
                if (g_client->container.type == CONTAINER_NET_CHEST)
                    avail = g_client->container.slots[slot].count;
                else
                    avail = (slot == HUD_FURNACE_SLOT_INPUT)  ? g_client->container.f_input_count
                          : (slot == HUD_FURNACE_SLOT_FUEL)   ? g_client->container.f_fuel_count
                          : g_client->container.f_output_count;
                if (avail > 0)
                    client_send_container_action(g_client,
                        g_container_x, g_container_y, g_container_z,
                        (uint8_t)slot, CONTAINER_DIR_TO_INV,
                        (uint8_t)(avail > 255 ? 255 : avail));
            } else if (g_client && g_client->container_open
                       && hit >= HUD_ID_CONINV0
                       && hit < HUD_ID_CONINV0 + HUD_SLOT_COUNT) {
                /* Whole-stack move of an inventory slot into the container. The
                 * server routes a smeltable/fuel to the right furnace slot and
                 * rejects anything that doesn't fit. */
                int slot = hit - HUD_ID_CONINV0;
                int avail = g_client->inventory.slots[slot].count;
                if (avail > 0)
                    client_send_container_action(g_client,
                        g_container_x, g_container_y, g_container_z,
                        (uint8_t)slot, CONTAINER_DIR_FROM_INV,
                        (uint8_t)(avail > 255 ? 255 : avail));
            }
        }

        /* In-world simulation (movement, look-target, mining) only runs while
         * playing; menus/overlays freeze the player. Networking, world meshing
         * and rendering below always run so the world stays live behind the
         * overlay and the server connection doesn't stall. */
        bool world_active = game_ui_world_active(g_ui_state);
        if (!world_active) {
            g_player.agent_forward = 0;
            g_player.agent_right   = 0;
            g_target.hit = false;
            g_target_mob = 0;
            g_mining_progress = 0.0f;
            g_mining_x = g_mining_y = g_mining_z = INT32_MIN;
            /* Cancel any in-progress eat hold while a menu/overlay is up. */
            g_eating = false;
            g_eat_timer = 0.0f;
            g_eat_slot = -1;
            hud_set_eat_progress(0.0f);
        }

        if (world_active) {
        player_update(&g_player, window, world, dt);

        /* Local-player knockback: drain any server impulse into the residual
         * velocity, then apply it as a decaying positional nudge ON TOP of
         * player_update's movement (player_update snaps the player's own
         * horizontal velocity from input each frame, so the shove can't live in
         * g_player.velocity). Collision-resolved by physics_move so it can't
         * push through walls. */
        if (g_client && g_client->kb_pending) {
            g_kb_vx += g_client->kb_dx;
            g_kb_vy += g_client->kb_dy;
            g_kb_vz += g_client->kb_dz;
            g_client->kb_pending = false;
            g_client->kb_dx = g_client->kb_dy = g_client->kb_dz = 0.0f;
        }
        if (g_kb_vx != 0.0f || g_kb_vy != 0.0f || g_kb_vz != 0.0f) {
            vec3 kvel = { g_kb_vx, g_kb_vy, g_kb_vz };
            physics_move(g_player.position, kvel, PLAYER_HALF_W, PLAYER_HEIGHT,
                         dt, /*crouch=*/false, world);
            /* Exponential decay so the shove fades over a few frames. */
            float k = 1.0f - COMBAT_KNOCKBACK_DECAY * dt;
            if (k < 0.0f) k = 0.0f;
            g_kb_vx *= k; g_kb_vy *= k; g_kb_vz *= k;
            if (fabsf(g_kb_vx) < 0.05f && fabsf(g_kb_vy) < 0.05f &&
                fabsf(g_kb_vz) < 0.05f)
                g_kb_vx = g_kb_vy = g_kb_vz = 0.0f;
        }

        /* Underwater HUD tint (dyb.4.3): the tint is keyed off the CAMERA being
         * submerged, not the feet, so it appears only when the head dips under
         * water. Sample the block at the eye position. */
        {
            int ex = (int)floorf(g_player.eye_pos[0]);
            int ey = (int)floorf(g_player.eye_pos[1]);
            int ez = (int)floorf(g_player.eye_pos[2]);
            bool head_in_water =
                world_get_block(world, ex, ey, ez) == BLOCK_WATER;
            hud_set_underwater(head_in_water);
        }

        /* Footstep triggers (63k): while moving on solid ground (not airborne,
         * not in water), accumulate horizontal distance and play a surface-aware
         * footstep every FOOTSTEP_STRIDE metres. The block under the feet picks
         * the material. dx/dz is measured from last frame's position. */
        {
            static float prev_x = 0.0f, prev_z = 0.0f;
            static bool  have_prev = false;
            float cx = g_player.position[0];
            float cz = g_player.position[2];
            if (have_prev && g_player.on_ground && !g_player.in_water) {
                float dx = cx - prev_x;
                float dz = cz - prev_z;
                float moved = sqrtf(dx * dx + dz * dz);
                int steps = footstep_step_count(&g_footstep_accum, moved,
                                                FOOTSTEP_STRIDE);
                if (steps > 0) {
                    /* Block directly beneath the feet (one below the feet cell). */
                    int fx = (int)floorf(g_player.position[0]);
                    int fy = (int)floorf(g_player.position[1]) - 1;
                    int fz = (int)floorf(g_player.position[2]);
                    BlockID under = world_get_block(world, fx, fy, fz);
                    FootstepMaterial mat = audio_footstep_material_for_block(under);
                    for (int s = 0; s < steps; s++)
                        audio_play_footstep(mat, g_footstep_seq++);
                }
            } else {
                /* Airborne / in water / first frame: don't bank distance. */
                g_footstep_accum = 0.0f;
            }
            prev_x = cx;
            prev_z = cz;
            have_prev = true;
        }

        /* Hold-to-eat: while F is held and the eat was armed on a still-valid
         * food slot, accumulate toward EAT_DURATION_SEC and send PKT_EAT once
         * complete (server validates + applies). Releasing F, swapping the
         * selected slot, or running out of that food cancels the hold. */
        if (g_eating) {
            bool f_held = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
            int  sel    = g_client ? g_client->inventory.selected : -1;
            ItemId held = (g_client && sel >= 0)
                        ? g_client->inventory.slots[sel].item : (ItemId)BLOCK_AIR;
            bool still_food = g_client && sel == g_eat_slot
                           && g_client->inventory.slots[sel].count > 0
                           && item_is_food(held);
            if (!f_held || !still_food) {
                g_eating = false;
                g_eat_timer = 0.0f;
                g_eat_slot = -1;
            } else {
                g_eat_timer += dt;
                if (g_eat_timer >= EAT_DURATION_SEC) {
                    client_send_eat(g_client, (uint8_t)sel);
                    g_eating = false;
                    g_eat_timer = 0.0f;
                    g_eat_slot = -1;
                }
            }
        }
        hud_set_eat_progress(g_eating ? (g_eat_timer / EAT_DURATION_SEC) : 0.0f);

        /* Refresh the look-target for outline rendering and click handling.
         * Must happen after player_update (so eye_pos / yaw / pitch are
         * current) and before mouse_button_callback can fire on the next
         * glfwPollEvents (i.e. before we yield to the event loop). */
        {
            vec3 dir;
            camera_get_front(&g_player.camera, dir);
            g_target = raycast_voxel(world, g_player.eye_pos, dir, MAX_REACH);
        }

        /* Raycast mobs and prefer a mob nearer than the targeted block. */
        g_target_mob = 0;
        if (g_mobs) {
            float mt = 0.0f;
            vec3 mdir; camera_get_front(&g_player.camera, mdir);
            uint16_t mid = mob_ray_hit(g_mobs, g_player.eye_pos, mdir, MAX_REACH, &mt);
            if (mid) {
                /* Prefer the mob if it's nearer than the targeted block. */
                bool block_blocks = g_target.hit;
                float block_d = block_blocks
                    ? glm_vec3_distance(g_player.eye_pos,
                        (vec3){ g_target.x + 0.5f, g_target.y + 0.5f, g_target.z + 0.5f })
                    : 1e30f;
                if (mt <= block_d) g_target_mob = mid;
            }
        }

        /* Timed mining: while the break button is held over a mineable block,
         * accumulate progress and send the break only when it completes. Any
         * change of target, button release, mob-priority, or an unbreakable
         * block resets progress. The break packet is sent once per completion;
         * the server applies the edit and echoes a PKT_BLOCK_CHANGE. */
        {
            bool lmb = (g_client &&
                        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)
                            == GLFW_PRESS);
            bool can_mine = lmb && !g_target_mob && g_target.hit;

            if (!can_mine) {
                g_mining_progress = 0.0f;
                g_mining_x = g_mining_y = g_mining_z = INT32_MIN;
            } else {
                /* Reset if the targeted cell changed since last frame. */
                if (g_target.x != g_mining_x ||
                    g_target.y != g_mining_y ||
                    g_target.z != g_mining_z) {
                    g_mining_progress = 0.0f;
                    g_mining_x = g_target.x;
                    g_mining_y = g_target.y;
                    g_mining_z = g_target.z;
                }
                /* Mining speed depends on the held hotbar item: a matching tool
                 * (and higher tier) breaks faster; a wrong item / block / hand
                 * uses the base time. */
                int    sel  = g_client->inventory.selected;
                ItemId held = g_client->inventory.slots[sel].item;
                float need = tool_break_time(held, g_target.block);
                if (need >= BLOCK_BREAK_UNBREAKABLE) {
                    /* Bedrock and friends: never completes. */
                    g_mining_progress = 0.0f;
                } else {
                    g_mining_progress += dt;
                    if (g_mining_progress >= need) {
                        client_send_break(g_client,
                                          g_target.x, g_target.y, g_target.z,
                                          (uint8_t)g_target.block,
                                          (uint8_t)sel);
                        /* Stop re-sending until the cell changes (the server
                         * echo will clear the block and move the target). */
                        g_mining_progress = 0.0f;
                        g_mining_x = g_mining_y = g_mining_z = INT32_MIN;
                    }
                }
            }
        }
        }  /* end if (world_active) */

        /* Networking tick */
        if (networking) {
            client_send_position(&client,
                                  g_player.position[0],
                                  g_player.position[1],
                                  g_player.position[2],
                                  g_player.camera.yaw,
                                  g_player.camera.pitch);
            client_poll(&client);
        }

        world_update(world, &physics, g_player.position);

        /* Apply server-authoritative block edits buffered from the network
         * thread. world_set_block marks chunks dirty for re-meshing.
         * Guarded: without networking the Client struct is uninitialised
         * stack memory and pending_block_change_count would be garbage.
         *
         * Host shared-world: the integrated server already applied each edit
         * directly to THIS world (it is the same World*), including the relight
         * and needs_remesh flag the main-thread world_update picks up. So we
         * must NOT re-apply it here (that would double-write blocks/lights from
         * two threads). We still wake block physics around the edit so sand /
         * water react. For a remote client the edit has only been received over
         * the wire, so it must be applied locally. */
        if (networking) {
            for (int i = 0; i < client.pending_block_change_count; i++) {
                int x = client.pending_block_changes[i].x;
                int y = client.pending_block_changes[i].y;
                int z = client.pending_block_changes[i].z;
                BlockID b = (BlockID)client.pending_block_changes[i].block;
                if ((unsigned)b >= BLOCK_COUNT) {
                    fprintf(stderr, "[main] pending block-change has invalid id %u at (%d,%d,%d), skipping\n",
                            (unsigned)b, x, y, z);
                    continue;
                }
                if (!world_is_borrowed) {
                    /* Remote client: apply the received edit to our local world. */
                    if (!world_set_block(world, x, y, z, b)) {
                        fprintf(stderr, "[main] world_set_block failed at (%d,%d,%d) block=%u; chunk unloaded or busy\n",
                                x, y, z, (unsigned)b);
                        continue;
                    }
                    /* PKT_BLOCK_CHANGE carries no meta channel, so network-placed
                     * water arrives with meta=0 and would dissipate next tick.
                     * Tag player-placed water as a source so it persists/spreads. */
                    if (b == BLOCK_WATER)
                        world_set_meta(world, x, y, z, WATER_SOURCE_LEVEL);
                }
                /* Wake the gravity/water simulators around the edit. Without
                 * this, sand placed mid-air just floats and water never flows
                 * into newly-dug space — the active sets are only seeded at
                 * chunk-load time (see world_update). */
                block_physics_notify(&physics, x, y, z);
            }
            client.pending_block_change_count = 0;
        }

        block_physics_update(&physics, world, g_player.position, dt);

        ChunkMesh* meshes;
        uint32_t mesh_count;
        world_get_meshes(world, &meshes, &mesh_count);

        mat4 view, proj;
        camera_get_view(&g_player.camera, g_player.eye_pos, view);
        float aspect = (float)renderer.swapchain.extent.width
                     / (float)renderer.swapchain.extent.height;
        camera_get_proj(&g_player.camera, aspect, proj);

        /* Per-mob walk-cycle state, indexed by mob slot (ClientMob has no room
         * for it and lives in an out-of-scope header). Persists across frames;
         * a slot reused for a new mob simply re-seeds from rest. */
        static float mob_walk_phase[MOB_MAX] = {0};
        static float mob_walk_speed[MOB_MAX] = {0};
        static vec3  mob_prev_pos[MOB_MAX]   = {{0}};
        static bool  mob_prev_valid[MOB_MAX] = {0};

        /* Local player walk-cycle state. Advanced every frame from the local
         * horizontal velocity so it is correct the moment the third-person /
         * front camera (dwk.3) draws the local body. The angles are written
         * into local_limb_angle each frame; dwk.3 attaches them to the local
         * player's render state. */
        static float local_walk_phase = 0.0f;
        static float local_walk_speed = 0.0f;
        float local_limb_angle[ANIM_PART_COUNT];
        {
            float vx = g_player.velocity[0];
            float vz = g_player.velocity[2];
            float hspeed = sqrtf(vx * vx + vz * vz);
            local_walk_speed = player_anim_speed_smooth(local_walk_speed, hspeed, dt);
            local_walk_phase = player_anim_walk_phase_advance(local_walk_phase,
                                                              local_walk_speed, dt);
            player_anim_walk(local_limb_angle, local_walk_phase, local_walk_speed);
        }
        (void)local_limb_angle;  /* consumed when dwk.3 draws the local body */

        /* Collect remote player states for rendering */
        PlayerRenderState rp_states[REMOTE_PLAYER_MAX + MOB_MAX];
        uint32_t rcount = 0;
        if (networking) {
            for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
                RemotePlayer* rp = &remote_players.players[i];
                if (!rp->active || rp->snapshot_count < 2) continue;
                vec3 pos; float yaw, pitch;
                remote_player_interpolate(rp, dt, pos, &yaw, &pitch);
                rp_states[rcount].pos[0] = pos[0];
                rp_states[rcount].pos[1] = pos[1];
                rp_states[rcount].pos[2] = pos[2];
                rp_states[rcount].yaw    = yaw;
                rp_states[rcount].tint[0] = 0.0f;   /* a=0 → unmodified player skin */
                rp_states[rcount].tint[1] = 0.0f;
                rp_states[rcount].tint[2] = 0.0f;
                rp_states[rcount].tint[3] = 0.0f;
                rp_states[rcount].tint2[0] = 0.0f;  /* unused for players */
                rp_states[rcount].tint2[1] = 0.0f;
                rp_states[rcount].tint2[2] = 0.0f;
                rp_states[rcount].scale[0] = 1.0f;  /* unscaled player model */
                rp_states[rcount].scale[1] = 1.0f;
                rp_states[rcount].scale[2] = 1.0f;
                rp_states[rcount].mesh_type = -1;   /* humanoid player mesh */
                /* Walk cycle: phase/speed from interpolated snapshot motion. */
                float wphase, wspeed;
                remote_player_update_anim(rp, dt, &wphase, &wspeed);
                player_anim_walk(rp_states[rcount].limb_angle, wphase, wspeed);
                rp_states[rcount].head_yaw = 0.0f;
                rcount++;
            }
            for (int i = 0; i < MOB_MAX && rcount < REMOTE_PLAYER_MAX + MOB_MAX; i++) {
                ClientMob* m = &mob_set.mobs[i];
                if (!m->active || m->snapshot_count < 2) {
                    mob_prev_valid[i] = false;  /* reset gait when slot idles */
                    continue;
                }
                vec3 pos; float yaw;
                client_mob_interpolate(m, dt, pos, &yaw);
                /* Per-type two-tone colours + body silhouette from the render
                 * table; a=1 flags "this is a mob" so the shader uses the
                 * supplied colours instead of the player skin. */
                MobRenderDef def = mob_render_def((MobType)m->type);
                rp_states[rcount].pos[0] = pos[0];
                rp_states[rcount].pos[1] = pos[1];
                rp_states[rcount].pos[2] = pos[2];
                rp_states[rcount].yaw    = yaw;
                rp_states[rcount].tint[0] = def.primary[0];
                rp_states[rcount].tint[1] = def.primary[1];
                rp_states[rcount].tint[2] = def.primary[2];
                rp_states[rcount].tint[3] = 1.0f;
                rp_states[rcount].tint2[0] = def.secondary[0];
                rp_states[rcount].tint2[1] = def.secondary[1];
                rp_states[rcount].tint2[2] = def.secondary[2];
                /* Each per-type mesh has its own normalized extents, so fit it
                 * to the type's silhouette (not a shared humanoid base). */
                float fit[3];
                mob_model_fit_scale((MobType)m->type, fit);
                rp_states[rcount].scale[0] = fit[0];
                rp_states[rcount].scale[1] = fit[1];
                rp_states[rcount].scale[2] = fit[2];
                /* Draw this mob with its own per-type baked mesh. */
                rp_states[rcount].mesh_type = (int)m->type;
                /* Walk cycle: horizontal speed from the interpolated position
                 * delta vs. the previous frame, smoothed + phase-advanced. */
                float mtarget = 0.0f;
                if (mob_prev_valid[i] && dt > 0.0f) {
                    float dx = pos[0] - mob_prev_pos[i][0];
                    float dz = pos[2] - mob_prev_pos[i][2];
                    mtarget = sqrtf(dx * dx + dz * dz) / dt;
                }
                mob_walk_speed[i] = player_anim_speed_smooth(mob_walk_speed[i], mtarget, dt);
                mob_walk_phase[i] = player_anim_walk_phase_advance(mob_walk_phase[i],
                                                                   mob_walk_speed[i], dt);
                glm_vec3_copy(pos, mob_prev_pos[i]);
                mob_prev_valid[i] = true;
                player_anim_walk(rp_states[rcount].limb_angle,
                                 mob_walk_phase[i], mob_walk_speed[i]);
                rp_states[rcount].head_yaw = 0.0f;
                rcount++;
            }
        }

        /* Underwater fade factor: 0 when the eye is at or above the water
         * surface, ramping linearly to 1 over UNDERWATER_FADE_DEPTH below
         * the surface. The surface y is found by walking up from the
         * eye's cell until we hit a non-water cell — handles deep water
         * columns where the surface isn't just eye_yi + 1. */
        float underwater = 0.0f;
        {
            int ex = (int)floorf(g_player.eye_pos[0]);
            int ey = (int)floorf(g_player.eye_pos[1]);
            int ez = (int)floorf(g_player.eye_pos[2]);
            if (world_get_block(world, ex, ey, ez) == BLOCK_WATER) {
                int surface_y = ey + 1;
                while (world_get_block(world, ex, surface_y, ez) == BLOCK_WATER)
                    surface_y++;
                float depth = (float)surface_y - g_player.eye_pos[1];
                const float UNDERWATER_FADE_DEPTH = 0.3f;
                if (depth > 0.0f) {
                    underwater = depth / UNDERWATER_FADE_DEPTH;
                    if (underwater > 1.0f) underwater = 1.0f;
                }
            }
        }

        /* Resolve the current time-of-day. Networking: smoothed estimate of
         * the server-authoritative clock. Singleplayer: a local clock advanced
         * from wall time at the server tick rate, seeded at noon. */
        uint32_t world_ticks;
        if (networking) {
            world_ticks = client_estimate_world_ticks(&client);
        } else {
            double elapsed = glfwGetTime() - sp_clock_start;
            if (elapsed < 0.0) elapsed = 0.0;
            world_ticks = (uint32_t)(DAY_LENGTH_TICKS / 4)
                        + (uint32_t)(elapsed * SERVER_TICK_RATE);
        }
        float day_phase = daynight_phase01(world_ticks);
        float day_brightness = daynight_brightness(day_phase);
        vec3  sky_color;  daynight_sky_color(day_phase, sky_color);

        /* Hand the perf overlay state to the renderer; it mirrors this frame's
         * counters into `perf` and draws the overlay in the UI pass. */
        renderer.show_stats    = g_show_stats;
        renderer.stats_overlay = &perf;

        renderer_draw_frame(&renderer, meshes, mesh_count,
                            rcount > 0 ? rp_states : NULL, rcount,
                            view, proj, sun_dir,
                            day_brightness, sky_color,
                            networking ? &client.inventory : NULL,
                            networking ? client.health : -1,
                            &g_target,
                            underwater,
                            dump_frame, dump_path);

        if (agent_mode) {
            float yaw_deg   = g_player.camera.yaw   * (180.0f / 3.14159265f);
            float pitch_deg = g_player.camera.pitch * (180.0f / 3.14159265f);
            AgentSnapshot snap = {
                .pos       = { g_player.position[0], g_player.position[1], g_player.position[2] },
                .vel       = { g_player.velocity[0], g_player.velocity[1], g_player.velocity[2] },
                .yaw       = yaw_deg,
                .pitch     = pitch_deg,
                .on_ground = g_player.on_ground ? 1 : 0,
                .mode      = (g_player.mode == MODE_FREE) ? 0 : 1,
                .tick      = tick,
            };
            /* Hotbar mirrors the server-authoritative inventory; only valid
             * when networking (the Client struct is otherwise uninitialised).
             * Report the selected slot and each slot's item count. */
            if (networking) {
                snap.selected_slot = client.inventory.selected;
                for (int i = 0; i < INVENTORY_SLOTS; i++)
                    snap.hotbar[i] = client.inventory.slots[i].count;
            } else {
                snap.selected_slot = 0;
                for (int i = 0; i < INVENTORY_SLOTS; i++)
                    snap.hotbar[i] = 0;
            }
            agent_emit_snapshot(&snap);
            tick++;
        }

        /* Headless screenshot: the frame requested above has now been drawn
         * and the PNG written — exit cleanly so the run self-terminates. */
        if (screenshot_path && total_frames >= (uint64_t)screenshot_frame)
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        /* FPS counter */
        total_frames++;
        frame_count++;
        if (now - fps_timer >= 2.0) {
            char title[128];
            snprintf(title, sizeof(title),
                     "Minecraft | FPS: %d | Chunks: %u | Pos: %.0f, %.0f, %.0f",
                     (int)(frame_count / (now - fps_timer)),
                     mesh_count,
                     g_player.eye_pos[0], g_player.eye_pos[1],
                     g_player.eye_pos[2]);
            glfwSetWindowTitle(window, title);
            frame_count = 0;
            fps_timer = now;
        }

        /* Perf stats to stdout ~1/s (rolling average; independent of the 2s
         * title cadence above). Emitted whenever the overlay is enabled. */
        if (g_show_stats && now - stats_print_timer >= 1.0) {
            printf("[stats] FPS %.1f  frametime %.2f ms  chunks %u  "
                   "culled %u/%u (%.0f%%)  draws %u\n",
                   perf_stats_avg_fps(&perf),
                   perf_stats_avg_frametime_ms(&perf),
                   perf.visible_chunks,
                   perf.culled_chunks, perf.total_chunks,
                   perf.total_chunks ? (100.0f * (float)perf.culled_chunks
                                        / (float)perf.total_chunks) : 0.0f,
                   perf.draw_calls);
            fflush(stdout);
            stats_print_timer = now;
        }
    }

    vkDeviceWaitIdle(renderer.device);

    if (networking) {
        /* Clear GLFW callbacks and g_client BEFORE network teardown so that
         * any callback firing during glfwDestroyWindow can't dereference a
         * client whose socket has already been closed. */
        g_client = NULL;
        g_mobs   = NULL;
        glfwSetMouseButtonCallback(window, NULL);
        glfwSetScrollCallback(window, NULL);
        client_disconnect(&client);
        net_thread_stop(&net_thread);
        net_socket_close(net_fd);
    }
    if (host_mode) {
        server_request_stop();         /* break the server loop so the join returns */
        pt_thread_join(server_thread); /* takes PT_Thread by value — also destroys
                                        * the shared world it owns */
    }

    /* Only destroy the world we own. In host mode the world belongs to the
     * server thread, which destroyed it during the join above; destroying it
     * again here would be a double-free. */
    if (!world_is_borrowed)
        world_destroy(world);
    block_physics_destroy(&physics);
    if (agent_mode) agent_destroy();
    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

/* ====================================================================== */
/*  Headless test harness (zqj / qne / c0j)                               */
/* ====================================================================== */
/*
 * Runs the REAL integrated authoritative server (on its own thread, headless:
 * renderer == NULL so it drives its own chunk pipeline) plus a client
 * SIMULATION on this thread — connecting to localhost over the same UDP/net
 * stack the game uses. There is NO glfwInit, NO renderer, and NO window: the
 * loop drains agent JSON commands, applies them (routing gameplay verbs through
 * the client->server packets so the server stays authoritative), pumps the
 * network, advances the player physics, and emits a rich JSON state line.
 *
 * Determinism: there is no real renderer to pace frames, so the harness uses a
 * FIXED dt (1/SERVER_TICK_RATE) per simulation tick. The `step {ticks}` command
 * advances exactly `ticks` such ticks, sleeping ~1 server-tick of real time per
 * iteration so the (wall-clock 20 Hz) server thread processes the packets we
 * send — then returns. Tests issue a `step` after each action and assert on the
 * resulting `get_state`, so they are reproducible and fast (no frame budget).
 */

/* Fill an AgentSnapshot from the live client/world/mob state — the same data
 * the renderer would read — plus a couple of test-only reads of the server. */
static void harness_fill_snapshot(AgentSnapshot* snap, Player* pl, Client* cl,
                                  ClientMobSet* mobs, World* world,
                                  RaycastHit* target, uint64_t tick)
{
    memset(snap, 0, sizeof(*snap));
    glm_vec3_copy(pl->position, snap->pos);
    glm_vec3_copy(pl->velocity, snap->vel);
    snap->yaw       = pl->camera.yaw   * (180.0f / 3.14159265f);
    snap->pitch     = pl->camera.pitch * (180.0f / 3.14159265f);
    snap->on_ground = pl->on_ground ? 1 : 0;
    snap->mode      = (pl->mode == MODE_FREE) ? 0 : 1;
    snap->tick      = tick;

    snap->selected_slot = cl->inventory.selected;
    snap->inventory_count = INVENTORY_SLOTS;
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        snap->hotbar[i]            = cl->inventory.slots[i].count;
        snap->inventory[i].item    = (int)cl->inventory.slots[i].item;
        snap->inventory[i].count   = cl->inventory.slots[i].count;
        snap->inventory[i].durability = cl->inventory.slots[i].durability;
    }

    snap->health = cl->health;
    snap->food   = cl->food;
    snap->air    = cl->air;
    snap->time_of_day = client_estimate_world_ticks(cl);
    WeatherState wx = client_get_weather(cl);
    snap->weather = (int)wx.kind;

    /* Gamemode is owned by the server; read it via the test backdoor. */
    Server* sv = server_get_instance();
    snap->gamemode = sv ? (int)sv->gamemode : 0;

    /* Per-SFX cumulative play counts (mine-vibe-2mh): lets headless scenarios
     * verify the right sound fired. Counts work with the silent backend. */
    for (int i = 0; i < SFX_COUNT; i++)
        snap->sounds[i] = audio_play_count((SoundId)i);

    /* Targeted block. */
    if (target && target->hit) {
        snap->target_hit   = 1;
        snap->target_x     = target->x;
        snap->target_y     = target->y;
        snap->target_z     = target->z;
        snap->target_block = (int)target->block;
    }
    (void)world;

    /* Nearby mobs from the client-side interpolated set. */
    int mc = 0;
    if (mobs) {
        for (int i = 0; i < MOB_MAX && mc < AGENT_MAX_MOBS; i++) {
            ClientMob* m = &mobs->mobs[i];
            if (!m->active) continue;
            snap->mobs[mc].id     = m->id;
            snap->mobs[mc].type   = m->type;
            snap->mobs[mc].health = m->health;
            /* Latest received snapshot position (index 1 is freshest). */
            int pi = (m->snapshot_count >= 2) ? 1 : 0;
            snap->mobs[mc].pos[0] = m->positions[pi][0];
            snap->mobs[mc].pos[1] = m->positions[pi][1];
            snap->mobs[mc].pos[2] = m->positions[pi][2];
            mc++;
        }
    }
    snap->mob_count = mc;

    /* Open container contents, if any. */
    if (cl->container_open) {
        snap->container_open = 1;
        if (cl->container.type == CONTAINER_NET_CHEST) {
            snap->container_type  = 0;
            snap->container_slots = CONTAINER_NET_CHEST_SLOTS;
            for (int i = 0; i < CONTAINER_NET_CHEST_SLOTS; i++) {
                snap->container[i].item  = cl->container.slots[i].item;
                snap->container[i].count = cl->container.slots[i].count;
            }
        } else {
            snap->container_type  = 1;   /* furnace */
            snap->container_slots = 3;
            snap->container[0].item  = cl->container.f_input;
            snap->container[0].count = cl->container.f_input_count;
            snap->container[1].item  = cl->container.f_fuel;
            snap->container[1].count = cl->container.f_fuel_count;
            snap->container[2].item  = cl->container.f_output;
            snap->container[2].count = cl->container.f_output_count;
        }
    }
}

/* Apply a gameplay verb / test helper. Returns false on quit. Gameplay verbs
 * route through client_send_* (authoritative server); helpers use the server
 * backdoor (server_get_instance + server_test_*). */
static bool harness_apply_command(const AgentCommand* cmd, Player* pl,
                                   Client* cl, World* world,
                                   RaycastHit* target, int* step_ticks)
{
    *step_ticks = 0;
    switch (cmd->type) {
    case CMD_MOVE:
        pl->agent_forward = cmd->move.forward;
        pl->agent_right   = cmd->move.right;
        /* Any nonzero movement releases the position hold so the player can
         * actually walk; a zero move re-pins them where they stand. */
        if (cmd->move.forward != 0.0f || cmd->move.right != 0.0f) {
            g_harness_hold = false;
        } else {
            g_harness_hold = true;
            g_harness_hold_x = pl->position[0];
            g_harness_hold_z = pl->position[2];
        }
        break;
    case CMD_LOOK:
        pl->camera.yaw   = cmd->look.yaw   * (3.14159265f / 180.0f);
        pl->camera.pitch = cmd->look.pitch * (3.14159265f / 180.0f);
        break;
    case CMD_JUMP:    pl->agent_jump   = true; g_harness_hold = false; break;
    case CMD_SPRINT:  pl->agent_sprint = (cmd->sprint.active != 0); break;
    case CMD_MODE:
        pl->mode = (cmd->mode.mode == 0) ? MODE_FREE : MODE_WALKING;
        glm_vec3_zero(pl->velocity);
        pl->on_ground = false; pl->in_water = false;
        break;
    case CMD_SELECT_SLOT:
        if (cmd->select_slot.slot >= 0 && cmd->select_slot.slot < INVENTORY_SLOTS)
            cl->inventory.selected = cmd->select_slot.slot;
        break;
    case CMD_GET_STATE: break;       /* state is emitted by the caller */
    case CMD_DUMP_FRAME:
        agent_emit_error("dump_frame is not supported in headless harness");
        break;
    case CMD_STEP:    *step_ticks = cmd->step.ticks; break;

    /* ---- Gameplay verbs (authoritative via packets) ---- */
    case CMD_PLACE: {
        /* Place `block` INTO cell (x,y,z): select a hotbar slot holding it, then
         * send a place against the cell below with face = +Y so the new block
         * lands in (x,y,z). Requires the player be within reach (use `tp`). */
        int slot = -1;
        for (int i = 0; i < INVENTORY_SLOTS; i++)
            if ((int)cl->inventory.slots[i].item == cmd->place.block
                && cl->inventory.slots[i].count > 0) { slot = i; break; }
        if (slot < 0) { agent_emit_error("place: block not held in inventory"); break; }
        cl->inventory.selected = slot;
        client_send_place(cl, cmd->place.x, cmd->place.y - 1, cmd->place.z,
                          (uint8_t)FACE_PY, (uint8_t)slot);
        break;
    }
    case CMD_BREAK: {
        int x = cmd->brk.x, y = cmd->brk.y, z = cmd->brk.z;
        if (x == INT32_MIN) {            /* no coords -> break the current target */
            if (!target || !target->hit) { agent_emit_error("break: no target"); break; }
            x = target->x; y = target->y; z = target->z;
        }
        BlockID b = world ? world_get_block(world, x, y, z) : BLOCK_AIR;
        client_send_break(cl, x, y, z, (uint8_t)b, (uint8_t)cl->inventory.selected);
        break;
    }
    case CMD_ATTACK:
        client_send_mob_attack(cl, (uint16_t)cmd->attack.mob_id);
        /* Mirror the local attacker-side audio cues that the GLFW left-click
         * path plays (mouse_button_callback): every swing whooshes, and a melee
         * blow that lands on a mob plays the connect "thwack" + the mob's hurt
         * grunt. The GLFW callbacks don't run headless, so the harness fires the
         * same cues here so SFX scenarios can verify them. (mine-vibe-2mh) */
        audio_play(SFX_SWING);
        audio_play(SFX_HIT);
        audio_play(SFX_MOB_HURT);
        break;
    case CMD_CRAFT:
        client_send_craft(cl, (uint16_t)cmd->craft.recipe);
        break;
    case CMD_OPEN:
        client_send_container_open(cl, cmd->open.x, cmd->open.y, cmd->open.z);
        g_container_x = cmd->open.x; g_container_y = cmd->open.y; g_container_z = cmd->open.z;
        break;
    case CMD_MOVE_ITEM: {
        int x = cmd->move_item.x, y = cmd->move_item.y, z = cmd->move_item.z;
        if (x == INT32_MIN) { x = g_container_x; y = g_container_y; z = g_container_z; }
        uint8_t dir = (cmd->move_item.dir == 1) ? CONTAINER_DIR_FROM_INV
                                                 : CONTAINER_DIR_TO_INV;
        int cnt = cmd->move_item.count; if (cnt < 1) cnt = 1; if (cnt > 255) cnt = 255;
        client_send_container_action(cl, x, y, z, (uint8_t)cmd->move_item.slot,
                                     dir, (uint8_t)cnt);
        break;
    }
    case CMD_EAT:
        client_send_eat(cl, (uint8_t)cl->inventory.selected);
        /* Mirror the local chew cue the GLFW eat path plays (key_callback, F):
         * the actual consume stays server-authoritative, but the SFX is a local
         * feedback that the headless harness must fire itself. (mine-vibe-2mh) */
        audio_play(SFX_EAT);
        break;

    /* ---- Test-only helpers (server backdoor) ---- */
    case CMD_GIVE: {
        Server* sv = server_get_instance();
        if (!server_test_give(sv, cmd->give.item, cmd->give.count))
            agent_emit_error("give: no server/client");
        break;
    }
    case CMD_TP: {
        Server* sv = server_get_instance();
        /* Move BOTH the server's authoritative position and the local sim. */
        server_test_tp(sv, cmd->tp.x, cmd->tp.y, cmd->tp.z);
        pl->position[0] = cmd->tp.x; pl->position[1] = cmd->tp.y; pl->position[2] = cmd->tp.z;
        glm_vec3_zero(pl->velocity);
        pl->on_ground = false;
        /* fn8: re-assert the position hold at the tp target so subsequent
         * `step`s keep the player there (reach-stable). Vertical physics still
         * runs, so a tp into the air falls (fall-damage scenarios). */
        g_harness_hold   = true;
        g_harness_hold_x = cmd->tp.x;
        g_harness_hold_z = cmd->tp.z;
        break;
    }
    case CMD_SPAWN_MOB: {
        Server* sv = server_get_instance();
        int id = server_test_spawn_mob(sv, cmd->spawn_mob.type,
                                       cmd->spawn_mob.x, cmd->spawn_mob.y, cmd->spawn_mob.z);
        if (!id) agent_emit_error("spawn_mob: failed");
        break;
    }
    case CMD_SET_TIME:
        server_test_set_time(server_get_instance(), (uint32_t)cmd->set_time.ticks);
        break;
    case CMD_SET_WEATHER:
        server_test_set_weather(server_get_instance(), cmd->set_weather.kind);
        break;
    case CMD_SET_FOOD:
        if (!server_test_set_food(server_get_instance(), cmd->set_food.value))
            agent_emit_error("set_food: no server/client");
        break;
    case CMD_SET_HEALTH:
        if (!server_test_set_health(server_get_instance(), cmd->set_health.value))
            agent_emit_error("set_health: no server/client");
        break;

    case CMD_QUIT:
        return false;
    }
    return true;
}

static int run_headless_harness(uint16_t port)
{
    printf("[harness] headless agent harness starting (no renderer/window)\n");
    fflush(stdout);

    /* --- Integrated authoritative server, headless (renderer == NULL) ---
     * Bind to port 0 so the OS assigns an EPHEMERAL port. This makes many
     * harness processes run back-to-back (and even in parallel under ctest -j)
     * with zero collisions: no two share a fixed port, and a lingering
     * TIME_WAIT socket from a previous run can't make bind fail or let the
     * client connect to a stale server. We read the real port back from the
     * server (server_get_port) and connect the in-process client to it. */
    (void)port;                          /* harness ignores any --port; uses ephemeral */
    ServerArgs* sargs = malloc(sizeof(ServerArgs));
    sargs->port = 0;                     /* OS-assigned ephemeral port */
    sargs->max  = SERVER_MAX_CLIENTS;
    sargs->seed = WORLD_SEED;
    sargs->save_path[0] = '\0';          /* legacy world.dat overlay */
    sargs->renderer = NULL;              /* headless: server drives its own pipeline */
    sargs->render_distance = 0;
    sargs->gamemode = GAMEMODE_SURVIVAL;
    PT_Thread server_thread = {0};
    pt_thread_create(&server_thread, server_thread_func, sargs);

    /* Wait for the server to bind and publish its actual ephemeral port. */
    uint16_t srv_port = 0;
    for (int tries = 0; tries < 4000 && !srv_port; tries++) {
        pt_sleep_ms(1);
        srv_port = server_get_port();
    }
    if (!srv_port) { fprintf(stderr, "[harness] server never bound a port\n"); return 1; }

    /* --- Client simulation (connects to localhost over real UDP) --- */
    int net_fd = net_socket_client();
    NetThread net_thread;
    net_thread_start(&net_thread, net_fd);

    struct sockaddr_in srv_addr = {0};
    if (net_resolve("127.0.0.1", srv_port, &srv_addr) != NET_RESOLVE_OK) {
        fprintf(stderr, "[harness] could not resolve localhost\n");
        return 1;
    }

    Client client;
    RemotePlayerSet remote_players;
    ClientMobSet mob_set;
    client_init(&client, &net_thread, &srv_addr);
    remote_player_set_init(&remote_players);
    client_mob_set_init(&mob_set);
    g_remote_players = &remote_players;
    g_client         = &client;
    g_mobs           = &mob_set;
    client_set_snapshot_cb(&client, on_snapshot, NULL);
    client_set_leave_cb(&client, on_player_leave, NULL);
    client_set_mobs_cb(&client, on_mobs, NULL);
    client_set_death_cb(&client, on_death, NULL);
    /* Share the server's world in-process: the headless server drives its own
     * world_update around the player anchor, so we DON'T need streamed chunks
     * and can read the same authoritative world for collision + raycasts. */
    client_set_shared_world(&client, true);
    client_set_render_distance(&client, 8);
    client_connect(&client);

    /* Borrow the server's authoritative world for player physics + raycasts. */
    World* world = NULL;
    for (int tries = 0; tries < 4000 && !world; tries++) {
        world = server_get_world();
        if (!world) pt_sleep_ms(1);
    }
    if (!world) { fprintf(stderr, "[harness] server world never appeared\n"); return 1; }
    g_net_world = NULL;   /* world is borrowed; not network-fed */

    int spawn_y = worldgen_get_height(0, 0, WORLD_SEED) + 4;
    vec3 spawn = { 0.5f, (float)spawn_y, 0.5f };
    player_init(&g_player, spawn);
    g_player_ptr = &g_player;
    glm_vec3_copy(spawn, g_spawn_pos);
    g_player.agent_mode = true;
    g_player.mode = MODE_WALKING;

    /* Bring up the audio engine so SFX play-counts accumulate (mine-vibe-2mh).
     * The NULL/silent backend needs no device — it just records "would play X",
     * which is exactly what the get_state "sounds" snapshot reports. Counters
     * start at zero here, so any nonzero count in a scenario means the SFX
     * fired during that run. */
    audio_init();

    agent_init();

    const float dt = 1.0f / (float)SERVER_TICK_RATE;   /* fixed deterministic step */
    RaycastHit target = {0};
    uint64_t tick = 0;
    bool running = true;

    /* One simulation tick: drive player, refresh target, pump network. */
    #define HARNESS_SIM_TICK() do { \
        client_send_position(&client, g_player.position[0], g_player.position[1], \
                             g_player.position[2], g_player.camera.yaw, g_player.camera.pitch); \
        g_player.agent_jump = (g_player.agent_jump); /* edge handled by player_update */ \
        player_update(&g_player, NULL, world, dt); \
        if (g_harness_hold) { /* fn8: pin XZ, kill horizontal drift; let gravity run */ \
            g_player.position[0] = g_harness_hold_x; \
            g_player.position[2] = g_harness_hold_z; \
            g_player.velocity[0] = 0.0f; \
            g_player.velocity[2] = 0.0f; \
        } \
        { vec3 d; camera_get_front(&g_player.camera, d); \
          target = raycast_voxel(world, g_player.eye_pos, d, MAX_REACH); } \
        client_poll(&client); \
        for (int _i = 0; _i < client.pending_block_change_count; _i++) {} \
        client.pending_block_change_count = 0; \
        g_player.agent_jump = false; \
        tick++; \
    } while (0)

    /* Live in-process server handle for tick-synchronized stepping. The server
     * runs on its own thread at 20 Hz; rather than sleep a fixed wall-clock
     * duration and HOPE it processed our packets (the old flaky behaviour), we
     * block on its monotonic tick counter so a step provably reflects N
     * fully-processed server ticks. */
    Server* sv = server_get_instance();
    for (int tries = 0; tries < 4000 && !sv; tries++) { pt_sleep_ms(1); sv = server_get_instance(); }
    if (!sv) { fprintf(stderr, "[harness] server instance never appeared\n"); return 1; }

    /* Advance `n` server ticks SYNCHRONOUSLY on the server's REAL progress —
     * no fixed wall-clock sleep that races the 20 Hz server thread.
     *
     * Pacing matters: we run exactly ONE local sim tick (which sends one
     * position packet + polls) per OBSERVED server tick. Spamming
     * client_send_position in a tight 1ms loop floods both UDP socket buffers
     * (server broadcasts world/mob state every tick too), overflowing the
     * kernel receive queue and DROPPING reliable inventory/health packets
     * faster than the 0.1s retransmit recovers — which is exactly what made the
     * scenarios flaky. So between server ticks we only client_poll() + yield
     * 1ms (draining inbound, sending acks, applying snapshots) and we send a
     * new position only when the server tick advances.
     *
     * After hitting the target tick we poll once more so the client has applied
     * the snapshot the server produced AT/AFTER the barrier. A generous 5s
     * wall-clock safety timeout turns a genuine server hang into a clear error
     * + non-zero exit rather than wedging the whole ctest suite forever. */
    #define HARNESS_SYNC_TICKS(n) do { \
        uint64_t _target = server_current_tick(sv) + (uint64_t)(n); \
        double _deadline = net_time() + 5.0; \
        uint64_t _last_seen = server_current_tick(sv); \
        HARNESS_SIM_TICK(); /* send our latest position + poll at least once */ \
        while (server_current_tick(sv) < _target) { \
            uint64_t _cur = server_current_tick(sv); \
            if (_cur != _last_seen) { \
                _last_seen = _cur; \
                HARNESS_SIM_TICK();   /* one sim tick per real server tick */ \
            } else { \
                client_poll(&client); /* just drain/ack between ticks */ \
            } \
            if (net_time() > _deadline) { \
                fprintf(stderr, "[harness] FATAL: server tick stalled " \
                        "(have %llu, want %llu) after 5s — aborting\n", \
                        (unsigned long long)server_current_tick(sv), \
                        (unsigned long long)_target); \
                fflush(stderr); \
                exit(2); \
            } \
            pt_sleep_ms(1); \
        } \
        /* Settle: a few more polls so any reliable snapshot in flight (and its \
         * possible retransmit) lands and is applied before state is emitted. */ \
        for (int _s = 0; _s < 5; _s++) { client_poll(&client); pt_sleep_ms(2); } \
    } while (0)

    /* Settle: wait for the connect handshake + spawn-column generation, synced
     * on real server progress rather than a guessed sleep. */
    HARNESS_SYNC_TICKS(10);

    agent_emit_ready();

    while (running) {
        AgentCommand cmd;
        bool got = agent_pop_command(&cmd);
        if (!got) {
            /* Idle: brief sleep so we don't busy-spin while waiting for stdin.
             * The I/O thread is blocked on fgets; nothing to do until a command
             * arrives. A closed stdin makes agent_pop never return again, so we
             * also detect EOF via the active flag flipping is unnecessary — the
             * test always ends with `quit`. */
            pt_sleep_ms(2);
            if (!agent_is_active()) break;
            continue;
        }

        int step_ticks = 0;
        running = harness_apply_command(&cmd, &g_player, &client, world,
                                        &target, &step_ticks);
        if (!running) break;

        if (cmd.type == CMD_STEP) {
            /* Block until the server has genuinely advanced step_ticks ticks
             * (deterministic — no wall-clock race), then the client has the
             * resulting snapshot. */
            HARNESS_SYNC_TICKS(step_ticks);
        } else {
            /* Mutating commands (place/break/attack/craft/give/...): wait the
             * same tick-synchronized way for a few ticks so the server has
             * processed the request AND the client has applied the resulting
             * authoritative snapshot before the get_state below emits. 3 ticks
             * covers request -> server apply -> snapshot -> client apply. */
            HARNESS_SYNC_TICKS(3);
        }

        /* Emit a fresh state line after every command. */
        AgentSnapshot snap;
        harness_fill_snapshot(&snap, &g_player, &client, &mob_set, world,
                              &target, tick);
        agent_emit_snapshot(&snap);
    }

    #undef HARNESS_SYNC_TICKS
    #undef HARNESS_SIM_TICK

    /* Teardown. */
    g_client = NULL; g_mobs = NULL; g_remote_players = NULL;
    client_disconnect(&client);
    net_thread_stop(&net_thread);
    net_socket_close(net_fd);
    server_request_stop();
    pt_thread_join(server_thread);
    agent_destroy();
    audio_shutdown();
    printf("[harness] headless harness exited cleanly\n");
    fflush(stdout);
    return 0;
}
