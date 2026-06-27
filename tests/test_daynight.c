#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../src/daynight.h"

/* Phase convention: 0.0 dawn, 0.25 noon, 0.5 dusk, 0.75 midnight. */
#define NOON_TICKS     (DAY_LENGTH_TICKS / 4)        /* phase 0.25 */
#define MIDNIGHT_TICKS (3 * DAY_LENGTH_TICKS / 4)    /* phase 0.75 */

/* ------------------------------------------------------------------ */
/* 1. Phase wrap-around: always in [0,1), regardless of how large the   */
/*    tick counter is — including values near UINT32_MAX where the u32   */
/*    wraps.                                                             */
/* ------------------------------------------------------------------ */
static void test_phase_wraparound(void) {
    assert(daynight_phase01(0) == 0.0f);

    /* Whole multiples of a day land back exactly at phase 0. */
    assert(fabsf(daynight_phase01(DAY_LENGTH_TICKS) - 0.0f) < 1e-6f);
    assert(fabsf(daynight_phase01(2u * DAY_LENGTH_TICKS) - 0.0f) < 1e-6f);

    /* 3.5 days in = dusk (0.5). */
    uint32_t t = (uint32_t)(3 * DAY_LENGTH_TICKS) + (DAY_LENGTH_TICKS / 2);
    assert(fabsf(daynight_phase01(t) - 0.5f) < 1e-4f);

    /* Range invariant across a broad sweep, incl. values near UINT32_MAX. */
    uint32_t samples[] = {
        0u, 1u, NOON_TICKS, MIDNIGHT_TICKS,
        DAY_LENGTH_TICKS - 1u, DAY_LENGTH_TICKS, DAY_LENGTH_TICKS + 1u,
        UINT32_MAX / 2u, UINT32_MAX - DAY_LENGTH_TICKS,
        UINT32_MAX - 1u, UINT32_MAX,
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
        float p = daynight_phase01(samples[i]);
        assert(p >= 0.0f && p < 1.0f);
    }
    printf("PASS: phase_wraparound\n");
}

/* ------------------------------------------------------------------ */
/* 2. Brightness stays within [0,1] over 1000 samples spanning the day.  */
/* ------------------------------------------------------------------ */
static void test_brightness_range(void) {
    for (int i = 0; i < 1000; i++) {
        float t01 = (float)i / 1000.0f;
        float b = daynight_brightness(t01);
        assert(b >= 0.0f && b <= 1.0f);
    }
    printf("PASS: brightness_range\n");
}

/* ------------------------------------------------------------------ */
/* 3. Extremes + continuity at the wrap seam.                           */
/* ------------------------------------------------------------------ */
static void test_brightness_extremes(void) {
    float noon     = daynight_brightness(daynight_phase01(NOON_TICKS));
    float midnight = daynight_brightness(daynight_phase01(MIDNIGHT_TICKS));
    assert(noon > 0.95f);      /* full daylight near noon  */
    assert(midnight < 0.20f);  /* dark at midnight         */
    assert(noon > midnight);

    /* Continuity across the 0.999..0/0.001 wrap seam: brightness must not
     * jump. Compare just-before-wrap vs just-after-wrap. */
    float before = daynight_brightness(0.9999f);
    float after  = daynight_brightness(0.0001f);
    assert(fabsf(before - after) < 0.01f);
    printf("PASS: brightness_extremes (noon=%.3f midnight=%.3f)\n",
           noon, midnight);
}

