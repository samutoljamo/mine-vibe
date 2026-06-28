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

/* Per-SFX synthesis recipe. Each is a tone/noise mix under a decay envelope. */
typedef struct {
    float    dur;       /* seconds */
    float    freq;      /* base frequency (Hz) */
    float    decay;     /* envelope decay factor */
    float    noise_mix; /* 0..1 amount of noise vs tone */
    int      square;    /* 1 = square osc, 0 = sine osc */
    uint32_t seed;      /* noise seed */
} SfxRecipe;

static SfxRecipe sfx_recipe(SoundId id)
{
    /* All SFX lean on sine tones (square reserved for a little grit on impacts)
     * at modest amplitude so they read as soft "sounds" rather than buzzers. */
    switch (id) {
        /* Dull crunchy thud: low sine body + a good helping of noise, fast decay. */
        case SFX_BLOCK_BREAK: return (SfxRecipe){ 0.22f, 120.0f, 6.0f, 0.55f, 0, 0x1001u };
        /* Soft click: short noisy blip, very fast decay. */
        case SFX_BLOCK_PLACE: return (SfxRecipe){ 0.12f, 220.0f, 9.0f, 0.45f, 0, 0x2002u };
        /* Footstep: brief muffled noise. */
        case SFX_STEP:        return (SfxRecipe){ 0.10f,  90.0f, 12.0f, 0.75f, 0, 0x3003u };
        /* Player hurt: descending sine tone, mid decay. */
        case SFX_HURT:        return (SfxRecipe){ 0.30f, 330.0f, 5.0f, 0.12f, 0, 0x4004u };
        /* Mob hurt: lower/grittier than player hurt — a touch of square. */
        case SFX_MOB_HURT:    return (SfxRecipe){ 0.28f, 180.0f, 5.0f, 0.30f, 1, 0x5005u };
        /* Eat: soft sine warble, gentle decay. */
        case SFX_EAT:         return (SfxRecipe){ 0.25f, 260.0f, 4.0f, 0.10f, 0, 0x6006u };
        default:              return (SfxRecipe){ 0.10f, 200.0f, 8.0f, 0.45f, 0, 0x7007u };
    }
}

/* Short linear attack ramp (in [0,1]) so envelopes START at zero rather than
 * snapping to full amplitude — that snap is an audible click/pop. ~3ms. */
static float attack_ramp(size_t i)
{
    enum { ATTACK = AUDIO_SAMPLE_RATE / 300 };   /* ~3.3 ms */
    if (ATTACK <= 0) return 1.0f;
    if (i >= (size_t)ATTACK) return 1.0f;
    return (float)i / (float)ATTACK;
}

size_t audio_gen_sfx(SoundId id, int16_t* out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if ((unsigned)id >= SFX_COUNT) id = SFX_STEP;

    SfxRecipe r = sfx_recipe(id);
    size_t n = (size_t)(r.dur * AUDIO_SAMPLE_RATE);
    if (n > cap) n = cap;

    /* Short release ramp so the tail lands exactly on zero (no end click). */
    enum { RELEASE = AUDIO_SAMPLE_RATE / 200 };   /* ~5 ms */

    for (size_t i = 0; i < n; i++) {
        float t   = (float)i / (float)AUDIO_SAMPLE_RATE;
        /* Pitch glides down over the sound for a more natural impact/hurt. */
        float f   = r.freq * (1.0f - 0.4f * (float)i / (float)n);
        float tone = r.square ? audio_osc_square(t, f) : audio_osc_sine(t, f);
        float ns   = audio_noise((uint32_t)i, r.seed);
        float mix  = tone * (1.0f - r.noise_mix) + ns * r.noise_mix;
        float env  = audio_env_decay(i, n, r.decay) * attack_ramp(i);
        /* Linear fade-out over the final RELEASE samples. */
        if (RELEASE > 0 && n >= (size_t)RELEASE && i >= n - (size_t)RELEASE)
            env *= (float)(n - i) / (float)RELEASE;
        /* ~0.5 amplitude headroom: gentle, never a buzzer. */
        out[i] = to_pcm(mix * env * 0.5f);
    }
    return n;
}

/* ---- Procedural music: a slow looping arpeggio over a soft pad. --------- */

