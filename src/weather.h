#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>
#include <stdbool.h>

/* Pure, deterministic weather state machine.
 *
 * The world cycles between CLEAR, RAIN and STORM phases. Each phase has a
 * randomized duration drawn from a per-kind [min,max] band; when it expires the
 * model rolls the next kind from a deterministic weighted table (clear is the
 * common case, rain is uncommon, storm is rare and tends to cluster with rain).
 *
 * All randomness is driven by a single seeded splitmix32 state carried in the
 * struct, so a given seed + dt sequence always reproduces the same weather
 * timeline. No rand()/time(). This is a pure module (no Vulkan/GLFW); the
 * server-authoritative ownership and the {kind,time_left} network sync are
 * wired up in a later ticket.
 *
 * The struct is plain POD so the future server can hold one WeatherState and
 * serialize {kind, time_left} over the wire (rng stays server-side).
 */

typedef enum {
    WEATHER_CLEAR = 0,
    WEATHER_RAIN  = 1,
    WEATHER_STORM = 2,
} WeatherKind;

typedef struct {
    WeatherKind kind;       /* current weather phase                       */
    float       time_left;  /* seconds remaining in the current phase      */
    uint32_t    rng;        /* splitmix32 state advanced on each roll      */
} WeatherState;

/* Per-kind phase duration bands, in seconds. Clear stretches are long; rain
 * and storm are shorter and storms shortest of all. Exposed so callers (and
 * tests) can reason about bounds. */
#define WEATHER_CLEAR_MIN_SECONDS  120.0f
#define WEATHER_CLEAR_MAX_SECONDS  600.0f
#define WEATHER_RAIN_MIN_SECONDS    60.0f
#define WEATHER_RAIN_MAX_SECONDS   300.0f
#define WEATHER_STORM_MIN_SECONDS   30.0f
#define WEATHER_STORM_MAX_SECONDS  120.0f

/* Rain intensity per kind, for the future particle/render step. */
#define WEATHER_INTENSITY_CLEAR  0.0f
#define WEATHER_INTENSITY_RAIN   0.5f
#define WEATHER_INTENSITY_STORM  1.0f

/* Initialize to a CLEAR phase with a seeded PRNG and a deterministic first
 * phase duration. Same seed -> same starting state. */
void weather_init(WeatherState *w, uint32_t seed);

/* Advance the simulation by dt seconds. Counts time_left down; on reaching 0 it
 * transitions to the next kind (deterministic weighted roll) and draws a fresh
 * phase duration from that kind's band. Pure in (w, dt). */
void weather_tick(WeatherState *w, float dt);

/* True when precipitation is falling (RAIN or STORM). */
bool weather_is_raining(const WeatherState *w);

/* Rain intensity in [0,1]: 0 clear, ~0.5 rain, 1 storm. */
float weather_rain_intensity(const WeatherState *w);

/* Per-kind duration band accessors (seconds). */
float weather_min_duration(WeatherKind kind);
float weather_max_duration(WeatherKind kind);

#endif /* WEATHER_H */
