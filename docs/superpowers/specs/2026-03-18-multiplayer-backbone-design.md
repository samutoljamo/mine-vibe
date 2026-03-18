# Multiplayer Backbone Design

**Date:** 2026-03-18
**Status:** Draft
**Scope:** Networking foundation — server authority, client prediction, remote player interpolation

## Overview

Add a multiplayer backbone to the Minecraft clone. The scope is player synchronization and the server-authoritative architecture; block modification is out of scope but the design accommodates it cleanly. The same binary supports three modes: dedicated server, client, and listen server (one player hosts while also playing).

## Modes

| Invocation | Mode |
|---|---|
| `./minecraft --server` | Dedicated server — no window, no Vulkan |
| `./minecraft` | Client only — connects to a remote server |
| `./minecraft --host` | Listen server — server thread + client in same process |

In listen-server mode the server runs on a background thread. The client connects to it over loopback (`127.0.0.1`) exactly as it would connect to a remote server. There is no special local shortcut — both code paths are identical, which keeps anti-cheat logic intact.

### Startup Flow

```
main() checks argv:
  --server  → server_run()                              [no GLFW, no Vulkan]
  --host    → thread_create(server_run) + client_connect("127.0.0.1")
  (default) → client_connect(argv[1] or prompt for IP)
```

## Architecture

### Network Thread

A single background thread owns all UDP socket I/O. It exchanges state with the game loop (main thread) via two queues:

- **Inbound queue** — packets received from the network, pushed to game logic
- **Outbound queue** — packets from game logic, sent to the network

The game loop and network thread never access each other's state directly. This keeps network jitter isolated from rendering and allows the server tick rate and render rate to be independent.

### New Modules

| File | Purpose |
|---|---|
| `src/net.h/.c` | Packet structs, serialization, UDP socket helpers |
| `src/net_thread.h/.c` | Background network thread, inbound/outbound queues |
| `src/server.h/.c` | Server tick loop, client management, anti-cheat |
| `src/client.h/.c` | Connection state, input history, prediction, reconciliation |
| `src/remote_player.h/.c` | Snapshot ring buffer, interpolation |

## Protocol

All packets are sent over a single UDP socket per connection. Every packet begins with a common 8-byte header.

### Packet Header

```c
typedef struct {
    uint8_t  type;       // PacketType enum
    uint8_t  player_id;  // sender ID (0 = server)
    uint16_t seq;        // sequence number
    uint16_t ack;        // last received seq from remote
    uint16_t ack_bits;   // bitmask ACK for 16 prior sequences
} PacketHeader;
```

### Reliability Channels

**Unreliable** — player positions, camera orientation. No retransmit. Out-of-order packets are silently dropped via sequence number check; latest packet wins.

**Reliable** — connect/disconnect, player join/leave, block changes (future). Uses a send buffer with retransmit after ~100ms if no ACK received. Sliding window of 32 in-flight packets. The `ack` field ACKs 1 sequence explicitly; the `ack_bits` bitmask (Glenn Fiedler technique) covers 16 prior sequences — together they acknowledge up to 17 sequences per packet, so most reliable packets are ACKed within a single round trip even under packet loss.

### Packet Types

```c
typedef enum {
    PKT_CONNECT_REQUEST,   // client → server (reliable)
    PKT_CONNECT_ACCEPT,    // server → client (reliable): assigns player_id
    PKT_DISCONNECT,        // either direction (reliable)
    PKT_INPUT,             // client → server (unreliable, 60Hz)
    PKT_WORLD_STATE,       // server → all clients (unreliable, 20Hz)
    PKT_PLAYER_JOIN,       // server → all clients (reliable)
    PKT_PLAYER_LEAVE,      // server → all clients (reliable)
    PKT_BLOCK_CHANGE,      // server → all clients (reliable, future)
} PacketType;
```

### Key Packet Payloads

**Input** (client → server, unreliable, 21 wire bytes):
```c
typedef struct {
    PacketHeader header;   //  8 bytes
    uint32_t     tick;     //  4 bytes — client tick counter
    uint8_t      keys;     //  1 byte  — bitmask (see key bit definitions below)
    float        yaw;      //  4 bytes
    float        pitch;    //  4 bytes
} InputPacket;             // = 21 wire bytes total
```

All packets are serialized/deserialized field-by-field (not via `memcpy` of the struct) to avoid platform-dependent padding. The wire format is exactly the field sizes listed above.

**Keys bitmask bit layout** (same definition used in both `InputState` and `InputPacket`):
```c
#define KEY_BIT_W      (1 << 0)
#define KEY_BIT_S      (1 << 1)
#define KEY_BIT_A      (1 << 2)
#define KEY_BIT_D      (1 << 3)
#define KEY_BIT_SPACE  (1 << 4)
#define KEY_BIT_SPRINT (1 << 5)   // Ctrl held in existing codebase; mapped here for network transport
#define KEY_BIT_SHIFT  (1 << 6)
```

