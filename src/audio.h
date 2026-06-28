#ifndef AUDIO_H
#define AUDIO_H

/*
 * Audio subsystem — procedural SFX/music with a pluggable playback backend.
 *
 * The engine is split in two layers:
 *
 *   1. Public API (audio_init/shutdown/play/play_at/set_music/update) plus the
 *      pure procedural PCM generators below. These have no OS dependency and are
 *      fully unit-testable.
 *
 *   2. A backend interface (see AudioBackend in audio.c) that owns the actual
 *      OS playback device. Only a NULL backend ships today: it records "would
 *      play X" and no-ops, so the engine compiles and runs with no audio device
 *      and no external library. A real backend (e.g. miniaudio) drops in behind
 *      the same interface — see the BACKEND PLUG-IN POINT comment in audio.c.
 *
 * All PCM is mono int16 at AUDIO_SAMPLE_RATE.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_SAMPLE_RATE 22050

/* Logical sound effects the engine can request. Keep SFX_COUNT last. */
typedef enum {
    SFX_BLOCK_BREAK = 0,
    SFX_BLOCK_PLACE,
    SFX_STEP,
    SFX_HURT,
    SFX_MOB_HURT,
    SFX_EAT,
    SFX_COUNT
} SoundId;

/* ---- Lifecycle ---------------------------------------------------------- */

/* Synthesize all SFX/music PCM and bring up the (NULL) backend. Idempotent and
 * always safe — never fails in a way the caller must handle. */
void audio_init(void);

/* Tear down the backend and free synthesized buffers. Safe to call when not
 * initialized, and safe to call twice. */
void audio_shutdown(void);

/* ---- Playback ----------------------------------------------------------- */

/* Play a non-positional (2D) one-shot. */
void audio_play(SoundId id);

/* Play a positional one-shot. `pos` is the world position of the sound,
 * `listener` the world position of the listener; the engine attenuates by
 * distance. Either pointer may be NULL, in which case it falls back to 2D. */
void audio_play_at(SoundId id, const float pos[3], const float listener[3]);

/* Enable/disable the procedural background music loop. Music defaults to ON
 * (it is real, quiet music now). */
void audio_set_music(bool on);

/* ---- Master volume / mute ------------------------------------------------ */

/* Set the master output volume, a linear scale in [0,1] applied to every
 * sound's gain in the mix path. Values are clamped. Default ~0.6. */
void audio_set_master_volume(float v);

/* Current master volume in [0,1]. */
float audio_get_master_volume(void);

/* Mute / unmute all audio. When muted, every submitted gain is forced to 0
 * (silence) without losing the master-volume setting. */
void audio_set_muted(bool muted);

/* Whether audio is currently muted. */
bool audio_get_muted(void);

/* The effective output gain for a given per-sound base gain, after applying
 * master volume and mute. Pure-ish (reads engine volume state). Exposed for
 * tests: audio_effective_gain(1.0f) == master volume, 0 when muted. */
float audio_effective_gain(float base_gain);

/* Per-frame tick. `listener` is the current listener world position (may be
 * NULL). Advances the music generator and lets the backend refill. */
void audio_update(const float listener[3]);

/* ---- Introspection (for tests / debugging) ------------------------------ */

/* Number of times audio_play/audio_play_at has been invoked since init for the
 * given sound id (counts requests, independent of backend). */
uint64_t audio_play_count(SoundId id);

/* Pointer to the synthesized PCM buffer for `id` and its length in samples.
 * Valid between audio_init() and audio_shutdown(). Returns NULL if not init. */
const int16_t* audio_sound_pcm(SoundId id, size_t* out_samples);

/* ===================================================================== *
 *  Pure procedural DSP generators (no global state, fully testable).
 *  Each writes mono int16 PCM into `out` (capacity `cap` samples) and
 *  returns the number of samples written.
 * ===================================================================== */

/* Exponential decay envelope value in [0,1] at sample `i` of `total`.
 * `decay` is the time-constant factor (larger = faster decay). Pure. */
float audio_env_decay(size_t i, size_t total, float decay);

/* One sample of a square wave at phase `t` (seconds) and frequency `freq`.
 * Returns -1.0 or +1.0. Pure. */
float audio_osc_square(float t, float freq);

/* One sample of a sine wave. Pure. */
float audio_osc_sine(float t, float freq);

/* Deterministic value noise in [-1,1] for sample index `i` and seed `seed`.
 * Pure (no rand()/global state). */
float audio_noise(uint32_t i, uint32_t seed);

/* Synthesize the PCM for a given SoundId. Returns samples written (<= cap).
 * Pure: depends only on `id` and the fixed sample rate. */
size_t audio_gen_sfx(SoundId id, int16_t* out, size_t cap);

/* Generate `cap` samples of the looping procedural music into `out`, starting
 * at absolute sample position `start` (so successive calls tile seamlessly).
 * Returns samples written (== cap unless cap is 0). Pure. */
size_t audio_gen_music(int16_t* out, size_t cap, uint64_t start);

/* Length of the music loop in samples (the generator is periodic over this). */
size_t audio_music_loop_samples(void);

#endif /* AUDIO_H */
