/*
 * audio.c — procedural SFX/music engine with a pluggable playback backend.
 *
 * Layering (see audio.h header comment for the overview):
 *
 *   - The pure DSP generators (audio_env_decay/osc/noise/gen_sfx/gen_music)
 *     have no global state and are unit-tested directly. They produce mono
 *     int16 PCM at AUDIO_SAMPLE_RATE.
 *
 *   - audio_init() synthesizes every SoundId into a small PCM buffer once and
 *     hands the engine to a backend. Playback requests are routed to the
 *     backend through the AudioBackend vtable.
 *
 *   - Only the NULL backend exists today: it owns no device, allocates nothing,
 *     and simply records "would play X". This keeps the whole engine buildable
 *     and runnable with no audio hardware and, crucially, with NO external
 *     dependency. A real backend (miniaudio, OpenAL, SDL_audio, …) plugs in at
 *     the single marked point below by providing another AudioBackend vtable.
 */

#include "audio.h"
#include "audio_backend.h"
#include "music.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===================================================================== *
 *  Pure DSP — no global state, safe to call any time, unit-tested.
 * ===================================================================== */

float audio_env_decay(size_t i, size_t total, float decay)
{
    if (total == 0) return 0.0f;
    /* Normalized position 0..1, then exp(-decay * pos). At pos==0 -> 1. */
    float pos = (float)i / (float)total;
    return expf(-decay * pos);
}

float audio_osc_square(float t, float freq)
{
    float phase = t * freq;
    float frac  = phase - floorf(phase);
    return frac < 0.5f ? 1.0f : -1.0f;
}

float audio_osc_sine(float t, float freq)
{
    return sinf(2.0f * (float)M_PI * freq * t);
}

float audio_noise(uint32_t i, uint32_t seed)
{
    /* Integer hash (xorshift-ish), mapped to [-1, 1]. Deterministic, no rand(). */
    uint32_t h = i * 747796405u + seed * 2891336453u + 1u;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h / (double)0xFFFFFFFFu) * 2.0f - 1.0f;
}

/* Clamp a float sample in [-1,1] to int16 with a touch of headroom so we never
 * wrap to the extreme rails (keeps the "non-clipping" test honest). */
static int16_t to_pcm(float s)
{
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    int v = (int)lrintf(s * 30000.0f);   /* ~0.916 full scale: headroom */
    if (v >  30000) v =  30000;
    if (v < -30000) v = -30000;
    return (int16_t)v;
}

/* ---- Voice allocation policy -------------------------------------------- *
 *
 * Pure oldest-voice-stealing selection shared by playback backends. Prefer a
 * free slot; when the whole pool is busy, steal the oldest (smallest age) so a
 * new sound replaces the most-stale one rather than being dropped. See audio.h.
 */
int audio_pick_voice(const uint8_t* active, const uint64_t* age, int count)
{
    if (count <= 0 || !active) return -1;

    /* First pass: any free slot wins (no stealing while one is idle). */
    for (int i = 0; i < count; i++)
        if (!active[i]) return i;

    /* All busy: steal the oldest active voice (smallest age stamp). With no
     * age array fall back to slot 0 (still graceful — never drops the new one). */
    if (!age) return 0;
    int oldest = 0;
    for (int i = 1; i < count; i++)
        if (age[i] < age[oldest]) oldest = i;
    return oldest;
}

/* ---- Footstep surface mapping (pure) ------------------------------------ *
 *
 * Block -> material -> SFX. All compile-time/constant: only the BLOCK_* enum
 * values are read, so this needs no link to block.c. Anything unmapped (air,
 * water, glass, …) falls back to the soft default so a footstep is never silent.
 */
FootstepMaterial audio_footstep_material_for_block(BlockID block)
{
    switch (block) {
        case BLOCK_GRASS:
        case BLOCK_LEAVES:
        case BLOCK_SNOW:
            return STEP_MAT_SOFT;
        case BLOCK_DIRT:
        case BLOCK_PATH:
            return STEP_MAT_DIRT;
        case BLOCK_STONE:
        case BLOCK_COBBLE:
        case BLOCK_MOSSY_COBBLESTONE:
        case BLOCK_BEDROCK:
        case BLOCK_COAL_ORE:
        case BLOCK_IRON_ORE:
        case BLOCK_GOLD_ORE:
        case BLOCK_DIAMOND_ORE:
        case BLOCK_FURNACE:
        case BLOCK_ICE:
            return STEP_MAT_STONE;
        case BLOCK_SAND:
        case BLOCK_SANDSTONE:
            return STEP_MAT_SAND;
        case BLOCK_WOOD:
        case BLOCK_PLANKS:
        case BLOCK_CHEST:
            return STEP_MAT_WOOD;
        default:
            return STEP_MAT_SOFT;   /* air/water/glass/torch/unknown -> soft */
    }
}

