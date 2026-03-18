# Agent Control API Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pipe-based stdin/stdout JSON control API so AI agents can drive the player, read game state, and capture frames for visual debugging.

**Architecture:** A new `agent` module owns the command ring buffer and event output. An I/O background thread reads stdin and enqueues parsed commands; the main thread drains commands each tick and writes events to stdout. Frame capture uses a Vulkan staging buffer readback with `stb_image_write`.

**Tech Stack:** C11, pthreads (already used), stb_image_write (stb already a dep), VMA (already used)

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `src/agent.h` | Public API: types, lifecycle, command drain, event emit, testable parse/format functions |
| Create | `src/agent.c` | I/O thread, ring buffer, JSON parser, event output |
| Create | `tests/test_agent_json.c` | Unit tests for `agent_parse_command` and `agent_format_snapshot` |
| Modify | `src/player.h` | Add `agent_mode`, `agent_forward`, `agent_right`, `agent_jump`, `agent_sprint` fields |
| Modify | `src/player.c` | Read agent input fields instead of GLFW keys when `agent_mode` is true |
| Modify | `src/main.c` | `int main(int argc, char *argv[])`, `--agent` flag, lifecycle calls, apply commands, dump_frame trigger |
| Modify | `src/renderer.h` | Add `last_image_index` field; declare `renderer_dump_frame` |
| Modify | `src/renderer.c` | Store `last_image_index` in `renderer_draw_frame`; implement `renderer_dump_frame` |
| Modify | `src/world.c` | Call `agent_notify_chunk_loaded(cx, cz)` when chunk transitions to `CHUNK_READY` |
| Modify | `CMakeLists.txt` | Add `src/agent.c` to sources; add `test_agent` executable |

---

### Task 1: Scaffold `agent.h` and stub `agent.c`, wire into CMakeLists.txt

**Files:**
- Create: `src/agent.h`
- Create: `src/agent.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/agent.h`**

```c
#ifndef AGENT_H
#define AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Command types                                                      */
/* ------------------------------------------------------------------ */

typedef enum AgentCommandType {
    CMD_MOVE,
    CMD_LOOK,
    CMD_JUMP,
    CMD_SPRINT,
    CMD_MODE,
    CMD_GET_STATE,
    CMD_DUMP_FRAME,
    CMD_QUIT,
} AgentCommandType;

typedef struct AgentCommand {
    AgentCommandType type;
    union {
        struct { float forward; float right; }  move;
        struct { float yaw;     float pitch;  } look;   /* degrees, pitch clamped [-90,90] */
        struct { int   active;                } sprint;
        struct { int   mode;                  } mode;   /* 0=free, 1=walk */
        struct { char  path[256];             } dump_frame;
    };
} AgentCommand;

/* ------------------------------------------------------------------ */
/*  Snapshot (main thread → stdout)                                   */
/* ------------------------------------------------------------------ */

typedef struct AgentSnapshot {
    float    pos[3];
    float    vel[3];
    float    yaw;        /* degrees */
    float    pitch;      /* degrees */
    int      on_ground;
    int      mode;       /* 0=free, 1=walk */
    uint64_t tick;
} AgentSnapshot;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Lifecycle — call from main thread only */
void agent_init(void);
void agent_destroy(void);
bool agent_is_active(void);

/* Main thread: drain one pending command, returns false when queue empty */
bool agent_pop_command(AgentCommand *out);

/* Main thread: emit events to stdout (thread-safe, mutex-protected) */
void agent_emit_snapshot(const AgentSnapshot *snap);
void agent_notify_chunk_loaded(int cx, int cz);
void agent_emit_ready(void);
void agent_emit_frame_saved(const char *path);
void agent_emit_error(const char *msg);

/* Testable pure functions (no global state) */
bool agent_parse_command(const char *line, AgentCommand *out);
void agent_format_snapshot(const AgentSnapshot *snap, char *buf, size_t buf_size);

#endif /* AGENT_H */
```

- [ ] **Step 2: Create stub `src/agent.c`**

