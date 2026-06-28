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
    /* All SFX lean on sine tones at LOW amplitude with short durations and a
     * little noise, so they read as subtle, soft cues rather than buzzers. The
     * per-sample amplitude is further scaled in audio_gen_sfx (see SFX_PEAK). */
    switch (id) {
        /* Block break: soft low thud, a touch of noise, quick decay. */
        case SFX_BLOCK_BREAK: return (SfxRecipe){ 0.14f, 130.0f, 7.0f, 0.35f, 0, 0x1001u };
        /* Block place: gentle short click, very fast decay, mostly tone. */
        case SFX_BLOCK_PLACE: return (SfxRecipe){ 0.09f, 240.0f, 11.0f, 0.25f, 0, 0x2002u };
        /* Footstep: very brief, muffled, low noise blip. */
        case SFX_STEP:        return (SfxRecipe){ 0.07f, 100.0f, 14.0f, 0.55f, 0, 0x3003u };
        /* Player hurt: short descending sine, mid decay, almost no noise. */
        case SFX_HURT:        return (SfxRecipe){ 0.18f, 320.0f, 6.0f, 0.08f, 0, 0x4004u };
        /* Mob hurt: lower than player hurt, a hint of noise (no harsh square). */
        case SFX_MOB_HURT:    return (SfxRecipe){ 0.17f, 190.0f, 6.0f, 0.20f, 0, 0x5005u };
        /* Eat: soft short sine warble. */
        case SFX_EAT:         return (SfxRecipe){ 0.14f, 270.0f, 5.0f, 0.06f, 0, 0x6006u };
        default:              return (SfxRecipe){ 0.08f, 200.0f, 10.0f, 0.30f, 0, 0x7007u };
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

/* Peak amplitude for SFX (fraction of full scale). Kept low so effects are
 * gentle cues that sit under the music. ~0.3. */
#define SFX_PEAK 0.30f

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
        /* Low peak amplitude (SFX_PEAK): subtle, never a buzzer. */
        out[i] = to_pcm(mix * env * SFX_PEAK);
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