SoundId audio_footstep_sfx_for_material(FootstepMaterial mat)
{
    switch (mat) {
        case STEP_MAT_SOFT:  return SFX_STEP_GRASS;
        case STEP_MAT_DIRT:  return SFX_STEP_DIRT;
        case STEP_MAT_STONE: return SFX_STEP_STONE;
        case STEP_MAT_SAND:  return SFX_STEP_SAND;
        case STEP_MAT_WOOD:  return SFX_STEP_WOOD;
        default:             return SFX_STEP_GRASS;
    }
}

SoundId audio_footstep_for_block(BlockID block)
{
    return audio_footstep_sfx_for_material(
        audio_footstep_material_for_block(block));
}

/* ---- SFX synthesis ------------------------------------------------------ *
 *
 * The redesign drops the "single osc + exp decay" buzzer in favour of small
 * additive-synth voices shaped by a real ADSR-style envelope:
 *
 *   - Smooth (raised-cosine) attack AND release so the buffer ramps in from 0
 *     and out to 0 with no boundary discontinuity (clicks/pops gone), plus a
 *     gentle exponential body decay for a natural percussive tail.
 *   - 1-3 harmonic partials at independent amplitudes, optionally detuned, for
 *     body and warmth instead of a thin sine or a harsh square.
 *   - A per-sound noise bed (filtered = averaged with its neighbour to tame the
 *     fizz) for impacts/footsteps, blended under the tonal partials.
 *   - An exponential pitch envelope (downward "chirp") so impacts feel physical.
 *
 * Everything is summed in float, scaled by the per-sound .peak (well under
 * full scale for mix headroom), then clamped once to int16. All pure: output
 * depends only on `id`.
 */

/* A single tonal partial: frequency multiple of the base, its own level, and a
 * small detune in Hz for chorus-like thickness (0 = none). */
typedef struct { float mult; float level; float detune; } Partial;

typedef struct {
    float    dur;        /* seconds (kept short — gameplay feedback) */
    float    freq;       /* base frequency (Hz) */
    float    pitch_drop; /* fraction the pitch falls over the sound (0..~0.8) */
    float    pitch_tau;  /* how fast the pitch chirp settles (samples-ish, larger=slower) */
    float    decay;      /* exponential body-decay factor (over the sustain span) */
    float    attack;     /* attack time in seconds (raised-cosine ease-in) */
    float    release;    /* release time in seconds (raised-cosine ease-out) */
    float    noise_mix;  /* 0..1 amount of (filtered) noise blended under the tone */
    float    peak;       /* per-sound output peak (fraction of full scale) */
    uint32_t seed;       /* deterministic noise seed */
    Partial  partials[3];
} SfxRecipe;

