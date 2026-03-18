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

**Reliable** — connect/disconnect, player join/leave, block changes (future). Uses a send buffer with retransmit after ~100ms if no ACK received. Sliding window of 32 in-flight packets. The `ack_bits` bitmask (Glenn Fiedler technique) allows one packet to ACK up to 17 prior sequences, so most reliable packets are ACKed within a single round trip even under packet loss.

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

**Input** (client → server, unreliable, ~21 bytes):
```c
typedef struct {
    PacketHeader header;
    uint32_t     tick;        // client tick counter
    uint8_t      keys;        // bitmask: W/A/S/D/space/sprint/shift
    float        yaw, pitch;  // camera orientation
} InputPacket;
```

The client sends inputs at 60Hz. The server derives authoritative movement from `keys` + `yaw`/`pitch` — it never trusts a position from the client.

**World State** (server → clients, unreliable, 8 + 9×N bytes):
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
2. For each `PKT_INPUT`: validate then apply to server-side player state
3. Run `player_physics_tick()` for all connected players
4. Broadcast `PKT_WORLD_STATE` to all clients
5. Handle connect/disconnect, timeout detection

The server owns a `World*` used only for collision queries. It does not render.

### Connected Client State

```c
typedef struct {
    struct sockaddr_in addr;
    uint8_t    player_id;       // 1–255; 0 reserved for server
    float      last_input_time; // for timeout detection
    uint32_t   last_input_tick; // for stale-input detection
    uint8_t    last_keys;
    float      yaw, pitch;
    Player     player;          // authoritative player state
} ConnectedClient;
```

Max clients is a runtime parameter (default 32). Player IDs are assigned from a free-slot pool and reused on disconnect.

### Anti-Cheat

The server only accepts `keys`, `yaw`, and `pitch` from clients. It runs physics itself. Three validation checks:

1. **Speed cap** — if simulated position delta exceeds `sprint_speed × 1.5` per tick, discard the packet and do not move the player. The 1.5× margin absorbs jitter without blocking legitimate movement.
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

Remote players are rendered at an interpolated position between the two most recent server snapshots, with a fixed **100ms interpolation delay** (5 server ticks at 20Hz):

```c
typedef struct {
    uint8_t  player_id;
    vec3     positions[2];
    float    yaws[2], pitches[2];
    double   snapshot_times[2];
    double   render_time;        // trails real time by 100ms
} RemotePlayer;
```

Each frame: advance `render_time`, compute `t = (render_time - snapshot_times[0]) / (snapshot_times[1] - snapshot_times[0])`, lerp position/yaw/pitch. If no new snapshot has arrived and `t > 1.0`, clamp to 1.0 — the player freezes rather than extrapolating.

Remote players are rendered as placeholder AABBs (0.6×1.8×0.6) matching the player hitbox dimensions until player models are added.

## Integration with Existing Code

### Modified Files

**`src/player.h/.c`** — Split input and physics into separate functions:
```c
void player_process_input(Player* player, GLFWwindow* window, InputState* out);
void player_physics_tick(Player* player, World* world, InputState* input, float dt);
```
`player_process_input` reads GLFW keys/mouse. `player_physics_tick` applies an `InputState` to physics — callable by both the client (prediction) and server (authoritative simulation) without GLFW.

**`src/main.c`** — Mode detection from `argv`. In client/listen-server mode: spawn `NetThread`, create `Client`, integrate `RemotePlayer` list into render loop. In `--server` mode: skip GLFW and Vulkan entirely, call `server_run()`.

**`src/renderer.h/.c`** — Add `renderer_draw_remote_players(Renderer*, RemotePlayer*, uint32_t count)` for placeholder AABB rendering. One new draw call after the existing chunk draw.

### Unchanged

Chunk system, mesher, world gen, worldgen, camera, texture, pipeline, swapchain, frustum, physics collision logic. The networking layer sits entirely above these.

## Data Flow

```
Client (main thread)                     Server (background thread)
─────────────────────────────────────    ─────────────────────────────────────
poll_input()                             drain inbound queue
  → InputState                             → for each PKT_INPUT: validate
player_process_input() [local prediction]    → player_physics_tick() (authoritative)
push PKT_INPUT → outbound queue          broadcast PKT_WORLD_STATE → outbound queue

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
3. Disconnecting one client does not crash the other or the server
4. Client-side prediction keeps local movement feeling instant at 100ms simulated RTT
5. Server rejects inputs that would move a player faster than `sprint_speed × 1.5`
6. Server detects and disconnects timed-out clients