The client sends inputs at 60Hz. The server derives authoritative movement from `keys` + `yaw`/`pitch` — it never trusts a position from the client.

**World State** (server → clients, unreliable, 9 + 21×N bytes):
```c
typedef struct {
    PacketHeader header;
    uint8_t      player_count;
    struct {
        uint8_t  player_id;
        float    x, y, z;
        float    yaw, pitch;
    } players[];
} WorldStatePacket;
```

Sent at 20Hz. Clients interpolate between received snapshots for smooth remote player rendering.

## Server Logic

### Server Tick (20Hz fixed)

Each tick:
1. Drain inbound queue — process all received packets
2. For each `PKT_INPUT`: validate then apply to server-side player state. Multiple inputs may arrive between server ticks (clients send at 60Hz, server ticks at 20Hz). All inputs accumulated since the last tick are processed in arrival order, running `player_physics_tick()` once per input. This ensures inputs are not lost and movement scales correctly.
3. Run `player_physics_tick()` for any player that received no input this tick (preserves gravity/friction)
4. Broadcast `PKT_WORLD_STATE` to all clients
5. Handle connect/disconnect, timeout detection

The server owns a `World*` used only for collision queries. It does not render.

### Connected Client State

```c
typedef struct {
    struct sockaddr_in addr;
    uint8_t    player_id;        // 1–255; 0 reserved for server
    double     last_input_time;  // for timeout detection (double: float loses precision at Unix timestamps)
    uint32_t   last_input_tick;  // for stale-input detection
    uint8_t    last_keys;
    float      yaw, pitch;
    Player     player;           // authoritative player state
} ConnectedClient;
```

Max clients is a runtime parameter (default 32). Player IDs are assigned from a free-slot pool and reused on disconnect. `PKT_PLAYER_LEAVE` for the departing player is always sent and ACKed (reliable channel) before a `PKT_PLAYER_JOIN` can be sent for a new player reusing the same ID, preventing stale world-state snapshots from rendering the wrong body at the wrong position.

### Anti-Cheat

The server only accepts `keys`, `yaw`, and `pitch` from clients. It runs physics itself. Three validation checks:

1. **Speed cap** — if simulated position delta exceeds `PLAYER_SPRINT_SPEED × 1.5` per tick, discard the packet and do not move the player. The current codebase defines this value as `SPRINT_SPEED` in `src/player.c` (file-local). This implementation must move it to `src/player.h` as a public `#define PLAYER_SPRINT_SPEED` so that `server.c` can include it — making `player.h` the single source of truth for both client and server. The 1.5× margin absorbs jitter without blocking legitimate movement.
2. **Stale input** — if packet `tick` ≤ `last_input_tick` for that client, discard (prevents input replay).
3. **Timeout** — no input received for 10 seconds triggers disconnect; `PKT_PLAYER_LEAVE` broadcast to all clients.

When block modification is added, all block change requests are validated server-side (range check, permissions) before being applied and rebroadcast.

## Client Logic

### Client-Side Prediction

The client applies its own input immediately using `player_physics_tick()` so input feels instant regardless of RTT. An input history ring buffer (128 ticks) records each frame:

```c
typedef struct {
    uint32_t tick;
    uint8_t  keys;
    float    yaw, pitch;
    vec3     predicted_position;
} InputRecord;
```

When `PKT_WORLD_STATE` arrives with the authoritative local player position:

1. Locate the matching tick in the history buffer
2. If server position diverges from predicted position by more than **0.5 blocks**: reconcile — reset to server position, re-simulate all buffered inputs from that tick forward
3. If divergence ≤ 0.5 blocks: smoothly lerp toward server position over ~100ms to avoid visible snapping

The 0.5-block threshold absorbs normal floating-point and network jitter.

### Remote Player Interpolation

Remote players are rendered at an interpolated position between the two most recent server snapshots, with a fixed **100ms interpolation delay** (2 server ticks at 20Hz, providing one spare snapshot buffer):

```c
typedef struct {
    uint8_t  player_id;
    vec3     positions[2];      // [0] = older snapshot, [1] = newer snapshot
    float    yaws[2], pitches[2];
    double   snapshot_times[2]; // client-side receive timestamp via glfwGetTime()
    uint8_t  snapshot_count;    // 0 = no data, 1 = one snapshot, 2 = two snapshots ready
    double   render_time;       // trails real time by 100ms; initialized to glfwGetTime() - 0.1 on first snapshot
} RemotePlayer;
```

**Snapshot insertion:** when a new snapshot arrives, the network thread records the receive timestamp via `glfwGetTime()` in the inbound queue entry. The game loop writes it into the ring buffer: shift `[1]` → `[0]`, write new data into `[1]`, increment `snapshot_count` up to 2. On the very first snapshot (`snapshot_count` was 0), initialize `render_time = snapshot_times[0] - 0.1` (100ms behind). Interpolation only begins once `snapshot_count == 2`; before that the player is not rendered.