static SfxRecipe sfx_recipe(SoundId id)
{
    switch (id) {
        /* Block break: woody downward thunk — fundamental + 2nd/3rd partials,
         * a strong pitch drop and a noise transient for the "crack". */
        case SFX_BLOCK_BREAK: return (SfxRecipe){
            .dur = 0.16f, .freq = 220.0f, .pitch_drop = 0.55f, .pitch_tau = 0.030f,
            .decay = 5.5f, .attack = 0.004f, .release = 0.045f,
            .noise_mix = 0.30f, .peak = 0.34f, .seed = 0x1001u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.45f, 1.5f}, {3.01f, 0.20f, 0.0f} } };
        /* Block place: short bright tap — mostly fundamental + octave, quick
         * release, slight chirp, a hint of noise for the "tock". */
        case SFX_BLOCK_PLACE: return (SfxRecipe){
            .dur = 0.11f, .freq = 300.0f, .pitch_drop = 0.30f, .pitch_tau = 0.020f,
            .decay = 6.5f, .attack = 0.003f, .release = 0.035f,
            .noise_mix = 0.18f, .peak = 0.30f, .seed = 0x2002u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.35f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Footstep: soft muffled low thud — mostly filtered noise over a low
         * sine body, fast settle, no bright partials (dull, earthy). */
        case SFX_STEP:        return (SfxRecipe){
            .dur = 0.09f, .freq = 120.0f, .pitch_drop = 0.45f, .pitch_tau = 0.018f,
            .decay = 7.0f, .attack = 0.004f, .release = 0.030f,
            .noise_mix = 0.62f, .peak = 0.24f, .seed = 0x3003u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.20f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Player hurt: a grunt — two close detuned partials with a downward
         * bend and almost no noise, mid-length so it reads as a vocalisation. */
        case SFX_HURT:        return (SfxRecipe){
            .dur = 0.20f, .freq = 300.0f, .pitch_drop = 0.35f, .pitch_tau = 0.070f,
            .decay = 3.8f, .attack = 0.008f, .release = 0.060f,
            .noise_mix = 0.10f, .peak = 0.32f, .seed = 0x4004u,
            .partials = { {1.0f, 1.0f, 4.0f}, {1.5f, 0.30f, 0.0f}, {2.0f, 0.18f, 6.0f} } };
        /* Mob hurt: lower, growlier grunt — fundamental + a fifth, a touch of
         * noise for grit, slower so it sits apart from the player hurt. */
        case SFX_MOB_HURT:    return (SfxRecipe){
            .dur = 0.19f, .freq = 175.0f, .pitch_drop = 0.30f, .pitch_tau = 0.060f,
            .decay = 3.6f, .attack = 0.008f, .release = 0.055f,
            .noise_mix = 0.22f, .peak = 0.32f, .seed = 0x5005u,
            .partials = { {1.0f, 1.0f, 3.0f}, {1.5f, 0.35f, 4.0f}, {2.0f, 0.15f, 0.0f} } };
        /* Eat: soft warm "nom" — low fundamental + octave, slight upward-then-
         * settling body via a mild chirp, no noise, mellow release. */
        case SFX_EAT:         return (SfxRecipe){
            .dur = 0.15f, .freq = 230.0f, .pitch_drop = 0.18f, .pitch_tau = 0.050f,
            .decay = 3.0f, .attack = 0.010f, .release = 0.050f,
            .noise_mix = 0.05f, .peak = 0.28f, .seed = 0x6006u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.30f, 2.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Melee connect: a short, bright "thwack" the attacker hears on a landed
         * hit — punchy attack, fast decay, a noise transient for the slap, sits
         * above the (lower, growlier) mob-hurt grunt so the two read distinctly. */
        case SFX_HIT:         return (SfxRecipe){
            .dur = 0.10f, .freq = 360.0f, .pitch_drop = 0.50f, .pitch_tau = 0.018f,
            .decay = 7.5f, .attack = 0.002f, .release = 0.030f,
            .noise_mix = 0.40f, .peak = 0.32f, .seed = 0x8008u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.40f, 0.0f}, {3.0f, 0.18f, 0.0f} } };
        /* Melee swing: a quick airy "whoosh" played on every swing (whether or
         * not it connects). Almost all (filtered) noise over a faint low body,
         * a downward pitch sweep for the through-the-air feel, soft and short so
         * it sits under the sharper SFX_HIT thwack when a blow lands. */
        case SFX_SWING:       return (SfxRecipe){
            .dur = 0.12f, .freq = 240.0f, .pitch_drop = 0.60f, .pitch_tau = 0.040f,
            .decay = 4.5f, .attack = 0.010f, .release = 0.045f,
            .noise_mix = 0.92f, .peak = 0.22f, .seed = 0x9009u,
            .partials = { {1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f} } };

        /* ---- Surface footsteps (dyb.4.2) -------------------------------- *
         * All quiet (peak ~0.18-0.24) and short (~70-100ms). Each is a quick
         * filtered-noise burst over a low body; the per-sound seed gives each
         * material a distinct grain so they don't sound like one another. */

        /* Grass/leaves/snow: very soft, mostly noise over a dull low body. */
        case SFX_STEP_GRASS:  return (SfxRecipe){
            .dur = 0.085f, .freq = 110.0f, .pitch_drop = 0.40f, .pitch_tau = 0.016f,
            .decay = 8.0f, .attack = 0.004f, .release = 0.030f,
            .noise_mix = 0.78f, .peak = 0.20f, .seed = 0xA101u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.15f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Dirt/path/gravel: a touch lower and tonal — a duller, heavier thud. */
        case SFX_STEP_DIRT:   return (SfxRecipe){
            .dur = 0.090f, .freq = 95.0f, .pitch_drop = 0.48f, .pitch_tau = 0.018f,
            .decay = 7.5f, .attack = 0.004f, .release = 0.030f,
            .noise_mix = 0.62f, .peak = 0.22f, .seed = 0xA202u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.22f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Stone/cobble/ore: harder and brighter — higher body, snappy tap, more
         * tonal content so it "clicks" against rock rather than thudding. */
        case SFX_STEP_STONE:  return (SfxRecipe){
            .dur = 0.075f, .freq = 200.0f, .pitch_drop = 0.55f, .pitch_tau = 0.012f,
            .decay = 7.5f, .attack = 0.003f, .release = 0.026f,
            .noise_mix = 0.45f, .peak = 0.23f, .seed = 0xA303u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.35f, 0.0f}, {3.0f, 0.15f, 0.0f} } };
        /* Sand/sandstone: the softest — a brief high, airy hiss, almost no body.
         * A gentler body decay keeps the hiss audible through the middle (so it
         * reads as a sustained "shhf" rather than a single click) while staying
         * the quietest of the footsteps. */
        case SFX_STEP_SAND:   return (SfxRecipe){
            .dur = 0.080f, .freq = 140.0f, .pitch_drop = 0.30f, .pitch_tau = 0.014f,
            .decay = 5.5f, .attack = 0.005f, .release = 0.032f,
            .noise_mix = 0.88f, .peak = 0.20f, .seed = 0xA404u,
            .partials = { {1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Wood/planks/chest: a hollow knock — clear low partial + octave, modest
         * noise, slightly longer ring than the others. */
        case SFX_STEP_WOOD:   return (SfxRecipe){
            .dur = 0.095f, .freq = 165.0f, .pitch_drop = 0.42f, .pitch_tau = 0.016f,
            .decay = 6.5f, .attack = 0.003f, .release = 0.034f,
            .noise_mix = 0.40f, .peak = 0.22f, .seed = 0xA505u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.40f, 0.0f}, {3.01f, 0.12f, 0.0f} } };

        /* ---- Ambient atmosphere (dyb.4.2) ------------------------------- *
         * Low, sparse and quiet — meant to sit far under everything else. */

        /* Cave water drip: a clean pitched "plip" — a sine that chirps DOWN fast
         * with a longer exponential tail so it reads as a drop into still water,
         * almost no noise. Quiet. */
        case SFX_AMBIENT_CAVE: return (SfxRecipe){
            .dur = 0.34f, .freq = 720.0f, .pitch_drop = 0.62f, .pitch_tau = 0.030f,
            .decay = 4.5f, .attack = 0.003f, .release = 0.120f,
            .noise_mix = 0.04f, .peak = 0.16f, .seed = 0xB606u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.18f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
        /* Wind gust: a long, breathy bed of (filtered) noise over a very low hum,
         * slow attack/release so it swells in and fades out — no transient. */
        case SFX_AMBIENT_WIND: return (SfxRecipe){
            .dur = 0.90f, .freq = 70.0f, .pitch_drop = 0.10f, .pitch_tau = 0.300f,
            .decay = 1.4f, .attack = 0.180f, .release = 0.260f,
            .noise_mix = 0.82f, .peak = 0.14f, .seed = 0xB707u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.20f, 1.0f}, {0.0f, 0.0f, 0.0f} } };

        default:              return (SfxRecipe){
            .dur = 0.10f, .freq = 220.0f, .pitch_drop = 0.30f, .pitch_tau = 0.030f,
            .decay = 6.0f, .attack = 0.004f, .release = 0.030f,
            .noise_mix = 0.25f, .peak = 0.28f, .seed = 0x7007u,
            .partials = { {1.0f, 1.0f, 0.0f}, {2.0f, 0.30f, 0.0f}, {0.0f, 0.0f, 0.0f} } };
    }
}

