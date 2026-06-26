# Hostile Mobs — Design

**Date:** 2026-06-26
**Status:** Draft for review
**Scope:** First mob implementation — a single hostile creature that spawns, chases the
player across terrain, and deals contact damage. Player can fight back and die.

---

## 1. Goal

Introduce living entities to the world. The first milestone is a **hostile mob that
chases**: a zombie-like creature that

- spawns on solid ground near players,
- acquires the nearest player as a target and walks toward it across terrain
  (stepping up single blocks, falling with gravity),
- deals melee damage on contact,
- can be hit and killed by the player,
- is consistent for every connected client.

This is deliberately a *vertical slice*: it touches entity simulation, terrain-aware
movement, combat, player health, networking, and rendering — but with exactly one mob
type and the simplest AI that still reads as "it's hunting me."

---

## 2. The keystone decision: server-authoritative mobs

### Why this is the hard part

Today the architecture is **client-authoritative with a thin relay server**:

- The server owns **no world**. It trusts clients on block content, validates only
  reach/inventory, and rebroadcasts player positions at 20 Hz (`PKT_WORLD_STATE`).
- Worldgen and physics run entirely client-side. Each client holds its own `World`.
- Single-player is a **loopback host**: the game spawns an in-process server thread and
  connects a local client to it (`main.c`, `--host` is the default).

A hostile mob that deals contact damage **must have a single authority**. If two clients
each simulated the same mob, they would disagree on its position and on "who got hit" —
combat would be unfair and desync-prone. So mobs need one owner.

### Decision

**The server simulates mobs.** It gains a headless world, runs mob AI + movement +
combat at its 20 Hz tick, and broadcasts mob state to all clients exactly the way it
already broadcasts player state. Clients render and interpolate mobs; they never simulate
them. Combat is request/validate: the client *asks* to hit a mob, the server decides.

This is the only option that keeps combat consistent in real multiplayer, and it matches
the latent intent already in the code — `world_create_headless()` exists and is documented
"for server use" but is currently unused. This design finally calls it.

### Alternatives considered and rejected

- **Host-authoritative** (the host client simulates mobs in its existing local world and
  relays them): less new server code, but couples gameplay authority to one specific
  client and leaves a dedicated `--server` unable to host mobs. A structural dead-end.
- **Client-local, no sync** (each client runs its own mobs): smallest, ships fastest, but
  every client sees different mobs — incompatible with the chosen milestone (consistent
  hostile combat). We would tear it out immediately.

### Cost we accept

In single-player the server now generates a second copy of nearby terrain (it already ran
in-process; this adds CPU + memory for worldgen near the player). We bound it by giving
the server a **small render distance** — only enough terrain for mob simulation — and by
running **no block-physics** on the server (water/falling-sand are cosmetic for mob
collision; `world_update` already accepts a `NULL` `BlockPhysics*`). See §10 for the
divergence this can cause and why it's acceptable for v1.

---

## 3. Architecture overview

```
                         SERVER (authoritative)
   ┌───────────────────────────────────────────────────────────┐
   │  headless World (small render distance, bp = NULL)          │
   │  MobSet  ── mob_simulate() @ 20 Hz ───────────────┐        │
   │    • acquire/track target (nearest player)         │        │
   │    • steer toward target, jump over 1-block steps  │        │
   │    • gravity + collision via physics_move(world)   │        │
   │    • contact damage → player health                │        │
   │  ServerClient[]  ── x,y,z,yaw + health             │        │
   └───────────┬───────────────────────────────┬───────┘        │
               │ PKT_MOB_STATE (20 Hz)          │ PKT_PLAYER_HEALTH
               │ PKT_WORLD_STATE (20 Hz)        │ (on change)
               ▼                                ▼
                          CLIENT (per machine)
   ┌───────────────────────────────────────────────────────────┐
   │  MobSet (interpolated, mirrors RemotePlayerSet)            │
   │  mob_model_draw()  — humanoid mesh, mob skin               │
   │  health HUD (hearts)                                        │
   │  attack input: raycast vs mob AABBs → PKT_MOB_ATTACK       │
   └───────────────────────────────────────────────────────────┘
```

