# Player Modes & Physics Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two player modes (free-fly and walking) with AABB collision, gravity, jumping, sprinting, water physics, and double-tap-space mode switching.

**Architecture:** A new `Player` struct owns position/velocity/mode and wraps `Camera` (now view-only). A new `physics` module provides AABB-vs-voxel collision. `world_get_block` exposes block queries. Physics runs at fixed 60Hz. The player module dispatches per-tick updates to free or walking mode handlers.

**Tech Stack:** C11, cglm, GLFW (input polling), existing voxel world/chunk system.

**Spec:** `docs/superpowers/specs/2026-03-17-player-modes-physics-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/physics.h` | Create | Collision function declarations |
| `src/physics.c` | Create | AABB vs voxel sweep, water detection |
| `src/player.h` | Create | Player struct, mode enum, public API |
| `src/player.c` | Create | Player init, input, fixed-timestep loop, free/walking ticks |
| `src/camera.h` | Modify | Remove position/speed fields, change signatures |
| `src/camera.c` | Modify | Remove `camera_process_keyboard`, update `camera_init`/`camera_get_view` |
| `src/world.h` | Modify | Add `world_get_block` declaration |
| `src/world.c` | Modify | Add `world_get_block` implementation |
| `src/main.c` | Modify | Replace `Camera` with `Player`, rewire loop |
| `CMakeLists.txt` | Modify | Add `player.c` and `physics.c` to sources |

---

### Task 1: Foundation — world_get_block + physics module

**Files:**
- Modify: `src/world.h`
- Modify: `src/world.c`
- Create: `src/physics.h`
- Create: `src/physics.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add `world_get_block` declaration to `src/world.h`**

Add `#include "block.h"` and the function declaration before the `#endif`:

```c
#include "block.h"

/* ... existing declarations ... */

BlockID world_get_block(World* world, int x, int y, int z);
```

- [ ] **Step 2: Implement `world_get_block` in `src/world.c`**

Add at the end of the file, before the closing of the public API section:

```c
BlockID world_get_block(World* world, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_Y) return BLOCK_AIR;

    int cx = (int)floorf((float)x / 16.0f);
    int cz = (int)floorf((float)z / 16.0f);

    Chunk* chunk = chunk_map_get(&world->map, cx, cz);
    if (!chunk) return BLOCK_AIR;
    if (atomic_load(&chunk->state) < CHUNK_GENERATED) return BLOCK_AIR;

    int lx = ((x % 16) + 16) % 16;
    int lz = ((z % 16) + 16) % 16;

    return chunk_get_block(chunk, lx, y, lz);
}
```

- [ ] **Step 3: Create `src/physics.h`**

```c
#ifndef PHYSICS_H
#define PHYSICS_H

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct World World;

typedef struct PhysicsResult {
    bool on_ground;
    bool in_water;
} PhysicsResult;

/*
 * Move player AABB through world, resolving collisions axis-by-axis.
 *   pos:    feet position (modified in place)
 *   vel:    velocity (components zeroed on collision)
 *   half_w: half-width of hitbox (0.3)
 *   height: hitbox height (1.8)
 *   dt:     time delta for this tick
 *   world:  world pointer for block queries
 */
PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, World* world);

bool physics_check_water(vec3 pos, float half_w, float height, World* world);

#endif
```

- [ ] **Step 4: Create `src/physics.c`**