/* Raised-cosine ease in [0,1] over a window of `len` samples at position `i`.
 * A smooth S-curve (0 at i=0, 1 at i=len) — its derivative is 0 at both ends,
 * so unlike a linear ramp it introduces no slope discontinuity (= no click). */
static float cosine_ease(size_t i, size_t len)
{
    if (len == 0) return 1.0f;
    if (i >= len) return 1.0f;
    float x = (float)i / (float)len;          /* 0..1 */
    return 0.5f - 0.5f * cosf((float)M_PI * x);
}

/* The full per-sample amplitude envelope: raised-cosine attack * exponential
 * body decay * raised-cosine release. Guarantees env(0)==0 and env(n-1)~0. */
static float sfx_envelope(size_t i, size_t n, const SfxRecipe* r)
{
    size_t atk = (size_t)(r->attack  * AUDIO_SAMPLE_RATE);
    size_t rel = (size_t)(r->release * AUDIO_SAMPLE_RATE);
    if (atk < 1) atk = 1;
    if (rel < 1) rel = 1;
    /* Don't let attack+release overrun a very short buffer. */
    if (atk + rel >= n) { atk = n / 4; rel = n / 4; if (atk < 1) atk = 1; if (rel < 1) rel = 1; }

    float env = audio_env_decay(i, n, r->decay);   /* gentle body decay */
    env *= cosine_ease(i, atk);                     /* smooth fade-in */
    if (i >= n - rel)                               /* smooth fade-out */
        env *= cosine_ease(n - 1 - i, rel);
    return env;
}