/* A minor pentatonic-ish arpeggio. Frequencies in Hz. */
static const float MUSIC_NOTES[] = {
    220.0f, 261.63f, 329.63f, 392.0f, 329.63f, 261.63f  /* A C E G E C */
};
#define MUSIC_NOTE_COUNT (sizeof(MUSIC_NOTES)/sizeof(MUSIC_NOTES[0]))
/* Seconds per note — slow/ambient. */
#define MUSIC_NOTE_SECONDS 0.55f

size_t audio_music_loop_samples(void)
{
    return (size_t)(MUSIC_NOTE_SECONDS * AUDIO_SAMPLE_RATE) * MUSIC_NOTE_COUNT;
}

size_t audio_gen_music(int16_t* out, size_t cap, uint64_t start)
{
    if (!out || cap == 0) return 0;

    size_t loop      = audio_music_loop_samples();
    size_t note_len  = (size_t)(MUSIC_NOTE_SECONDS * AUDIO_SAMPLE_RATE);

    for (size_t k = 0; k < cap; k++) {
        size_t s   = (size_t)((start + k) % loop);     /* position in the loop */
        size_t ni  = (note_len ? (s / note_len) : 0) % MUSIC_NOTE_COUNT;
        size_t off = note_len ? (s % note_len) : 0;
        float  t   = (float)off / (float)AUDIO_SAMPLE_RATE;

        float note = MUSIC_NOTES[ni];
        /* Per-note attack so each pluck eases in from zero (no click at note
         * boundaries), then a gentle decay across the note. */
        float nenv = audio_env_decay(off, note_len, 2.2f) * attack_ramp(off);
        float arp  = audio_osc_sine(t, note) * nenv;
        /* ... over a quiet drone an octave below for body. */
        float gt   = (float)((start + k) % loop) / (float)AUDIO_SAMPLE_RATE;
        float pad  = audio_osc_sine(gt, MUSIC_NOTES[0] * 0.5f) * 0.22f;

        out[k] = to_pcm((arp * 0.5f + pad) * 0.45f);   /* keep music quiet */
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
static void null_update(void)   {}
static void null_stop_loop(void){}
static void null_shutdown(void) {}

static const AudioBackend NULL_BACKEND = {
    .name       = "null",
    .init       = null_init,
    .submit_pcm = null_submit_pcm,
    .update     = null_update,
    .stop_loop  = null_stop_loop,
    .shutdown   = null_shutdown,
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
    uint64_t             music_pos;       /* absolute sample cursor for music */
} g_audio;

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

void audio_init(void)
{
    if (g_audio.initialized) return;     /* idempotent */
    memset(&g_audio, 0, sizeof(g_audio));

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
    g_audio.initialized = false;
    g_audio.backend = NULL;
}

/* Submit a slot's PCM to the backend at the given gain. */
static void submit_sound(SoundId id, float gain)
{
    if (!g_audio.initialized || (unsigned)id >= SFX_COUNT) return;
    SoundSlot* s = &g_audio.slots[id];
    if (!s->pcm || s->samples == 0) return;
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

void audio_set_music(bool on)
{
    if (g_audio.music_on == on) return;
    g_audio.music_on = on;
    if (!g_audio.initialized) return;
    if (!on && g_audio.backend && g_audio.backend->stop_loop)
        g_audio.backend->stop_loop();
}

void audio_update(const float listener[3])
{
    (void)listener;   /* reserved for future 3D listener orientation */
    if (!g_audio.initialized) return;

    /* Advance the music cursor by roughly one frame's worth so a real backend
     * could stream the next chunk. The NULL backend ignores the data. */
    if (g_audio.music_on) {
        enum { CHUNK = AUDIO_SAMPLE_RATE / 30 };   /* ~33ms */
        static int16_t chunk[CHUNK];
        size_t n = audio_gen_music(chunk, CHUNK, g_audio.music_pos);
        g_audio.music_pos += n;
        if (g_audio.backend && g_audio.backend->submit_pcm)
            g_audio.backend->submit_pcm(chunk, n, 0.5f, false);
    }

    if (g_audio.backend && g_audio.backend->update) g_audio.backend->update();
}
