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

**Velocity computation:**
```
dt_snap = snapshot_times[1] - snapshot_times[0]
vel_x   = (positions[1][0] - positions[0][0]) / dt_snap
vel_y   = (positions[1][1] - positions[0][1]) / dt_snap
vel_z   = (positions[1][2] - positions[0][2]) / dt_snap
yaw_vel = (yaws[1] - yaws[0]) / dt_snap
pit_vel = (pitches[1] - pitches[0]) / dt_snap
```

**Extrapolation:**
```
excess  = clamp(render_time - snapshot_times[1], 0.0, 2.0)
out_pos = positions[1] + vel * excess
out_yaw = yaws[1] + yaw_vel * excess
out_pitch = pitches[1] + pit_vel * excess
```

**Guard:** If `dt_snap <= 0` (identical timestamps), fall back to clamped interpolation as today — no division by zero.

### No New Fields

Velocity is computed on the fly from existing snapshot data. No changes to `RemotePlayer` struct or `remote_player.h`.

### Trade-offs

| | Linear extrapolation (chosen) | Decay extrapolation | Freeze (current) |
|---|---|---|---|
| Correctness during lag | Good | Moderate | Poor |
| Overshoots on stop | Yes, briefly | Less | None |
| Implementation complexity | Low | Medium | — |

A 2-second cap means extrapolation is conservative — after 2s of no packets the player stops, which is preferable to drifting infinitely.

## Files Changed

- `src/remote_player.c` — `remote_player_interpolate()` only
