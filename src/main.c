#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
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
#include "mob_render.h"
#include "platform_thread.h"
#include "raycast.h"
#include "gameplay.h"
#include "inventory.h"
#include "crafting.h"
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
static bool g_show_stats = false; /* perf overlay visibility; toggled with F3 */

/* Crafting-panel row -> recipe-index map, rebuilt each frame the inventory
 * screen registers its hit-test rects so a click resolves to the right recipe.
 * g_craft_count rows are valid in g_craft_idx[]. */
static int g_craft_idx[HUD_CRAFT_ROWS];
static int g_craft_count = 0;

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
    if (!g_target.hit && !g_target_mob) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        /* Mob melee stays a single click. Block breaking is now timed: the
         * held-button progress is accumulated each frame in the main loop and
         * the PKT_BLOCK_BREAK is sent only when mining completes. */
        if (g_target_mob) {
            client_send_mob_attack(g_client, g_target_mob);
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
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
        /* Esc toggles pause from in-world and closes the inventory; in the main
         * menu it does nothing (use the Quit button to leave). */
        ui_set_state(game_ui_toggle_pause(g_ui_state));
    }
    if (key == GLFW_KEY_E && action == GLFW_PRESS) {
        /* E opens/closes the inventory screen (no-op in menu/pause). */
        ui_set_state(game_ui_toggle_inventory(g_ui_state));
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
static ClientMobSet*    g_mobs           = NULL;
static Player*          g_player_ptr     = NULL;
static vec3             g_spawn_pos      = {0, 0, 0};

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
} ServerArgs;
static void* server_thread_func(void* arg)
{
    ServerArgs* a = (ServerArgs*)arg;
    server_run_ex(a->port, a->max, a->seed, a->save_path,
                  a->renderer, a->render_distance);
    free(a);
    return NULL;
}

int main(int argc, char *argv[])
{
    bool agent_mode   = false;
    bool server_mode  = false;
    bool host_mode    = false;
    bool client_mode  = false;
    const char* connect_ip = "127.0.0.1";
    uint16_t    port       = NET_DEFAULT_PORT;

    /* Graphics/perf settings, defaulting to values that run well on
     * integrated GPUs. Override via CLI flags below. */
    RenderSettings gfx = render_settings_default();

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--agent")  == 0) agent_mode  = true;
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
        else if (strcmp(argv[i], "--present") == 0 && i + 1 < argc) {
            const char* m = argv[++i]; /* falls back to FIFO at init if unsupported */
            if      (strcmp(m, "mailbox")   == 0) gfx.present = PRESENT_PREF_MAILBOX;
            else if (strcmp(m, "immediate") == 0) gfx.present = PRESENT_PREF_IMMEDIATE;
            else                                  gfx.present = PRESENT_PREF_FIFO;
        }
        else if (strcmp(argv[i], "--stats") == 0) {
            g_show_stats = true;          /* start with the perf overlay on */
        }
    }

    printf("Settings: render-distance=%d chunks, msaa=%dx, aniso=%dx, present=%s\n",
           gfx.render_distance, gfx.msaa, gfx.aniso,
           gfx.present == PRESENT_PREF_MAILBOX   ? "mailbox" :
           gfx.present == PRESENT_PREF_IMMEDIATE ? "immediate" : "fifo");

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

        struct sockaddr_in srv_addr = {0};
        srv_addr.sin_family      = AF_INET;
        srv_addr.sin_port        = htons(port);
        inet_pton(AF_INET, connect_ip, &srv_addr.sin_addr);

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
        world = world_create(&renderer, session_seed, gfx.render_distance);
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

        glfwPollEvents();

        bool dump_frame = false;
        char dump_path[256] = {0};
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
        }

        if (world_active) {
        player_update(&g_player, window, world, dt);

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
                rcount++;
            }
            for (int i = 0; i < MOB_MAX && rcount < REMOTE_PLAYER_MAX + MOB_MAX; i++) {
                ClientMob* m = &mob_set.mobs[i];
                if (!m->active || m->snapshot_count < 2) continue;
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
                rp_states[rcount].scale[0] = def.half_w / MOB_RENDER_BASE_HALF_W;
                rp_states[rcount].scale[1] = def.height / MOB_RENDER_BASE_HEIGHT;
                rp_states[rcount].scale[2] = def.depth  / MOB_RENDER_BASE_DEPTH;
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

        /* FPS counter */
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
            printf("[stats] FPS %.1f  frametime %.2f ms  chunks %u  draws %u\n",
                   perf_stats_avg_fps(&perf),
                   perf_stats_avg_frametime_ms(&perf),
                   perf.visible_chunks, perf.draw_calls);
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
