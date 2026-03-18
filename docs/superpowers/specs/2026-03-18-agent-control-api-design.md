# Agent Control API — Design Spec

**Date:** 2026-03-18
**Status:** Draft
**Scope:** Pipe-based agent control API for the Minecraft clone, enabling AI agents to drive the player, read game state, and capture frames for visual debugging.

## Overview

Agents currently have access only to code, compilation output, and stderr. This is insufficient for debugging visual or gameplay bugs in a graphical application. This sub-project adds a lightweight control API that lets an agent:

- Drive the existing player (movement, look, jump, mode switching)
- Read structured game state each tick
- Trigger frame captures to PNG for visual inspection

The API uses **stdin/stdout pipes** with newline-delimited JSON. It is activated via a `--agent` CLI flag and is otherwise a no-op. The design intentionally mirrors what a future multiplayer client thread would look like, making that transition straightforward.

## Architecture

### New Module: `agent.h/.c`

One new module handles all agent logic. Active only when `--agent` is passed.

**Agent I/O thread** owns stdin/stdout exclusively. It:
- Reads lines from stdin, parses JSON commands, pushes them to a mutex-protected command ring buffer (capacity 64 commands; on overflow, newest command is dropped and an error event is queued)
- Receives state snapshots pushed by the main thread and writes JSON events to stdout

**Shared state** — two structs bridging the threads:

```c
// Written by main thread once per rendered frame; agent thread reads to emit state events
typedef struct {
    float    pos[3];
    float    vel[3];
    float    yaw, pitch;
    int      on_ground;
    int      mode;        // mirrors PlayerMode enum
    uint64_t tick;        // increments once per rendered frame
} AgentSnapshot;          // protected by a single mutex

// One entry per pending command; ring buffer (capacity 64), mutex-protected
typedef struct {
    AgentCommandType type;  // enum: CMD_MOVE, CMD_LOOK, CMD_JUMP, CMD_SPRINT,
                            //       CMD_MODE, CMD_GET_STATE, CMD_DUMP_FRAME, CMD_QUIT
    union {
        struct { float forward; float right; } move;   // range [-1, 1]
        struct { float yaw; float pitch; }    look;    // degrees, pitch clamped to [-90, 90]
        struct { int   active; }              sprint;
        struct { int   mode; }                mode;    // PlayerMode enum value
        struct { char  path[256]; }           dump_frame;
    };
} AgentCommand;
```

**Ring buffer overflow policy:** drop the newest incoming command (tail drop). Emit `{"event": "error", "msg": "command queue full, command dropped"}` on the output queue.

**Integration points (minimal):**

| File | Change |
|------|--------|
| `main.c` | Change signature to `int main(int argc, char *argv[])`; parse `--agent`; call `agent_init()`, `agent_tick()` each frame after `world_update`, before `renderer_draw_frame`; pass `dump_frame` flag + path to `renderer_draw_frame` |
| `player.c` | In agent mode, `player_update()` consumes agent movement/look/jump/sprint/mode commands instead of polling GLFW input |
| `renderer.c` | Add `renderer_dump_frame(Renderer*, const char *path)` |
| `world.c` | Notify agent (via event queue) when a chunk transitions to READY state |

All game diagnostic output must use **stderr only**. stdout is reserved exclusively for agent protocol traffic.

## Protocol

Newline-delimited JSON. One JSON object per line in both directions.

### Agent → Game (stdin)

| Command | Fields | Description |
|---------|--------|-------------|
| `move` | `forward` (float [-1,1]), `right` (float [-1,1]) | Held each tick; 0 means no input. Thresholds at 0 to emulate key press/release — does not scale velocity continuously. |
| `look` | `yaw` (float), `pitch` (float) | Absolute camera orientation in degrees. Pitch clamped to [-90, 90]. |
| `jump` | — | Triggers jump if on ground, swim-up if in water. Does **not** participate in the double-tap mode-switch state machine — use `mode` command to switch modes. |
| `sprint` | `active` (bool as 0/1) | Enable/disable sprint. |
| `mode` | `value` ("walk"\|"free") | Switch player mode directly, bypassing double-tap logic. |
| `get_state` | — | Request an immediate state snapshot on stdout. |
| `dump_frame` | `path` (string, max 255 chars) | Capture current frame to PNG at given path. Processed after `renderer_draw_frame` completes for the current frame. |
| `quit` | — | Cleanly exit the game. |

### Movement semantics

`move.forward` and `move.right` are treated as discrete on/off per tick (threshold: any nonzero value = key held). In walking mode the movement vector is flattened to XZ (identical to keyboard behavior). In free mode it uses the full 3D `front` vector including pitch. This matches the existing physics code exactly — no changes to velocity math are needed.

### Game → Agent (stdout)

