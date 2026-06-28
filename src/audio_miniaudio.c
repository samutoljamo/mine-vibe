/*
 * audio_miniaudio.c — real playback backend for the audio engine, built on
 * miniaudio (header-only, https://github.com/mackron/miniaudio).
 *
 * This is the ONE translation unit that defines MA_IMPLEMENTATION, so the
 * miniaudio code is compiled exactly once (no duplicate symbols).
 *
 * Role in the engine (see audio.c / audio_backend.h):
 *   - audio.c owns the pure procedural generators and the public API. It hands
 *     PCM (mono int16 @ AUDIO_SAMPLE_RATE) to a backend through the AudioBackend
 *     vtable. This file implements that vtable against an OS playback device.
 *
 * How it plays:
 *   - init() opens a single shared playback device matching the engine format
 *     (1 channel, s16, AUDIO_SAMPLE_RATE) and starts it.
 *   - submit_pcm(pcm, n, gain, loop=false) copies the PCM into a free one-shot
 *     "voice" slot. The game thread calls this for every SFX and for each
 *     ~33ms streamed music chunk produced by audio_update().
 *   - submit_pcm(..., loop=true) installs the buffer as the single looping
 *     music voice (honored even though the current engine streams music as
 *     non-looping chunks instead).
 *   - The miniaudio data callback (audio thread) mixes every active voice plus
 *     the looping voice additively into the output, clamping to int16. Voices
 *     are freed when their cursor reaches the end.
 *
 * Threading: submit_pcm runs on the game thread; the data callback runs on
 * miniaudio's audio thread. A mutex guards the voice table. Each voice owns a
 * private copy of its PCM, so the engine's buffer lifetime is irrelevant.
 *
 * Fallback: if the device cannot be opened (headless/CI, no audio hardware),
 * init() returns non-zero and audio.c transparently falls back to the NULL
 * backend. Nothing here ever aborts the process.
 */

#define MA_IMPLEMENTATION
/* Trim miniaudio to what we need: a playback device. Disable decoders,
 * encoders, generation and capture to keep the build lean and avoid pulling
 * in extra platform plumbing we never use. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_API static
#include "miniaudio.h"

#include "audio.h"
#include "audio_backend.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Up to this many simultaneous one-shot SFX. Beyond this, the oldest-free
 * policy simply drops the new request (audibly harmless). */
#define MA_BACKEND_MAX_VOICES 32

typedef struct {
    int16_t* pcm;       /* owned copy of the PCM, NULL when slot is free */
    size_t   samples;   /* total samples in pcm */
    size_t   cursor;    /* next sample to play */
    float    gain;      /* linear 0..1 */
    bool     active;
} Voice;

static struct {
    ma_device device;
    ma_mutex  lock;
    bool      device_ok;     /* device opened+started successfully */

    /* The format/channel/rate the data callback must actually emit. We REQUEST
     * s16/mono/AUDIO_SAMPLE_RATE, but a backend is free to hand us a different
     * app-facing format (miniaudio's internal converter handles the hardware
     * side either way). The callback reads these at runtime and writes in
     * whatever miniaudio settled on — writing s16/mono blindly into, say, an
     * f32/stereo buffer is exactly what produces garbled/"weird" output. */
    ma_format dev_format;    /* device.playback.format   (app-facing) */
    ma_uint32 dev_channels;  /* device.playback.channels (app-facing) */

    Voice     voices[MA_BACKEND_MAX_VOICES];

    /* Dedicated looping voice (used only if submit_pcm is called with loop). */
    int16_t*  loop_pcm;
    size_t    loop_samples;
    size_t    loop_cursor;
    float     loop_gain;
    bool      loop_active;
} g_ma;

/* Mix one s16 sample of `gain`-scaled `src` into the f32 accumulator. */
static inline float mix_sample(float acc, int16_t src, float gain)
{
    return acc + ((float)src / 32768.0f) * gain;
}