The data flow deliberately reuses existing patterns: mob state broadcast mirrors player
state broadcast; client-side mob interpolation mirrors `remote_player.c`; mob rendering
mirrors `player_model.c`; the attack request mirrors `client_send_break`.

---

## 4. Components

### 4.1 `mob.{h,c}` — shared mob model and logic (new)

Pure-ish module, **no Vulkan, no networking** — so it unit-tests like `remote_player.c`.

```c
typedef enum { MOB_ZOMBIE = 0, MOB_TYPE_COUNT } MobType;

typedef struct {
    uint16_t id;            /* 1..65535, 0 = none */
    bool     active;
    MobType  type;
    vec3     position;      /* feet */
    vec3     velocity;
    float    yaw;           /* facing, radians */
    bool     on_ground;
    int16_t  health;        /* current; <=0 => dead */
    uint16_t target_player; /* player_id being chased, 0 = none */
    float    attack_cooldown;/* seconds until next contact hit allowed */
    float    wander_timer;   /* idle re-heading timer */
} Mob;

typedef struct { Mob mobs[MOB_MAX]; } MobSet;   /* MOB_MAX = 64 */
```

The struct is shared by client and server. On the **server** all fields are live. On the
**client** only `id/active/type/position/yaw/health` are meaningful (filled from
snapshots; see §4.6).

Pure logic functions (server-side, but testable in isolation):

- `mob_acquire_target()` — given a mob and the array of player positions/ids, return the
  nearest player within `MOB_AGGRO_RANGE`, or keep the current target until it leaves
  `MOB_DEAGGRO_RANGE`.
- `mob_steer()` — given mob pos + target pos, return desired horizontal velocity
  (unit direction × `MOB_SPEED`) and the yaw that faces it. **Pure function**, no world.
- `mob_wants_jump()` — given the mob, its desired heading, and a world block sampler,
  return true when a solid block blocks the next step at foot height *and* the cell above
  it is clear (climbable 1-block step). This is the entirety of "pathfinding" for v1.
- `mob_combat_apply()` — damage arithmetic and death (used by both attack handling and
  contact damage). Pure.

### 4.2 Server world (`server.c` changes)

`server_run` creates a headless world:

```c
World* world = world_create_headless(seed, SERVER_MOB_RENDER_DIST);  /* e.g. 8 */
```

Each tick, before mob simulation, the server streams terrain around an **anchor**:

```c
world_update(world, /*bp=*/NULL, anchor_pos);
```

**v1 uses a single anchor = the first connected player's position.** This is correct for
single-player and for co-op players who stay near each other. Players who roam far apart
will have mobs simulated only near the anchor. **Multi-anchor streaming is explicit future
work** (§10) and needs a `world_update` generalization; we do not build it now.

The server needs the world seed. It is currently a client-only constant (`WORLD_SEED` in
`main.c`). We pass it to `server_run` (and thus to the in-process host thread) so server
and client generate identical terrain.

### 4.3 Mob simulation tick (`server.c`, 20 Hz)

Runs inside `server_tick` after inbound packets are drained, gated to the existing tick
cadence. Per mob:

1. **Target**: `mob_acquire_target()` over the connected players.
2. **Intent**: if it has a target, `mob_steer()` toward it; else idle wander
   (`mob_wander()` picks a slow random heading every `MOB_WANDER_INTERVAL`).
3. **Jump**: if `mob_wants_jump()` and `on_ground`, set `velocity.y = MOB_JUMP_SPEED`.
4. **Gravity + move**: apply gravity to `velocity.y` (clamp at terminal velocity), then
   `physics_move(pos, vel, MOB_HALF_W, MOB_HEIGHT, dt, /*crouch=*/false, world)` with
   `dt = 1/SERVER_TICK_RATE` (0.05 s). `physics_move` substeps collision internally
   (`sweep_axis_substepped`), so the coarser 20 Hz step does not tunnel through floors.
   Reuses the player collision sweep verbatim — terrain collision, step landing, and
   `on_ground` come for free.