size_t audio_gen_sfx(SoundId id, int16_t* out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if ((unsigned)id >= SFX_COUNT) id = SFX_STEP;

    SfxRecipe r = sfx_recipe(id);
    size_t n = (size_t)(r.dur * AUDIO_SAMPLE_RATE);
    if (n > cap) n = cap;
    if (n == 0) return 0;

    /* Normalise the tonal partials so a 3-partial voice isn't louder than a
     * 1-partial one (keeps per-sound peaks honest before the .peak scale). */
    float psum = 0.0f;
    for (int p = 0; p < 3; p++) psum += r.partials[p].level;
    if (psum <= 0.0f) psum = 1.0f;

    for (size_t i = 0; i < n; i++) {
        float t = (float)i / (float)AUDIO_SAMPLE_RATE;

        /* Exponential pitch envelope: f starts high and settles toward the base
         * times (1 - pitch_drop). A short tau gives a snappy "chirp/thunk". */
        float prog = expf(-t / (r.pitch_tau > 1e-5f ? r.pitch_tau : 1e-5f));
        float fscale = (1.0f - r.pitch_drop) + r.pitch_drop * prog;
        float f0 = r.freq * fscale;

        /* Sum the tonal partials (each a sine; detune adds a slow beating). */
        float tone = 0.0f;
        for (int p = 0; p < 3; p++) {
            if (r.partials[p].level <= 0.0f) continue;
            float pf = f0 * r.partials[p].mult + r.partials[p].detune;
            tone += r.partials[p].level * audio_osc_sine(t, pf);
        }
        tone /= psum;

        /* Filtered noise bed: average two neighbouring noise samples to soften
         * the high fizz into a duller, more impact-like rustle. */
        float ns = 0.5f * (audio_noise((uint32_t)i, r.seed) +
                           audio_noise((uint32_t)i + 1u, r.seed));

        float mix = tone * (1.0f - r.noise_mix) + ns * r.noise_mix;
        float env = sfx_envelope(i, n, &r);
        out[i] = to_pcm(mix * env * r.peak);
    }
    return n;
}

/* ---- Procedural music: render the declarative song (see music.c). -------
 *
 * The actual music is described as data (tempo + tracks of notes) in music.c
 * and rendered by the pure sequencer there. audio_gen_music() is a thin int16
 * adapter: it renders the float song into a small scratch buffer and converts.
 * The engine renders the whole loop once at init and loops it, so the per-call
 * cost only matters for the one-time fill. */

/* Overall music level — real music now, so keep it quiet under the SFX. */
#define MUSIC_LEVEL 0.55f

size_t audio_music_loop_samples(void)
{
    return music_song_samples(music_default_song(), AUDIO_SAMPLE_RATE);
}

size_t audio_gen_music(int16_t* out, size_t cap, uint64_t start)
{
    if (!out || cap == 0) return 0;

    const Song* song = music_default_song();

    /* Render in modest chunks of float into the stack, then convert to int16. */
    enum { CHUNK = 1024 };
    float fbuf[CHUNK];
    size_t done = 0;
    while (done < cap) {
        size_t n = cap - done;
        if (n > CHUNK) n = CHUNK;
        music_render(song, fbuf, n, start + done, AUDIO_SAMPLE_RATE);
        for (size_t i = 0; i < n; i++)
            out[done + i] = to_pcm(fbuf[i] * MUSIC_LEVEL);
        done += n;
    }
    return cap;
}

/* ===================================================================== *
 *  Backend interface.
 *
 *  The AudioBackend vtable lives in audio_backend.h so concrete backends
 *  (audio_miniaudio.c) can implement it without depending on this TU. A
 *  backend owns the OS audio device; the engine talks to it only through
 *  the vtable, so a real implementation drops in without touching the
 *  engine or the pure generators.
 * ===================================================================== */

/* --------------------------------------------------------------------- *
 *  NULL backend — no device, no allocation, no external dependency.
 *  It records what *would* play. This is the only backend shipped today.
 * --------------------------------------------------------------------- */

static uint64_t g_null_submits;     /* diagnostics counter */

static int  null_init(void)        { g_null_submits = 0; return 0; }
static void null_submit_pcm(const int16_t* pcm, size_t samples,
                            float gain, bool loop)
{
    (void)pcm; (void)samples; (void)gain; (void)loop;
    g_null_submits++;
    /* A real backend would copy/mix this into its ring buffer here.
     * The NULL backend just notes the request. (Uncomment for tracing.)
     * fprintf(stderr, "[audio:null] would play %zu samples gain=%.2f%s\n",
     *         samples, gain, loop ? " (loop)" : ""); */
}
static void null_update(void)            {}
static void null_set_loop_gain(float g)  { (void)g; }
static void null_stop_loop(void)         {}
static void null_shutdown(void)          {}

static const AudioBackend NULL_BACKEND = {
    .name          = "null",
    .init          = null_init,
    .submit_pcm    = null_submit_pcm,
    .update        = null_update,
    .set_loop_gain = null_set_loop_gain,
    .stop_loop     = null_stop_loop,
    .shutdown      = null_shutdown,
};