```c
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

static bool g_active = false;

void  agent_init(void)    { g_active = true; }
void  agent_destroy(void) { g_active = false; }
bool  agent_is_active(void) { return g_active; }
bool  agent_pop_command(AgentCommand *out) { (void)out; return false; }
void  agent_emit_snapshot(const AgentSnapshot *s) { (void)s; }
void  agent_notify_chunk_loaded(int cx, int cz) { (void)cx; (void)cz; }
void  agent_emit_ready(void) {}
void  agent_emit_frame_saved(const char *p) { (void)p; }
void  agent_emit_error(const char *m) { (void)m; }
bool  agent_parse_command(const char *line, AgentCommand *out) { (void)line; (void)out; return false; }
void  agent_format_snapshot(const AgentSnapshot *s, char *buf, size_t n) { (void)s; (void)buf; (void)n; }
```

- [ ] **Step 3: Add `src/agent.c` to CMakeLists.txt**

In the `add_executable(minecraft ...)` block, add `src/agent.c` after `src/physics.c`.

Also add the test target at the end of CMakeLists.txt, before the final closing:

```cmake
# Agent JSON unit tests (no Vulkan/GLFW required)
add_executable(test_agent tests/test_agent_json.c src/agent.c)
target_include_directories(test_agent PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_agent PRIVATE Threads::Threads m)
```

- [ ] **Step 4: Verify build**

```bash
cd /var/home/samu/minecraft
cmake -B build -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
cmake --build build --target minecraft test_agent -- -j$(nproc) 2>&1 | tail -10
```

Expected: both targets build without errors.

- [ ] **Step 5: Commit scaffold**

```bash
git add src/agent.h src/agent.c CMakeLists.txt
git commit -m "feat: scaffold agent module with stub implementations"
```

---

### Task 2: Implement `agent_parse_command` with TDD

**Files:**
- Create: `tests/test_agent_json.c`
- Modify: `src/agent.c`

- [ ] **Step 1: Create `tests/` directory and write failing tests**

```bash
mkdir -p /var/home/samu/minecraft/tests
```

Create `tests/test_agent_json.c`:

```c
#include "agent.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

static void test_parse_move(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"move\",\"forward\":1.0,\"right\":-0.5}", &cmd));
    assert(cmd.type == CMD_MOVE);
    assert(fabsf(cmd.move.forward - 1.0f) < 0.001f);
    assert(fabsf(cmd.move.right - (-0.5f)) < 0.001f);
}

static void test_parse_look(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"look\",\"yaw\":90.0,\"pitch\":-45.0}", &cmd));
    assert(cmd.type == CMD_LOOK);
    assert(fabsf(cmd.look.yaw - 90.0f) < 0.001f);
    assert(fabsf(cmd.look.pitch - (-45.0f)) < 0.001f);
}

static void test_parse_look_pitch_clamp(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"look\",\"yaw\":0.0,\"pitch\":95.0}", &cmd));
    assert(cmd.type == CMD_LOOK);
    assert(fabsf(cmd.look.pitch - 90.0f) < 0.001f);

    assert(agent_parse_command("{\"cmd\":\"look\",\"yaw\":0.0,\"pitch\":-95.0}", &cmd));
    assert(fabsf(cmd.look.pitch - (-90.0f)) < 0.001f);
}

static void test_parse_jump(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"jump\"}", &cmd));
    assert(cmd.type == CMD_JUMP);
}

static void test_parse_sprint(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"sprint\",\"active\":1}", &cmd));
    assert(cmd.type == CMD_SPRINT);
    assert(cmd.sprint.active == 1);
}

static void test_parse_mode(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"mode\",\"value\":\"walk\"}", &cmd));
    assert(cmd.type == CMD_MODE);
    assert(cmd.mode.mode == 1);

    assert(agent_parse_command("{\"cmd\":\"mode\",\"value\":\"free\"}", &cmd));
    assert(cmd.mode.mode == 0);
}

static void test_parse_get_state(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"get_state\"}", &cmd));
    assert(cmd.type == CMD_GET_STATE);
}

static void test_parse_dump_frame(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"dump_frame\",\"path\":\"frame_001.png\"}", &cmd));
    assert(cmd.type == CMD_DUMP_FRAME);
    assert(strcmp(cmd.dump_frame.path, "frame_001.png") == 0);
}

static void test_parse_quit(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"quit\"}", &cmd));
    assert(cmd.type == CMD_QUIT);
}

static void test_parse_unknown_returns_false(void) {
    AgentCommand cmd;
    assert(!agent_parse_command("{\"cmd\":\"fly\"}", &cmd));
}

static void test_format_snapshot(void) {
    AgentSnapshot snap = {
        .pos      = {1.5f, 65.0f, -3.0f},
        .vel      = {0.0f,  0.0f,   0.0f},
        .yaw      = 45.0f,
        .pitch    = -10.0f,
        .on_ground = 1,
        .mode     = 1,
        .tick     = 42,
    };
    char buf[512];
    agent_format_snapshot(&snap, buf, sizeof(buf));
    /* Must contain required keys */
    assert(strstr(buf, "\"event\":\"state\"") != NULL);
    assert(strstr(buf, "\"tick\":42")        != NULL);
    assert(strstr(buf, "\"on_ground\":1")    != NULL);
}

int main(void) {
    test_parse_move();
    test_parse_look();
    test_parse_look_pitch_clamp();
    test_parse_jump();
    test_parse_sprint();
    test_parse_mode();
    test_parse_get_state();
    test_parse_dump_frame();
    test_parse_quit();
    test_parse_unknown_returns_false();
    test_format_snapshot();
    printf("All agent JSON tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Run tests to verify they fail (stub returns false)**

```bash
cmake --build /var/home/samu/minecraft/build --target test_agent -- -j$(nproc) 2>&1 | tail -5
/var/home/samu/minecraft/build/test_agent
```

Expected: assertion failure (stub always returns false).

- [ ] **Step 3: Implement `agent_parse_command` and `agent_format_snapshot` in `agent.c`**

Replace the stubs with real implementations. Use `strstr` for key detection and `sscanf` for value extraction:

```c
bool agent_parse_command(const char *line, AgentCommand *out)
{
    if (!line || !out) return false;

    /* Extract "cmd" value */
    const char *cmd_pos = strstr(line, "\"cmd\"");
    if (!cmd_pos) return false;
    char cmd_str[32] = {0};
    if (sscanf(cmd_pos, "\"cmd\":\"%31[^\"]\"", cmd_str) != 1) return false;

    if (strcmp(cmd_str, "move") == 0) {
        out->type = CMD_MOVE;
        out->move.forward = 0.0f;
        out->move.right   = 0.0f;
        sscanf(line, "%*[^f]forward\":%f", &out->move.forward);
        sscanf(line, "%*[^r]right\":%f",   &out->move.right);
        return true;
    }
    if (strcmp(cmd_str, "look") == 0) {
        out->type = CMD_LOOK;
        out->look.yaw   = 0.0f;
        out->look.pitch = 0.0f;
        sscanf(line, "%*[^y]yaw\":%f",   &out->look.yaw);
        sscanf(line, "%*[^p]pitch\":%f", &out->look.pitch);
        /* Clamp pitch */
        if (out->look.pitch >  90.0f) out->look.pitch =  90.0f;
        if (out->look.pitch < -90.0f) out->look.pitch = -90.0f;
        return true;
    }
    if (strcmp(cmd_str, "jump") == 0) {
        out->type = CMD_JUMP;
        return true;
    }
    if (strcmp(cmd_str, "sprint") == 0) {
        out->type = CMD_SPRINT;
        out->sprint.active = 0;
        sscanf(line, "%*[^a]active\":%d", &out->sprint.active);
        return true;
    }
    if (strcmp(cmd_str, "mode") == 0) {
        out->type = CMD_MODE;
        char val[8] = {0};
        const char *vp = strstr(line, "\"value\"");
        if (!vp) return false;
        sscanf(vp, "\"value\":\"%7[^\"]\"", val);
        if      (strcmp(val, "walk") == 0) out->mode.mode = 1;
        else if (strcmp(val, "free") == 0) out->mode.mode = 0;
        else return false;
        return true;
    }
    if (strcmp(cmd_str, "get_state") == 0) {
        out->type = CMD_GET_STATE;
        return true;
    }
    if (strcmp(cmd_str, "dump_frame") == 0) {
        out->type = CMD_DUMP_FRAME;
        out->dump_frame.path[0] = '\0';
        const char *pp = strstr(line, "\"path\"");
        if (!pp) return false;
        sscanf(pp, "\"path\":\"%255[^\"]\"", out->dump_frame.path);
        return true;
    }
    if (strcmp(cmd_str, "quit") == 0) {
        out->type = CMD_QUIT;
        return true;
    }
    return false;
}