5. **Contact damage**: if a target exists, the mob is within `MOB_ATTACK_RANGE` of it, and
   `attack_cooldown <= 0`, deal `MOB_ATTACK_DAMAGE` to that player's server-side health and
   reset the cooldown.

Mobs whose target chunk isn't generated yet (`world_get_block` returns AIR everywhere)
simply fall; this self-corrects once terrain loads. No special-casing for v1.

### 4.4 Spawning & despawning (`server.c`)

A minimal spawner keeps the world populated without day/night (none exists yet):

- Maintain at most `MOB_CAP` live mobs **within the simulated region** (near the anchor).
- Every `MOB_SPAWN_INTERVAL`, if under cap, attempt one spawn: pick a random column in a
  ring `[MOB_SPAWN_MIN, MOB_SPAWN_MAX]` blocks from the anchor, find the surface (top
  solid block with two air blocks above), and place a `MOB_ZOMBIE` there.
- Despawn any mob farther than `MOB_DESPAWN_RANGE` from all players, or whose chunk has
  unloaded.

No light-level or biome rules in v1 (no lighting query on the server world, no biomes).
Spawn validity = "solid ground with head room." Determinism is not required; randomness
varies by `tick` (the server already counts ticks) plus mob index, avoiding `rand()` in
the hot path is not necessary server-side — `rand()` is fine here.

### 4.5 Player health, death, respawn (`server.c` + client)

Health is **server-authoritative**:

- `ServerClient` gains `int16_t health` (init `PLAYER_MAX_HEALTH = 20`) and
  `bool dead`.
- Contact damage (and any future source) decrements it via `mob_combat_apply()`.
- Whenever a client's health changes, the server sends that client a `PKT_PLAYER_HEALTH`.
- On reaching 0: server sets the death flag in `PKT_PLAYER_HEALTH`, then **resets the
  client's health to max** server-side (the "respawn").

Because movement is client-authoritative, the **client owns the respawn teleport**: on
receiving `PKT_PLAYER_HEALTH` with the death flag set, the client zeroes the player's
velocity and snaps the player position back to the spawn point. This keeps the authority
split clean — server owns *health and the death event*, client owns *position*.

No knockback and no health regen in v1 (both noted in §10). Respawn refills health, which
is the only recovery path for now.

### 4.6 Client: mob interpolation (`mob.c` shared + `main.c` wiring)

Mirrors `remote_player.c` almost exactly. A client-side `MobSet` stores, per active mob,
two timestamped snapshots and interpolates with the same `REMOTE_PLAYER_DELAY` lag. We
factor the existing two-snapshot interpolation so mobs reuse it rather than copy-pasting:

- `mob_push_snapshot(set, id, type, x,y,z, yaw, health, recv_time)`
- `mob_interpolate(mob, dt, out_pos, out_yaw)` — same lerp/extrapolate math already proven
  for players (see `test_remote_player.c`).

Spawn/despawn are **presence-based** by diffing the broadcast set (simpler than adding a
reliable per-mob despawn packet, which is what players use). The rule, stated to avoid the
obvious UDP pitfall:

- A mob id present in a **received** `PKT_MOB_STATE` is spawned/updated.
- A mob id that was active but is **absent from a received** `PKT_MOB_STATE` is marked
  inactive — because each broadcast is the *full* authoritative mob list.
- A **dropped** packet is not an empty list: if no `PKT_MOB_STATE` arrives this tick, the
  client simply doesn't update and mobs keep interpolating from their last snapshots. We
  must not mark mobs inactive merely because a packet didn't arrive, or a single lost
  datagram would flicker every mob out and back in.

### 4.7 Client: mob rendering (`mob_model.{h,c}` + renderer)

Reuses the **existing player graphics pipeline** and the mat4-push-constant convention
from `player_model.c`:

- `mob_model.{h,c}` builds a humanoid mesh. For v1 it may reuse the player box geometry
  (head/torso/arms/legs); the visual distinction comes from a **separate mob skin texture**
  (a green zombie skin) bound through its own descriptor set on the player pipeline.