```c
#include "physics.h"
#include "world.h"
#include "block.h"
#include <math.h>

/*
 * Sweep one axis: move pos[axis] by vel[axis]*dt, then check the resulting
 * AABB against all solid blocks.  If overlap is found, push position to the
 * nearest block face on that axis and zero the velocity component.
 *
 * For Y-axis falling, sets *on_ground = true.
 */
static void sweep_axis(vec3 pos, vec3 vel, float half_w, float height,
                       int axis, float delta, World* world, bool* on_ground)
{
    if (delta == 0.0f) return;
    pos[axis] += delta;

    float min_x = pos[0] - half_w, max_x = pos[0] + half_w;
    float min_y = pos[1],          max_y = pos[1] + height;
    float min_z = pos[2] - half_w, max_z = pos[2] + half_w;

    int bx0 = (int)floorf(min_x), bx1 = (int)floorf(max_x);
    int by0 = (int)floorf(min_y), by1 = (int)floorf(max_y);
    int bz0 = (int)floorf(min_z), bz1 = (int)floorf(max_z);

    for (int by = by0; by <= by1; by++)
    for (int bx = bx0; bx <= bx1; bx++)
    for (int bz = bz0; bz <= bz1; bz++) {
        if (!block_is_solid(world_get_block(world, bx, by, bz)))
            continue;

        /* Verify real overlap (handles exact-boundary cases) */
        float bf[3] = { (float)bx, (float)by, (float)bz };
        if (max_x <= bf[0] || min_x >= bf[0] + 1.0f) continue;
        if (max_y <= bf[1] || min_y >= bf[1] + 1.0f) continue;
        if (max_z <= bf[2] || min_z >= bf[2] + 1.0f) continue;

        /* Push position to nearest block face on this axis */
        if (axis == 1) { /* Y */
            if (delta < 0.0f) {
                pos[1] = bf[1] + 1.0f;
                *on_ground = true;
            } else {
                pos[1] = bf[1] - height;
            }
        } else { /* X or Z */
            if (delta < 0.0f)
                pos[axis] = bf[axis] + 1.0f + half_w;
            else
                pos[axis] = bf[axis] - half_w;
        }
        vel[axis] = 0.0f;

        /* Recompute bounds for remaining block checks */
        min_x = pos[0] - half_w; max_x = pos[0] + half_w;
        min_y = pos[1];          max_y = pos[1] + height;
        min_z = pos[2] - half_w; max_z = pos[2] + half_w;
    }
}

PhysicsResult physics_move(vec3 pos, vec3 vel, float half_w, float height,
                           float dt, World* world)
{
    PhysicsResult result = { false, false };

    /* Y first (ground detection), then X, then Z */
    sweep_axis(pos, vel, half_w, height, 1, vel[1] * dt, world, &result.on_ground);
    sweep_axis(pos, vel, half_w, height, 0, vel[0] * dt, world, &result.on_ground);
    sweep_axis(pos, vel, half_w, height, 2, vel[2] * dt, world, &result.on_ground);

    result.in_water = physics_check_water(pos, half_w, height, world);
    return result;
}

bool physics_check_water(vec3 pos, float half_w, float height, World* world)
{
    int bx0 = (int)floorf(pos[0] - half_w), bx1 = (int)floorf(pos[0] + half_w);
    int by0 = (int)floorf(pos[1]),           by1 = (int)floorf(pos[1] + height);
    int bz0 = (int)floorf(pos[2] - half_w), bz1 = (int)floorf(pos[2] + half_w);

    for (int by = by0; by <= by1; by++)
    for (int bx = bx0; bx <= bx1; bx++)
    for (int bz = bz0; bz <= bz1; bz++) {
        if (world_get_block(world, bx, by, bz) == BLOCK_WATER)
            return true;
    }
    return false;
}
```

- [ ] **Step 5: Add new source files to `CMakeLists.txt`**

In the `add_executable(minecraft ...)` block, add after `src/world.c`:

```cmake
    src/player.c
    src/physics.c
```

(player.c doesn't exist yet — create an empty file so the build doesn't fail.)

```c
/* src/player.c — placeholder, implemented in Task 2 */
#include "player.h"
```

And create the corresponding placeholder header:

```c
/* src/player.h — placeholder, implemented in Task 2 */
#ifndef PLAYER_H
#define PLAYER_H
#endif
```

- [ ] **Step 6: Build to verify**

Run: `cmake --build build -j$(nproc)`

Expected: Build succeeds with no errors. The new code compiles but nothing calls it yet.

- [ ] **Step 7: Commit**

```bash
git add src/physics.h src/physics.c src/world.h src/world.c src/player.h src/player.c CMakeLists.txt
git commit -m "feat: add world_get_block and physics collision module"
```

---

### Task 2: Camera refactor + player module + main.c integration

