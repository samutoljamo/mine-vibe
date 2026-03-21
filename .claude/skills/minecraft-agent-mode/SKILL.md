---
name: minecraft-agent-mode
description: Use when controlling the minecraft Vulkan game programmatically — sending movement, capturing frames, reading state, or running automated sequences via the pipe-based agent API
---

# Minecraft Agent Mode

## Overview

Pipe-based remote control API activated with `--agent`. Send newline-delimited JSON to stdin; read JSON events from stdout. Do NOT use distrobox to run — only for building. **Build first** using the `build-minecraft` skill.

## Launch with tmux (recommended)

tmux is the best way to interact: `send-keys` to send commands, `capture-pane` to read output.

```bash
# Start game in a tmux session
tmux new-session -d -s mc -x 220 -y 50
tmux send-keys -t mc "./build/minecraft --agent" Enter

# Send a command
tmux send-keys -t mc '{"cmd":"get_state"}' Enter

# Read output (state events flood stdout — check files directly for frame_saved)
tmux capture-pane -t mc -p | grep '"pos"' | tail -1
```

## Commands (you → game stdin)

| JSON | Effect |
|------|--------|
| `{"cmd":"move","forward":1.0,"right":0.0}` | Move; nonzero = held, 0 = released; range [-1, 1] |
| `{"cmd":"look","yaw":90.0,"pitch":-20.0}` | Set absolute camera; **negative pitch = look down**, positive = up |
| `{"cmd":"jump"}` | Jump or swim-up; edge-triggered, one frame only |
| `{"cmd":"sprint","active":1}` | Sprint on (1) or off (0) |
| `{"cmd":"mode","value":"walk"}` | `"walk"` or `"free"` (noclip/fly) |
| `{"cmd":"get_state"}` | Request immediate state snapshot |
| `{"cmd":"dump_frame","path":"/tmp/f.png"}` | Save current frame to PNG |
| `{"cmd":"quit"}` | Clean exit |

## Events (game stdout → you)

| JSON | When emitted |
|------|-------------|
| `{"event":"ready"}` | ~30% of chunks loaded — safe to start sending commands |
| `{"event":"state","pos":[x,y,z],"vel":[vx,vy,vz],"yaw":f,"pitch":f,"mode":"walk","on_ground":1,"tick":N}` | Every rendered frame |
| `{"event":"chunk_loaded","cx":N,"cz":N}` | Each chunk transitions to CHUNK_READY |
| `{"event":"frame_saved","path":"/tmp/f.png"}` | After successful PNG write |
| `{"event":"error","msg":"..."}` | Unknown command, malformed JSON, queue full, capture failed |

## Free mode flight

In free mode, the player moves in the direction they're facing — pitch controls vertical movement:
- `pitch: 89` + `forward: 1` → fly straight up
- `pitch: -89` + `forward: 1` → fly straight down
- `pitch: 0` + `forward: 1` → fly horizontal

Easy pattern to fly up to surface: look up (`pitch: 89`), move forward, then level off.

## Reading frames

State events flood stdout and scroll off `frame_saved` events. Don't grep for `frame_saved` — just check the file directly:

```bash
tmux send-keys -t mc '{"cmd":"dump_frame","path":"/tmp/frame.png"}' Enter
sleep 2
ls -la /tmp/frame.png   # check it exists
# then view it:
# Read tool: Read /tmp/frame.png
```

## Common Mistakes

| Mistake | Fix |
|---------|-----|
| Running via distrobox | Just `./build/minecraft --agent` directly |
| Positive pitch to look down | Negative pitch looks down; positive looks up |
| Grepping stdout for `frame_saved` | State spam scrolls it off — check the file directly |
| Flying underground in free mode | Check y in state; fly up with `pitch:89, forward:1` to surface |
| Sending commands before `ready` | World not loaded; wait for `{"event":"ready"}` first |

## Notes

- Stdout is protocol-only; all diagnostics go to stderr
- Ring buffer holds 64 commands; overflow drops newest + emits error
- Water is blue (renderer captures correctly after R↔B swapchain fix)
- `on_ground` may flicker 0/1 on flat terrain — normal physics behavior