- `mob_model_draw(r, cmd, model, states, count)` mirrors `player_model_draw` — one draw
  per mob with a transl/yaw model matrix.
- `renderer_draw_frame` gains a mob-states parameter (`MobRenderState{pos[3], yaw}`,
  identical shape to `PlayerRenderState`) and draws mobs after remote players, inside the
  existing world render pass.

No per-mob animation or health bar in v1 (static pose, like remote players today).

### 4.8 Client: attack input & health HUD (`main.c`, `ui/hud.c`)

**Attack**: in the per-frame target refresh (`main.c`), in addition to
`raycast_voxel`, raycast the ray against active mob AABBs (segment-vs-AABB over the
interpolated mob positions, within `MAX_REACH`). Then on left mouse button:

- if a mob is hit and is nearer than the targeted block → `client_send_mob_attack(id)`;
- else fall back to the existing `client_send_break` behavior.

The server validates the attack: the attacker's known position must be within
`MOB_ATTACK_RANGE + slop` of the mob, and the client is rate-limited to
`PLAYER_ATTACK_COOLDOWN` between accepted hits. Valid hits apply `PLAYER_ATTACK_DAMAGE`;
a mob at 0 health is removed and vanishes from the next broadcast.

**HUD**: `ui/hud.c` gains a hearts row driven by the client's last-known health (stored on
`Client`, updated from `PKT_PLAYER_HEALTH`). 10 hearts = 20 hp, half-hearts for odd
values. Reuses the existing HUD quad/textured-rect path.

---

## 5. Networking — new packets

Wire helpers live in `net.h` beside the existing ones, little-endian, same 8-byte header.

### `PKT_MOB_STATE` (server → all, unreliable, 20 Hz)
Broadcast every tick alongside `PKT_WORLD_STATE`.

```
[header 8][count u16][ mobs: count × MOB_WIRE ]
MOB_WIRE (20 bytes): id u16 | type u8 | x f32 | y f32 | z f32 | yaw f32 | health u8
```

Only **active** mobs are sent. In practice ≤ `MOB_CAP` (8) are live → ~10 + 8×20 = 170
bytes. Absolute worst case if all `MOB_MAX` (64) were active is 10 + 64×20 = 1290 bytes,
still under `NET_MAX_PACKET` (1400). If the cap ever grows past the MTU budget, split the
broadcast across packets or send only mobs near each recipient (future).

### `PKT_MOB_ATTACK` (client → server, reliable)
```
[header 8][mob_id u16]   = 10 bytes
```
Reliable so a kill blow isn't dropped. Server validates range + per-client cooldown.

### `PKT_PLAYER_HEALTH` (server → one, reliable)
```
[header 8][health u8][flags u8]   = 10 bytes   (flags bit0 = died-this-event)
```
Reliable so the death event and final health are never missed.

New `PacketType` enum values appended (11, 12, 13) so existing values are unchanged.

---

## 6. Tunables (v1 defaults)

| Constant | Value | Meaning |
|---|---|---|
| `MOB_MAX` | 64 | Hard cap on simultaneous mobs (wire + arrays) |
| `MOB_CAP` | 8 | Live mobs kept near the anchor by the spawner |
| `MOB_HALF_W` | 0.3 | Hitbox half-width (matches player) |
| `MOB_HEIGHT` | 1.8 | Hitbox height (matches player) |
| `MOB_SPEED` | 2.0 b/s | Chase speed (slower than player walk 4.3; escapable) |
| `MOB_JUMP_SPEED` | 7.95 | Vertical impulse; = player `JUMP_VEL`. Peak ≈ v²/2g ≈ 1.25 b, clears a 1-block step |
| `MOB_AGGRO_RANGE` | 16 b | Target acquisition radius |
| `MOB_DEAGGRO_RANGE` | 24 b | Target drop radius (hysteresis) |
| `MOB_ATTACK_RANGE` | 1.5 b | Contact-damage reach |
| `MOB_ATTACK_DAMAGE` | 4 | Damage per contact hit (2 hearts) |
| `MOB_ATTACK_INTERVAL` | 1.0 s | Mob contact-hit cooldown |
| `MOB_HEALTH` | 20 | Zombie hit points |
| `MOB_SPAWN_INTERVAL` | 3.0 s | Spawn attempt cadence when under cap |
| `MOB_SPAWN_MIN/MAX` | 12 / 28 b | Spawn ring around the anchor |
| `MOB_DESPAWN_RANGE` | 44 b | Distance from all players that despawns a mob |
| `MOB_WANDER_INTERVAL` | 4.0 s | Idle re-heading period |
| `SERVER_MOB_RENDER_DIST` | 8 | Server world chunk radius |
| `PLAYER_MAX_HEALTH` | 20 | Player hit points (10 hearts) |
| `PLAYER_ATTACK_DAMAGE` | 5 | Player melee damage to mobs |
| `PLAYER_ATTACK_COOLDOWN` | 0.25 s | Min time between accepted player hits |