This task refactors camera to view-only, creates the player module with basic free-fly movement (matching current behavior), and rewires main.c. After this task the game should behave identically to before.

**Files:**
- Modify: `src/camera.h`
- Modify: `src/camera.c`
- Rewrite: `src/player.h`
- Rewrite: `src/player.c`
- Modify: `src/main.c`

- [ ] **Step 1: Rewrite `src/camera.h`**

Replace entire contents:

```c
#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct Camera {
    float yaw;       /* radians */
    float pitch;     /* radians */
    float sensitivity;
    float fov;
    float last_x, last_y;
    bool  first_mouse;
} Camera;

void camera_init(Camera* cam);
void camera_process_mouse(Camera* cam, double xpos, double ypos);
void camera_get_view(Camera* cam, vec3 eye_pos, mat4 out);
void camera_get_proj(Camera* cam, float aspect, mat4 out);
void camera_get_front(Camera* cam, vec3 out);

#endif
```

- [ ] **Step 2: Rewrite `src/camera.c`**

Replace entire contents:

```c
#include "camera.h"
#include <math.h>

void camera_init(Camera* cam)
{
    cam->yaw         = -(float)GLM_PI_2;
    cam->pitch       = 0.0f;
    cam->sensitivity = 0.002f;
    cam->fov         = glm_rad(70.0f);
    cam->first_mouse = true;
    cam->last_x      = 0.0f;
    cam->last_y      = 0.0f;
}

void camera_get_front(Camera* cam, vec3 out)
{
    out[0] = cosf(cam->pitch) * cosf(cam->yaw);
    out[1] = sinf(cam->pitch);
    out[2] = cosf(cam->pitch) * sinf(cam->yaw);
    glm_vec3_normalize(out);
}

void camera_process_mouse(Camera* cam, double xpos, double ypos)
{
    float xf = (float)xpos;
    float yf = (float)ypos;

    if (cam->first_mouse) {
        cam->last_x = xf;
        cam->last_y = yf;
        cam->first_mouse = false;
        return;
    }

    float dx = xf - cam->last_x;
    float dy = yf - cam->last_y;
    cam->last_x = xf;
    cam->last_y = yf;

    cam->yaw   += dx * cam->sensitivity;
    cam->pitch -= dy * cam->sensitivity;

    float limit = (float)GLM_PI_2 - 0.01f;
    if (cam->pitch >  limit) cam->pitch =  limit;
    if (cam->pitch < -limit) cam->pitch = -limit;
}

void camera_get_view(Camera* cam, vec3 eye_pos, mat4 out)
{
    vec3 front;
    camera_get_front(cam, front);

    vec3 center;
    glm_vec3_add(eye_pos, front, center);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    glm_lookat(eye_pos, center, up, out);
}

void camera_get_proj(Camera* cam, float aspect, mat4 out)
{
    glm_perspective(cam->fov, aspect, 0.1f, 1000.0f, out);
    out[1][1] *= -1.0f;
}
```

- [ ] **Step 3: Rewrite `src/player.h`**

Replace the placeholder:

```c
#ifndef PLAYER_H
#define PLAYER_H

#include "camera.h"
#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct GLFWwindow GLFWwindow;
typedef struct World World;

typedef enum PlayerMode {
    MODE_FREE,
    MODE_WALKING,
} PlayerMode;

typedef struct Player {
    Camera      camera;
    vec3        position;       /* feet position in world space */
    vec3        velocity;
    vec3        eye_pos;        /* computed: position + eye offset */
    PlayerMode  mode;
    bool        on_ground;
    bool        in_water;
    bool        sprinting;
    bool        noclip;         /* free mode: collision toggle */
    bool        prev_space;     /* edge detection */
    bool        prev_v;         /* edge detection */
    float       last_space_time;
    float       accumulator;    /* fixed-timestep dt accumulator */
} Player;

void player_init(Player* player, vec3 start_pos);
void player_update(Player* player, GLFWwindow* window, World* world, float dt);

#endif
```

- [ ] **Step 4: Rewrite `src/player.c` with basic free-fly**