Each frame: advance `render_time` by `frame_dt`. Compute `t = (render_time - snapshot_times[0]) / (snapshot_times[1] - snapshot_times[0])`. Lerp position/yaw/pitch. If no new snapshot has arrived and `t > 1.0`, clamp to 1.0 — the player freezes rather than extrapolating. Both `render_time` and `snapshot_times` use `glfwGetTime()` as the single clock source.

Remote players are rendered as placeholder AABBs (0.6×1.8×0.6) matching the player hitbox dimensions until player models are added.

## Integration with Existing Code

### Modified Files

**`src/player.h`** — The `Player` struct already has a complete agent mode (added for the agent control API): `agent_mode`, `agent_forward`, `agent_right`, `agent_jump`, `agent_sprint`. When `agent_mode == true`, `player_update()` bypasses all GLFW calls and uses these fields instead. **No new functions are needed.** The server drives player physics by:

1. Setting `player.agent_mode = true`
2. Setting `player.camera.yaw` / `player.camera.pitch` from the deserialized packet
3. Mapping the `keys` bitmask to agent fields:
```c
// In server.c, when applying a PKT_INPUT:
p->player.agent_forward = (keys & KEY_BIT_W) ? 1.0f : (keys & KEY_BIT_S) ? -1.0f : 0.0f;
p->player.agent_right   = (keys & KEY_BIT_D) ? 1.0f : (keys & KEY_BIT_A) ? -1.0f : 0.0f;
p->player.agent_jump    = (keys & KEY_BIT_SPACE) != 0;
p->player.agent_sprint  = (keys & KEY_BIT_SPRINT) != 0;
player_update(&p->player, NULL, world, PHYSICS_DT);  // NULL window is safe in agent mode
```

`PHYSICS_DT` (`1.0f / 60.0f`) must be moved from the local `#define` in `player.c` to a public `#define` in `player.h` so `server.c` can use it. `PLAYER_SPRINT_SPEED` (currently `SPRINT_SPEED` in `player.c`) must also be moved to `player.h` for the anti-cheat speed cap.

**`src/player.h/.c`** — Add `InputState` to `player.h` for use in the client's input history buffer (the server uses agent fields directly, but the client needs to record inputs for prediction/reconciliation):
```c
typedef struct {
    uint8_t keys;       // KEY_BIT_* bitmask
    float   yaw, pitch;
} InputState;
```

**`src/main.c`** — Mode detection from `argv`. In client/listen-server mode: spawn `NetThread`, create `Client`, integrate `RemotePlayer` list into render loop. In `--server` mode: skip GLFW and Vulkan entirely, call `server_run()`.

**`src/renderer.h/.c`** — Add `renderer_draw_remote_players(Renderer*, RemotePlayer*, uint32_t count)` for placeholder AABB rendering. One new draw call after the existing chunk draw.

### Server World Management

The server calls `world_update(world, &bp, player_pos)` which now requires a `BlockPhysics*` parameter (added alongside block physics simulation). The server must:
- Create and own a `BlockPhysics bp` instance (`block_physics_init(&bp)` on startup, `block_physics_destroy(&bp)` on shutdown)
- Call `block_physics_update(&bp, world, player_pos, dt)` each server tick
- Pass `&bp` to `world_update()`

When `PKT_BLOCK_CHANGE` is implemented (future), the server will call `world_set_block()` / `world_set_meta()` — these APIs already exist in `world.h`.

### Unchanged

Chunk system, mesher, world gen, camera, texture, pipeline, swapchain, frustum, physics collision logic. The networking layer sits entirely above these.

## Data Flow

```
Client (main thread)                         Server (background thread)
─────────────────────────────────────        ─────────────────────────────────────
read GLFW → InputState (for history)         drain inbound queue
player_update() [local prediction]             → for each PKT_INPUT: validate
push PKT_INPUT → outbound queue                → set agent fields → player_update()
                                             block_physics_update()
                                             broadcast PKT_WORLD_STATE → outbound queue

NetThread
─────────────────────────────────────
recv() → push to inbound queue
pop outbound queue → send()

Client (on PKT_WORLD_STATE)
─────────────────────────────────────
local player: reconcile if diverged
remote players: push snapshot into ring buffer
```

## Non-Goals (this spec)

- Block placement / destruction (architecture supports it; add later)
- Server browser / matchmaking
- Player models (placeholder AABBs used)
- Chat
- Latency display / debug overlay
- Encryption or authentication
- Saving/loading world state across sessions

## Success Criteria

1. `--server` starts a headless server, `--host` starts a listen server with a playable client
2. Two clients can connect and see each other's positions update in real time
3. Server handles N simultaneous clients up to the configured maximum without crash or degradation
4. Disconnecting one client does not crash the other or the server
5. Client-side prediction keeps local movement feeling instant at 100ms simulated RTT
6. Server rejects inputs that would move a player faster than `PLAYER_SPRINT_SPEED × 1.5`
7. Server detects and disconnects timed-out clients after 10 seconds of silence
