#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

/*
 * audio_backend.h — the playback backend vtable shared between the engine
 * (audio.c) and concrete backends (e.g. audio_miniaudio.c).
 *
 * A backend owns the OS audio device. The engine talks to it only through
 * this vtable, so a real implementation can be dropped in without touching
 * the engine or the pure generators. All PCM is mono int16 at
 * AUDIO_SAMPLE_RATE (see audio.h).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct AudioBackend {
    const char* name;
    /* Bring up the device. Return 0 on success, non-zero on failure. */
    int  (*init)(void);
    /* Submit a one-shot PCM buffer for immediate playback. `gain` is a linear
     * 0..1 volume (post-attenuation). `loop` requests looping playback. The
     * backend copies whatever it needs; the engine's buffer is not retained. */
    void (*submit_pcm)(const int16_t* pcm, size_t samples, float gain, bool loop);
    /* Per-frame pump (a real ring-buffer backend refills here). */
    void (*update)(void);
    /* Update the gain of the active looping voice IN PLACE (no buffer realloc,
     * no cursor reset -> no artifacts). Used so master-volume/mute changes
     * affect the single render-once music voice live. May be NULL. */
    void (*set_loop_gain)(float gain);
    /* Stop the looping stream started with loop=true (used for music off). */
    void (*stop_loop)(void);
    /* Release the device. */
    void (*shutdown)(void);
} AudioBackend;

/*
 * Concrete-backend hook. A backend TU defines this to return its vtable;
 * the engine calls it from audio_select_backend(). It is declared weak so the
 * engine (audio.c) can be linked WITHOUT any backend TU (e.g. the pure
 * test_audio unit test): when unresolved it is NULL and the engine uses the
 * always-safe NULL backend. The miniaudio backend (audio_miniaudio.c) provides
 * the strong definition in the real game build.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const AudioBackend* audio_miniaudio_backend(void);

#endif /* AUDIO_BACKEND_H */