This initial version uses direct position modification (matching old camera behavior exactly). It will be rewritten in Task 3 to use velocity + fixed timestep.

```c
#include "player.h"
#include <math.h>
#include <GLFW/glfw3.h>

#define PLAYER_EYE_H  1.62f
#define FLY_SPEED      20.0f

void player_init(Player* player, vec3 start_pos)
{
    camera_init(&player->camera);
    glm_vec3_copy(start_pos, player->position);
    glm_vec3_zero(player->velocity);
    glm_vec3_zero(player->eye_pos);
    player->mode            = MODE_FREE;
    player->on_ground       = false;
    player->in_water        = false;
    player->sprinting       = false;
    player->noclip          = true;
    player->prev_space      = false;
    player->prev_v          = false;
    player->last_space_time = 0.0f;
    player->accumulator     = 0.0f;
}

void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    (void)world;

    vec3 front;
    camera_get_front(&player->camera, front);

    /* Horizontal forward (XZ only) — matches old camera behavior */
    vec3 forward = { front[0], 0.0f, front[2] };
    glm_vec3_normalize(forward);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(forward, up, right);
    glm_vec3_normalize(right);

    float vel = FLY_SPEED * dt;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        player->position[0] += forward[0] * vel;
        player->position[2] += forward[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        player->position[0] -= forward[0] * vel;
        player->position[2] -= forward[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        player->position[0] -= right[0] * vel;
        player->position[2] -= right[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        player->position[0] += right[0] * vel;
        player->position[2] += right[2] * vel;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        player->position[1] += vel;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        player->position[1] -= vel;
    }

    /* Update eye position */
    glm_vec3_copy(player->position, player->eye_pos);
    player->eye_pos[1] += PLAYER_EYE_H;
}
```

- [ ] **Step 5: Rewrite `src/main.c`**

Replace entire contents:

```c
#include <stdio.h>
#include <stdbool.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"
#include "player.h"
#include "world.h"
#include "chunk_mesh.h"

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

int main(void)
{
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetKeyCallback(window, key_callback);

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        fprintf(stderr, "Failed to init renderer\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    player_init(&g_player, (vec3){0, 80, 0});
    World* world = world_create(&renderer, 42, 32);

    vec3 sun_dir = { -0.5f, -0.8f, -0.3f };
    glm_vec3_normalize(sun_dir);

    double last_time = glfwGetTime();
    int frame_count = 0;
    double fps_timer = last_time;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        glfwPollEvents();
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
    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 6: Build and verify**

Run: `cmake --build build -j$(nproc) && ./build/minecraft`

Expected: Game launches. Free-fly movement works identically to before (WASD + Space/Shift). The camera starts 1.62 blocks higher than before (eye at y≈81.62 instead of y=80) — this is expected since position is now feet-based.

- [ ] **Step 7: Commit**

```bash
git add src/camera.h src/camera.c src/player.h src/player.c src/main.c
git commit -m "refactor: camera becomes view-only, player owns position"
```

---

### Task 3: Proper free mode — velocity, fixed timestep, noclip, collision

Rewrite `player_update` in `src/player.c` to use velocity-based movement with a fixed 60Hz timestep. Add noclip toggle (V key) and collision when noclip is off.

**Files:**
- Modify: `src/player.c`

- [ ] **Step 1: Rewrite `src/player.c`**

Replace entire contents:

```c
#include "player.h"
#include "physics.h"
#include <math.h>
#include <GLFW/glfw3.h>

#define PLAYER_HALF_W  0.3f
#define PLAYER_HEIGHT  1.8f
#define PLAYER_EYE_H   1.62f
#define FLY_SPEED      20.0f
#define PHYSICS_DT     (1.0f / 60.0f)

void player_init(Player* player, vec3 start_pos)
{
    camera_init(&player->camera);
    glm_vec3_copy(start_pos, player->position);
    glm_vec3_zero(player->velocity);
    glm_vec3_zero(player->eye_pos);
    player->mode            = MODE_FREE;
    player->on_ground       = false;
    player->in_water        = false;
    player->sprinting       = false;
    player->noclip          = true;
    player->prev_space      = false;
    player->prev_v          = false;
    player->last_space_time = 0.0f;
    player->accumulator     = 0.0f;
}