/* ============================ BACKEND PLUG-IN POINT ===================== *
 *  audio_select_backend() picks the playback backend. The real game build
 *  links src/audio_miniaudio.c, which provides audio_miniaudio_backend()
 *  (a miniaudio-driven device). If that TU is NOT linked (e.g. the pure
 *  test_audio unit test, which links only audio.c), audio_miniaudio_backend
 *  resolves to a weak NULL and we transparently use the NULL backend.
 *
 *  Note: this only SELECTS the backend. If the chosen backend's init() fails
 *  at runtime (no audio device — headless/CI), audio_init() below falls back
 *  to the always-safe NULL backend. So selection never crashes a no-device
 *  machine; the device handshake is what falls back.
 * ======================================================================= */
static const AudioBackend* audio_select_backend(void)
{
    if (audio_miniaudio_backend) {
        const AudioBackend* be = audio_miniaudio_backend();
        if (be) return be;
    }
    /* No real backend linked in (or it declined): use the NULL backend. */
    return &NULL_BACKEND;
}

/* ===================================================================== *
 *  Engine state + public API.
 * ===================================================================== */

#define AUDIO_MAX_SFX_SAMPLES (AUDIO_SAMPLE_RATE)   /* SFX <= 1s, comfortably */

typedef struct {
    int16_t* pcm;       /* synthesized buffer, NULL until init */
    size_t   samples;
} SoundSlot;

static struct {
    bool                 initialized;
    const AudioBackend*  backend;
    SoundSlot            slots[SFX_COUNT];
    uint64_t             play_counts[SFX_COUNT];
    bool                 music_on;
    /* The music is rendered ONCE into this buffer at init / music-on and handed
     * to the backend as a single looping voice. audio_update() never touches it
     * — that per-frame streaming was the P0 "music too fast + clipping" bug. */
    int16_t*             music_pcm;       /* owned full-loop buffer, NULL if none */
    size_t               music_samples;   /* length of music_pcm */
    uint64_t             music_submits;   /* times the loop voice was submitted */
    float                master_volume;   /* linear 0..1, applied to all gains */
    bool                 muted;           /* forces effective gain to 0 */
    bool                 vol_inited;      /* master_volume/muted have defaults */
} g_audio;

/* Base mix level for the looping music voice (master volume + mute apply on top
 * via audio_music_effective_gain). Matches the level the old per-frame stream
 * used, so the music sits at the same volume under the SFX. */
#define MUSIC_BASE_GAIN 0.5f

/* Default master volume — comfortable but not blaring. */
#define AUDIO_DEFAULT_VOLUME 0.6f

/* Volume/mute live independently of audio_init() (the UI may set them before or
 * after device bring-up, and audio_init() memsets the engine struct). Apply the
 * defaults exactly once, the first time any volume function touches them. */
static void ensure_volume_defaults(void)
{
    if (!g_audio.vol_inited) {
        g_audio.master_volume = AUDIO_DEFAULT_VOLUME;
        g_audio.muted         = false;
        g_audio.vol_inited    = true;
    }
}

float audio_effective_gain(float base_gain)
{
    ensure_volume_defaults();
    if (g_audio.muted) return 0.0f;
    float g = base_gain * g_audio.master_volume;
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    return g;
}

float audio_music_effective_gain(void)
{
    return audio_effective_gain(MUSIC_BASE_GAIN);
}

/* Push the current effective music gain to the backend's live looping voice.
 * The buffer is submitted once; only its gain follows volume/mute (no realloc,
 * no cursor reset -> no artifacts). Safe when music is off / not initialized. */
static void audio_refresh_music_gain(void)
{
    if (!g_audio.initialized || !g_audio.music_on) return;
    if (g_audio.backend && g_audio.backend->set_loop_gain)
        g_audio.backend->set_loop_gain(audio_music_effective_gain());
}

void audio_set_master_volume(float v)
{
    ensure_volume_defaults();
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_audio.master_volume = v;
    audio_refresh_music_gain();   /* music tracks volume live */
}

float audio_get_master_volume(void)
{
    ensure_volume_defaults();
    return g_audio.master_volume;
}

void audio_set_muted(bool muted)
{
    ensure_volume_defaults();
    g_audio.muted = muted;
    audio_refresh_music_gain();   /* mute => music truly silent, live */
}

bool audio_get_muted(void)
{
    ensure_volume_defaults();
    return g_audio.muted;
}

uint64_t audio_play_count(SoundId id)
{
    if ((unsigned)id >= SFX_COUNT) return 0;
    return g_audio.play_counts[id];
}

