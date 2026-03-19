# Remote Player Linear Extrapolation

**Date:** 2026-03-19
**Status:** Approved

## Problem

Remote players freeze in place between server ticks. The server broadcasts world state at 20 Hz, giving a 50ms window between snapshots. When `render_time` advances past the latest snapshot, the current interpolation clamps `t` at `1.0`, leaving the remote player frozen until the next packet arrives.

## Design

### Scope

Single function change: `remote_player_interpolate()` in `src/remote_player.c`.

### Approach: Linear Extrapolation with 2-Second Cap

When `render_time` exceeds `snapshot_times[1]`, extrapolate the remote player's position and rotation linearly using the velocity derived from the last two snapshots, capped at 2 seconds past the last known snapshot.

### Preconditions and Guards

**`snapshot_count < 2`:** When fewer than two snapshots exist, velocity derivation is invalid. The fallback is to use slot `[1]` directly (no movement). When `snapshot_count == 1`, slot `[1]` holds the one valid snapshot. `snapshot_count == 0` on an `active` player cannot occur: `remote_player_push_snapshot` sets `active = true` and increments the count atomically within the same function call, and `remote_player_remove` sets `active = false` without resetting the count — but slot reuse calls `memset(p, 0, ...)` before setting `active`, so `snapshot_count` is always 0 when a newly-allocated slot is first activated.

Note: the existing code does not guard on `snapshot_count` and produces the same visible result for `snapshot_count == 1` via arithmetic accident (`snapshot_times[0] == 0` causes `t` to clamp to 1.0, rendering at `positions[1]`). The new explicit guard makes this correct by construction rather than by accident. The `snapshot_count < 2` and `dt_snap <= 0` conditions are logically independent: the former guards against missing snapshots; the latter guards against two snapshots arriving with identical or reversed timestamps.

**`dt_snap <= 0`:** If `snapshot_times[1] - snapshot_times[0] <= 0.0`, fall back to clamped interpolation — no division by zero.

### Time Domain

`render_time` is a `double`, advanced by accumulated `float dt` per frame, starting at `recv_time - REMOTE_PLAYER_DELAY` (100ms) on first snapshot. `snapshot_times` are `double` wall-clock values from `net_time()`. Both share the same epoch.

`dt_snap` must be computed as `double`. Velocity values (`vel_x`, `vel_y`, `vel_z`, `yaw_vel`, `pit_vel`) divide `float` position/rotation differences by a `double dt_snap`; the float operand is implicitly promoted to `double` for the division, which is the intended precision.

`excess = render_time - snapshot_times[1]` is a `double - double` subtraction — no cast needed. Over the 2-second cap window at 60 Hz (~120 frames), float accumulation drift into `render_time` is at most ~140 µs — negligible against the 50ms snapshot interval.

**Interaction with `REMOTE_PLAYER_DELAY`:** `render_time` starts 100ms behind the first snapshot. The interpolation path is entered first as `render_time` catches up. Extrapolation is an "overrun" guard — it triggers only after `render_time` has fully consumed the inter-snapshot interval. If the render clock stalls (e.g., the game is paused), `render_time` may not reach `snapshot_times[1]` even during a real network outage, so extrapolation would not trigger. This is intentional: extrapolation is not a recovery mechanism for a stalled render clock.

### Logic (branch structure)

```
if snapshot_count < 2 or dt_snap <= 0.0:
    out_pos   = positions[1]
    out_yaw   = yaws[1]
    out_pitch = pitches[1]
    return

if render_time <= snapshot_times[1]:
    // interpolation path (existing logic, with yaw wrapping added)
    t = (render_time - snapshot_times[0]) / dt_snap   // dt_snap is double
    t = clamp(t, 0.0, 1.0)
    out_pos   = lerp(positions[0], positions[1], t)
    dyaw      = yaws[1] - yaws[0]
    dyaw      = dyaw - 2π * roundf(dyaw / 2π)         // wrap to [-π, π] using roundf
    out_yaw   = yaws[0] + t * dyaw
    out_pitch = pitches[0] + t * (pitches[1] - pitches[0])
else:
    // extrapolation path (new)
    excess = render_time - snapshot_times[1]           // double subtraction
    excess = clamp(excess, 0.0, 2.0)

    // vel_x/y/z and yaw_vel/pit_vel are float (float - float / double → float)
    vel_x   = (float)((positions[1][0] - positions[0][0]) / dt_snap)
    vel_y   = (float)((positions[1][1] - positions[0][1]) / dt_snap)
    vel_z   = (float)((positions[1][2] - positions[0][2]) / dt_snap)

    dyaw    = yaws[1] - yaws[0]
    dyaw    = dyaw - 2π * roundf(dyaw / 2π)           // wrap to [-π, π] using roundf
    yaw_vel = (float)(dyaw / dt_snap)

    pit_vel = (float)((pitches[1] - pitches[0]) / dt_snap)

    fexcess = (float)excess
    out_pos[0] = positions[1][0] + vel_x * fexcess
    out_pos[1] = positions[1][1] + vel_y * fexcess
    out_pos[2] = positions[1][2] + vel_z * fexcess
    out_yaw    = yaws[1] + yaw_vel * fexcess
    out_yaw    = out_yaw - 2π * roundf(out_yaw / 2π)  // normalize to [-π, π] using roundf
    out_pitch  = clamp(pitches[1] + pit_vel * fexcess, -π/2, π/2)
```

### Yaw Handling (both paths)

Both paths now wrap the yaw difference to `[-π, π]` before scaling. The extrapolation path additionally normalizes `out_yaw` back to `[-π, π]` after accumulation, since up to 2 seconds of spinning can push the value far outside a bounded range. This is a fix for a pre-existing latent wrap bug in the interpolation path as well.

### Pitch Clamping (extrapolation path only)

Extrapolated pitch is clamped to `[-π/2, π/2]`. Interpolated pitch between two valid values stays in range naturally; extrapolated pitch does not have that guarantee.

### The 2-Second Cap

The server times out clients after 10 seconds of silence (`src/server.c`). A 2-second cap is chosen because:
- It covers transient packet loss (jitter, brief congestion) without infinite drift
- It is well under the 10s server timeout — a player still frozen after 2s is almost certainly disconnected
- Beyond 2s, holding the last extrapolated position is preferable to continuing to drift

### No New Fields

Velocity is computed on the fly from existing snapshot data. No changes to `RemotePlayer` struct or `remote_player.h`.

### Trade-offs

| | Linear extrapolation (chosen) | Decay extrapolation | Freeze (current) |
|---|---|---|---|
| Correctness during lag | Good | Moderate | Poor |
| Overshoots on stop | Yes, briefly | Less | None |
| Implementation complexity | Low | Medium | — |

## Files Changed

- `src/remote_player.c` — `remote_player_interpolate()` only
