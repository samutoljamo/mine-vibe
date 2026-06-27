# Day/Night Cycle with Time-Gated Mob Spawning

Beads: mine-vibe-h8q (depends on mine-vibe-9u4 — protocol version field).

## Goal
Server owns a world-time clock. It advances every tick, is broadcast cheaply to
clients, drives the sky/sun/ambient brightness in the renderer, and gates
hostile mob spawning to darkness. Server-authoritative: clients display time but
never advance it independently (except singleplayer, which has no server).

## Design principles honored
- Server authoritative: `Server.world_ticks` is the single source of truth.
- No new packet: time piggybacks on the existing `PKT_WORLD_STATE` broadcast
  (already sent every server tick at 20 Hz). +4 bytes per packet.
- Version-guarded: the new trailing field changes the wire format; mine-vibe-9u4
  rejects mismatched clients at handshake before any `PKT_WORLD_STATE` is sent,
  so appending the field is safe.
- Small bandwidth + client-side smoothing so the sky never freezes or pops.

## 1. Server world-time representation
- Add `uint32_t world_ticks;` to `Server` (src/server.h, near `mob_spawn_timer`).
- `#define DAY_LENGTH_TICKS (20*60*20)` = 24000 ticks (20-min day at 20 Hz) in a
  new shared header `src/daynight.h`.
- Phase: `t01 = (world_ticks % DAY_LENGTH_TICKS) / (float)DAY_LENGTH_TICKS` in
  [0,1). Convention: 0.0 dawn, 0.25 noon, 0.5 dusk, 0.75 midnight. Tests pin it.
- Advance: `s->world_ticks++` once per tick in `server_tick` (src/server.c
  ~line 513). Init `s.world_ticks = DAY_LENGTH_TICKS/4` (noon) in `server_run`.

## 2. Wire format
Tradeoff: a dedicated `PKT_WORLD_TIME` is cleaner/lower-rate but wastes a
datagram for 4 bytes; piggyback on the already-sent (unreliable, idempotent)
`PKT_WORLD_STATE`. Chosen: piggyback.

Layout: `[header 8][count u8][players 21*count][world_ticks u32]` (+4 bytes,
appended after the player array so existing offsets are unchanged).

- net.h: `net_write_world_state` gains a `uint32_t world_ticks` param, appends
  it; update the doc comment.
- server.c:566: pass `s->world_ticks`.
- client.c PKT_WORLD_STATE branch (~163-198): extend the truncation length check
  by +4, read trailing u32, store into new `Client.world_ticks` /
  `world_ticks_recv_time` (client.h ~line 40).

Client smoothing (no drift): render-time estimate
`est = world_ticks + (now - recv_time)*SERVER_TICK_RATE`; snap/re-anchor on each
packet. Missed packets keep advancing from last anchor; wrap re-anchors safely.

## 3. Renderer
Key lever: `block.frag:44` multiplies baked sky-light by `ubo.sun_color`, so
scaling `sun_color` by a day-factor uniformly darkens the world at night without
touching baked lighting.

New pure module `src/daynight.{c,h}` (no Vulkan/net/globals):
- `float daynight_brightness(float t01)` → 0..1, plateau at midday, ~0.12 floor
  at night, cosine ramps over dawn/dusk windows. Drives `sun_color` and
  `ambient = 0.06 + 0.24*brightness`.
- `void daynight_sky_color(float t01, float out[3])` → day (0.53,0.81,0.92),
  night (0.02,0.02,0.06), warm sunrise/sunset tint (0.95,0.55,0.30) blended via
  a triangular weight peaking mid-dawn/dusk window.
- optional `void daynight_sun_dir(float t01, float out[3])` → sun arc.

Wiring: replace the static `sun_dir`/clear color. Recommended: add
`float day_brightness, vec3 sky_color` params to `renderer_draw_frame`
(renderer.h + call sites main.c:320, :513). In renderer_frame.c set
`ubo.sun_color = day_brightness` (was 1.0), `ubo.ambient` (replaces const at
line 72), and the clear color (lines 106-108) = `sky_color` lerped toward
deep-water by the existing `underwater` factor. No shader source changes needed.
Singleplayer: advance a local tick counter (recommended) or lock at noon.

## 4. Mob spawn gating
`bool daynight_is_dark(uint32_t world_ticks)` → brightness < SPAWN_DARK_THRESHOLD
(~0.25). In server_tick gate `server_try_spawn` (src/server.c 527-531) on
darkness; only accumulate/attempt the spawn timer when dark to avoid a dusk
burst. Only MOB_ZOMBIE exists today (all hostile) so gating the whole path is
correct; when passive mobs arrive, gate only hostile types.

## 5. Testability (tests/test_daynight.c, mirrors test_ore.c)
Pure functions: `daynight_phase01`, `daynight_brightness`, `daynight_sky_color`,
`daynight_is_dark`, (opt) `daynight_sun_dir`. Cases:
1. phase wrap-around (0, DAY_LENGTH, 3.5*DAY_LENGTH, near UINT32_MAX); always [0,1).
2. brightness in [0,1] over 1000 samples.
3. extremes: noon ≈ max (>0.95), midnight < 0.20; continuity at wrap seam.
4. monotonic increase across dawn, decrease across dusk.
5. sky color channels in [0,1]; day ≈ (0.53,0.81,0.92); midnight dark; dawn warm (R>B).
6. is_dark(midnight)==true, is_dark(noon)==false; determinism.
7. determinism: same args → same output.

CMake: add `src/daynight.c` to lib sources (~line 133); `test_daynight` target
mirroring test_ore (~336); add to warnings foreach (~343).

## 6. Commit sequence (land AFTER mine-vibe-9u4)
1. Pure daynight module + tests (no integration; ctest -R daynight green).
2. Server world clock + wire field (update test_net/test_client lengths).
3. Client smoothing + renderer drive (verify via agent-mode frame dumps noon vs midnight).
4. Mob spawn gating (verify zombies stop by day, resume at night).
5. (optional) polish: moving sun, daybreak despawn, horizon fog.

## 7. Risks / edge cases
- Time sync on join: init `Client.world_ticks = DAY_LENGTH_TICKS/4` to avoid a
  one-frame dawn flash before the first packet.
- Wrap-around: `phase01` modulo makes u32 wrap harmless; client re-anchors each
  packet so a backward jump is fine. Test near UINT32_MAX.
- Underwater composition: lerp day-night sky → deep-water in that order so both
  effects stack (underwater-at-night reads dark).
- Don't double-darken caves to black: keep `MIN_BRIGHT` floor in block.frag.
- Singleplayer must advance a local clock or the sky freezes — document in main.c.
- Unreliable time packet loss is acceptable (idempotent, re-anchored).
- Keep dawn/dusk window edges as named constants so look-tuning doesn't break tests.

## Critical files
- src/server.c (world_ticks ~513, spawn gate 358-372/527-531, send 566)
- src/net.h (extend net_write_world_state, 183-200)
- src/client.c (PKT_WORLD_STATE parse + smoothing, 163-198)
- src/renderer_frame.c (UBO sun_color/ambient 64-72, clear color 106-113)
- src/renderer.h + main.c:320,513 (renderer_draw_frame signature/calls)
- CMakeLists.txt (new src/daynight.c + test_daynight)
- New: src/daynight.{c,h}, tests/test_daynight.c
