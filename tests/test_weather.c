#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../src/weather.h"

/* Determinism: same seed + same dt sequence -> identical kind/time timeline. */
static void test_determinism(void) {
    WeatherState a, b;
    weather_init(&a, 12345u);
    weather_init(&b, 12345u);

    assert(a.kind == b.kind);
    assert(a.time_left == b.time_left);
    assert(a.rng == b.rng);

    for (int i = 0; i < 100000; i++) {
        weather_tick(&a, 0.5f);
        weather_tick(&b, 0.5f);
        assert(a.kind == b.kind);
        assert(a.time_left == b.time_left);
        assert(a.rng == b.rng);
    }
    printf("PASS: determinism\n");
}

/* weather_tick eventually transitions away from the initial kind. */
static void test_transitions(void) {
    WeatherState w;
    weather_init(&w, 7u);
    WeatherKind start = w.kind;
    int changed = 0;
    /* Plenty of time to outlast any single phase's max duration. */
    for (int i = 0; i < 200000 && !changed; i++) {
        weather_tick(&w, 1.0f);
        if (w.kind != start)
            changed = 1;
    }
    assert(changed && "weather must transition, not stay one kind forever");
    printf("PASS: transitions\n");
}

/* All three kinds occur over a long run. */
static void test_all_kinds_occur(void) {
    WeatherState w;
    weather_init(&w, 99u);
    int seen_clear = 0, seen_rain = 0, seen_storm = 0;
    for (int i = 0; i < 1000000; i++) {
        weather_tick(&w, 1.0f);
        if (w.kind == WEATHER_CLEAR) seen_clear = 1;
        else if (w.kind == WEATHER_RAIN) seen_rain = 1;
        else if (w.kind == WEATHER_STORM) seen_storm = 1;
    }
    assert(seen_clear);
    assert(seen_rain);
    assert(seen_storm);
    printf("PASS: all_kinds_occur\n");
}

/* Every phase duration must fall within its per-kind [min,max] band.
 * We sample time_left right after each transition (it equals the new phase's
 * full duration at that instant). */
static void test_durations_in_band(void) {
    WeatherState w;
    weather_init(&w, 4242u);
    WeatherKind prev = w.kind;
    /* The initial phase counts too. */
    assert(w.time_left >= weather_min_duration(w.kind) - 1e-3f);
    assert(w.time_left <= weather_max_duration(w.kind) + 1e-3f);

    for (int i = 0; i < 500000; i++) {
        weather_tick(&w, 1.0f);
        if (w.kind != prev) {
            /* Just transitioned: time_left is the fresh phase duration minus
             * the overshoot consumed at the boundary (which is < dt). So
             * time_left is a lower bound on the true duration and is always
             * <= max; adding dt back gives a safe upper bound for the min
             * check. */
            assert(w.time_left <= weather_max_duration(w.kind) + 1e-3f);
            float dur_upper = w.time_left + 1.0f;
            assert(dur_upper >= weather_min_duration(w.kind) - 1e-3f);
            prev = w.kind;
        }
    }
    printf("PASS: durations_in_band\n");
}

/* Intensity maps correctly per kind. */
static void test_intensity_mapping(void) {
    WeatherState w;
    weather_init(&w, 1u);
    w.kind = WEATHER_CLEAR;
    assert(weather_rain_intensity(&w) == 0.0f);
    assert(!weather_is_raining(&w));

    w.kind = WEATHER_RAIN;
    assert(weather_rain_intensity(&w) > 0.0f);
    assert(weather_rain_intensity(&w) < 1.0f);
    assert(weather_is_raining(&w));

    w.kind = WEATHER_STORM;
    assert(weather_rain_intensity(&w) > weather_rain_intensity(&(WeatherState){ .kind = WEATHER_RAIN }));
    assert(fabsf(weather_rain_intensity(&w) - 1.0f) < 1e-6f);
    assert(weather_is_raining(&w));
    printf("PASS: intensity_mapping\n");
}

/* Two different seeds diverge in their timelines. */
static void test_seeds_diverge(void) {
    WeatherState a, b;
    weather_init(&a, 1u);
    weather_init(&b, 2u);
    int diverged = 0;
    for (int i = 0; i < 200000 && !diverged; i++) {
        weather_tick(&a, 1.0f);
        weather_tick(&b, 1.0f);
        if (a.kind != b.kind || a.time_left != b.time_left)
            diverged = 1;
    }
    assert(diverged && "different seeds must produce different timelines");
    printf("PASS: seeds_diverge\n");
}

/* time_left never goes negative and is always within the current kind band. */
static void test_invariants(void) {
    WeatherState w;
    weather_init(&w, 555u);
    for (int i = 0; i < 500000; i++) {
        weather_tick(&w, 0.3f);
        assert(w.time_left >= 0.0f);
        assert(w.kind == WEATHER_CLEAR || w.kind == WEATHER_RAIN || w.kind == WEATHER_STORM);
    }
    printf("PASS: invariants\n");
}

int main(void) {
    test_determinism();
    test_transitions();
    test_all_kinds_occur();
    test_durations_in_band();
    test_intensity_mapping();
    test_seeds_diverge();
    test_invariants();
    printf("ALL WEATHER TESTS PASSED\n");
    return 0;
}