/* Write one frame's clamped value `s` (already in [-1,1]) into `out` at frame
 * index `f`, in the device's ACTUAL format, duplicated across all channels.
 * Returns nothing; advances nothing — the caller owns the loop. */
static inline void write_frame(void* out, ma_uint32 f, float s,
                               ma_format fmt, ma_uint32 channels)
{
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;

    ma_uint32 base = f * channels;
    switch (fmt) {
        case ma_format_f32: {
            float* o = (float*)out;
            for (ma_uint32 c = 0; c < channels; c++) o[base + c] = s;
            break;
        }
        case ma_format_s32: {
            int32_t* o = (int32_t*)out;
            /* round-to-nearest, full-scale s32 */
            int64_t v = (int64_t)(s * 2147483647.0f);
            if (v >  2147483647LL) v =  2147483647LL;
            if (v < -2147483648LL) v = -2147483648LL;
            for (ma_uint32 c = 0; c < channels; c++) o[base + c] = (int32_t)v;
            break;
        }
        case ma_format_s16:
        default: {
            /* Default also covers u8/s24, which we don't request; clamping to
             * s16 and letting miniaudio refuse such a config keeps us safe. */
            int16_t* o = (int16_t*)out;
            int v = (int)(s * 32767.0f + (s >= 0.0f ? 0.5f : -0.5f));
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            for (ma_uint32 c = 0; c < channels; c++) o[base + c] = (int16_t)v;
            break;
        }
    }
}

/* miniaudio playback callback — runs on the audio thread. Fills `frame_count`
 * frames in `output`, mixing every active voice + the loop voice. Output is
 * written in the device's ACTUAL negotiated format and channel count (see
 * g_ma.dev_format/dev_channels), NOT blindly as s16/mono — that mismatch is
 * the classic source of garbled playback. Voices are mono s16; we accumulate
 * in float, clamp once, then fan out to the device's channels. */
static void data_callback(ma_device* dev, void* output, const void* input,
                          ma_uint32 frame_count)
{
    (void)dev; (void)input;

    const ma_format fmt      = g_ma.dev_format;
    const ma_uint32 channels = g_ma.dev_channels ? g_ma.dev_channels : 1;

    ma_mutex_lock(&g_ma.lock);
    for (ma_uint32 f = 0; f < frame_count; f++) {
        float acc = 0.0f;

        /* One-shot voices. */
        for (int v = 0; v < MA_BACKEND_MAX_VOICES; v++) {
            Voice* vo = &g_ma.voices[v];
            if (!vo->active) continue;
            acc = mix_sample(acc, vo->pcm[vo->cursor], vo->gain);
            if (++vo->cursor >= vo->samples) {
                free(vo->pcm);
                vo->pcm = NULL;
                vo->active = false;
            }
        }

        /* Looping music voice. */
        if (g_ma.loop_active && g_ma.loop_samples > 0) {
            acc = mix_sample(acc, g_ma.loop_pcm[g_ma.loop_cursor], g_ma.loop_gain);
            if (++g_ma.loop_cursor >= g_ma.loop_samples) g_ma.loop_cursor = 0;
        }

        /* Clamp once and emit in the device's real format across all channels. */
        write_frame(output, f, acc, fmt, channels);
    }
    ma_mutex_unlock(&g_ma.lock);
}

/* ---- AudioBackend vtable -------------------------------------------------- */

static int ma_backend_init(void)
{
    memset(&g_ma, 0, sizeof(g_ma));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = 1;
    cfg.sampleRate        = AUDIO_SAMPLE_RATE;
    cfg.dataCallback      = data_callback;

    if (ma_device_init(NULL, &cfg, &g_ma.device) != MA_SUCCESS) {
        /* No device (headless/CI). Caller falls back to the NULL backend. */
        return 1;
    }
    if (ma_mutex_init(&g_ma.lock) != MA_SUCCESS) {
        ma_device_uninit(&g_ma.device);
        return 1;
    }
    /* Record the ACTUAL app-facing format/channels miniaudio gave us. With the
     * config above this is normally s16/1ch, but a backend may hand us a
     * different format (e.g. f32) and/or channel count; miniaudio's internal
     * converter bridges to the hardware. The callback emits in exactly this
     * format/channel count, so it stays correct regardless of negotiation. */
    g_ma.dev_format   = g_ma.device.playback.format;
    g_ma.dev_channels = g_ma.device.playback.channels;
    if (g_ma.dev_channels == 0) g_ma.dev_channels = 1;

    if (ma_device_start(&g_ma.device) != MA_SUCCESS) {
        ma_mutex_uninit(&g_ma.lock);
        ma_device_uninit(&g_ma.device);
        return 1;
    }
    g_ma.device_ok = true;
    return 0;
}

