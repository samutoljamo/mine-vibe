#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"
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
#include "platform_thread.h"
#include "raycast.h"
#include "gameplay.h"
#include "inventory.h"
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

static void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    (void)w; (void)xoff;
    int dir = (yoff > 0) ? -1 : 1;
    if (!g_client) return;
    g_client->inventory.selected =
        (g_client->inventory.selected + dir + INVENTORY_SLOTS) % INVENTORY_SLOTS;
}

static void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    (void)window;
    camera_process_mouse(&g_player.camera, xpos, ypos);
}

static void mouse_button_callback(GLFWwindow* w, int button, int action, int mods)
{
    (void)w; (void)mods;
    if (action != GLFW_PRESS) return;
    if (!g_client) return;
    if (!g_target.hit) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        client_send_break(g_client,
                          g_target.x, g_target.y, g_target.z,
                          (uint8_t)g_target.block);
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
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
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

typedef struct { uint16_t port; int max; int seed; } ServerArgs;
static void* server_thread_func(void* arg)
{
    ServerArgs* a = (ServerArgs*)arg;
    server_run(a->port, a->max, a->seed);
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

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--agent")  == 0) agent_mode  = true;
        else if (strcmp(argv[i], "--server") == 0) server_mode = true;
        else if (strcmp(argv[i], "--host")   == 0) host_mode   = true;
        else if (strcmp(argv[i], "--client") == 0) {
            client_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                connect_ip = argv[++i];
        }
    }

    /* Single-player is a loopback host: spawn an in-process server and
     * connect to it. Gameplay (inventory, block break/place) is server-
     * authoritative, so without a server there is no interaction. */
    if (!host_mode && !client_mode && !server_mode)
        host_mode = true;

    if (server_mode) {
        server_run(port, SERVER_MAX_CLIENTS, WORLD_SEED);
        return 0;
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    if (!agent_mode) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetKeyCallback(window, key_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
    }

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        fprintf(stderr, "Failed to init renderer\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    renderer_init_player_mesh(&renderer);

    PT_Thread server_thread = {0};
    if (host_mode) {
        ServerArgs* sargs = malloc(sizeof(ServerArgs));
        sargs->port = port;
        sargs->max  = SERVER_MAX_CLIENTS;
        sargs->seed = WORLD_SEED;
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

    int spawn_y = worldgen_get_height(0, 0, WORLD_SEED) + 4;
    vec3 g_spawn = { 0, (float)spawn_y, 0 };
    player_init(&g_player, g_spawn);
    g_player_ptr = &g_player;
    glm_vec3_copy(g_spawn, g_spawn_pos);
    g_player.agent_mode = agent_mode;
    World* world = world_create(&renderer, WORLD_SEED, 32);

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

            renderer_draw_frame(&renderer, meshes, mesh_count, NULL, 0, view, proj, sun_dir,
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

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

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
         * stack memory and pending_block_change_count would be garbage. */
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
                if (!world_set_block(world, x, y, z, b)) {
                    fprintf(stderr, "[main] world_set_block failed at (%d,%d,%d) block=%u; chunk unloaded or busy\n",
                            x, y, z, (unsigned)b);
                    continue;
                }
                /* The PKT_BLOCK_CHANGE protocol carries no meta channel, so
                 * network-placed water arrives with meta=0 — water_tick reads
                 * level 0, subtracts WATER_DISSIPATION, and removes the block
                 * on the very next tick. Tag player-placed water as a source
                 * so it persists and spreads like a bucket-placed block. */
                if (b == BLOCK_WATER) {
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
                rcount++;
            }
            for (int i = 0; i < MOB_MAX && rcount < REMOTE_PLAYER_MAX + MOB_MAX; i++) {
                ClientMob* m = &mob_set.mobs[i];
                if (!m->active || m->snapshot_count < 2) continue;
                vec3 pos; float yaw;
                client_mob_interpolate(m, dt, pos, &yaw);
                rp_states[rcount].pos[0] = pos[0];
                rp_states[rcount].pos[1] = pos[1];
                rp_states[rcount].pos[2] = pos[2];
                rp_states[rcount].yaw    = yaw;
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

        renderer_draw_frame(&renderer, meshes, mesh_count,
                            rcount > 0 ? rp_states : NULL, rcount,
                            view, proj, sun_dir,
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
            snap.selected_slot = 0;             /* TODO Task 11: wire from client.inventory */
            for (int i = 0; i < INVENTORY_SLOTS; i++)
                snap.hotbar[i] = 0;             /* TODO Task 11 */
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
    if (host_mode)
        pt_thread_join(server_thread); /* takes PT_Thread by value */

    world_destroy(world);
    block_physics_destroy(&physics);
    if (agent_mode) agent_destroy();
    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