void agent_format_snapshot(const AgentSnapshot *snap, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size,
        "{\"event\":\"state\","
        "\"pos\":[%.3f,%.3f,%.3f],"
        "\"vel\":[%.3f,%.3f,%.3f],"
        "\"yaw\":%.3f,\"pitch\":%.3f,"
        "\"mode\":\"%s\","
        "\"on_ground\":%d,"
        "\"tick\":%" PRIu64 "}\n",
        snap->pos[0], snap->pos[1], snap->pos[2],
        snap->vel[0], snap->vel[1], snap->vel[2],
        snap->yaw, snap->pitch,
        snap->mode == 0 ? "free" : "walk",
        snap->on_ground,
        snap->tick);
}
```

Add `#include <inttypes.h>` at the top of `agent.c` (for `PRIu64`).

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build /var/home/samu/minecraft/build --target test_agent -- -j$(nproc) 2>&1 | tail -5
/var/home/samu/minecraft/build/test_agent
```

Expected output: `All agent JSON tests passed.`

- [ ] **Step 5: Commit**

```bash
git add tests/test_agent_json.c src/agent.c
git commit -m "feat: implement agent JSON parser and snapshot formatter (TDD)"
```

---

### Task 3: Implement command ring buffer and I/O thread

**Files:**
- Modify: `src/agent.c`

- [ ] **Step 1: Replace stubs with full implementation in `agent.c`**

Replace the entire `agent.c` with the complete implementation. Keep `agent_parse_command` and `agent_format_snapshot` exactly as written in Task 2; add the threading infrastructure around them:

```c
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/*  Ring buffer (capacity must be power of two)                       */
/* ------------------------------------------------------------------ */
#define CMD_RING_CAP 64

typedef struct {
    AgentCommand   cmds[CMD_RING_CAP];
    int            head;   /* main thread reads here */
    int            tail;   /* I/O thread writes here */
    pthread_mutex_t mtx;
} CmdRing;

static bool      g_active = false;
static CmdRing   g_ring;
static pthread_t g_io_thread;
static pthread_mutex_t g_stdout_mtx;

/* ------------------------------------------------------------------ */
/*  Internal: stdout helper                                           */
/* ------------------------------------------------------------------ */
static void emit_raw(const char *s)
{
    pthread_mutex_lock(&g_stdout_mtx);
    fputs(s, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_stdout_mtx);
}

/* ------------------------------------------------------------------ */
/*  I/O thread: reads stdin, pushes commands                          */
/* ------------------------------------------------------------------ */
static void *io_thread_func(void *arg)
{
    (void)arg;
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        AgentCommand cmd;
        if (!agent_parse_command(line, &cmd)) {
            char err[128];
            snprintf(err, sizeof(err),
                "{\"event\":\"error\",\"msg\":\"unknown command\"}\n");
            emit_raw(err);
            continue;
        }

        pthread_mutex_lock(&g_ring.mtx);
        int next_tail = (g_ring.tail + 1) % CMD_RING_CAP;
        if (next_tail == g_ring.head) {
            /* Queue full: tail drop */
            pthread_mutex_unlock(&g_ring.mtx);
            emit_raw("{\"event\":\"error\",\"msg\":\"command queue full, command dropped\"}\n");
            continue;
        }
        g_ring.cmds[g_ring.tail] = cmd;
        g_ring.tail = next_tail;
        pthread_mutex_unlock(&g_ring.mtx);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
void agent_init(void)
{
    g_active = true;
    g_ring.head = 0;
    g_ring.tail = 0;
    pthread_mutex_init(&g_ring.mtx, NULL);
    pthread_mutex_init(&g_stdout_mtx, NULL);
    pthread_create(&g_io_thread, NULL, io_thread_func, NULL);
}

void agent_destroy(void)
{
    if (!g_active) return;
    g_active = false;
    /* Note: I/O thread will exit on its own when stdin closes (game exit) */
    pthread_join(g_io_thread, NULL);
    pthread_mutex_destroy(&g_ring.mtx);
    pthread_mutex_destroy(&g_stdout_mtx);
}

bool agent_is_active(void) { return g_active; }

bool agent_pop_command(AgentCommand *out)
{
    pthread_mutex_lock(&g_ring.mtx);
    if (g_ring.head == g_ring.tail) {
        pthread_mutex_unlock(&g_ring.mtx);
        return false;
    }
    *out = g_ring.cmds[g_ring.head];
    g_ring.head = (g_ring.head + 1) % CMD_RING_CAP;
    pthread_mutex_unlock(&g_ring.mtx);
    return true;
}

void agent_emit_snapshot(const AgentSnapshot *snap)
{
    char buf[512];
    agent_format_snapshot(snap, buf, sizeof(buf));
    emit_raw(buf);
}

void agent_notify_chunk_loaded(int cx, int cz)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"chunk_loaded\",\"cx\":%d,\"cz\":%d}\n", cx, cz);
    emit_raw(buf);
}