const int16_t* audio_sound_pcm(SoundId id, size_t* out_samples)
{
    if (!g_audio.initialized || (unsigned)id >= SFX_COUNT) return NULL;
    if (out_samples) *out_samples = g_audio.slots[id].samples;
    return g_audio.slots[id].pcm;
}

const int16_t* audio_music_loop_pcm(size_t* out_samples)
{
    if (!g_audio.initialized) { if (out_samples) *out_samples = 0; return NULL; }
    if (out_samples) *out_samples = g_audio.music_samples;
    return g_audio.music_pcm;
}

uint64_t audio_music_submit_count(void)
{
    return g_audio.music_submits;
}

bool audio_music_is_playing(void)
{
    return g_audio.initialized && g_audio.music_on;
}

/* Render the full music loop ONCE into g_audio.music_pcm (idempotent — reuses
 * the buffer if already rendered). Returns true if a buffer is available. */
static bool audio_render_music_loop(void)
{
    if (g_audio.music_pcm && g_audio.music_samples > 0) return true;
    size_t n = audio_music_loop_samples();
    if (n == 0) return false;
    int16_t* buf = (int16_t*)malloc(sizeof(int16_t) * n);
    if (!buf) return false;
    /* audio_gen_music tiles by absolute position; starting at 0 fills the whole
     * loop. It already bakes MUSIC_LEVEL and clamps with headroom, so this single
     * voice is in-range with no overlap (the bug was overlapping per-frame chunks). */
    audio_gen_music(buf, n, 0);
    g_audio.music_pcm     = buf;
    g_audio.music_samples = n;
    return true;
}

/* Submit the rendered loop to the backend as the single looping voice at the
 * current effective gain. Renders the buffer first if needed. No-op if muted
 * away or no buffer. Increments the submit counter on each (re)submit. */
static void audio_submit_music_loop(void)
{
    if (!g_audio.initialized) return;
    if (!audio_render_music_loop()) return;
    float g = audio_music_effective_gain();
    if (g_audio.backend && g_audio.backend->submit_pcm) {
        /* Submit even at gain 0 (muted) so the voice exists and unmuting is
         * instant via set_loop_gain; the backend mixes a 0-gain voice silently. */
        g_audio.backend->submit_pcm(g_audio.music_pcm, g_audio.music_samples,
                                    g, true);
        g_audio.music_submits++;
    }
}

void audio_init(void)
{
    if (g_audio.initialized) return;     /* idempotent */

    /* Preserve volume/mute across (re)init — they are user/UI settings that
     * live independently of device bring-up, and the memset below would wipe
     * them. */
    ensure_volume_defaults();
    float saved_vol   = g_audio.master_volume;
    bool  saved_muted = g_audio.muted;

    memset(&g_audio, 0, sizeof(g_audio));

    g_audio.master_volume = saved_vol;
    g_audio.muted         = saved_muted;
    g_audio.vol_inited    = true;
    g_audio.music_on      = true;        /* music defaults ON (quiet) */

    /* Synthesize each SFX once into its slot. */
    for (int id = 0; id < SFX_COUNT; id++) {
        int16_t* buf = (int16_t*)malloc(sizeof(int16_t) * AUDIO_MAX_SFX_SAMPLES);
        if (!buf) { g_audio.slots[id].pcm = NULL; g_audio.slots[id].samples = 0; continue; }
        size_t n = audio_gen_sfx((SoundId)id, buf, AUDIO_MAX_SFX_SAMPLES);
        g_audio.slots[id].pcm     = buf;
        g_audio.slots[id].samples = n;
    }

    g_audio.backend = audio_select_backend();
    if (g_audio.backend && g_audio.backend->init) {
        if (g_audio.backend->init() != 0) {
            /* Device came up unhappy (no audio hardware — headless/CI, or a
             * busy device). Fall back to the always-safe NULL backend so the
             * game still runs silently rather than crashing. */
            fprintf(stderr,
                    "[audio] backend '%s' init failed; falling back to null backend\n",
                    g_audio.backend->name ? g_audio.backend->name : "?");
            g_audio.backend = &NULL_BACKEND;
            g_audio.backend->init();
        }
    }
    g_audio.initialized = true;

    /* Music defaults ON: render the loop once and start the single looping voice
     * now. From here audio_update() never touches music. */
    if (g_audio.music_on) audio_submit_music_loop();
}

void audio_shutdown(void)
{
    if (!g_audio.initialized) return;    /* safe when not init / double call */
    if (g_audio.backend && g_audio.backend->shutdown) g_audio.backend->shutdown();
    for (int id = 0; id < SFX_COUNT; id++) {
        free(g_audio.slots[id].pcm);
        g_audio.slots[id].pcm = NULL;
        g_audio.slots[id].samples = 0;
    }
    /* Free the single looping music buffer. */
    free(g_audio.music_pcm);
    g_audio.music_pcm     = NULL;
    g_audio.music_samples = 0;
    g_audio.initialized = false;
    g_audio.backend = NULL;
}