Gravity (`25.2`), `JUMP_VEL` (`7.95`), and `TERMINAL_VEL` (`78.4`) currently live as
`#define`s **inside `player.c`**. They must be **promoted to `player.h`** (which already
holds the shared physics constants `PHYSICS_DT`, `PLAYER_SPRINT_SPEED`, `PLAYER_SNEAK_SPEED`)
so the server-side mob simulation reuses the exact same values the client uses for the
local player. This keeps mob fall/jump behavior identical to the player's.

---

## 7. Data flow — one chase, end to end

1. Server spawner places a `MOB_ZOMBIE` on the surface 20 blocks from the player.
2. Each tick the server acquires the player as target, steers the mob toward it, jumps it
   over a 1-block ledge, and moves it with `physics_move` against the server world.
3. Server broadcasts `PKT_MOB_STATE`; every client pushes a snapshot into its `MobSet`.
4. Clients interpolate and render the mob with `mob_model_draw` — it visibly walks toward
   the player.
5. The mob closes to 1.5 b; server applies 4 contact damage on its cooldown, decrements
   the player's health, sends `PKT_PLAYER_HEALTH`; the client's hearts drop.
6. The player aims at the mob and left-clicks; client raycast hits the mob first and sends
   `PKT_MOB_ATTACK`. Server validates range, applies 5 damage; after enough hits the mob
   reaches 0 and is removed — it vanishes from the next broadcast on every client.
7. If the player's health hits 0 first, the server flags death in `PKT_PLAYER_HEALTH` and
   refills; the client teleports itself to spawn.

---

## 8. Module boundaries & files

New:
- `src/mob.h`, `src/mob.c` — shared mob struct, `MobSet`, pure AI/combat helpers, and the
  client-side snapshot/interpolation (factored to share player interpolation math).
- `src/mob_model.h`, `src/mob_model.c` — client mob mesh + draw (mirrors `player_model`).
- `tests/test_mob.c` — unit tests (see §9).
- mob skin asset (added to the asset bake that produces `assets_generated.c`).

Changed:
- `src/net.h` — three new packet types + wire helpers.
- `src/server.{h,c}` — headless world, `MobSet`, simulation/spawn tick, health fields,
  mob-attack handler, `PKT_MOB_STATE` / `PKT_PLAYER_HEALTH` broadcast; `server_run` gains a
  seed parameter.
- `src/client.{h,c}` — mob snapshot callback, `client_send_mob_attack`, health field +
  `PKT_PLAYER_HEALTH` handling.
- `src/main.c` — pass seed to host server; client-side `MobSet`; mob raycast + attack
  input; mob render states into `renderer_draw_frame`; respawn-on-death.
- `src/renderer.{h,c}` / `renderer_frame.c` — mob model lifecycle + draw, mob skin
  texture/descriptor, extended `renderer_draw_frame` signature.
- `src/ui/hud.{h,c}` — hearts row.
- `CMakeLists.txt` — compile `mob.c`/`mob_model.c` into `minecraft`; add `test_mob`.