| Event | Fields | Description |
|-------|--------|-------------|
| `ready` | — | World loaded (25% chunks ready gate cleared); agent may begin issuing commands |
| `state` | `pos[3]`, `vel[3]`, `yaw`, `pitch`, `mode`, `on_ground`, `tick` | Emitted once per rendered frame (one `agent_tick()` call). Also emitted immediately on `get_state`. |
| `frame_saved` | `path` | Frame PNG written successfully |
| `chunk_loaded` | `cx`, `cz` | A chunk transitioned to READY state |
| `error` | `msg` | Unknown command, bad payload, queue overflow, or frame capture failure |

**Example exchange:**
```
← {"event": "ready"}
→ {"cmd": "move", "forward": 1.0, "right": 0.0}
← {"event": "state", "pos": [0.5, 65.0, 0.5], "vel": [0.0, 0.0, 0.0], "yaw": 0.0, "pitch": 0.0, "mode": "walk", "on_ground": true, "tick": 120}
→ {"cmd": "dump_frame", "path": "frame_001.png"}
← {"event": "frame_saved", "path": "frame_001.png"}
```

## Frame Capture

`renderer_dump_frame(Renderer *r, const char *path)` is called by the main thread **immediately after `vkQueueWaitIdle` at the end of `renderer_draw_frame`**, before the function returns. This guarantees the image that was just rendered is in a known layout and the GPU is idle. The swapchain image index used for the current frame is captured via a parameter passed into `renderer_dump_frame` from `renderer_draw_frame`'s local scope.

Steps:
1. Allocate a host-visible VMA staging buffer sized to the swapchain image (width × height × 4 bytes RGBA)
2. Submit a one-shot command buffer: transition swapchain image → `TRANSFER_SRC_OPTIMAL`, `vkCmdCopyImageToBuffer`, transition back → `PRESENT_SRC_KHR`
3. `vkQueueWaitIdle` — blocks the game loop for this one frame (acceptable; this is an infrequent debug call)
4. Map the buffer, write PNG via `stb_image_write_png()` (add `#define STB_IMAGE_WRITE_IMPLEMENTATION` alongside existing `STB_IMAGE_IMPLEMENTATION`)
5. Free staging buffer; emit `frame_saved` event via the agent event queue

The staging buffer and command buffer are created and destroyed per call — no persistent overhead when `dump_frame` is not used.

The `Renderer` struct gains one field: `uint32_t last_image_index` — set each frame in `renderer_draw_frame` so `renderer_dump_frame` knows which swapchain image to read.

## Activation & Headless Execution

When `--agent` is passed:
- GLFW mouse callback and keyboard input polling are **not registered**
- `agent_init()` spawns the I/O thread
- `{"event": "ready"}` is emitted once the existing loading-screen gate clears (25% of chunks loaded)
- The game window opens normally — Vulkan requires a real surface

**Headless environments (no display):** Use `Xvfb`:

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 ./build/minecraft --agent
```

No offscreen Vulkan path is needed. GLFW gets a surface, rendering proceeds normally, frame dumps work. This keeps the implementation significantly simpler than a true headless renderer.

## JSON Parsing

No external JSON library. Commands are simple enough to parse with `sscanf` / `strstr` pattern matching on known keys. The command set is small and fixed. Output is formatted with `snprintf` directly.

## Error Handling

- Unknown commands → `{"event": "error", "msg": "unknown command: <cmd>"}`
- `dump_frame` failure (VK error, write error) → `{"event": "error", "msg": "..."}`
- Ring buffer full → drop newest command, emit `{"event": "error", "msg": "command queue full, command dropped"}`
- Agent I/O thread exits on EOF (pipe closed) → game continues running normally until `quit` or window close
- Malformed JSON (missing required keys) → emit error event, discard command
- `dump_frame` path longer than 255 chars → emit error, discard command
- `look` pitch outside [-90, 90] → clamp silently, no error

## Future Compatibility

The agent I/O thread structure (owns pipe, pushes commands to a queue, main thread applies them) is identical to what a future multiplayer client receive thread would look like. When multiplayer is added, `agent.c` either:
- Gets replaced by a proper network client, or
- Continues to exist as a local-only debug interface alongside multiplayer

No multiplayer-specific design decisions are made here.

## Success Criteria

1. `./build/minecraft --agent` accepts JSON commands on stdin and writes events to stdout
2. `dump_frame` produces a valid PNG that visually matches what the game window shows
3. `move`, `look`, `jump`, `sprint`, `mode` correctly drive the player with identical behavior to keyboard input
4. `state` events emit once per rendered frame with accurate player state
5. `chunk_loaded` events fire when chunks become ready
6. In normal mode (no `--agent`), no agent code executes and there is no performance impact
7. Works under `Xvfb` for headless agent use
8. Ring buffer overflow produces an error event rather than a crash or hang

## Non-Goals

- Multiplayer networking
- Block placement/destruction via agent (deferred to interaction sub-project)
- World state queries beyond player state (e.g. "what block is at X,Y,Z") — deferred
- Authentication or security (local debug tool only)
- Performance-optimized frame capture (blocking readback is acceptable)
- Continuous analog velocity scaling (discrete on/off is sufficient and matches existing physics)
