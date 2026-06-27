#ifndef DAYNIGHT_H
#define DAYNIGHT_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Day/night cycle — pure time-of-day math (no Vulkan/net/globals).   */
/*                                                                     */
/*  The server owns a monotonically increasing `world_ticks` counter   */
/*  (advanced once per 20 Hz tick). These functions map that counter   */
/*  onto a normalized time-of-day phase and derive sky/sun/ambient     */
/*  lighting plus a hostile-spawn darkness gate. Everything here is a   */
/*  deterministic pure function of its arguments so it is fully unit-   */
/*  testable; see tests/test_daynight.c.                                */
/* ------------------------------------------------------------------ */

/* One full day = 20 minutes at the 20 Hz server tick rate. */
#define DAY_LENGTH_TICKS (20 * 60 * 20)   /* 24000 */

/* Brightness below this counts as "dark" for hostile mob spawning. */
#define SPAWN_DARK_THRESHOLD 0.25f

/* Dawn/dusk ramp windows, expressed as fractions of a full day. Kept as
 * named constants so look-tuning doesn't silently break the tests. */
#define DAYNIGHT_DAWN_CENTER 0.00f   /* phase 0.0 = dawn  (sunrise)  */
#define DAYNIGHT_DUSK_CENTER 0.50f   /* phase 0.5 = dusk  (sunset)   */
#define DAYNIGHT_RAMP_HALF   0.10f   /* half-width of each ramp window */

/* Normalized time-of-day in [0,1): 0.0 dawn, 0.25 noon, 0.5 dusk,
 * 0.75 midnight. Modulo of the u32 tick counter, so u32 wrap is harmless. */
float daynight_phase01(uint32_t world_ticks);

/* Overall daylight factor in [0,1]: ~1.0 plateau around noon, a low
 * floor (~0.12) at night, with smooth cosine ramps across dawn and dusk.
 * Drives the renderer's sun_color scale and ambient term. */
float daynight_brightness(float t01);

/* Sky/clear color for the given phase, written to out[3], each channel
 * in [0,1]: daytime blue, near-black at midnight, warm tint at dawn/dusk. */
void daynight_sky_color(float t01, float out[3]);

/* True when it is dark enough for hostile mobs to spawn. */
bool daynight_is_dark(uint32_t world_ticks);

#endif /* DAYNIGHT_H */
