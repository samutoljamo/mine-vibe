# Hostile Mobs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a server-authoritative hostile mob that spawns near players, chases them across terrain, and deals contact damage; the player has health, can fight back, and dies/respawns.

**Architecture:** The server gains a headless world (`world_create_headless`) and simulates mobs at its 20 Hz tick — reactive steering toward the nearest player, `physics_move` for terrain collision, contact damage. Mob state is broadcast like player state; clients interpolate and render mobs by reusing the existing player humanoid model. Combat is request/validate: the client asks to hit a mob, the server decides. Player health is server-authoritative; the client owns the respawn teleport (movement stays client-authoritative).

**Tech Stack:** C11, Vulkan (volk + VMA), cglm, GLFW, CMake, CTest. UDP networking with a custom reliable channel.

## Global Constraints

- **Per-message wire cap is 512 bytes** (`NET_THREAD_MAX_MSG`), *not* `NET_MAX_PACKET` (1400). `PKT_MOB_STATE` must fit in 512 → at most 24 mobs per broadcast. `MOB_CAP` is 8, so this is slack, but the broadcast loop must hard-cap at 24.
- **Mobs are reused as the player humanoid model for v1** — no new texture, pipeline, or descriptor set. They render via the existing `players` array passed to `renderer_draw_frame`. A dedicated mob skin is explicitly future work.
- **`mob.c` is Vulkan/GLFW/socket-free** so it unit-tests like `remote_player.c`. Keep all rendering/networking out of it.
- **Yaw convention** (from `camera_get_front`): horizontal facing is `front_xz = (cos yaw, sin yaw)`. A mob faces target offset `(dx, dz)` with `yaw = atan2f(dz, dx)`. `player_model_draw` already rotates by `GLM_PI_2 - yaw`; reuse verbatim.
- **Server simulation timestep** is `1.0f / SERVER_TICK_RATE` (0.05 s). `physics_move` substeps collision internally, so this does not tunnel.
- **Single-anchor server streaming** for v1: the server streams chunks around the first connected player only. Multi-anchor is future work.
- TDD throughout: failing test → verify fail → implement → verify pass → commit. Build/test in the normal build environment; the **Vulkan game is run by the user** for manual playtests (do not attempt to launch it yourself).
- Commit message trailers (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01PRWFdtRqTX5JPWq51c5awK
  ```

---

## File Structure

New files:
- `src/mob.h` / `src/mob.c` — shared mob model: `MobType`, server `Mob` + `MobSet`, pure AI/combat helpers, and the client-side `ClientMob`/`ClientMobSet` snapshot + interpolation. No Vulkan/GLFW/sockets.
- `tests/test_mob.c` — unit tests for targeting, steering, combat, client snapshot apply/interpolate, and ray-vs-AABB.

Modified:
- `src/player.h` — promote `GRAVITY`, `JUMP_VEL`, `TERMINAL_VEL` from `player.c`.
- `src/player.c` — drop the promoted `#define`s.
- `src/gameplay.h` — add `PLAYER_MAX_HEALTH`, `PLAYER_ATTACK_DAMAGE`, `PLAYER_ATTACK_COOLDOWN`.
- `src/net.h` — `PKT_MOB_STATE`, `PKT_MOB_ATTACK`, `PKT_PLAYER_HEALTH` + wire helpers + `NetMobState`.
- `src/server.h` / `src/server.c` — headless world, `MobSet`, sim/spawn tick, broadcast, health fields, mob-attack handler; `server_run` gains a `seed` arg.
- `src/client.h` / `src/client.c` — mob-state callback, `client_send_mob_attack`, `health` field + `PKT_PLAYER_HEALTH` handling.
- `src/main.c` — pass seed to host server; client `ClientMobSet`; mob raycast + attack routing; append mob render states; respawn-on-death; pass health to renderer.
- `src/renderer.h` / `src/renderer_frame.c` — add `int player_health` param to `renderer_draw_frame`; thread to HUD.
- `src/ui/hud.h` / `src/ui/hud.c` — `hud_build` gains health; draw a hearts row.
- `CMakeLists.txt` — add `src/mob.c` to `minecraft`; add `test_mob`.
- `tests/test_net.c` — wire round-trip tests for the three new packets.

---

## Task 1: Promote shared physics constants to `player.h`

**Files:**
- Modify: `src/player.h` (add 3 defines under the existing "shared with server-side simulation" comment)
- Modify: `src/player.c:10-12` (remove 3 defines)

**Interfaces:**
- Produces: `GRAVITY` (25.2f), `JUMP_VEL` (7.95f), `TERMINAL_VEL` (78.4f) as macros in `player.h`, usable by `mob.c`/`server.c`.

- [ ] **Step 1: Add the constants to `player.h`**

In `src/player.h`, under the existing block:
```c
/* Physics constants — shared with server-side simulation */
#define PHYSICS_DT          (1.0f / 60.0f)
#define PLAYER_SPRINT_SPEED 5.6f
#define PLAYER_SNEAK_SPEED  1.3f
#define PLAYER_SNEAK_EYE_DIP 0.3f   /* world units; matches MC's ~5/16 dip */
```
add:
```c
#define GRAVITY             25.2f   /* m/s^2, applied per tick as v -= GRAVITY*dt */
#define JUMP_VEL            7.95f   /* upward impulse; peak ≈ v^2/2g ≈ 1.25 blocks */
#define TERMINAL_VEL        78.4f   /* fall-speed clamp */
```

- [ ] **Step 2: Remove the now-duplicate defines from `player.c`**

In `src/player.c`, delete these three lines (lines 10-12):
```c
#define GRAVITY          25.2f
#define JUMP_VEL         7.95f
#define TERMINAL_VEL     78.4f
```
Leave `FLY_SPEED`, `WALK_SPEED`, `SWIM_SPEED`, etc. in place. `player.c` already `#include "player.h"`, so the macros resolve.

- [ ] **Step 3: Build to verify no breakage**

Run: `cmake --build build --target minecraft`
Expected: compiles cleanly (no redefinition warnings, no undefined `GRAVITY`).

- [ ] **Step 4: Commit**

```bash
git add src/player.h src/player.c
git commit -m "refactor(player): promote GRAVITY/JUMP_VEL/TERMINAL_VEL to player.h for server reuse"
```

---

## Task 2: Mob module — struct, set management, pure AI/combat

**Files:**
- Create: `src/mob.h`
- Create: `src/mob.c`
- Create: `tests/test_mob.c`
- Modify: `CMakeLists.txt` (add `src/mob.c` to `minecraft`; add `test_mob` executable + test; add `test_mob` to the msvc atomics `foreach`)

**Interfaces:**
- Produces (server-side): `MobType`, `Mob`, `MobSet`, `MobTargetInfo`, and:
  - `void mob_set_init(MobSet*)`
  - `Mob* mob_set_spawn(MobSet*, MobType, vec3 pos)` — returns NULL if full; assigns a unique nonzero id
  - `Mob* mob_set_get(MobSet*, uint16_t id)`
  - `void mob_set_remove(MobSet*, uint16_t id)`
  - `uint16_t mob_acquire_target(const Mob*, const MobTargetInfo* players, int count)`
  - `void mob_steer(const Mob*, vec3 target_pos, float* out_vx, float* out_vz, float* out_yaw)`
  - `bool mob_combat_apply(int16_t* health, int dmg)` — subtract dmg; returns true if it crossed to ≤0
  - All mob tunable macros (used by later tasks).

- [ ] **Step 1: Write `src/mob.h`**

```c
#ifndef MOB_H
#define MOB_H

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#define MOB_MAX 64   /* hard array/wire cap */

typedef enum { MOB_ZOMBIE = 0, MOB_TYPE_COUNT } MobType;

/* ---- Tunables (v1) ---- */
#define MOB_CAP             8       /* live mobs kept near the anchor */
#define MOB_HALF_W          0.3f
#define MOB_HEIGHT          1.8f
#define MOB_SPEED           2.0f    /* blocks/s chase speed */
#define MOB_JUMP_SPEED      JUMP_VEL/* = player jump; clears a 1-block step */
#define MOB_AGGRO_RANGE     16.0f
#define MOB_DEAGGRO_RANGE   24.0f
#define MOB_ATTACK_RANGE    1.5f
#define MOB_ATTACK_DAMAGE   4
#define MOB_ATTACK_INTERVAL 1.0f    /* seconds between contact hits */
#define MOB_HEALTH          20
#define MOB_SPAWN_INTERVAL  3.0f
#define MOB_SPAWN_MIN       12.0f
#define MOB_SPAWN_MAX       28.0f
#define MOB_DESPAWN_RANGE   44.0f
#define MOB_WANDER_INTERVAL 4.0f
#define MOB_STATE_MAX_WIRE  24      /* mobs per PKT_MOB_STATE (512B cap) */

/* ---- Server simulation struct ---- */
typedef struct {
    uint16_t id;             /* 1.. ; 0 = none */
    bool     active;
    MobType  type;
    vec3     position;       /* feet */
    vec3     velocity;
    float    yaw;            /* facing, radians (atan2(dz,dx)) */
    bool     on_ground;
    int16_t  health;
    uint16_t target_player;  /* player_id chased, 0 = none */
    float    attack_cooldown;/* seconds until next contact hit */
    float    wander_timer;
} Mob;

typedef struct { Mob mobs[MOB_MAX]; } MobSet;

/* Player position record passed to targeting. */
typedef struct {
    uint16_t player_id;
    vec3     position;       /* feet */
} MobTargetInfo;

void mob_set_init(MobSet* s);
Mob* mob_set_spawn(MobSet* s, MobType type, vec3 pos);
Mob* mob_set_get(MobSet* s, uint16_t id);
void mob_set_remove(MobSet* s, uint16_t id);

/* Pure: nearest player within aggro, with deaggro hysteresis on current target. */
uint16_t mob_acquire_target(const Mob* m, const MobTargetInfo* players, int count);

/* Pure: desired horizontal velocity + facing yaw toward target. */
void mob_steer(const Mob* m, vec3 target_pos,
               float* out_vx, float* out_vz, float* out_yaw);

/* Pure: subtract dmg, returns true if this hit dropped health to <= 0. */
bool mob_combat_apply(int16_t* health, int dmg);

#endif /* MOB_H */
```
Note `MOB_JUMP_SPEED` references `JUMP_VEL`, so `mob.h` consumers must include `player.h` first where the jump value is used; `mob.c` includes it (Step 3).

- [ ] **Step 2: Write the failing tests in `tests/test_mob.c`**

```c
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/player.h"   /* JUMP_VEL, needed by mob.h tunables */
#include "../src/mob.h"

#define EPS 1e-4f
static int feq(float a, float b) { return fabsf(a - b) < EPS; }

static void test_spawn_assigns_unique_ids(void) {
    MobSet s; mob_set_init(&s);
    Mob* a = mob_set_spawn(&s, MOB_ZOMBIE, (vec3){0,64,0});
    Mob* b = mob_set_spawn(&s, MOB_ZOMBIE, (vec3){1,64,0});
    assert(a && b);
    assert(a->id != 0 && b->id != 0 && a->id != b->id);
    assert(a->health == MOB_HEALTH);
    assert(mob_set_get(&s, a->id) == a);
    mob_set_remove(&s, a->id);
    assert(mob_set_get(&s, a->id) == NULL);
    printf("PASS: spawn_assigns_unique_ids\n");
}

static void test_acquire_target_nearest_in_range(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    MobTargetInfo players[2] = {
        { .player_id = 1 }, { .player_id = 2 },
    };
    glm_vec3_copy((vec3){5,64,0},  players[0].position);  /* dist 5 */
    glm_vec3_copy((vec3){3,64,0},  players[1].position);  /* dist 3 (nearest) */
    assert(mob_acquire_target(&m, players, 2) == 2);
    printf("PASS: acquire_target_nearest_in_range\n");
}

static void test_acquire_target_out_of_range(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    MobTargetInfo p = { .player_id = 1 };
    glm_vec3_copy((vec3){100,64,0}, p.position);  /* beyond aggro */
    assert(mob_acquire_target(&m, &p, 1) == 0);
    printf("PASS: acquire_target_out_of_range\n");
}

static void test_acquire_target_hysteresis(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    m.target_player = 1;                       /* already chasing player 1 */
    MobTargetInfo p = { .player_id = 1 };
    glm_vec3_copy((vec3){20,64,0}, p.position); /* between aggro(16) and deaggro(24) */
    assert(mob_acquire_target(&m, &p, 1) == 1); /* retained */
    glm_vec3_copy((vec3){30,64,0}, p.position); /* beyond deaggro */
    assert(mob_acquire_target(&m, &p, 1) == 0); /* dropped */
    printf("PASS: acquire_target_hysteresis\n");
}

static void test_steer_points_at_target(void) {
    Mob m; memset(&m, 0, sizeof(m));
    glm_vec3_copy((vec3){0,64,0}, m.position);
    float vx, vz, yaw;
    mob_steer(&m, (vec3){10,64,0}, &vx, &vz, &yaw);  /* +X */
    assert(feq(vx, MOB_SPEED));
    assert(feq(vz, 0.0f));
    assert(feq(yaw, atan2f(0.0f, 10.0f)));           /* = 0 */
    mob_steer(&m, (vec3){0,64,10}, &vx, &vz, &yaw);  /* +Z */
    assert(feq(vz, MOB_SPEED));
    assert(feq(yaw, atan2f(10.0f, 0.0f)));           /* = PI/2 */
    printf("PASS: steer_points_at_target\n");
}

static void test_combat_apply_and_death(void) {
    int16_t h = 20;
    assert(mob_combat_apply(&h, 5) == false); assert(h == 15);
    assert(mob_combat_apply(&h, 15) == true); assert(h <= 0);
    /* further hits on a dead entity still report dead but don't underflow weirdly */
    assert(mob_combat_apply(&h, 5) == false);  /* already dead -> not a fresh kill */
    printf("PASS: combat_apply_and_death\n");
}

int main(void) {
    test_spawn_assigns_unique_ids();
    test_acquire_target_nearest_in_range();
    test_acquire_target_out_of_range();
    test_acquire_target_hysteresis();
    test_steer_points_at_target();
    test_combat_apply_and_death();
    printf("All mob tests passed.\n");
    return 0;
}
```

- [ ] **Step 3: Write `src/mob.c`**

```c
#include "player.h"   /* JUMP_VEL (used by MOB_JUMP_SPEED) */
#include "mob.h"
#include <math.h>
#include <string.h>

void mob_set_init(MobSet* s) { memset(s, 0, sizeof(*s)); }

Mob* mob_set_get(MobSet* s, uint16_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < MOB_MAX; i++)
        if (s->mobs[i].active && s->mobs[i].id == id) return &s->mobs[i];
    return NULL;
}

void mob_set_remove(MobSet* s, uint16_t id) {
    Mob* m = mob_set_get(s, id);
    if (m) m->active = false;
}

Mob* mob_set_spawn(MobSet* s, MobType type, vec3 pos) {
    int slot = -1;
    for (int i = 0; i < MOB_MAX; i++)
        if (!s->mobs[i].active) { slot = i; break; }
    if (slot < 0) return NULL;

    /* Smallest unused nonzero id (≤ MOB_MAX active, so this terminates fast). */
    uint16_t id = 0;
    for (uint32_t cand = 1; cand <= 0xFFFF; cand++) {
        bool used = false;
        for (int i = 0; i < MOB_MAX; i++)
            if (s->mobs[i].active && s->mobs[i].id == (uint16_t)cand) { used = true; break; }
        if (!used) { id = (uint16_t)cand; break; }
    }
    if (id == 0) return NULL;

    Mob* m = &s->mobs[slot];
    memset(m, 0, sizeof(*m));
    m->id     = id;
    m->active = true;
    m->type   = type;
    glm_vec3_copy(pos, m->position);
    m->health = MOB_HEALTH;
    return m;
}

uint16_t mob_acquire_target(const Mob* m, const MobTargetInfo* players, int count) {
    /* Keep current target while within deaggro range (hysteresis). */
    if (m->target_player != 0) {
        for (int i = 0; i < count; i++) {
            if (players[i].player_id == m->target_player) {
                float d = glm_vec3_distance((float*)players[i].position,
                                            (float*)m->position);
                if (d <= MOB_DEAGGRO_RANGE) return m->target_player;
                break;
            }
        }
    }
    /* Otherwise acquire nearest within aggro range. */
    uint16_t best = 0;
    float best_d = MOB_AGGRO_RANGE;
    for (int i = 0; i < count; i++) {
        float d = glm_vec3_distance((float*)players[i].position, (float*)m->position);
        if (d <= best_d) { best_d = d; best = players[i].player_id; }
    }
    return best;
}

void mob_steer(const Mob* m, vec3 target_pos,
               float* out_vx, float* out_vz, float* out_yaw) {
    float dx = target_pos[0] - m->position[0];
    float dz = target_pos[2] - m->position[2];
    float len = sqrtf(dx*dx + dz*dz);
    if (len < 1e-4f) { *out_vx = 0.0f; *out_vz = 0.0f; *out_yaw = m->yaw; return; }
    *out_vx  = dx / len * MOB_SPEED;
    *out_vz  = dz / len * MOB_SPEED;
    *out_yaw = atan2f(dz, dx);   /* matches camera_get_front XZ convention */
}

bool mob_combat_apply(int16_t* health, int dmg) {
    bool was_alive = (*health > 0);
    *health = (int16_t)(*health - dmg);
    return was_alive && (*health <= 0);
}
```

- [ ] **Step 4: Wire CMake — add `mob.c` to the game and create `test_mob`**

In `CMakeLists.txt`, add to the `add_executable(minecraft ...)` source list (after `src/remote_player.c`):
```cmake
    src/mob.c
```
After the `test_remote_player` block, add:
```cmake
add_executable(test_mob
    tests/test_mob.c
    src/mob.c
)
target_include_directories(test_mob PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mob PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mob PRIVATE m)
endif()
add_test(NAME mob COMMAND test_mob)
```
Add `test_mob` to the final `foreach(t ...)` atomics list.

- [ ] **Step 5: Configure + build the test, run to verify it fails first**

Run: `cmake -S . -B build && cmake --build build --target test_mob`
Expected: **fails to link/compile** before `mob.c` exists — but since Step 3 wrote `mob.c`, instead verify the test *passes*. To honor red-first, temporarily stub `mob_acquire_target` to `return 0;`, build, run `ctest --test-dir build -R '^mob$' -V`, observe `acquire_target_nearest_in_range` FAIL, then restore the real body.

- [ ] **Step 6: Build and run tests to verify pass**

Run: `cmake --build build --target test_mob && ctest --test-dir build -R '^mob$' -V`
Expected: `All mob tests passed.` / `100% tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/mob.h src/mob.c tests/test_mob.c CMakeLists.txt
git commit -m "feat(mob): core mob struct, set management, pure AI/combat helpers with tests"
```

---

## Task 3: Networking — mob/health packet wire formats

**Files:**
- Modify: `src/net.h` (enum + `NetMobState` + 3 write/read helper pairs)
- Modify: `tests/test_net.c` (round-trip tests)

**Interfaces:**
- Consumes: existing `PacketHeader`, `net_write_*`/`net_read_*` primitives.
- Produces:
  - enum values `PKT_MOB_STATE = 11`, `PKT_MOB_ATTACK = 12`, `PKT_PLAYER_HEALTH = 13`
  - `typedef struct { uint16_t id; uint8_t type; float x,y,z,yaw; uint8_t health; } NetMobState;`
  - `size_t net_write_mob_state(uint8_t* buf, const PacketHeader*, const NetMobState*, uint16_t count)`
  - `void net_read_mob_state_header(const uint8_t*, size_t* off, uint16_t* out_count)` + `net_read_mob_state_entry(const uint8_t*, size_t* off, NetMobState*)`
  - `size_t net_write_mob_attack(uint8_t* buf, const PacketHeader*, uint16_t mob_id)` / `void net_read_mob_attack(const uint8_t*, PacketHeader*, uint16_t* mob_id)`
  - `size_t net_write_player_health(uint8_t* buf, const PacketHeader*, uint8_t health, uint8_t flags)` / `void net_read_player_health(const uint8_t*, PacketHeader*, uint8_t* health, uint8_t* flags)`
  - `#define MOB_HEALTH_FLAG_DIED 0x01`

- [ ] **Step 1: Write failing round-trip tests in `tests/test_net.c`**

Add these functions and call them from `main` (mirror the file's existing style; it already includes `net.h` and uses `assert`):
```c
static void test_mob_state_roundtrip(void) {
    uint8_t buf[512];
    PacketHeader hdr = { .type = PKT_MOB_STATE, .player_id = 0 };
    NetMobState in[2] = {
        { .id = 7, .type = 0, .x = 1.5f, .y = 64.0f, .z = -3.25f, .yaw = 1.0f, .health = 20 },
        { .id = 9, .type = 0, .x = 9.0f, .y = 65.0f, .z =  2.00f, .yaw = -2.0f, .health = 5 },
    };
    size_t len = net_write_mob_state(buf, &hdr, in, 2);
    assert(len == 8 + 2 + 2 * 20);

    PacketHeader h; size_t off = 0; net_read_header(buf, &off, &h);
    assert(h.type == PKT_MOB_STATE);
    uint16_t count; net_read_mob_state_header(buf, &off, &count);
    assert(count == 2);
    for (int i = 0; i < 2; i++) {
        NetMobState m; net_read_mob_state_entry(buf, &off, &m);
        assert(m.id == in[i].id && m.health == in[i].health);
        assert(m.x == in[i].x && m.y == in[i].y && m.z == in[i].z && m.yaw == in[i].yaw);
    }
    printf("PASS: mob_state_roundtrip\n");
}

static void test_mob_attack_roundtrip(void) {
    uint8_t buf[64];
    PacketHeader hdr = { .type = PKT_MOB_ATTACK, .player_id = 3 };
    size_t len = net_write_mob_attack(buf, &hdr, 42);
    assert(len == 10);
    PacketHeader h; uint16_t id;
    net_read_mob_attack(buf, &h, &id);
    assert(h.type == PKT_MOB_ATTACK && h.player_id == 3 && id == 42);
    printf("PASS: mob_attack_roundtrip\n");
}

static void test_player_health_roundtrip(void) {
    uint8_t buf[64];
    PacketHeader hdr = { .type = PKT_PLAYER_HEALTH, .player_id = 0 };
    size_t len = net_write_player_health(buf, &hdr, 12, MOB_HEALTH_FLAG_DIED);
    assert(len == 10);
    PacketHeader h; uint8_t hp, fl;
    net_read_player_health(buf, &h, &hp, &fl);
    assert(h.type == PKT_PLAYER_HEALTH && hp == 12 && fl == MOB_HEALTH_FLAG_DIED);
    printf("PASS: player_health_roundtrip\n");
}
```
Add to `main`: `test_mob_state_roundtrip(); test_mob_attack_roundtrip(); test_player_health_roundtrip();`

- [ ] **Step 2: Run test_net to verify it fails**

Run: `cmake --build build --target test_net`
Expected: FAIL — `PKT_MOB_STATE` / `net_write_mob_state` undeclared.

- [ ] **Step 3: Add enum values in `src/net.h`**

In the `PacketType` enum, after `PKT_INVENTORY = 10,`:
```c
    PKT_MOB_STATE      = 11, /* server → all:  mob snapshot broadcast        */
    PKT_MOB_ATTACK     = 12, /* client → server: melee a mob by id           */
    PKT_PLAYER_HEALTH  = 13, /* server → one:  authoritative health + death  */
```

- [ ] **Step 4: Add the struct, flag, and helpers in `src/net.h`**

After the inventory helpers, before the UDP socket section, add:
```c
/* ------------------------------------------------------------------ */
/*  Mob + health packets                                               */
/*    PKT_MOB_STATE    — [header 8][count u16][ count × 20 ]           */
/*       entry: id u16 | type u8 | x f32 | y f32 | z f32 | yaw f32 | health u8
 *    PKT_MOB_ATTACK   — [header 8][mob_id u16]            = 10 bytes  */
/*    PKT_PLAYER_HEALTH— [header 8][health u8][flags u8]   = 10 bytes  */
/* ------------------------------------------------------------------ */
#define MOB_STATE_ENTRY_SIZE 20
#define MOB_HEALTH_FLAG_DIED 0x01

typedef struct {
    uint16_t id;
    uint8_t  type;
    float    x, y, z, yaw;
    uint8_t  health;
} NetMobState;

static inline size_t net_write_mob_state(uint8_t* buf, const PacketHeader* hdr,
                                         const NetMobState* mobs, uint16_t count) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u16(buf, &off, count);
    for (uint16_t i = 0; i < count; i++) {
        net_write_u16  (buf, &off, mobs[i].id);
        net_write_u8   (buf, &off, mobs[i].type);
        net_write_float(buf, &off, mobs[i].x);
        net_write_float(buf, &off, mobs[i].y);
        net_write_float(buf, &off, mobs[i].z);
        net_write_float(buf, &off, mobs[i].yaw);
        net_write_u8   (buf, &off, mobs[i].health);
    }
    return off;
}

static inline void net_read_mob_state_header(const uint8_t* buf, size_t* off,
                                             uint16_t* out_count) {
    *out_count = net_read_u16(buf, off);
}

static inline void net_read_mob_state_entry(const uint8_t* buf, size_t* off,
                                            NetMobState* m) {
    m->id     = net_read_u16(buf, off);
    m->type   = net_read_u8(buf, off);
    m->x      = net_read_float(buf, off);
    m->y      = net_read_float(buf, off);
    m->z      = net_read_float(buf, off);
    m->yaw    = net_read_float(buf, off);
    m->health = net_read_u8(buf, off);
}

static inline size_t net_write_mob_attack(uint8_t* buf, const PacketHeader* hdr,
                                          uint16_t mob_id) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u16(buf, &off, mob_id);
    return off;
}

static inline void net_read_mob_attack(const uint8_t* buf, PacketHeader* hdr,
                                       uint16_t* mob_id) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *mob_id = net_read_u16(buf, &off);
}

static inline size_t net_write_player_health(uint8_t* buf, const PacketHeader* hdr,
                                             uint8_t health, uint8_t flags) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, health);
    net_write_u8(buf, &off, flags);
    return off;
}

static inline void net_read_player_health(const uint8_t* buf, PacketHeader* hdr,
                                          uint8_t* health, uint8_t* flags) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *health = net_read_u8(buf, &off);
    *flags  = net_read_u8(buf, &off);
}
```

- [ ] **Step 5: Build and run to verify pass**

Run: `cmake --build build --target test_net && ctest --test-dir build -R '^net$' -V`
Expected: the three new PASS lines; suite passes.

- [ ] **Step 6: Commit**

```bash
git add src/net.h tests/test_net.c
git commit -m "feat(net): PKT_MOB_STATE/PKT_MOB_ATTACK/PKT_PLAYER_HEALTH wire formats with tests"
```

---

## Task 4: Server headless world + seed plumbing

**Files:**
- Modify: `src/server.h` (`server_run` signature; add `World*`, `int seed` to `Server`)
- Modify: `src/server.c` (create/destroy world; stream around anchor each tick)
- Modify: `src/main.c` (pass `WORLD_SEED` to host thread and `--server` path)

**Interfaces:**
- Consumes: `world_create_headless`, `world_update`, `world_destroy`, `world_get_ready_count` (from `world.h`).
- Produces: `void server_run(uint16_t port, int max_clients, int seed)`; `Server.world`, `Server.seed`.

- [ ] **Step 1: Update `server.h`**

Add includes and fields. At top with other includes:
```c
#include "world.h"
```
Add to `struct Server` (the `typedef struct { ... } Server;`):
```c
    World*       world;          /* headless; mob terrain + collision      */
    int          seed;
    MobSet       mobs;           /* (added/used in Task 5)                  */
```
For Task 4 `MobSet` isn't referenced yet; include `#include "mob.h"` now and leave the field (it is zero-initialized and unused until Task 5). Change the prototype:
```c
void server_run(uint16_t port, int max_clients, int seed);
```

- [ ] **Step 2: Create + stream + destroy the world in `server.c`**

In `server_run`, after `Server s = {0};` initialization, before the loop:
```c
    s.seed  = seed;
    s.world = world_create_headless(seed, 8 /* SERVER_MOB_RENDER_DIST */);
```
Add a helper above `server_tick`:
```c
/* Anchor = first active client's position; (0,0,0) until someone connects. */
static void server_anchor(Server* s, vec3 out) {
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (s->clients[i].active && s->clients[i].position_received) {
            out[0] = s->clients[i].x; out[1] = s->clients[i].y; out[2] = s->clients[i].z;
            return;
        }
    }
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;
}
```
In `server_tick`, after the inbound drain (after the `/* Only remaining steps at 20 Hz */` guard), add. **Declare `anchor` at this scope** — Task 5's mob sim and spawner reuse the same variable:
```c
    /* Compute once per tick; shared by terrain streaming (here) and mob
     * simulation/spawning (Task 5). */
    vec3 anchor;
    server_anchor(s, anchor);

    /* Stream terrain around the anchor so mobs have ground to walk on. */
    if (s->world) {
        world_update(s->world, /*bp=*/NULL, anchor);
    }
```
`server_tick` needs the `Server* s`; it already has it. Add `#include <cglm/cglm.h>` to `server.c` if not present (it is via `mob.h`/`world.h`).
After the loop, before `net_socket_close(fd);`, add:
```c
    if (s.world) world_destroy(s.world);
```

- [ ] **Step 3: Update call sites in `main.c`**

`main.c` already defines `#define WORLD_SEED 420`. Change the standalone server path:
```c
    if (server_mode) {
        server_run(port, SERVER_MAX_CLIENTS, WORLD_SEED);
        return 0;
    }
```
Change the host thread. Update `ServerArgs` and `server_thread_func`:
```c
typedef struct { uint16_t port; int max; int seed; } ServerArgs;
static void* server_thread_func(void* arg)
{
    ServerArgs* a = (ServerArgs*)arg;
    server_run(a->port, a->max, a->seed);
    free(a);
    return NULL;
}
```
And where it's launched:
```c
        sargs->port = port;
        sargs->max  = SERVER_MAX_CLIENTS;
        sargs->seed = WORLD_SEED;
```

- [ ] **Step 4: Build the game**

Run: `cmake --build build --target minecraft`
Expected: compiles. (No test — this is integration; verified by run.)

- [ ] **Step 5: Manual smoke check (user runs)**

Ask the user to run the game normally (default host mode) for ~10 seconds and confirm it still starts, loads, and the world renders as before (the server now also generates terrain in the background; there should be no visible change yet). Server stderr should show a second `World created: ... render_distance=8`.

- [ ] **Step 6: Commit**

```bash
git add src/server.h src/server.c src/main.c
git commit -m "feat(server): headless world streamed around the first player (seed plumbed through)"
```

---

## Task 5: Server mob simulation, spawning, and broadcast

**Files:**
- Modify: `src/server.c` (init `MobSet`; `server_simulate_mobs`; spawn/despawn; broadcast `PKT_MOB_STATE`)

**Interfaces:**
- Consumes: `mob.h` API (Task 2), `physics_move` (`physics.h`), `world_get_block`/`block_is_solid`, `net_write_mob_state` (Task 3), `GRAVITY`/`TERMINAL_VEL` (`player.h`), `CHUNK_Y` (`chunk.h`).
- Produces: per-tick mob state on the wire; mobs visible to packet-capture / client (Task 6).

- [ ] **Step 1: Includes + init**

In `server.c` add includes:
```c
#include "mob.h"
#include "physics.h"
#include "player.h"      /* GRAVITY, TERMINAL_VEL */
#include "block.h"       /* block_is_solid */
#include "chunk.h"       /* CHUNK_Y */
#include "world.h"
#include <math.h>
```
In `server_run`, after creating the world:
```c
    mob_set_init(&s.mobs);
```

- [ ] **Step 2: Spawn helper — find ground in a ring around the anchor**

Add above `server_tick`:
```c
/* Find the surface y at (x,z): topmost solid with two air blocks above.
 * Returns -1 if none (ungenerated/air column). */
static int server_surface_y(World* w, int x, int z) {
    for (int y = CHUNK_Y - 3; y >= 1; y--) {
        if (block_is_solid(world_get_block(w, x, y, z))
            && world_get_block(w, x, y + 1, z) == BLOCK_AIR
            && world_get_block(w, x, y + 2, z) == BLOCK_AIR)
            return y;
    }
    return -1;
}

static void server_try_spawn(Server* s, vec3 anchor) {
    int live = 0;
    for (int i = 0; i < MOB_MAX; i++) if (s->mobs.mobs[i].active) live++;
    if (live >= MOB_CAP) return;

    /* Random point in the spawn ring. rand() is fine server-side. */
    float ang = (float)rand() / (float)RAND_MAX * 6.2831853f;
    float rad = MOB_SPAWN_MIN + (float)rand() / (float)RAND_MAX * (MOB_SPAWN_MAX - MOB_SPAWN_MIN);
    int x = (int)floorf(anchor[0] + cosf(ang) * rad);
    int z = (int)floorf(anchor[2] + sinf(ang) * rad);
    int y = server_surface_y(s->world, x, z);
    if (y < 0) return;  /* terrain not ready / no valid column — try again next interval */
    mob_set_spawn(&s->mobs, MOB_ZOMBIE, (vec3){ (float)x + 0.5f, (float)(y + 1), (float)z + 0.5f });
}
```

- [ ] **Step 3: The mob simulation step**

Add above `server_tick`:
```c
static void server_simulate_mobs(Server* s, float dt) {
    if (!s->world) return;

    /* Snapshot connected players for targeting. */
    MobTargetInfo players[SERVER_MAX_CLIENTS];
    int pcount = 0;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active || !c->position_received) continue;
        players[pcount].player_id = c->player_id;
        players[pcount].position[0] = c->x;
        players[pcount].position[1] = c->y;
        players[pcount].position[2] = c->z;
        pcount++;
    }

    for (int i = 0; i < MOB_MAX; i++) {
        Mob* m = &s->mobs.mobs[i];
        if (!m->active) continue;

        if (m->attack_cooldown > 0.0f) m->attack_cooldown -= dt;

        /* 1. Target + intent */
        m->target_player = mob_acquire_target(m, players, pcount);
        float vx = 0.0f, vz = 0.0f;
        const MobTargetInfo* tgt = NULL;
        if (m->target_player != 0) {
            for (int p = 0; p < pcount; p++)
                if (players[p].player_id == m->target_player) { tgt = &players[p]; break; }
        }
        if (tgt) {
            mob_steer(m, (float*)tgt->position, &vx, &vz, &m->yaw);
        } else {
            /* Idle wander: pick a slow heading periodically. */
            m->wander_timer -= dt;
            if (m->wander_timer <= 0.0f) {
                m->wander_timer = MOB_WANDER_INTERVAL;
                m->yaw = (float)rand() / (float)RAND_MAX * 6.2831853f;
            }
            vx = cosf(m->yaw) * (MOB_SPEED * 0.3f);
            vz = sinf(m->yaw) * (MOB_SPEED * 0.3f);
        }
        m->velocity[0] = vx;
        m->velocity[2] = vz;

        /* 2. Jump over a 1-block step in the heading direction. */
        if (m->on_ground) {
            float hn = sqrtf(vx*vx + vz*vz);
            if (hn > 1e-3f) {
                int ax = (int)floorf(m->position[0] + (vx / hn) * (MOB_HALF_W + 0.3f));
                int az = (int)floorf(m->position[2] + (vz / hn) * (MOB_HALF_W + 0.3f));
                int fy = (int)floorf(m->position[1]);
                bool blocked = block_is_solid(world_get_block(s->world, ax, fy, az));
                bool clear   = world_get_block(s->world, ax, fy + 1, az) == BLOCK_AIR
                            && world_get_block(s->world, ax, fy + 2, az) == BLOCK_AIR;
                if (blocked && clear) m->velocity[1] = MOB_JUMP_SPEED;
            }
        }

        /* 3. Gravity + move (physics_move substeps collision internally). */
        m->velocity[1] -= GRAVITY * dt;
        if (m->velocity[1] < -TERMINAL_VEL) m->velocity[1] = -TERMINAL_VEL;
        PhysicsResult pr = physics_move(m->position, m->velocity,
                                        MOB_HALF_W, MOB_HEIGHT, dt,
                                        /*crouch=*/false, s->world);
        m->on_ground = pr.on_ground;

        /* 4. Contact damage to the target. */
        if (tgt && m->attack_cooldown <= 0.0f) {
            float d = glm_vec3_distance((float*)tgt->position, m->position);
            if (d <= MOB_ATTACK_RANGE) {
                for (int c = 0; c < SERVER_MAX_CLIENTS; c++) {
                    if (s->clients[c].active && s->clients[c].player_id == m->target_player) {
                        server_damage_player(s, &s->clients[c], MOB_ATTACK_DAMAGE); /* Task 7 */
                        break;
                    }
                }
                m->attack_cooldown = MOB_ATTACK_INTERVAL;
            }
        }

        /* 5. Despawn if far from every player. */
        bool near = false;
        for (int p = 0; p < pcount; p++)
            if (glm_vec3_distance((float*)players[p].position, m->position) <= MOB_DESPAWN_RANGE)
                { near = true; break; }
        if (pcount > 0 && !near) m->active = false;
    }
}
```
**Note:** `server_damage_player` is introduced in Task 7. For Task 5, temporarily replace that call with a no-op comment `/* damage applied in Task 7 */` so this task builds standalone; Task 7 wires it in. (Keep the surrounding range/cooldown logic.)

- [ ] **Step 4: Broadcast `PKT_MOB_STATE` + spawn timer in `server_tick`**

Add a spawn accumulator field `float mob_spawn_timer;` to the `Server` struct in `server.h`. In `server_tick`, immediately after the Task 4 terrain-stream block (the `anchor` variable is already in scope from Task 4):
```c
    if (s->world) {
        server_simulate_mobs(s, 1.0f / SERVER_TICK_RATE);

        s->mob_spawn_timer += 1.0f / SERVER_TICK_RATE;
        if (s->mob_spawn_timer >= MOB_SPAWN_INTERVAL) {
            s->mob_spawn_timer = 0.0f;
            server_try_spawn(s, anchor);
        }
    }
```
Then, in the broadcast section (step 4 of `server_tick`, after sending `PKT_WORLD_STATE`), add the mob broadcast:
```c
    {
        NetMobState mobs_wire[MOB_STATE_MAX_WIRE];
        uint16_t mc = 0;
        for (int i = 0; i < MOB_MAX && mc < MOB_STATE_MAX_WIRE; i++) {
            Mob* m = &s->mobs.mobs[i];
            if (!m->active) continue;
            mobs_wire[mc].id     = m->id;
            mobs_wire[mc].type   = (uint8_t)m->type;
            mobs_wire[mc].x      = m->position[0];
            mobs_wire[mc].y      = m->position[1];
            mobs_wire[mc].z      = m->position[2];
            mobs_wire[mc].yaw    = m->yaw;
            mobs_wire[mc].health = (uint8_t)(m->health < 0 ? 0 : m->health);
            mc++;
        }
        uint8_t mbuf[NET_MAX_PACKET];
        PacketHeader mhdr = { .type = PKT_MOB_STATE, .player_id = 0 };
        size_t mlen = net_write_mob_state(mbuf, &mhdr, mobs_wire, mc);
        for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
            if (!s->clients[i].active) continue;
            net_thread_push_outbound(s->net, mbuf, (int)mlen, &s->clients[i].addr);
        }
    }
```
`server_tick`'s signature takes `Server* s` — adjust the helper calls to use `s`. The existing code uses `s` already.

- [ ] **Step 5: Build**

Run: `cmake --build build --target minecraft`
Expected: compiles (with the Task-7 call stubbed as noted).

- [ ] **Step 6: Manual smoke check (user runs)**

Add a temporary `printf` in `server_try_spawn` on success (`[server] spawned mob %u at (%d,%d)\n`). User runs the game; server stderr should log spawns after ~3 s, and (with a debug print of mob[0] position in `server_simulate_mobs`, optional) the mob position should drift toward the player. Remove debug prints before commit or keep one concise spawn log.

- [ ] **Step 7: Commit**

```bash
git add src/server.h src/server.c
git commit -m "feat(server): mob simulation, spawning, and PKT_MOB_STATE broadcast"
```

---

## Task 6: Client mob receive, interpolation, and rendering

**Files:**
- Modify: `src/mob.h` / `src/mob.c` (client-side `ClientMob`, `ClientMobSet`, snapshot apply + interpolation)
- Modify: `tests/test_mob.c` (apply/interpolate tests)
- Modify: `src/client.h` / `src/client.c` (mob-state callback)
- Modify: `src/main.c` (own a `ClientMobSet`, register callback, interpolate + append render states)

**Interfaces:**
- Consumes: `net_read_mob_state_*` (Task 3), `PlayerRenderState` (`player_model.h`).
- Produces:
  - `typedef struct { uint16_t id; uint8_t type; float x,y,z,yaw; uint8_t health; } ClientMobSnapshot;`
  - `ClientMob` / `ClientMobSet` + `void client_mob_set_init(ClientMobSet*)`, `void client_mob_set_apply(ClientMobSet*, const ClientMobSnapshot*, int count, double recv_time)`, `void client_mob_interpolate(ClientMob*, float dt, vec3 out_pos, float* out_yaw)`.
  - `typedef void (*ClientMobsCb)(const ClientMobSnapshot* mobs, int count, double recv_time, void* user);` + `void client_set_mobs_cb(Client*, ClientMobsCb, void* user);`

- [ ] **Step 1: Add client-side types + API to `mob.h`**

```c
/* ---- Client-side interpolated mob ---- */
#define MOB_INTERP_DELAY 0.025   /* seconds, matches remote players */

typedef struct {
    uint16_t id;
    uint8_t  type;
    float    x, y, z, yaw;
    uint8_t  health;
} ClientMobSnapshot;

typedef struct {
    uint16_t id;
    bool     active;
    uint8_t  type;
    uint8_t  health;
    vec3     positions[2];
    float    yaws[2];
    double   snapshot_times[2];
    uint8_t  snapshot_count;
    double   render_time;
} ClientMob;

typedef struct { ClientMob mobs[MOB_MAX]; } ClientMobSet;

void client_mob_set_init(ClientMobSet* s);
/* Full authoritative list: pushes snapshots for present ids, deactivates absent ones. */
void client_mob_set_apply(ClientMobSet* s, const ClientMobSnapshot* snaps,
                          int count, double recv_time);
void client_mob_interpolate(ClientMob* m, float dt, vec3 out_pos, float* out_yaw);
```

- [ ] **Step 2: Write failing tests in `tests/test_mob.c`**

```c
static void test_client_apply_and_deactivate(void) {
    ClientMobSet s; client_mob_set_init(&s);
    ClientMobSnapshot a[1] = {{ .id = 5, .type = 0, .x = 0, .y = 64, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, a, 1, 100.0);
    /* present again, moved */
    ClientMobSnapshot b[1] = {{ .id = 5, .type = 0, .x = 10, .y = 64, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, b, 1, 100.05);
    int found = -1;
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].active && s.mobs[i].id == 5) found = i;
    assert(found >= 0 && s.mobs[found].snapshot_count == 2);
    /* absent → deactivated */
    client_mob_set_apply(&s, NULL, 0, 100.10);
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].id == 5) assert(!s.mobs[i].active);
    printf("PASS: client_apply_and_deactivate\n");
}

static void test_client_interpolate_midpoint(void) {
    ClientMobSet s; client_mob_set_init(&s);
    ClientMobSnapshot a[1] = {{ .id = 1, .x = 0,  .y = 0, .z = 0, .yaw = 0, .health = 20 }};
    ClientMobSnapshot b[1] = {{ .id = 1, .x = 10, .y = 0, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, a, 1, 0.0);
    client_mob_set_apply(&s, b, 1, 1.0);
    ClientMob* m = NULL;
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].active && s.mobs[i].id == 1) m = &s.mobs[i];
    assert(m); m->render_time = 0.5;
    vec3 pos; float yaw;
    client_mob_interpolate(m, 0.0f, pos, &yaw);
    assert(feq(pos[0], 5.0f));
    printf("PASS: client_interpolate_midpoint\n");
}
```
Add both to `main`.

- [ ] **Step 3: Implement in `src/mob.c`**

```c
#define MOB_TWO_PI_F 6.28318530f

void client_mob_set_init(ClientMobSet* s) { memset(s, 0, sizeof(*s)); }

static ClientMob* client_mob_find(ClientMobSet* s, uint16_t id) {
    for (int i = 0; i < MOB_MAX; i++)
        if (s->mobs[i].active && s->mobs[i].id == id) return &s->mobs[i];
    return NULL;
}

static void client_mob_push(ClientMob* m, const ClientMobSnapshot* snap, double recv_time) {
    if (m->snapshot_count > 0) {
        glm_vec3_copy(m->positions[1], m->positions[0]);
        m->yaws[0] = m->yaws[1];
        m->snapshot_times[0] = m->snapshot_times[1];
    }
    m->positions[1][0] = snap->x;
    m->positions[1][1] = snap->y;
    m->positions[1][2] = snap->z;
    m->yaws[1] = snap->yaw;
    m->snapshot_times[1] = recv_time;
    m->type = snap->type;
    m->health = snap->health;
    if (m->snapshot_count < 2) {
        m->snapshot_count++;
        if (m->snapshot_count == 1) m->render_time = recv_time - MOB_INTERP_DELAY;
    }
}

void client_mob_set_apply(ClientMobSet* s, const ClientMobSnapshot* snaps,
                          int count, double recv_time) {
    /* Mark all currently-active as unseen this update. */
    bool seen[MOB_MAX] = { false };
    for (int i = 0; i < count; i++) {
        ClientMob* m = client_mob_find(s, snaps[i].id);
        if (!m) {
            for (int j = 0; j < MOB_MAX; j++) if (!s->mobs[j].active) {
                memset(&s->mobs[j], 0, sizeof(s->mobs[j]));
                s->mobs[j].active = true;
                s->mobs[j].id = snaps[i].id;
                m = &s->mobs[j];
                break;
            }
            if (!m) continue; /* full */
        }
        client_mob_push(m, &snaps[i], recv_time);
        for (int j = 0; j < MOB_MAX; j++) if (&s->mobs[j] == m) { seen[j] = true; break; }
    }
    for (int j = 0; j < MOB_MAX; j++)
        if (s->mobs[j].active && !seen[j]) s->mobs[j].active = false;
}

void client_mob_interpolate(ClientMob* m, float dt, vec3 out_pos, float* out_yaw) {
    m->render_time += dt;
    double dt_snap = m->snapshot_times[1] - m->snapshot_times[0];
    if (m->snapshot_count < 2 || dt_snap <= 0.0) {
        glm_vec3_copy(m->positions[1], out_pos);
        *out_yaw = m->yaws[1];
        return;
    }
    if (m->render_time <= m->snapshot_times[1]) {
        double t = (m->render_time - m->snapshot_times[0]) / dt_snap;
        if (t > 1.0) t = 1.0; if (t < 0.0) t = 0.0;
        float tf = (float)t;
        out_pos[0] = m->positions[0][0] + tf * (m->positions[1][0] - m->positions[0][0]);
        out_pos[1] = m->positions[0][1] + tf * (m->positions[1][1] - m->positions[0][1]);
        out_pos[2] = m->positions[0][2] + tf * (m->positions[1][2] - m->positions[0][2]);
        float dyaw = m->yaws[1] - m->yaws[0];
        dyaw -= MOB_TWO_PI_F * roundf(dyaw / MOB_TWO_PI_F);
        *out_yaw = m->yaws[0] + tf * dyaw;
    } else {
        double excess = m->render_time - m->snapshot_times[1];
        if (excess > 2.0) excess = 2.0;
        float fe = (float)excess;
        float vx = (float)((m->positions[1][0] - m->positions[0][0]) / dt_snap);
        float vy = (float)((m->positions[1][1] - m->positions[0][1]) / dt_snap);
        float vz = (float)((m->positions[1][2] - m->positions[0][2]) / dt_snap);
        out_pos[0] = m->positions[1][0] + vx * fe;
        out_pos[1] = m->positions[1][1] + vy * fe;
        out_pos[2] = m->positions[1][2] + vz * fe;
        float dyaw = m->yaws[1] - m->yaws[0];
        dyaw -= MOB_TWO_PI_F * roundf(dyaw / MOB_TWO_PI_F);
        float yv = m->yaws[1] + (float)(dyaw / dt_snap) * fe;
        yv -= MOB_TWO_PI_F * roundf(yv / MOB_TWO_PI_F);
        *out_yaw = yv;
    }
}
```

- [ ] **Step 4: Run mob tests (should pass)**

Run: `cmake --build build --target test_mob && ctest --test-dir build -R '^mob$' -V`
Expected: all PASS including the two new tests.

- [ ] **Step 5: Add the client callback in `client.h`/`client.c`**

In `client.h`, after the leave-cb typedef, add:
```c
#include "mob.h"
typedef void (*ClientMobsCb)(const ClientMobSnapshot* mobs, int count,
                             double recv_time, void* user);
void client_set_mobs_cb(Client* c, ClientMobsCb cb, void* user);
```
In `client.c`, add globals + setter next to the others:
```c
static ClientMobsCb g_mobs_cb   = NULL;
static void*        g_mobs_user = NULL;
void client_set_mobs_cb(Client* c, ClientMobsCb cb, void* user) {
    (void)c; g_mobs_cb = cb; g_mobs_user = user;
}
```
In `client_poll`, add a branch (after the `PKT_WORLD_STATE` branch):
```c
        } else if (type == PKT_MOB_STATE && c->state == CLIENT_CONNECTED) {
            size_t off = 0; PacketHeader hdr; net_read_header(msg->data, &off, &hdr);
            uint16_t count; net_read_mob_state_header(msg->data, &off, &count);
            int required = (int)off + (int)count * MOB_STATE_ENTRY_SIZE;
            if (count > MOB_MAX || required > msg->len) { free(msg); continue; }
            ClientMobSnapshot snaps[MOB_MAX];
            for (uint16_t i = 0; i < count; i++) {
                NetMobState m; net_read_mob_state_entry(msg->data, &off, &m);
                snaps[i].id = m.id; snaps[i].type = m.type;
                snaps[i].x = m.x; snaps[i].y = m.y; snaps[i].z = m.z;
                snaps[i].yaw = m.yaw; snaps[i].health = m.health;
            }
            if (g_mobs_cb) g_mobs_cb(snaps, (int)count, msg->recv_time, g_mobs_user);
```

- [ ] **Step 6: Wire `ClientMobSet` + render in `main.c`**

Near `g_remote_players`:
```c
static ClientMobSet* g_mobs = NULL;
static void on_mobs(const ClientMobSnapshot* mobs, int count, double recv_time, void* user) {
    (void)user;
    if (g_mobs) client_mob_set_apply(g_mobs, mobs, count, recv_time);
}
```
In `main`, where `remote_player_set_init` is called, add:
```c
    ClientMobSet mob_set;
    client_mob_set_init(&mob_set);
    g_mobs = &mob_set;
    client_set_mobs_cb(&client, on_mobs, NULL);
```
(Place inside the `if (networking)` block alongside the other callback registrations.)

In the render-state collection block (`rp_states` loop), enlarge the array and append mobs. Replace the `PlayerRenderState rp_states[REMOTE_PLAYER_MAX];` declaration with:
```c
        PlayerRenderState rp_states[REMOTE_PLAYER_MAX + MOB_MAX];
```
After the remote-player fill loop (still inside `if (networking)`), append mobs:
```c
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
```
`renderer_draw_frame` already draws all `rcount` entries as humanoids — mobs render with no renderer change. Add `g_mobs = NULL;` in the networking-teardown block next to `g_client = NULL;`.

- [ ] **Step 7: Build**

Run: `cmake --build build --target minecraft`
Expected: compiles.

- [ ] **Step 8: Manual playtest (user runs)**

User runs the game. Within ~3 s a humanoid figure should appear within ~28 blocks and **walk toward the player**, climbing 1-block steps and stopping/var when close. (It looks like a player — expected for v1.) Confirm motion is smooth (interpolated), not teleporting.

- [ ] **Step 9: Commit**

```bash
git add src/mob.h src/mob.c tests/test_mob.c src/client.h src/client.c src/main.c
git commit -m "feat(mob): client-side mob interpolation + render via player model"
```

---

## Task 7: Player health — damage, PKT_PLAYER_HEALTH, hearts HUD, respawn

**Files:**
- Modify: `src/gameplay.h` (player combat constants)
- Modify: `src/server.h` / `src/server.c` (`health`/`dead` fields, `server_damage_player`, send health)
- Modify: `src/client.h` / `src/client.c` (`health` field, `PKT_PLAYER_HEALTH` handler, death-cb)
- Modify: `src/main.c` (respawn on death; pass health to renderer)
- Modify: `src/renderer.h` / `src/renderer_frame.c` (health param → HUD)
- Modify: `src/ui/hud.h` / `src/ui/hud.c` (hearts row)

**Interfaces:**
- Consumes: `net_write_player_health`/`net_read_player_health`, `MOB_HEALTH_FLAG_DIED` (Task 3); `send_reliable` (server.c static).
- Produces: `PLAYER_MAX_HEALTH`, `PLAYER_ATTACK_DAMAGE`, `PLAYER_ATTACK_COOLDOWN` (gameplay.h); `void server_damage_player(Server*, ServerClient*, int dmg)`; `Client.health`; `ClientHealthCb`.

- [ ] **Step 1: Add gameplay constants**

In `src/gameplay.h`, before `#endif`:
```c
#define PLAYER_MAX_HEALTH      20
#define PLAYER_ATTACK_DAMAGE   5
#define PLAYER_ATTACK_COOLDOWN 0.25   /* seconds between accepted player hits */
```

- [ ] **Step 2: Server health fields + damage function**

In `server.h`, add to `ServerClient`:
```c
    int16_t  health;
    bool     dead_pending;     /* send death flag once, then refill */
    double   last_attack_time; /* rate-limit player→mob attacks (Task 8) */
```
Add to `Server` struct nothing new (uses clients). In `server.c`, set initial health where clients are allocated (`alloc_client`, after `inventory_init`):
```c
    c->health = PLAYER_MAX_HEALTH;
```
Add `#include "gameplay.h"` (already included). Add the function above `server_simulate_mobs`:
```c
static void server_send_health(Server* s, ServerClient* c, uint8_t flags) {
    uint8_t buf[16];
    PacketHeader h = { .type = PKT_PLAYER_HEALTH, .player_id = 0 };
    uint8_t hp = (uint8_t)(c->health < 0 ? 0 : c->health);
    size_t len = net_write_player_health(buf, &h, hp, flags);
    send_reliable(s, c, buf, (uint16_t)len);
}

void server_damage_player(Server* s, ServerClient* c, int dmg) {
    if (c->health <= 0) return;
    c->health = (int16_t)(c->health - dmg);
    if (c->health <= 0) {
        c->health = 0;
        server_send_health(s, c, MOB_HEALTH_FLAG_DIED);
        c->health = PLAYER_MAX_HEALTH;   /* respawn refill (client teleports) */
    } else {
        server_send_health(s, c, 0);
    }
}
```
Now replace the Task-5 stub in `server_simulate_mobs` with the real call:
```c
                        server_damage_player(s, &s->clients[c], MOB_ATTACK_DAMAGE);
```
`server_damage_player` must be declared before `server_simulate_mobs`; place it above. (`send_reliable` is defined earlier in the file.)

- [ ] **Step 3: Client health field + handler + death callback**

In `client.h`, add to `Client`:
```c
    int16_t health;   /* last server-reported; PLAYER_MAX_HEALTH at init */
```
and after the mobs-cb:
```c
typedef void (*ClientDeathCb)(void* user);
void client_set_death_cb(Client* c, ClientDeathCb cb, void* user);
```
In `client.c`, init in `client_init` (after `inventory_init`):
```c
    c->health = PLAYER_MAX_HEALTH;   /* requires #include "gameplay.h" */
```
Add `#include "gameplay.h"` to `client.c`. Add globals + setter:
```c
static ClientDeathCb g_death_cb = NULL;
static void*         g_death_user = NULL;
void client_set_death_cb(Client* c, ClientDeathCb cb, void* user) {
    (void)c; g_death_cb = cb; g_death_user = user;
}
```
Add branch in `client_poll`:
```c
        } else if (type == PKT_PLAYER_HEALTH && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0; net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= 10) {
                uint8_t hp, fl; net_read_player_health(msg->data, &h, &hp, &fl);
                c->health = (int16_t)hp;
                if ((fl & MOB_HEALTH_FLAG_DIED) && g_death_cb) {
                    c->health = PLAYER_MAX_HEALTH;
                    g_death_cb(g_death_user);
                }
            }
```

- [ ] **Step 4: Respawn + health HUD wiring in `main.c`**

Store the spawn position. Where `spawn_y` is computed:
```c
    vec3 g_spawn = { 0, (float)spawn_y, 0 };
    player_init(&g_player, g_spawn);
```
Add a death callback near `on_mobs`:
```c
static Player* g_player_ptr = NULL;
static vec3    g_spawn_pos  = {0,0,0};
static void on_death(void* user) {
    (void)user;
    if (g_player_ptr) {
        glm_vec3_copy(g_spawn_pos, g_player_ptr->position);
        glm_vec3_zero(g_player_ptr->velocity);
        g_player_ptr->on_ground = false;
    }
}
```
Set these up: after `player_init`, `g_player_ptr = &g_player; glm_vec3_copy(g_spawn, g_spawn_pos);` and in the `if (networking)` block: `client_set_death_cb(&client, on_death, NULL);`
(Adjust: declare `g_spawn` before `player_init`; reuse it for `g_spawn_pos`.)

Pass health to the renderer (two call sites). In the loading-loop `renderer_draw_frame` call, pass `-1` for health (no hearts). In the main-loop call, pass `networking ? client.health : -1`. The new parameter goes right after `inventory` — see Step 5 for the exact signature.

- [ ] **Step 5: Extend `renderer_draw_frame` signature → HUD**

In `renderer.h`, update the prototype to add `int player_health` after the inventory param:
```c
void renderer_draw_frame(Renderer* r,
                         ChunkMesh* meshes, uint32_t mesh_count,
                         const PlayerRenderState* players, uint32_t player_count,
                         mat4 view, mat4 proj, vec3 sun_dir,
                         const struct Inventory* inventory,
                         int player_health,            /* <0 = no hearts */
                         const struct RaycastHit* target,
                         float underwater,
                         bool dump_frame, const char* dump_path);
```
In `renderer_frame.c`, add `int player_health,` to the definition in the same position, and change the HUD call:
```c
    hud_build(inventory, player_health, sw, sh);
```
Update both `renderer_draw_frame(...)` calls in `main.c` to pass the health argument in the new position (loading loop: `-1`; main loop: `networking ? client.health : -1`).

- [ ] **Step 6: Hearts row in the HUD**

In `ui/hud.h`, change the prototype:
```c
void hud_build(const struct Inventory* inv, int player_health, float sw, float sh);
```
In `ui/hud.c`, update the signature and draw hearts when `player_health >= 0`. Add near the end of `hud_build` (before the closing brace), after the hotbar loop:
```c
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
        for (int i = 0; i < hearts; i++) {
            float x = hx0 + i * (HSZ + HGAP);
            int hp_here = player_health - i * 2;   /* 2 hp per heart */
            ui_rect(x, hy0, HSZ, HSZ, empty);
            if (hp_here >= 2)      ui_rect(x, hy0, HSZ, HSZ, full);
            else if (hp_here == 1) ui_rect(x, hy0, HSZ * 0.5f, HSZ, half);
        }
    }
```
(`SLOT_SIZE` is defined at the top of `hud.c`; `ui_rect` and `vec4` are already in scope.)

- [ ] **Step 7: Build**

Run: `cmake --build build --target minecraft`
Expected: compiles. Also rebuild `test_net`/`test_mob` to ensure headers still compile: `cmake --build build`.

- [ ] **Step 8: Manual playtest (user runs)**

User runs the game. Let a mob reach the player: hearts (bottom-center, above hotbar) should drop ~2 per hit on ~1 s cadence. When health hits 0, the player snaps back to spawn with full hearts. Confirm no crash on death.

- [ ] **Step 9: Commit**

```bash
git add src/gameplay.h src/server.h src/server.c src/client.h src/client.c src/main.c src/renderer.h src/renderer_frame.c src/ui/hud.h src/ui/hud.c
git commit -m "feat(combat): server-authoritative player health, contact damage, hearts HUD, respawn"
```

---

## Task 8: Player → mob attack

**Files:**
- Modify: `src/mob.h` / `src/mob.c` (`mob_ray_hit` — pure ray vs mob AABBs over client mobs)
- Modify: `tests/test_mob.c` (ray-AABB tests)
- Modify: `src/client.h` / `src/client.c` (`client_send_mob_attack`)
- Modify: `src/server.c` (`handle_mob_attack`: validate range + cooldown, apply damage, remove dead)
- Modify: `src/main.c` (raycast mobs; route left-click)

**Interfaces:**
- Consumes: `ClientMobSet` (Task 6), `client_mob_interpolate`, `PKT_MOB_ATTACK` (Task 3), `mob_combat_apply`/`mob_set_get`/`mob_set_remove` (Task 2), `server_damage_player`/`PLAYER_*` (Task 7), `MAX_REACH` (gameplay.h).
- Produces:
  - `uint16_t mob_ray_hit(const ClientMobSet*, vec3 origin, vec3 dir, float max_dist, float* out_t)` — nearest mob id along the ray (0 = none); `*out_t` = entry distance.
  - `void client_send_mob_attack(Client*, uint16_t mob_id)`.

- [ ] **Step 1: Write failing ray-AABB tests in `tests/test_mob.c`**

```c
static void test_mob_ray_hit(void) {
    ClientMobSet s; client_mob_set_init(&s);
    /* Place an active, render-ready mob at feet (5,0,0). */
    ClientMobSnapshot a[1] = {{ .id = 3, .x = 5, .y = 0, .z = 0, .yaw = 0, .health = 20 }};
    client_mob_set_apply(&s, a, 1, 0.0);
    client_mob_set_apply(&s, a, 1, 1.0);   /* 2 snapshots so it's render-ready */
    for (int i = 0; i < MOB_MAX; i++) if (s.mobs[i].id == 3) s.mobs[i].render_time = 1.0;

    float t = 0.0f;
    /* Ray from origin (0, 1, 0) pointing +X at chest height hits the mob. */
    uint16_t id = mob_ray_hit(&s, (vec3){0,1,0}, (vec3){1,0,0}, 10.0f, &t);
    assert(id == 3);
    assert(t > 4.0f && t < 5.2f);   /* AABB front face near x≈4.7 */

    /* Ray pointing away misses. */
    id = mob_ray_hit(&s, (vec3){0,1,0}, (vec3){-1,0,0}, 10.0f, &t);
    assert(id == 0);
    printf("PASS: mob_ray_hit\n");
}
```
Add to `main`.

- [ ] **Step 2: Implement `mob_ray_hit` in `mob.c`**

```c
/* Slab ray-AABB; returns entry t>=0 or -1 on miss. dir need not be normalized
 * (t is in units of |dir|; callers pass a unit dir so t is world distance). */
static float ray_aabb(vec3 o, vec3 d, vec3 lo, vec3 hi) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; a++) {
        if (fabsf(d[a]) < 1e-8f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return -1.0f;
        } else {
            float inv = 1.0f / d[a];
            float t1 = (lo[a] - o[a]) * inv;
            float t2 = (hi[a] - o[a]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return -1.0f;
        }
    }
    return tmin;
}

uint16_t mob_ray_hit(const ClientMobSet* s, vec3 origin, vec3 dir,
                     float max_dist, float* out_t) {
    uint16_t best = 0;
    float best_t = max_dist;
    for (int i = 0; i < MOB_MAX; i++) {
        const ClientMob* m = &s->mobs[i];
        if (!m->active || m->snapshot_count < 2) continue;
        /* Use the latest snapshot position as the AABB anchor (feet). */
        vec3 lo = { m->positions[1][0] - MOB_HALF_W, m->positions[1][1],
                    m->positions[1][2] - MOB_HALF_W };
        vec3 hi = { m->positions[1][0] + MOB_HALF_W, m->positions[1][1] + MOB_HEIGHT,
                    m->positions[1][2] + MOB_HALF_W };
        float t = ray_aabb(origin, dir, lo, hi);
        if (t >= 0.0f && t < best_t) { best_t = t; best = m->id; }
    }
    if (best && out_t) *out_t = best_t;
    return best;
}
```
Declare in `mob.h`:
```c
uint16_t mob_ray_hit(const ClientMobSet* s, vec3 origin, vec3 dir,
                     float max_dist, float* out_t);
```

- [ ] **Step 3: Build + run mob tests**

Run: `cmake --build build --target test_mob && ctest --test-dir build -R '^mob$' -V`
Expected: all PASS, including `mob_ray_hit`.

- [ ] **Step 4: `client_send_mob_attack`**

In `client.h`:
```c
void client_send_mob_attack(Client* c, uint16_t mob_id);
```
In `client.c` (mirror `client_send_break`):
```c
void client_send_mob_attack(Client* c, uint16_t mob_id) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_MOB_ATTACK, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[16];
    size_t len = net_write_mob_attack(buf, &h, mob_id);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}
```

- [ ] **Step 5: Server `handle_mob_attack`**

In `server.c`, add a handler (mirror `handle_block_break`'s reliable handling):
```c
static void handle_mob_attack(Server* s, ServerClient* c,
                              const uint8_t* data, size_t len) {
    if (len < 10) return;
    PacketHeader h; uint16_t mob_id;
    net_read_mob_attack(data, &h, &mob_id);
    bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
    if (!is_new) return;
    c->last_recv_time = net_time();
    if (!c->position_received || !s->world) return;

    /* Rate-limit. */
    double now = net_time();
    if (now - c->last_attack_time < PLAYER_ATTACK_COOLDOWN) return;

    Mob* m = mob_set_get(&s->mobs, mob_id);
    if (!m) return;

    /* Range check from the attacker's eye to the mob centre. */
    float cx = m->position[0], cy = m->position[1] + MOB_HEIGHT * 0.5f, cz = m->position[2];
    float dx = cx - c->x, dy = cy - (c->y + PLAYER_EYE_H), dz = cz - c->z;
    if (dx*dx + dy*dy + dz*dz > (MAX_REACH + 1.0f) * (MAX_REACH + 1.0f)) return;

    c->last_attack_time = now;
    if (mob_combat_apply(&m->health, PLAYER_ATTACK_DAMAGE))
        mob_set_remove(&s->mobs, mob_id);   /* dead → vanishes next broadcast */
}
```
Wire it into the packet dispatch in `server_tick` (the `else if (c)` chain):
```c
            else if (type == PKT_MOB_ATTACK)   handle_mob_attack(s, c, msg->data, (size_t)msg->len);
```
`handle_mob_attack` uses `PLAYER_EYE_H` and `MAX_REACH` (gameplay.h, already included) and `PLAYER_ATTACK_COOLDOWN`/`PLAYER_ATTACK_DAMAGE`.

- [ ] **Step 6: Route left-click in `main.c`**

The client owns `g_mobs` (Task 6) and `g_target` (block raycast). Extend the per-frame target refresh to also raycast mobs, and change `mouse_button_callback` to prefer the mob. Add file-scope state:
```c
static uint16_t g_target_mob = 0;   /* nearest mob under the crosshair, 0 = none */
```
In the per-frame block after `g_target = raycast_voxel(...)`:
```c
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
```
In `mouse_button_callback`, change the left-button branch:
```c
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (g_target_mob) {
            client_send_mob_attack(g_client, g_target_mob);
        } else if (g_target.hit) {
            client_send_break(g_client, g_target.x, g_target.y, g_target.z,
                              (uint8_t)g_target.block);
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
```
(The existing right-button place branch is unchanged; note the current code early-returns on `!g_target.hit` — remove that guard or relax it so a mob with no block behind it is still clickable. Replace the `if (!g_target.hit) return;` near the top of `mouse_button_callback` with `if (!g_target.hit && !g_target_mob) return;`.)

- [ ] **Step 7: Build**

Run: `cmake --build build`
Expected: compiles (game + all tests).

- [ ] **Step 8: Manual playtest (user runs)**

User runs the game, lets a mob approach, aims at it, and left-clicks repeatedly: after 4 hits (20 hp / 5 dmg) the mob should die and disappear on the spot. Confirm left-clicking blocks (no mob under crosshair) still breaks blocks, and right-click still places.

- [ ] **Step 9: Commit**

```bash
git add src/mob.h src/mob.c tests/test_mob.c src/client.h src/client.c src/server.c src/main.c
git commit -m "feat(combat): player melee attack on mobs (raycast + validated server damage)"
```

---

## Self-Review Notes

- **Spec coverage:** §4.1 mob module → Task 2/6; §4.2 server world → Task 4; §4.3 sim → Task 5; §4.4 spawn/despawn → Task 5; §4.5 health/death/respawn → Task 7; §4.6 client interp → Task 6; §4.7 rendering → Task 6 (reuse player model, deviation from spec's dedicated skin — documented as v1 simplification); §4.8 attack + HUD → Task 7 (HUD) + Task 8 (attack); §5 packets → Task 3; §6 tunables → Task 2 macros + gameplay.h. All covered.
- **Deviation from spec (intentional):** mobs reuse the player humanoid model/skin for v1 instead of a dedicated `mob_model` + green skin. This removes the asset-pipeline/descriptor/pipeline work and the visible cost is "mobs look like players." Dedicated skin remains future work.
- **Wire safety:** broadcast hard-capped at `MOB_STATE_MAX_WIRE` (24) to respect the 512-byte `NET_THREAD_MAX_MSG` queue cap; client validates `count <= MOB_MAX` and length before reading.
- **Type consistency:** `mob_combat_apply(int16_t*, int)`, `mob_ray_hit(... float* out_t)`, `server_damage_player(Server*, ServerClient*, int)`, `client_mob_set_apply(... double recv_time)` are used identically across tasks.
- **Known v1 limitations (carried from spec §10):** single-anchor streaming, no real pathfinding, server `bp=NULL` terrain divergence, double worldgen in single-player, no knockback/regen/loot/animation, one mob type.