Keeping mob logic in `mob.c` (not piled into `server.c`) preserves the project's
small-focused-file style and is what makes the AI/combat unit-testable without Vulkan,
GLFW, or a socket.

---

## 9. Testing strategy

Follow the existing Vulkan-free unit-test pattern (`test_remote_player`, `test_net`,
`test_inventory`). `test_mob` links only `mob.c` (+ `cglm`) and covers the **pure logic**:

- **Targeting**: nearest-player selection; aggro acquired inside `MOB_AGGRO_RANGE`;
  retained until `MOB_DEAGGRO_RANGE` (hysteresis); dropped beyond it.
- **Steering**: desired velocity points at the target; yaw faces it; speed == `MOB_SPEED`.
- **Combat math** (`mob_combat_apply`): damage subtracts; death at ≤0; player attack and
  mob contact share the same arithmetic; cooldown gating.
- **Interpolation**: mobs reuse the player interpolation, already covered — add one mob
  snapshot round-trip test for the shared path.

Wire round-trips go in `test_net`: write→read identity for `PKT_MOB_STATE` (incl. the
multi-mob count loop and the near-MTU max-count case), `PKT_MOB_ATTACK`, `PKT_PLAYER_HEALTH`.

`mob_wants_jump` and the `physics_move` integration depend on a `World` (heavy, opaque),
so they are validated by manual playtest rather than unit test in v1 — the steering/jump
*decision* is pure and tested; only the world sampling is left to integration.

Manual acceptance checklist: mob spawns near player; walks toward player across flat
ground; climbs a 1-block step; falls off a cliff without tunneling; deals damage on
contact (hearts drop); dies after N hits and vanishes on a second connected client
simultaneously; player death snaps to spawn with full health.

---

## 10. Known limitations / explicit future work

Called out so they are choices, not omissions:

- **Single-anchor server streaming.** Mobs simulate only near the first player. Multi-
  anchor chunk loading (generalizing `world_update` to a set of anchor positions) is
  deferred.
- **No real pathfinding.** Reactive steering + 1-block step-up only. Mobs get stuck on
  walls >1 block, fences, or overhangs, and won't route around obstacles. A* / nav comes
  later.
- **Server terrain divergence.** Server runs `world_update` with `bp = NULL`, so server-
  side sand doesn't fall and water doesn't flow. A mob could briefly stand where the
  client renders falling sand. Rare; acceptable for v1.
- **Double worldgen in single-player.** Server and client each generate nearby terrain.
  Bounded by `SERVER_MOB_RENDER_DIST`. A shared-world fast path for loopback host is
  possible later.
- **No knockback** (conflicts with client-authoritative movement; needs a reconciliation
  design), **no health regen**, **no drops/loot**, **no death animation or hurt flash**,
  **no per-mob health bar**, **one mob type**, **no day/night or light-based spawning**,
  **no sound**.
- **Mob simulation cost on the host thread.** 20 Hz over ≤`MOB_CAP` mobs is trivial; the
  spawner and `world_update` dominate. Fine at v1 scale.

---

## 11. Suggested build phases

Ordered so each phase is independently verifiable:

1. **Server world**: headless world in `server.c`, seed plumbed through, anchor streaming.
   Verify chunks generate server-side (log ready count); no behavior change for clients.
2. **Mob module + sim, server-only**: `mob.{h,c}`, simulation tick, spawner. Verify via
   server logs that mobs spawn, target, and move (positions printed).
3. **Networking**: `PKT_MOB_STATE` broadcast + client `MobSet` + interpolation. Verify
   mobs appear and move on the client (render as placeholder boxes first if convenient).
4. **Rendering**: `mob_model` + mob skin + `renderer_draw_frame` wiring.
5. **Combat**: player health on server, contact damage, `PKT_PLAYER_HEALTH`, hearts HUD,
   respawn-on-death.
6. **Player→mob attack**: mob raycast, `PKT_MOB_ATTACK`, server validation, mob death.
7. **Tests** alongside each phase (targeting/steering/combat in `test_mob`, wire in
   `test_net`).