static void ma_backend_submit_pcm(const int16_t* pcm, size_t samples,
                                  float gain, bool loop)
{
    if (!g_ma.device_ok || !pcm || samples == 0) return;
    if (gain <= 0.0f) return;   /* fully attenuated — nothing to play */

    int16_t* copy = (int16_t*)malloc(samples * sizeof(int16_t));
    if (!copy) return;
    memcpy(copy, pcm, samples * sizeof(int16_t));

    ma_mutex_lock(&g_ma.lock);
    if (loop) {
        /* Replace the dedicated looping voice. */
        free(g_ma.loop_pcm);
        g_ma.loop_pcm     = copy;
        g_ma.loop_samples = samples;
        g_ma.loop_cursor  = 0;
        g_ma.loop_gain    = gain;
        g_ma.loop_active  = true;
        ma_mutex_unlock(&g_ma.lock);
        return;
    }

    /* One-shot: find a free voice slot. */
    for (int v = 0; v < MA_BACKEND_MAX_VOICES; v++) {
        if (!g_ma.voices[v].active) {
            g_ma.voices[v].pcm     = copy;
            g_ma.voices[v].samples = samples;
            g_ma.voices[v].cursor  = 0;
            g_ma.voices[v].gain    = gain;
            g_ma.voices[v].active  = true;
            ma_mutex_unlock(&g_ma.lock);
            return;
        }
    }
    /* No free slot — drop this one-shot. */
    ma_mutex_unlock(&g_ma.lock);
    free(copy);
}

static void ma_backend_update(void)
{
    /* The data callback pulls audio on its own thread; nothing to pump here. */
}

static void ma_backend_stop_loop(void)
{
    if (!g_ma.device_ok) return;
    ma_mutex_lock(&g_ma.lock);
    free(g_ma.loop_pcm);
    g_ma.loop_pcm    = NULL;
    g_ma.loop_samples = 0;
    g_ma.loop_cursor = 0;
    g_ma.loop_active = false;
    ma_mutex_unlock(&g_ma.lock);
}

static void ma_backend_shutdown(void)
{
    if (!g_ma.device_ok) return;
    /* Stop the audio thread first so the callback can't touch freed buffers. */
    ma_device_uninit(&g_ma.device);

    ma_mutex_lock(&g_ma.lock);
    for (int v = 0; v < MA_BACKEND_MAX_VOICES; v++) {
        free(g_ma.voices[v].pcm);
        g_ma.voices[v].pcm = NULL;
        g_ma.voices[v].active = false;
    }
    free(g_ma.loop_pcm);
    g_ma.loop_pcm = NULL;
    g_ma.loop_active = false;
    ma_mutex_unlock(&g_ma.lock);

    ma_mutex_uninit(&g_ma.lock);
    g_ma.device_ok = false;
}

static const AudioBackend MINIAUDIO_BACKEND = {
    .name       = "miniaudio",
    .init       = ma_backend_init,
    .submit_pcm = ma_backend_submit_pcm,
    .update     = ma_backend_update,
    .stop_loop  = ma_backend_stop_loop,
    .shutdown   = ma_backend_shutdown,
};

/* Strong definition of the hook declared (weak) in audio_backend.h. The engine
 * (audio.c) calls this from audio_select_backend(). */
const AudioBackend* audio_miniaudio_backend(void)
{
    return &MINIAUDIO_BACKEND;
}
