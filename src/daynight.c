#include "daynight.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float daynight_phase01(uint32_t world_ticks) {
    /* Modulo keeps us in [0, DAY_LENGTH_TICKS); u32 wrap is harmless because
     * the result is always taken modulo the day length. */
    uint32_t d = (uint32_t)DAY_LENGTH_TICKS;
    uint32_t r = world_ticks % d;
    return (float)r / (float)d;
}

/* Distance from t01 to a ramp center, measured on the circular [0,1) phase
 * so the dawn window straddling the 0/1 seam works correctly. Result in
 * [0, 0.5]. */
static float circular_dist(float t01, float center) {
    float d = fabsf(t01 - center);
    if (d > 0.5f) d = 1.0f - d;
    return d;
}

/* Smooth 0..1 rise as x goes 0..1 (cosine ease, C1-continuous at endpoints). */
static float smooth01(float x) {
    x = clampf(x, 0.0f, 1.0f);
    return 0.5f - 0.5f * cosf((float)M_PI * x);
}

float daynight_brightness(float t01) {
    /* Night floor and day peak. */
    const float NIGHT = 0.12f;
    const float DAY   = 1.0f;

    /* "Daylightness" = 1 fully lit (noon), 0 fully dark (midnight).
     * Built from the two ramps: brightness rises across the dawn window
     * centered at 0.0 and falls across the dusk window centered at 0.5.
     *
     * Model: full day for phase in (dawn_end, dusk_start); full night
     * outside the ramp windows on the night side; cosine ramps in between.
     * We express this via circular distance to dawn vs dusk centers. */
    float dd_dawn = circular_dist(t01, DAYNIGHT_DAWN_CENTER);
    float dd_dusk = circular_dist(t01, DAYNIGHT_DUSK_CENTER);
    float half    = DAYNIGHT_RAMP_HALF;

    float day_factor;  /* 0..1 */

    /* Are we on the day arc (between dawn and dusk centers, i.e. phase in
     * (0,0.5)) or the night arc (phase in (0.5,1))? Compare which center is
     * nearer in the non-circular sense for the plateau decision. */
    bool day_side = (t01 > DAYNIGHT_DAWN_CENTER && t01 < DAYNIGHT_DUSK_CENTER);

    if (dd_dawn <= half) {
        /* Inside the dawn ramp window: ease from night (before dawn) to day
         * (after dawn). Signed position within the window: -1..+1. */
        float s = (t01 <= 0.5f ? (t01 - DAYNIGHT_DAWN_CENTER)
                               : (t01 - 1.0f - DAYNIGHT_DAWN_CENTER)) / half;
        /* s in [-1,1]; map to [0,1] rise. */
        day_factor = smooth01((s + 1.0f) * 0.5f);
    } else if (dd_dusk <= half) {
        /* Inside the dusk ramp window: ease from day (before dusk) to night. */
        float s = (t01 - DAYNIGHT_DUSK_CENTER) / half;  /* -1..1 */
        day_factor = smooth01(1.0f - (s + 1.0f) * 0.5f);
    } else {
        /* Plateau: full day on the day arc, full night on the night arc. */
        day_factor = day_side ? 1.0f : 0.0f;
    }

    return clampf(NIGHT + (DAY - NIGHT) * day_factor, 0.0f, 1.0f);
}

void daynight_sky_color(float t01, float out[3]) {
    static const float DAY[3]   = { 0.53f, 0.81f, 0.92f };
    static const float NIGHT[3] = { 0.02f, 0.02f, 0.06f };
    static const float WARM[3]  = { 0.95f, 0.55f, 0.30f };

    /* Base day/night blend tracks brightness (reuse the same ramp shape). */
    float b = daynight_brightness(t01);
    /* Remap brightness [NIGHT_FLOOR..1] -> [0..1] for the sky mix so the
     * night sky reaches its dark target rather than stopping at the floor. */
    float mix = clampf((b - 0.12f) / (1.0f - 0.12f), 0.0f, 1.0f);
    for (int k = 0; k < 3; k++)
        out[k] = NIGHT[k] * (1.0f - mix) + DAY[k] * mix;

    /* Warm tint peaks mid-dawn and mid-dusk (triangular weight over each
     * ramp window). Strongest exactly at the ramp center distance ~half/2. */
    float dd_dawn = circular_dist(t01, DAYNIGHT_DAWN_CENTER);
    float dd_dusk = circular_dist(t01, DAYNIGHT_DUSK_CENTER);
    float half    = DAYNIGHT_RAMP_HALF;
    float w = 0.0f;
    if (dd_dawn <= half) w = 1.0f - dd_dawn / half;
    if (dd_dusk <= half) { float wd = 1.0f - dd_dusk / half; if (wd > w) w = wd; }
    /* Triangular: 0 at window edge, 1 at center. Scale down so the tint is a
     * blend, not a full takeover. */
    w *= 0.6f;
    for (int k = 0; k < 3; k++)
        out[k] = clampf(out[k] * (1.0f - w) + WARM[k] * w, 0.0f, 1.0f);
}

bool daynight_is_dark(uint32_t world_ticks) {
    return daynight_brightness(daynight_phase01(world_ticks)) < SPAWN_DARK_THRESHOLD;
}
