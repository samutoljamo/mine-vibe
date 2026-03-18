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
#include "hud.h"
#include "net.h"
#include "net_thread.h"
#include "server.h"
#include "client.h"
#include "remote_player.h"
#include "player_model.h"
#include "platform_thread.h"
#include <arpa/inet.h>
#include <time.h>

#define WORLD_SEED 420

static Player g_player;
static HUD    g_hud;

static void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    (void)w; (void)xoff;
    int dir = (yoff > 0) ? -1 : 1;
    g_hud.selected_slot =
        (g_hud.selected_slot + dir + HUD_SLOT_COUNT) % HUD_SLOT_COUNT;
}

static void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    (void)window;
    camera_process_mouse(&g_player.camera, xpos, ypos);
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
        if (s >= 0 && s < HUD_SLOT_COUNT)
            g_hud.selected_slot = s;
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

typedef struct { uint16_t port; int max; } ServerArgs;
static void* server_thread_func(void* arg)
{
    ServerArgs* a = (ServerArgs*)arg;
    server_run(a->port, a->max);
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

    if (server_mode) {
        server_run(port, SERVER_MAX_CLIENTS);
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
        pt_thread_create(&server_thread, server_thread_func, sargs);
        /* Give server 200ms to bind before client tries to connect */
        struct timespec ts = { 0, 200000000 };
        nanosleep(&ts, NULL);
    }

    int net_fd = -1;
    NetThread net_thread;
    Client client;
    RemotePlayerSet remote_players;
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

        g_remote_players = &remote_players;
        client_set_snapshot_cb(&client, on_snapshot, NULL);
        client_set_leave_cb(&client, on_player_leave, NULL);
        client_connect(&client);
    }

    int spawn_y = worldgen_get_height(0, 0, WORLD_SEED) + 4;
    player_init(&g_player, (vec3){0, (float)spawn_y, 0});
    g_player.agent_mode = agent_mode;
    World* world = world_create(&renderer, WORLD_SEED, 32);

    BlockPhysics physics;
    block_physics_init(&physics);
    hud_init(&g_hud);
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
                                &g_hud, false, NULL);

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
        PlayerRenderState rp_states[REMOTE_PLAYER_MAX];
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
        }

        renderer_draw_frame(&renderer, meshes, mesh_count,
                            rcount > 0 ? rp_states : NULL, rcount,
                            view, proj, sun_dir,
                            &g_hud, dump_frame, dump_path);

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
            snap.selected_slot = g_hud.selected_slot;
            for (int i = 0; i < HUD_SLOT_COUNT; i++)
                snap.hotbar[i] = (int)g_hud.slot_blocks[i];
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
