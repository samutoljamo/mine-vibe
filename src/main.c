#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"
#include "player.h"
#include "world.h"
#include "chunk_mesh.h"
#include "worldgen.h"
#include "agent.h"

#define WORLD_SEED 420

static Player g_player;

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

int main(int argc, char *argv[])
{
    bool agent_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent") == 0) agent_mode = true;
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    if (!agent_mode) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetKeyCallback(window, key_callback);
    }

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        fprintf(stderr, "Failed to init renderer\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    int spawn_y = worldgen_get_height(0, 0, WORLD_SEED) + 4;
    player_init(&g_player, (vec3){0, (float)spawn_y, 0});
    g_player.agent_mode = agent_mode;
    World* world = world_create(&renderer, WORLD_SEED, 32);

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
            world_update(world, g_player.position);
            world_get_meshes(world, &meshes, &mesh_count);

            mat4 view, proj;
            camera_get_view(&g_player.camera, g_player.eye_pos, view);
            float aspect = (float)renderer.swapchain.extent.width
                         / (float)renderer.swapchain.extent.height;
            camera_get_proj(&g_player.camera, aspect, proj);

            renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir);

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

        world_update(world, g_player.position);

        ChunkMesh* meshes;
        uint32_t mesh_count;
        world_get_meshes(world, &meshes, &mesh_count);

        mat4 view, proj;
        camera_get_view(&g_player.camera, g_player.eye_pos, view);
        float aspect = (float)renderer.swapchain.extent.width
                     / (float)renderer.swapchain.extent.height;
        camera_get_proj(&g_player.camera, aspect, proj);

        renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir);

        if (agent_mode && dump_frame) {
            if (renderer_dump_frame(&renderer, dump_path))
                agent_emit_frame_saved(dump_path);
            else
                agent_emit_error("frame capture failed");
        }

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
    world_destroy(world);
    if (agent_mode) agent_destroy();
    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
