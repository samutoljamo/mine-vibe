#include "weather.h"

/* splitmix32: tiny, well-mixed pure PRNG step (same construction as loot.c).
 * Advances *state and returns a uniformly-distributed uint32. */
static uint32_t weather_rng_next(uint32_t *state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

/* Map a uint32 roll to a float in [0,1). */
static float weather_unit(uint32_t r)
{
    return (float)(r >> 8) / (float)(1u << 24);
}

float weather_min_duration(WeatherKind kind)
{
    switch (kind) {
    case WEATHER_RAIN:  return WEATHER_RAIN_MIN_SECONDS;
    case WEATHER_STORM: return WEATHER_STORM_MIN_SECONDS;
    case WEATHER_CLEAR:
    default:            return WEATHER_CLEAR_MIN_SECONDS;
    }
}

float weather_max_duration(WeatherKind kind)
{
    switch (kind) {
    case WEATHER_RAIN:  return WEATHER_RAIN_MAX_SECONDS;
    case WEATHER_STORM: return WEATHER_STORM_MAX_SECONDS;
    case WEATHER_CLEAR:
    default:            return WEATHER_CLEAR_MAX_SECONDS;
    }
}

/* Draw a phase duration within the kind's band from one rng step. */
static float weather_roll_duration(WeatherKind kind, uint32_t *rng)
{
    float lo = weather_min_duration(kind);
    float hi = weather_max_duration(kind);
    float u  = weather_unit(weather_rng_next(rng));
    return lo + u * (hi - lo);
}

/* Choose the next kind with a deterministic weighted roll. The weights depend
 * on the kind we're leaving so that storms cluster with rain:
 *   - leaving CLEAR: mostly stay-ish toward more clear, sometimes rain, rarely
 *     jump straight to storm.
 *   - leaving RAIN: often clears up, but a fair chance to escalate to storm.
 *   - leaving STORM: usually settles down to rain, sometimes straight to clear.
 * Weights are integers; we pick proportionally like loot_select_index. */
static WeatherKind weather_next_kind(WeatherKind from, uint32_t *rng)
{
    /* [CLEAR, RAIN, STORM] weights per originating kind. */
    static const uint32_t WEIGHTS[3][3] = {
        /* from CLEAR */ { 70, 25,  5 },
        /* from RAIN  */ { 55, 15, 30 },
        /* from STORM */ { 35, 55, 10 },
    };
    const uint32_t *w = WEIGHTS[from];
    uint32_t total = w[0] + w[1] + w[2];
    uint32_t r = weather_rng_next(rng) % total;

    uint32_t cumulative = 0;
    for (int i = 0; i < 3; i++) {
        cumulative += w[i];
        if (r < cumulative)
            return (WeatherKind)i;
    }
    return WEATHER_CLEAR; /* unreachable when total > 0 */
}

void weather_init(WeatherState *w, uint32_t seed)
{
    if (!w)
        return;
    /* Avoid a zero state degenerating; splitmix32 handles it fine, but a
     * non-zero seed makes distinct seeds visibly distinct from tick 0. */
    w->rng = seed ? seed : 0x1u;
    w->kind = WEATHER_CLEAR;
    w->time_left = weather_roll_duration(WEATHER_CLEAR, &w->rng);
}

void weather_tick(WeatherState *w, float dt)
{
    if (!w || dt <= 0.0f)
        return;

    w->time_left -= dt;

    /* A single tick may span more than one short phase if dt is large; loop
     * until the budget is exhausted so the timeline stays exact. */
    while (w->time_left <= 0.0f) {
        float overshoot = -w->time_left; /* >= 0 */
        WeatherKind next = weather_next_kind(w->kind, &w->rng);
        float dur = weather_roll_duration(next, &w->rng);
        w->kind = next;
        /* Carry the overshoot into the new phase so dt accounting is exact. */
        w->time_left = dur - overshoot;
    }
}

bool weather_is_raining(const WeatherState *w)
{
    if (!w)
        return false;
    return w->kind == WEATHER_RAIN || w->kind == WEATHER_STORM;
}

float weather_rain_intensity(const WeatherState *w)
{
    if (!w)
        return WEATHER_INTENSITY_CLEAR;
    switch (w->kind) {
    case WEATHER_RAIN:  return WEATHER_INTENSITY_RAIN;
    case WEATHER_STORM: return WEATHER_INTENSITY_STORM;
    case WEATHER_CLEAR:
    default:            return WEATHER_INTENSITY_CLEAR;
    }
}