/* Submit a slot's PCM to the backend at the given gain. */
static void submit_sound(SoundId id, float gain)
{
    if (!g_audio.initialized || (unsigned)id >= SFX_COUNT) return;
    SoundSlot* s = &g_audio.slots[id];
    if (!s->pcm || s->samples == 0) return;
    /* Apply master volume + mute on top of the per-sound gain. */
    gain = audio_effective_gain(gain);
    if (gain <= 0.0f) return;   /* muted / fully attenuated — nothing to play */
    if (g_audio.backend && g_audio.backend->submit_pcm)
        g_audio.backend->submit_pcm(s->pcm, s->samples, gain, false);
}

void audio_play(SoundId id)
{
    if ((unsigned)id >= SFX_COUNT) return;
    g_audio.play_counts[id]++;
    submit_sound(id, 1.0f);
}

void audio_play_at(SoundId id, const float pos[3], const float listener[3])
{
    if ((unsigned)id >= SFX_COUNT) return;
    g_audio.play_counts[id]++;

    float gain = 1.0f;
    if (pos && listener) {
        float dx = pos[0] - listener[0];
        float dy = pos[1] - listener[1];
        float dz = pos[2] - listener[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        /* Linear roll-off to silence at AUDIO_MAX_DIST. */
        const float AUDIO_MAX_DIST = 32.0f;
        gain = 1.0f - dist / AUDIO_MAX_DIST;
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 1.0f) gain = 1.0f;
    }
    submit_sound(id, gain);
}

/* A small deterministic per-step gain wobble in [STEP_GAIN_MIN, 1.0] so back-to
 * -back footsteps aren't bit-identical (the buffer is fixed; only the level
 * varies). Derived purely from the material + step counter — no rand(). */
#define STEP_GAIN_MIN 0.80f
static float footstep_gain(FootstepMaterial mat, uint64_t step_seq)
{
    /* audio_noise() is our deterministic hash; fold the step index + material
     * into [0,1] and map onto the small gain band. */
    float n = audio_noise((uint32_t)step_seq, 0xF007u + (uint32_t)mat);
    float u = 0.5f * (n + 1.0f);                 /* [-1,1] -> [0,1] */
    return STEP_GAIN_MIN + (1.0f - STEP_GAIN_MIN) * u;
}

void audio_play_footstep(FootstepMaterial mat, uint64_t step_seq)
{
    SoundId id = audio_footstep_sfx_for_material(mat);
    if ((unsigned)id >= SFX_COUNT) return;
    g_audio.play_counts[id]++;
    submit_sound(id, footstep_gain(mat, step_seq));
}

void audio_play_footstep_at(FootstepMaterial mat, uint64_t step_seq,
                            const float pos[3], const float listener[3])
{
    SoundId id = audio_footstep_sfx_for_material(mat);
    if ((unsigned)id >= SFX_COUNT) return;
    g_audio.play_counts[id]++;

    float gain = footstep_gain(mat, step_seq);
    if (pos && listener) {
        float dx = pos[0] - listener[0];
        float dy = pos[1] - listener[1];
        float dz = pos[2] - listener[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        const float AUDIO_MAX_DIST = 32.0f;
        float roll = 1.0f - dist / AUDIO_MAX_DIST;
        if (roll < 0.0f) roll = 0.0f;
        if (roll > 1.0f) roll = 1.0f;
        gain *= roll;
    }
    submit_sound(id, gain);
}

void audio_set_music(bool on)
{
    if (g_audio.music_on == on) return;
    g_audio.music_on = on;
    if (!g_audio.initialized) return;
    if (on) {
        /* (Re)submit the single looping voice. Rendered once and cached. */
        audio_submit_music_loop();
    } else if (g_audio.backend && g_audio.backend->stop_loop) {
        g_audio.backend->stop_loop();
    }
}

void audio_update(const float listener[3])
{
    (void)listener;   /* reserved for future 3D listener orientation */
    if (!g_audio.initialized) return;

    /* NOTE: audio_update() deliberately does NOT touch music. Music is a single
     * looping voice submitted once (see audio_submit_music_loop) and looped by
     * the backend at the device rate. The old code generated+submitted a ~33ms
     * music chunk here EVERY frame, which above 30fps pushed music faster than
     * real-time and piled overlapping chunks into clipping — the P0 bug. This is
     * now reserved purely for 3D-listener / per-frame backend pumping. */
    if (g_audio.backend && g_audio.backend->update) g_audio.backend->update();
}
