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
- Reads lines from stdin, parses JSON commands, pushes them to a mutex-protected command ring buffer
- Receives state snapshots from the main thread and writes JSON events to stdout

**Shared state** — two structs bridging the threads:

```c
// Written by main thread each tick; agent thread reads to emit state events
typedef struct {
    float    pos[3];
    float    vel[3];
    float    yaw, pitch;
    int      on_ground;
    int      mode;        // mirrors PlayerMode enum
    uint64_t tick;
} AgentSnapshot;          // protected by a single mutex

// One entry per pending command; ring buffer, mutex-protected
typedef struct {
    AgentCommandType type;
    union { /* per-command payload */ };
} AgentCommand;
```

**Integration points (minimal):**

| File | Change |
|------|--------|
| `main.c` | Parse `--agent`; call `agent_init()`, `agent_tick()` each frame |
| `player.c` | In agent mode, `player_update()` consumes agent commands instead of keyboard input |
| `renderer.c` | Add `renderer_dump_frame(const char *path)` |

No other files change.

## Protocol

Newline-delimited JSON. One JSON object per line in both directions.

### Agent → Game (stdin)

| Command | Fields | Description |
|---------|--------|-------------|
| `move` | `forward` (float), `right` (float) | Analog movement input, range [-1, 1] |
| `look` | `yaw` (float), `pitch` (float) | Absolute camera orientation in degrees |
| `jump` | — | Trigger jump |
| `sprint` | `active` (bool) | Enable/disable sprint |
| `mode` | `value` ("walk"\|"free") | Switch player mode |
| `get_state` | — | Request an immediate state snapshot |
| `dump_frame` | `path` (string) | Capture current frame to PNG at given path |
| `quit` | — | Cleanly exit the game |

### Game → Agent (stdout)

| Event | Fields | Description |
|-------|--------|-------------|
| `ready` | — | World loaded, agent may begin issuing commands |
| `state` | `pos`, `vel`, `yaw`, `pitch`, `mode`, `on_ground`, `tick` | Emitted every tick (and on `get_state`) |
| `frame_saved` | `path` | Frame PNG written successfully |
| `chunk_loaded` | `cx`, `cz` | A chunk became ready |
| `error` | `msg` | Unknown command or bad payload |

**Example exchange:**
```
← {"event": "ready"}
→ {"cmd": "move", "forward": 1.0, "right": 0.0}
← {"event": "state", "pos": [0.5, 65.0, 0.5], "vel": [0.0, 0.0, 0.0], "yaw": 0.0, "pitch": 0.0, "mode": "walk", "on_ground": true, "tick": 120}
→ {"cmd": "dump_frame", "path": "frame_001.png"}
← {"event": "frame_saved", "path": "frame_001.png"}
```

## Frame Capture

`renderer_dump_frame(path)` is called by the main thread when it drains a `dump_frame` command. Steps:

1. Allocate a host-visible VMA staging buffer sized to the swapchain image (width × height × 4 bytes RGBA)
2. Submit a one-shot command buffer: transition swapchain image → `TRANSFER_SRC_OPTIMAL`, `vkCmdCopyImageToBuffer`, transition back → `PRESENT_SRC_KHR`
3. `vkQueueWaitIdle` — blocks the game loop for this one frame (acceptable; this is an infrequent debug call)
4. Map the buffer, write PNG via `stb_image_write_png()` (add `#define STB_IMAGE_WRITE_IMPLEMENTATION` alongside existing `STB_IMAGE_IMPLEMENTATION`)
5. Free staging buffer; emit `frame_saved` event

The staging buffer and command buffer are created and destroyed per call — no persistent overhead when `dump_frame` is not used.

## Activation & Headless Execution

When `--agent` is passed:
- GLFW keyboard/mouse callbacks are **not registered**
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
- Agent thread exits on EOF (pipe closed) → game continues running normally until `quit` or window close
- Malformed JSON (missing keys) → silently ignored with error event

## Future Compatibility

The agent I/O thread structure (owns socket/pipe, pushes commands to a queue, main thread applies them) is identical to what a future multiplayer client receive thread would look like. When multiplayer is added, `agent.c` either:
- Gets replaced by a proper network client, or
- Continues to exist as a local-only debug interface alongside multiplayer

No multiplayer-specific design decisions are made here.

## Success Criteria

1. `./build/minecraft --agent` accepts JSON commands on stdin and writes events to stdout
2. `dump_frame` produces a valid PNG that visually matches what the game window shows
3. `move`, `look`, `jump`, `sprint`, `mode` correctly drive the player
4. `state` events reflect accurate player state each tick
5. In normal mode (no `--agent`), no agent code executes and there is no performance impact
6. Works under `Xvfb` for headless agent use

## Non-Goals

- Multiplayer networking
- Block placement/destruction via agent (deferred to interaction sub-project)
- World state queries beyond player state (e.g. "what block is at X,Y,Z") — deferred
- Authentication or security (local debug tool only)
- Performance-optimized frame capture (blocking readback is acceptable)