void agent_emit_ready(void)
{
    emit_raw("{\"event\":\"ready\"}\n");
}

void agent_emit_frame_saved(const char *path)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"frame_saved\",\"path\":\"%s\"}\n", path);
    emit_raw(buf);
}

void agent_emit_error(const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"error\",\"msg\":\"%s\"}\n", msg);
    emit_raw(buf);
}

/* ---- Pure functions kept exactly as in Task 2 ---- */

bool agent_parse_command(const char *line, AgentCommand *out)
{
    /* ... exact code from Task 2 ... */
}

void agent_format_snapshot(const AgentSnapshot *snap, char *buf, size_t buf_size)
{
    /* ... exact code from Task 2 ... */
}
```

> **Note:** The `/* ... exact code from Task 2 ... */` placeholders are intentional — copy the complete `agent_parse_command` and `agent_format_snapshot` bodies written in Task 2, Step 3 verbatim into this file. Do not leave the placeholder comments in the final code; the file will not compile if you do.

- [ ] **Step 2: Build and re-run tests to confirm nothing broke**

```bash
cmake --build /var/home/samu/minecraft/build --target test_agent minecraft -- -j$(nproc) 2>&1 | tail -10
/var/home/samu/minecraft/build/test_agent
```

Expected: `All agent JSON tests passed.` and `minecraft` binary built successfully.

- [ ] **Step 3: Commit**

```bash
git add src/agent.c
git commit -m "feat: implement agent ring buffer and I/O thread"
```

---

### Task 4: Wire `--agent` into `main.c`

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Update `main.c`**

Change `int main(void)` to `int main(int argc, char *argv[])`. Skip GLFW input callbacks and cursor lock in agent mode. Call `agent_init/tick/destroy`. Add a `dump_frame` flag that is set when a `CMD_DUMP_FRAME` command is drained.

Also add a static tick counter and the loading-screen gate for `agent_emit_ready`.

Add `#include "agent.h"` at the top.

The updated `main.c`:

```c
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
#include "agent.h"

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

/* Apply one agent command from the queue */
static bool apply_agent_command(const AgentCommand *cmd, Player *player,
                                 Renderer *renderer,
                                 bool *dump_frame, char *dump_path)
{
    switch (cmd->type) {
    case CMD_MOVE:
        player->agent_forward = cmd->move.forward;
        player->agent_right   = cmd->move.right;
        break;
    case CMD_LOOK: {
        /* Convert degrees to radians; Camera stores radians */
        float yaw_rad   =  cmd->look.yaw   * (3.14159265f / 180.0f);
        float pitch_rad =  cmd->look.pitch  * (3.14159265f / 180.0f);
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
        /* Snapshot emitted at end of agent_tick — no extra action needed */
        break;
    case CMD_DUMP_FRAME:
        *dump_frame = true;
        strncpy(dump_path, cmd->dump_frame.path, 255);
        dump_path[255] = '\0';
        break;
    case CMD_QUIT:
        return false; /* signal caller to exit */
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

    player_init(&g_player, (vec3){0, 80, 0});
    g_player.agent_mode = agent_mode;

    World* world = world_create(&renderer, 42, 32);

    if (agent_mode) agent_init();

    vec3 sun_dir = { -0.5f, -0.8f, -0.3f };
    glm_vec3_normalize(sun_dir);

    double last_time = glfwGetTime();
    int    frame_count = 0;
    double fps_timer   = last_time;
    uint64_t tick = 0;
    bool ready_emitted = false;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        glfwPollEvents();

        /* Drain agent commands */
        bool dump_frame = false;
        char dump_path[256] = {0};
        if (agent_mode) {
            AgentCommand cmd;
            /* Reset per-frame edge-triggered inputs */
            g_player.agent_jump = false;
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

        /* Emit ready once loading gate clears */
        if (agent_mode && !ready_emitted) {
            uint32_t total = (uint32_t)((2*32+1)*(2*32+1));
            if (world_get_ready_count(world) >= total / 4) {
                agent_emit_ready();
                ready_emitted = true;
            }
        }

        ChunkMesh* meshes;
        uint32_t mesh_count;
        world_get_meshes(world, &meshes, &mesh_count);

        mat4 view, proj;
        camera_get_view(&g_player.camera, g_player.eye_pos, view);
        float aspect = (float)renderer.swapchain.extent.width
                     / (float)renderer.swapchain.extent.height;
        camera_get_proj(&g_player.camera, aspect, proj);

        renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir);

        /* Frame capture (after draw, uses last_image_index) */
        if (agent_mode && dump_frame) {
            if (renderer_dump_frame(&renderer, dump_path)) {
                agent_emit_frame_saved(dump_path);
            } else {
                agent_emit_error("frame capture failed");
            }
        }

        /* Emit state snapshot once per frame */
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
    if (agent_mode) agent_destroy();
    world_destroy(world);
    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 2: Build to verify it compiles**

```bash
cmake --build /var/home/samu/minecraft/build --target minecraft -- -j$(nproc) 2>&1 | tail -10
```

Expected: compiles. It will fail to link `renderer_dump_frame` — that is expected and will be fixed in Task 6.

- [ ] **Step 3: Commit**

```bash
git add src/main.c
git commit -m "feat: wire --agent flag and agent lifecycle into main.c"
```

---

### Task 5: Add agent input fields to `player.h/.c`

**Files:**
- Modify: `src/player.h`
- Modify: `src/player.c`

- [ ] **Step 1: Add agent input fields to `Player` struct in `player.h`**

After the `accumulator` field, add:

```c
    /* Agent mode input — set by main.c before player_update each frame */
    bool  agent_mode;
    float agent_forward;  /* [-1, 1]; nonzero = key held */
    float agent_right;    /* [-1, 1]; nonzero = key held */
    bool  agent_jump;     /* edge-triggered: set true for one frame */
    bool  agent_sprint;
```

- [ ] **Step 2: Update `player_init` in `player.c` to zero the new fields**

After `player->accumulator = 0.0f;`, add:

```c
    player->agent_mode    = false;
    player->agent_forward = 0.0f;
    player->agent_right   = 0.0f;
    player->agent_jump    = false;
    player->agent_sprint  = false;
```

- [ ] **Step 3: Update `tick_free` in `player.c` to use agent input when active**

Replace the GLFW key polling block in `tick_free` with a conditional:

```c
    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;

    if (player->agent_mode) {
        if (player->agent_forward != 0.0f) {
            dir[0] += front[0] * (player->agent_forward > 0 ? 1.0f : -1.0f);
            dir[1] += front[1] * (player->agent_forward > 0 ? 1.0f : -1.0f);
            dir[2] += front[2] * (player->agent_forward > 0 ? 1.0f : -1.0f);
            has_input = true;
        }
        if (player->agent_right != 0.0f) {
            dir[0] += right[0] * (player->agent_right > 0 ? 1.0f : -1.0f);
            dir[2] += right[2] * (player->agent_right > 0 ? 1.0f : -1.0f);
            has_input = true;
        }
        if (player->agent_jump) {
            dir[1] += 1.0f;
            has_input = true;
        }
    } else {
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { ... } /* existing code */
        /* ... rest of existing GLFW block unchanged ... */
    }
```

- [ ] **Step 4: Update `tick_walking` in `player.c` to use agent input when active**

Replace the GLFW key polling block in `tick_walking` similarly:

```c
    vec3 dir = { 0.0f, 0.0f, 0.0f };
    bool has_input = false;

    if (player->agent_mode) {
        if (player->agent_forward != 0.0f) {
            dir[0] += forward[0] * (player->agent_forward > 0 ? 1.0f : -1.0f);
            dir[2] += forward[2] * (player->agent_forward > 0 ? 1.0f : -1.0f);
            has_input = true;
        }
        if (player->agent_right != 0.0f) {
            dir[0] += right[0] * (player->agent_right > 0 ? 1.0f : -1.0f);
            dir[2] += right[2] * (player->agent_right > 0 ? 1.0f : -1.0f);
            has_input = true;
        }
        /* Sprint: agent_sprint flag */
        player->sprinting = has_input && player->agent_sprint;
    } else {
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { ... } /* existing code */
        /* ... rest of existing GLFW block unchanged ... */
    }