static void tick_free(Player* player, GLFWwindow* window, World* world)
{
    vec3 front;
    camera_get_front(&player->camera, front);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(front, up, right);
    glm_vec3_normalize(right);

    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        dir[0] += front[0]; dir[1] += front[1]; dir[2] += front[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        dir[0] -= front[0]; dir[1] -= front[1]; dir[2] -= front[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        dir[0] -= right[0]; dir[2] -= right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        dir[0] += right[0]; dir[2] += right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        dir[1] += 1.0f;
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        dir[1] -= 1.0f;
        has_input = true;
    }

    /* Normalize and set velocity */
    if (has_input) {
        float len = glm_vec3_norm(dir);
        if (len > 0.0001f)
            glm_vec3_scale(dir, FLY_SPEED / len, player->velocity);
        else
            glm_vec3_zero(player->velocity);
    } else {
        glm_vec3_zero(player->velocity);
    }

    /* Move */
    if (player->noclip) {
        player->position[0] += player->velocity[0] * PHYSICS_DT;
        player->position[1] += player->velocity[1] * PHYSICS_DT;
        player->position[2] += player->velocity[2] * PHYSICS_DT;
    } else {
        physics_move(player->position, player->velocity,
                     PLAYER_HALF_W, PLAYER_HEIGHT, PHYSICS_DT, world);
    }
}

void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    /* Edge detection for V key (noclip toggle) */
    bool v_held = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
    bool v_pressed = v_held && !player->prev_v;
    player->prev_v = v_held;

    if (v_pressed && player->mode == MODE_FREE)
        player->noclip = !player->noclip;

    /* Fixed timestep */
    player->accumulator += dt;
    if (player->accumulator > 0.05f)
        player->accumulator = 0.05f;

    while (player->accumulator >= PHYSICS_DT) {
        tick_free(player, window, world);
        player->accumulator -= PHYSICS_DT;
    }

    /* Update eye position */
    glm_vec3_copy(player->position, player->eye_pos);
    player->eye_pos[1] += PLAYER_EYE_H;
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build -j$(nproc) && ./build/minecraft`

Expected:
- Free-fly works. W/S now moves in the 3D look direction (looking up + pressing W flies upward). This is a deliberate change from the old XZ-only forward.
- Press V to toggle noclip. With noclip off, you collide with terrain — you can't fly through solid blocks. With noclip on (default), you fly freely.
- Test collision: fly near ground, press V, try to fly into blocks.

- [ ] **Step 3: Commit**

```bash
git add src/player.c
git commit -m "feat: free mode with velocity, fixed timestep, noclip toggle"
```

---

### Task 4: Walking mode + mode switching

Add the walking mode tick (gravity, jumping, sprinting, drag, collision) and double-tap-space mode switching.

**Files:**
- Modify: `src/player.c`

- [ ] **Step 1: Rewrite `src/player.c`**

Replace entire contents with the final version that includes both modes:

```c
#include "player.h"
#include "physics.h"
#include <math.h>
#include <GLFW/glfw3.h>

#define PLAYER_HALF_W    0.3f
#define PLAYER_HEIGHT    1.8f
#define PLAYER_EYE_H     1.62f
#define FLY_SPEED        20.0f
#define WALK_SPEED       4.3f
#define SPRINT_SPEED     5.6f
#define SWIM_SPEED       2.0f
#define GRAVITY          25.2f
#define JUMP_VEL         7.95f
#define TERMINAL_VEL     78.4f
#define WATER_SINK       2.0f
#define SWIM_UP_VEL      4.0f
#define DRAG_GROUND      0.85f
#define DRAG_AIR         0.98f
#define DRAG_WATER       0.80f
#define WATER_Y_DRAG     0.80f
#define PHYSICS_DT       (1.0f / 60.0f)
#define DOUBLETAP_WINDOW 0.3f

void player_init(Player* player, vec3 start_pos)
{
    camera_init(&player->camera);
    glm_vec3_copy(start_pos, player->position);
    glm_vec3_zero(player->velocity);
    glm_vec3_zero(player->eye_pos);
    player->mode            = MODE_FREE;
    player->on_ground       = false;
    player->in_water        = false;
    player->sprinting       = false;
    player->noclip          = true;
    player->prev_space      = false;
    player->prev_v          = false;
    player->last_space_time = 0.0f;
    player->accumulator     = 0.0f;
}

/* ------------------------------------------------------------------ */
/*  Free mode tick                                                     */
/* ------------------------------------------------------------------ */

static void tick_free(Player* player, GLFWwindow* window, World* world)
{
    vec3 front;
    camera_get_front(&player->camera, front);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(front, up, right);
    glm_vec3_normalize(right);

    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        dir[0] += front[0]; dir[1] += front[1]; dir[2] += front[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        dir[0] -= front[0]; dir[1] -= front[1]; dir[2] -= front[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        dir[0] -= right[0]; dir[2] -= right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        dir[0] += right[0]; dir[2] += right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        dir[1] += 1.0f;
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        dir[1] -= 1.0f;
        has_input = true;
    }

    if (has_input) {
        float len = glm_vec3_norm(dir);
        if (len > 0.0001f)
            glm_vec3_scale(dir, FLY_SPEED / len, player->velocity);
        else
            glm_vec3_zero(player->velocity);
    } else {
        glm_vec3_zero(player->velocity);
    }

    if (player->noclip) {
        player->position[0] += player->velocity[0] * PHYSICS_DT;
        player->position[1] += player->velocity[1] * PHYSICS_DT;
        player->position[2] += player->velocity[2] * PHYSICS_DT;
    } else {
        physics_move(player->position, player->velocity,
                     PLAYER_HALF_W, PLAYER_HEIGHT, PHYSICS_DT, world);
    }
}

/* ------------------------------------------------------------------ */
/*  Walking mode tick                                                  */
/* ------------------------------------------------------------------ */

static void tick_walking(Player* player, GLFWwindow* window, World* world)
{
    /* 1. Compute desired movement direction on XZ plane */
    vec3 front;
    camera_get_front(&player->camera, front);

    vec3 forward = { front[0], 0.0f, front[2] };
    glm_vec3_normalize(forward);

    vec3 up = { 0.0f, 1.0f, 0.0f };
    vec3 right;
    glm_vec3_cross(forward, up, right);
    glm_vec3_normalize(right);

    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        dir[0] += forward[0]; dir[2] += forward[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        dir[0] -= forward[0]; dir[2] -= forward[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        dir[0] -= right[0]; dir[2] -= right[2];
        has_input = true;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        dir[0] += right[0]; dir[2] += right[2];
        has_input = true;
    }

    /* Normalize direction */
    float len = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (len > 0.0001f) {
        dir[0] /= len;
        dir[2] /= len;
    }

    /* 2. Sprint check */
    player->sprinting = has_input
        && (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
        && (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);

    float speed = WALK_SPEED;
    if (player->sprinting) speed = SPRINT_SPEED;
    if (player->in_water)  speed = SWIM_SPEED;

    /* Snap-to-speed: set horizontal velocity directly */
    if (has_input) {
        player->velocity[0] = dir[0] * speed;
        player->velocity[2] = dir[2] * speed;
    }

    /* 3. Gravity / water physics */
    if (!player->in_water) {
        if (!player->on_ground)
            player->velocity[1] -= GRAVITY * PHYSICS_DT;
    } else {
        /* Damp vertical velocity (prevents carrying fall speed into water) */
        player->velocity[1] *= WATER_Y_DRAG;
        player->velocity[1] -= WATER_SINK * PHYSICS_DT;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            player->velocity[1] = SWIM_UP_VEL;
    }

    /* 4. Terminal velocity */
    if (player->velocity[1] < -TERMINAL_VEL)
        player->velocity[1] = -TERMINAL_VEL;

    /* 5. Jump */
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS
        && player->on_ground && !player->in_water) {
        player->velocity[1] = JUMP_VEL;
    }

    /* 6. Collision */
    PhysicsResult result = physics_move(player->position, player->velocity,
                                        PLAYER_HALF_W, PLAYER_HEIGHT,
                                        PHYSICS_DT, world);
    player->on_ground = result.on_ground;
    player->in_water  = result.in_water;

    /* 7. Drag (only when no input) */
    if (!has_input) {
        float drag = DRAG_AIR;
        if (player->on_ground) drag = DRAG_GROUND;
        if (player->in_water)  drag = DRAG_WATER;
        player->velocity[0] *= drag;
        player->velocity[2] *= drag;
    }
}

/* ------------------------------------------------------------------ */
/*  Player update (called once per frame)                              */
/* ------------------------------------------------------------------ */

void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    /* Edge detection */
    bool space_held = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    bool space_pressed = space_held && !player->prev_space;
    player->prev_space = space_held;

    bool v_held = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
    bool v_pressed = v_held && !player->prev_v;
    player->prev_v = v_held;

    /* Mode switching: double-tap space */
    if (space_pressed) {
        float now = (float)glfwGetTime();
        if (player->last_space_time > 0.0f
            && (now - player->last_space_time) < DOUBLETAP_WINDOW) {
            if (player->mode == MODE_WALKING) {
                player->mode = MODE_FREE;
                player->noclip = true;
            } else {
                player->mode = MODE_WALKING;
            }
            glm_vec3_zero(player->velocity);
            player->on_ground = false;
            player->in_water  = false;
            player->last_space_time = 0.0f;
        } else {
            player->last_space_time = now;
        }
    }

    /* Noclip toggle (V key, free mode only) */
    if (v_pressed && player->mode == MODE_FREE)
        player->noclip = !player->noclip;

    /* Fixed timestep physics */
    player->accumulator += dt;
    if (player->accumulator > 0.05f)
        player->accumulator = 0.05f;

    while (player->accumulator >= PHYSICS_DT) {
        if (player->mode == MODE_FREE)
            tick_free(player, window, world);
        else
            tick_walking(player, window, world);
        player->accumulator -= PHYSICS_DT;
    }

    /* Update eye position */
    glm_vec3_copy(player->position, player->eye_pos);
    player->eye_pos[1] += PLAYER_EYE_H;
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build -j$(nproc) && ./build/minecraft`

Expected:
- Game starts in free-fly mode (same as before).
- **Double-tap space** to switch to walking mode. You should immediately fall and land on terrain.
- In walking mode: WASD to walk, Space to jump (~1.25 blocks), Ctrl+W to sprint (noticeably faster).
- Collision works: you can't walk through blocks. Sliding along walls works.
- **Double-tap space** again to return to free-fly mode.
- V key toggles noclip in free mode.
- Test jumping onto 1-block-high obstacles to verify jump height.

- [ ] **Step 3: Commit**

```bash
git add src/player.c
git commit -m "feat: walking mode with gravity, jumping, sprinting, mode switching"
```

---

### Task 5: Verify and polish

Final verification pass. Confirm all features work together correctly.

**Files:** None (testing only)

- [ ] **Step 1: Full feature test**

Run: `cmake --build build -j$(nproc) && ./build/minecraft`

Test checklist:
1. Free mode: WASD flies in look direction, Space/Shift for vertical
2. Free mode noclip: V toggles collision. Fly into terrain with noclip off — should collide
3. Mode switch: double-tap space toggles between free and walking
4. Walking: gravity pulls you down, land on terrain
5. Walking: Space to jump, verify you clear 1-block obstacles
6. Walking: Ctrl+W to sprint (faster movement)
7. Walking: find water (near sea level ≈62). Walk into it — you should sink slowly
8. Walking in water: Space to swim up, movement is slower
9. Walking: walk against walls — you slide along them, no getting stuck
10. Triple-tap space: should only toggle once (no double-toggle)

- [ ] **Step 2: Commit if any fixes were needed**

```bash
git add -u
git commit -m "fix: polish player modes and physics"
```

(Skip this step if no fixes were needed.)