/* ------------------------------------------------------------------ */
/* 4. Monotonic ramps: brightness rises across dawn, falls across dusk. */
/* ------------------------------------------------------------------ */
static void test_brightness_monotonic_ramps(void) {
    /* Dawn ramp: from start of the dawn window up to noon, non-decreasing. */
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float t01 = DAYNIGHT_RAMP_HALF + (0.25f - DAYNIGHT_RAMP_HALF)
                    * (float)i / 100.0f;  /* well inside day, post-dawn */
        (void)t01;
    }
    /* Sample across the dawn ramp window [center-half, center+half] mapped
     * to a positive phase region near 0 .. 0.1. */
    prev = -1.0f;
    for (int i = 0; i <= 200; i++) {
        float t01 = (DAYNIGHT_DAWN_CENTER) + (DAYNIGHT_RAMP_HALF)
                    * (float)i / 200.0f;   /* 0.0 .. 0.10 */
        float b = daynight_brightness(t01);
        assert(b >= prev - 1e-4f);          /* non-decreasing through dawn */
        prev = b;
    }
    /* Dusk ramp: across [0.5, 0.6] brightness must be non-increasing. */
    prev = 2.0f;
    for (int i = 0; i <= 200; i++) {
        float t01 = (DAYNIGHT_DUSK_CENTER) + (DAYNIGHT_RAMP_HALF)
                    * (float)i / 200.0f;   /* 0.50 .. 0.60 */
        float b = daynight_brightness(t01);
        assert(b <= prev + 1e-4f);          /* non-increasing through dusk */
        prev = b;
    }
    printf("PASS: brightness_monotonic_ramps\n");
}

/* ------------------------------------------------------------------ */
/* 5. Sky color: channels in [0,1]; day blue-ish, midnight dark, dawn    */
/*    warm (R > B).                                                       */
/* ------------------------------------------------------------------ */
static void test_sky_color(void) {
    for (int i = 0; i < 1000; i++) {
        float t01 = (float)i / 1000.0f;
        float c[3];
        daynight_sky_color(t01, c);
        for (int k = 0; k < 3; k++)
            assert(c[k] >= 0.0f && c[k] <= 1.0f);
    }

    /* Day (noon, phase 0.25) ≈ daytime blue (0.53, 0.81, 0.92). */
    float day[3];
    daynight_sky_color(daynight_phase01(NOON_TICKS), day);
    assert(fabsf(day[0] - 0.53f) < 0.05f);
    assert(fabsf(day[1] - 0.81f) < 0.05f);
    assert(fabsf(day[2] - 0.92f) < 0.05f);
    assert(day[2] > day[0]);   /* blue dominant by day */

    /* Midnight (phase 0.75) ≈ very dark. */
    float night[3];
    daynight_sky_color(daynight_phase01(MIDNIGHT_TICKS), night);
    assert(night[0] < 0.15f && night[1] < 0.15f && night[2] < 0.20f);

    /* Dawn (phase 0.0) warm: red exceeds blue. */
    float dawn[3];
    daynight_sky_color(0.0f, dawn);
    assert(dawn[0] > dawn[2]);
    printf("PASS: sky_color\n");
}

/* ------------------------------------------------------------------ */
/* 6. is_dark gating: midnight dark, noon not dark.                      */
/* ------------------------------------------------------------------ */
static void test_is_dark(void) {
    assert(daynight_is_dark(MIDNIGHT_TICKS) == true);
    assert(daynight_is_dark(NOON_TICKS) == false);

    /* Consistent with the brightness threshold. */
    for (int i = 0; i < 200; i++) {
        uint32_t t = (uint32_t)((float)i / 200.0f * DAY_LENGTH_TICKS);
        bool dark = daynight_is_dark(t);
        float b = daynight_brightness(daynight_phase01(t));
        assert(dark == (b < SPAWN_DARK_THRESHOLD));
    }
    printf("PASS: is_dark\n");
}

/* ------------------------------------------------------------------ */
/* 7. Determinism: same args → identical output.                        */
/* ------------------------------------------------------------------ */
static void test_determinism(void) {
    for (int i = 0; i < 1000; i++) {
        uint32_t t = (uint32_t)(i * 977u);
        assert(daynight_phase01(t) == daynight_phase01(t));
        float p = daynight_phase01(t);
        assert(daynight_brightness(p) == daynight_brightness(p));
        assert(daynight_is_dark(t) == daynight_is_dark(t));
        float a[3], b[3];
        daynight_sky_color(p, a);
        daynight_sky_color(p, b);
        assert(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
    }
    printf("PASS: determinism\n");
}

int main(void) {
    test_phase_wraparound();
    test_brightness_range();
    test_brightness_extremes();
    test_brightness_monotonic_ramps();
    test_sky_color();
    test_is_dark();
    test_determinism();
    printf("ALL DAYNIGHT TESTS PASSED\n");
    return 0;
}