```

For water swim-up in `tick_walking`, add an agent branch for the space/swim-up check:

```c
    if (player->in_water) {
        /* ... existing water drag code ... */
        bool swim_up = player->agent_mode ? player->agent_jump
                                          : glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (swim_up)
            player->velocity[1] = SWIM_UP_VEL;
    }
```

For jump in `tick_walking`:

```c
    bool do_jump = player->agent_mode ? player->agent_jump
                                      : (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
    if (do_jump && player->on_ground && !player->in_water) {
        player->velocity[1] = JUMP_VEL;
    }
```

Also in `player_update`, skip the GLFW edge-detection block entirely when in agent mode (the double-tap mode switch and V key noclip toggle are both bypassed — `CMD_MODE` handles mode switching directly from main.c):

```c
void player_update(Player* player, GLFWwindow* window, World* world, float dt)
{
    if (!player->agent_mode) {
        /* Edge detection */
        bool space_held = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        bool space_pressed = space_held && !player->prev_space;
        player->prev_space = space_held;

        bool v_held = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
        bool v_pressed = v_held && !player->prev_v;
        player->prev_v = v_held;

        /* Mode switching: double-tap space */
        /* ... existing mode-switch block ... */

        /* Noclip toggle (V key, free mode only) */
        /* ... existing noclip block ... */
    }

    /* Fixed timestep physics — unchanged */
    player->accumulator += dt;
    /* ... rest unchanged ... */
}
```

- [ ] **Step 5: Build to verify**

```bash
cmake --build /var/home/samu/minecraft/build --target minecraft -- -j$(nproc) 2>&1 | tail -10
```

Expected: compiles (still missing `renderer_dump_frame` link error is expected).

- [ ] **Step 6: Commit**

```bash
git add src/player.h src/player.c
git commit -m "feat: add agent input fields to player, bypass GLFW in agent mode"
```

---

### Task 6: Implement `renderer_dump_frame`

**Files:**
- Modify: `src/renderer.h`
- Modify: `src/renderer.c`

- [ ] **Step 1: Update `renderer.h`**

Add `last_image_index` field to `Renderer` struct after `current_frame`:

```c
    uint32_t                    current_frame;
    uint32_t                    last_image_index;   /* set each frame for dump_frame */
```

Add `renderer_dump_frame` declaration after `renderer_cleanup`:

```c
bool renderer_dump_frame(Renderer* r, const char *path);
```

- [ ] **Step 2: Store `last_image_index` in `renderer_draw_frame` in `renderer.c`**

At the point where `image_index` is acquired (after the `vkAcquireNextImageKHR` call succeeds), add:

```c
    r->last_image_index = image_index;
```

Place this immediately after the `if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)` guard, before step 3 (Reset fence).

- [ ] **Step 3: Add `#define STB_IMAGE_WRITE_IMPLEMENTATION` to `renderer.c`**

`STB_IMAGE_IMPLEMENTATION` is defined in `texture.c`. Add only the write implementation to `renderer.c` at the top — it must be its own translation unit:

```c
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
```

- [ ] **Step 4: Implement `renderer_dump_frame` in `renderer.c`**

Add this function before `renderer_cleanup`:

```c
bool renderer_dump_frame(Renderer* r, const char *path)
{
    uint32_t width  = r->swapchain.extent.width;
    uint32_t height = r->swapchain.extent.height;
    VkDeviceSize buf_size = (VkDeviceSize)width * height * 4;

    /* 1. Allocate host-visible staging buffer */
    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = buf_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc_ci = {
        .usage = VMA_MEMORY_USAGE_CPU_ONLY,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer      staging_buf;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;
    if (vmaCreateBuffer(r->allocator, &buf_ci, &alloc_ci,
                        &staging_buf, &staging_alloc, &staging_info) != VK_SUCCESS) {
        fprintf(stderr, "renderer_dump_frame: failed to create staging buffer\n");
        return false;
    }

    /* 2. Wait for GPU idle */
    vkQueueWaitIdle(r->graphics_queue);

    /* 3. Submit copy command */
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);

    VkImage src_image = r->swapchain.images[r->last_image_index];

    /* Transition: PRESENT_SRC → TRANSFER_SRC */
    VkImageMemoryBarrier barrier_to_src = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = src_image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier_to_src);

    /* Copy image → buffer */
    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, src_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buf, 1, &region);

    /* Transition back: TRANSFER_SRC → PRESENT_SRC */
    VkImageMemoryBarrier barrier_to_present = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = src_image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier_to_present);

    renderer_end_single_cmd(r, cmd);

    /* 4. Write PNG */
    bool ok = stb_image_write_png(path, (int)width, (int)height, 4,
                                  staging_info.pMappedData, (int)(width * 4)) != 0;
    if (!ok)
        fprintf(stderr, "renderer_dump_frame: stb_image_write_png failed\n");

    /* 5. Cleanup */
    vmaDestroyBuffer(r->allocator, staging_buf, staging_alloc);
    return ok;
}
```

Also verify that `r->swapchain.images` is accessible — check `swapchain.h` for the images array. It should be a `VkImage* images` field.

- [ ] **Step 5: Check swapchain.h for `images` field**

```bash
grep -n "images" /var/home/samu/minecraft/src/swapchain.h
```

If `images` is not a field, check `swapchain.c` to see how images are stored and adjust `r->swapchain.images[r->last_image_index]` accordingly.

- [ ] **Step 6: Build to verify — should now link cleanly**

```bash
cmake --build /var/home/samu/minecraft/build --target minecraft -- -j$(nproc) 2>&1 | tail -10
```

Expected: full build success, no linker errors.

- [ ] **Step 7: Commit**

```bash
git add src/renderer.h src/renderer.c
git commit -m "feat: implement renderer_dump_frame with Vulkan staging buffer readback"
```

---

### Task 7: Wire `chunk_loaded` events from `world.c`

**Files:**
- Modify: `src/world.c`

- [ ] **Step 1: Add `#include "agent.h"` to `world.c`**

After the existing includes in `world.c`, add:

```c
#include "agent.h"
```

- [ ] **Step 2: Call `agent_notify_chunk_loaded` at the two `CHUNK_READY` transitions**

In `world.c` around line 331 and 336, both `atomic_store(&chunk->state, CHUNK_READY)` calls. After each one, add:

```c
    atomic_store(&chunk->state, CHUNK_READY);
    if (agent_is_active())
        agent_notify_chunk_loaded(chunk->cx, chunk->cz);
```

Apply this to both the `vertex_count > 0` path (mesh uploaded) and the empty mesh path.

- [ ] **Step 3: Build to verify**

```bash
cmake --build /var/home/samu/minecraft/build --target minecraft -- -j$(nproc) 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/world.c
git commit -m "feat: emit chunk_loaded agent events when chunks become ready"
```

---

### Task 8: Integration smoke test

**Goal:** Verify the full pipeline works end-to-end: `--agent` launches, emits `ready`, responds to `get_state`, and `dump_frame` produces a valid PNG.

- [ ] **Step 1: Start Xvfb**

```bash
Xvfb :99 -screen 0 1280x720x24 &
sleep 1
```

- [ ] **Step 2: Run smoke test with a short pipeline script**

```bash
(
  echo '{"cmd":"get_state"}'
  sleep 3
  echo '{"cmd":"dump_frame","path":"/tmp/agent_test_frame.png"}'
  sleep 2
  echo '{"cmd":"quit"}'
) | DISPLAY=:99 /var/home/samu/minecraft/build/minecraft --agent 2>/dev/null
```

Expected stdout (order may vary):
```
{"event":"state",...}
{"event":"ready"}
{"event":"state",...}   (many of these, one per frame)
{"event":"frame_saved","path":"/tmp/agent_test_frame.png"}
```

- [ ] **Step 3: Verify the PNG exists and is valid**

```bash
file /tmp/agent_test_frame.png
```

Expected: `PNG image data, 1280 x 720, ...`

- [ ] **Step 4: Kill Xvfb**

```bash
pkill Xvfb
```

- [ ] **Step 5: Final commit**

```bash
git add docs/superpowers/plans/2026-03-18-agent-control-api.md
git commit -m "docs: add agent control API implementation plan"
```
