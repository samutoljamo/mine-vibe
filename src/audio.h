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

#include "block.h"   /* BlockID, for the pure block -> footstep mapping below */

#define AUDIO_SAMPLE_RATE 22050

/* Logical sound effects the engine can request. Keep SFX_COUNT last. */
typedef enum {
    SFX_BLOCK_BREAK = 0,
    SFX_BLOCK_PLACE,
    SFX_STEP,           /* generic/legacy footstep (kept for back-compat) */
    SFX_HURT,
    SFX_MOB_HURT,
    SFX_EAT,
    SFX_HIT,            /* attacker-side cue: the local player landed a melee blow */
    /* Surface-aware footsteps (dyb.4.2). Short, quiet noise bursts shaped per
     * material so grass/dirt read soft and earthy, stone hard and bright, sand
     * the softest hiss, wood a hollow knock. Picked via audio_footstep_for_block. */
    SFX_STEP_GRASS,
    SFX_STEP_DIRT,
    SFX_STEP_STONE,
    SFX_STEP_SAND,
    SFX_STEP_WOOD,
    /* Ambient atmosphere (dyb.4.2). Low, sparse, quiet beds the future trigger
     * layer can sprinkle on a timer / in caves. */
    SFX_AMBIENT_CAVE,   /* a single soft cave water drip with a short reverb tail */
    SFX_AMBIENT_WIND,   /* a low, breathy filtered-noise wind gust */
    SFX_COUNT
} SoundId;

/* Surface materials a footstep can land on. A small, block.h-independent enum so
 * the per-surface footstep mapping is pure and the audio engine never has to
 * reason about world blocks directly. */
typedef enum {
    STEP_MAT_SOFT = 0,  /* grass / leaves / snow — muffled, earthy */
    STEP_MAT_DIRT,      /* dirt / path / gravel — duller thud */
    STEP_MAT_STONE,     /* stone / cobble / ore — hard, bright tap */
    STEP_MAT_SAND,      /* sand / sandstone — soft high hiss */
    STEP_MAT_WOOD,      /* wood / planks / chest — hollow knock */
    STEP_MAT_COUNT
} FootstepMaterial;

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

/* ---- Footsteps & ambient (dyb.4.2) -------------------------------------- *
 *
 * Convenience play helpers so the future trigger wiring in main.c/player.c is
 * trivial. These are thin wrappers over audio_play/audio_play_at — they map a
 * surface material to the right SFX_STEP_* buffer and add a small, deterministic
 * gain jitter per step so repeated footsteps don't sound mechanically identical.
 *
 * TRIGGERING IS DEFERRED: nothing here decides WHEN to play. The player/main
 * loop owns that (footstep on ground-contact while moving, ambient on a timer /
 * when underground). This module only provides the sounds and the play API. */

/* Play a footstep for the given surface material as a 2D one-shot. `step_seq` is
 * a caller-supplied, monotonically-increasing step counter used only to vary the
 * gain a touch between consecutive steps (pass 0 if you don't care). */
void audio_play_footstep(FootstepMaterial mat, uint64_t step_seq);

/* Positional variant: same material -> SFX mapping, attenuated by distance.
 * Either pointer may be NULL (falls back to 2D). */
void audio_play_footstep_at(FootstepMaterial mat, uint64_t step_seq,
                            const float pos[3], const float listener[3]);

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
 * NULL). Does NOT touch music — music is a single looping voice submitted once
 * (audio_init / audio_set_music) and looped by the backend at the device rate.
 * Reserved for 3D-listener updates and per-frame backend pumping. */
void audio_update(const float listener[3]);

/* ---- Introspection (for tests / debugging) ------------------------------ */

/* Number of times audio_play/audio_play_at has been invoked since init for the
 * given sound id (counts requests, independent of backend). */
uint64_t audio_play_count(SoundId id);

/* Pointer to the synthesized PCM buffer for `id` and its length in samples.
 * Valid between audio_init() and audio_shutdown(). Returns NULL if not init. */
const int16_t* audio_sound_pcm(SoundId id, size_t* out_samples);

/* The single looping music buffer rendered at audio_init()/audio_set_music(true)
 * and its length in samples (== audio_music_loop_samples()). This is the ONE
 * voice the backend loops forever — there is no per-frame music streaming.
 * Returns NULL when not initialized. */
const int16_t* audio_music_loop_pcm(size_t* out_samples);

/* Number of times the looping music buffer has been submitted to the backend
 * since init. The fix guarantees this is incremented ONCE per audio_set_music
 * (true) transition and NEVER by audio_update(). */
uint64_t audio_music_submit_count(void);

/* Whether the looping music voice is currently active (music on + initialized). */
bool audio_music_is_playing(void);

/* The effective output gain currently applied to the looping music voice, after
 * master volume + mute. Tracks volume/mute live (the loop is submitted once but
 * its gain follows the current settings). 0 when muted. Exposed for tests. */
float audio_music_effective_gain(void);

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

/* ---- Voice allocation policy (pure, unit-tested) ------------------------- *
 *
 * Backends keep a fixed pool of one-shot voice slots. When a new SFX is
 * submitted this picks which slot to use:
 *
 *   - Prefer ANY free (inactive) slot — never steal while one is idle.
 *   - When all slots are busy, STEAL the OLDEST voice (smallest `age`) instead
 *     of dropping the new sound, so under load (mining + mobs + steps at once)
 *     the freshest feedback always plays rather than starving.
 *
 * `active[i]` is non-zero if slot i currently plays a voice; `age[i]` is a
 * monotonic submission stamp (larger = more recently started) for active slots
 * (ignored for free slots). Returns the chosen slot index in [0,count), or -1
 * if count <= 0. Pure: depends only on its arguments. */
int audio_pick_voice(const uint8_t* active, const uint64_t* age, int count);

/* ---- Footstep surface mapping (pure, unit-tested) ------------------------ *
 *
 * Map the block a player is standing on to a footstep material, then to the
 * concrete SFX_STEP_* sound. Both are pure (no engine/global state): given the
 * same block they always return the same value. Unknown/edge blocks (air, water)
 * fall back to the soft default rather than going silent. */

/* The footstep material for the surface block under the player. Pure. */
FootstepMaterial audio_footstep_material_for_block(BlockID block);

/* The footstep SFX for a surface material. Pure; total over STEP_MAT_*. */
SoundId audio_footstep_sfx_for_material(FootstepMaterial mat);

/* Convenience: block -> footstep SFX in one call (composition of the two
 * above). The future player/main trigger calls this. Pure. */
SoundId audio_footstep_for_block(BlockID block);

#endif /* AUDIO_H */
