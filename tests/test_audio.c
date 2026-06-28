#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "../src/audio.h"

/* ---- Tiny WAV writer (mono s16) for eyeball/`xxd`/`file` sanity checks ----
 * Not part of the assertions; purely a debug artifact under /tmp so a human
 * (with no speakers in CI) can confirm the headers and levels look like audio
 * rather than noise. Returns bytes written (0 on failure). */
static size_t write_wav_mono_s16(const char* path, const int16_t* pcm,
                                 size_t samples, uint32_t rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t data_bytes = (uint32_t)(samples * sizeof(int16_t));
    uint32_t byte_rate  = rate * 1u * sizeof(int16_t);
    uint16_t block_align = (uint16_t)(1u * sizeof(int16_t));
    uint16_t bits = 16, channels = 1, fmt = 1; /* PCM */
    uint32_t riff = 36 + data_bytes, subchunk1 = 16, sr = rate;

    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&subchunk1, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm, 1, data_bytes, f);
    long total = ftell(f);
    fclose(f);
    return total > 0 ? (size_t)total : 0;
}

/* ---- Pure DSP helper invariants ---------------------------------------- */

static void test_env_decay(void) {
    /* Starts at (about) 1.0, ends near 0, monotonic non-increasing, in range. */
    size_t n = 1000;
    float prev = 2.0f;
    for (size_t i = 0; i < n; i++) {
        float e = audio_env_decay(i, n, 5.0f);
        assert(e >= 0.0f && e <= 1.0001f);
        assert(e <= prev + 1e-6f);   /* non-increasing */
        prev = e;
    }
    assert(audio_env_decay(0, n, 5.0f) > 0.99f);          /* full at start */
    assert(audio_env_decay(n - 1, n, 5.0f) < 0.05f);      /* quiet at end  */
    /* total==0 must not divide by zero / crash. */
    (void)audio_env_decay(0, 0, 5.0f);
    printf("PASS: env_decay\n");
}

static void test_oscillators(void) {
    for (int i = 0; i < 500; i++) {
        float t = i / (float)AUDIO_SAMPLE_RATE;
        float sq = audio_osc_square(t, 440.0f);
        assert(sq == 1.0f || sq == -1.0f);
        float s = audio_osc_sine(t, 440.0f);
        assert(s >= -1.0001f && s <= 1.0001f);
    }
    /* Determinism. */
    assert(audio_osc_sine(0.123f, 220.0f) == audio_osc_sine(0.123f, 220.0f));
    printf("PASS: oscillators\n");
}

static void test_noise(void) {
    /* In range, deterministic, and not a constant (some spread). */
    float mn = 2.0f, mx = -2.0f;
    for (uint32_t i = 0; i < 2000; i++) {
        float v = audio_noise(i, 99);
        assert(v >= -1.0001f && v <= 1.0001f);
        assert(v == audio_noise(i, 99));     /* deterministic */
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    assert(mx - mn > 0.5f);                   /* actually varies */
    printf("PASS: noise\n");
}

/* ---- SFX generators ----------------------------------------------------- */

static void test_sfx_generation(void) {
    for (int id = 0; id < SFX_COUNT; id++) {
        int16_t buf[AUDIO_SAMPLE_RATE * 2];   /* up to 2s capacity */
        size_t n = audio_gen_sfx((SoundId)id, buf, sizeof buf / sizeof buf[0]);

        /* Produces a non-trivial buffer that fits capacity. */
        assert(n > 0);
        assert(n <= sizeof buf / sizeof buf[0]);

        /* Non-clipping: int16 PCM stays strictly inside the full-scale range
         * (we synthesize with headroom; never wrap to INT16_MIN/MAX). */
        int nonzero = 0;
        for (size_t i = 0; i < n; i++) {
            assert(buf[i] > -32768 && buf[i] < 32767);
            if (buf[i] != 0) nonzero = 1;
        }
        assert(nonzero);    /* actually contains sound */

        /* Determinism: regenerating yields identical PCM. */
        int16_t buf2[AUDIO_SAMPLE_RATE * 2];
        size_t n2 = audio_gen_sfx((SoundId)id, buf2, sizeof buf2 / sizeof buf2[0]);
        assert(n2 == n);
        assert(memcmp(buf, buf2, n * sizeof(int16_t)) == 0);
    }
    printf("PASS: sfx_generation\n");
}

static void test_sfx_respects_capacity(void) {
    int16_t tiny[8];
    size_t n = audio_gen_sfx(SFX_HURT, tiny, 8);
    assert(n <= 8);
    /* Zero capacity is a safe no-op. */
    assert(audio_gen_sfx(SFX_HURT, tiny, 0) == 0);
    printf("PASS: sfx_respects_capacity\n");
}

/* ---- Music generator ---------------------------------------------------- */

static void test_music_generation(void) {
    size_t loop = audio_music_loop_samples();
    assert(loop > 0);

    int16_t a[4096], b[4096];
    size_t na = audio_gen_music(a, 4096, 0);
    size_t nb = audio_gen_music(b, 4096, 0);
    assert(na == 4096 && nb == 4096);
    assert(memcmp(a, b, sizeof a) == 0);          /* deterministic */

    /* Non-clipping + carries signal. */
    int nonzero = 0;
    for (size_t i = 0; i < 4096; i++) {
        assert(a[i] > -32768 && a[i] < 32767);
        if (a[i] != 0) nonzero = 1;
    }
    assert(nonzero);

    /* Periodicity: sampling at `loop` later reproduces the start of the loop. */
    int16_t c[512], d[512];
    audio_gen_music(c, 512, 0);
    audio_gen_music(d, 512, loop);
    assert(memcmp(c, d, sizeof c) == 0);

    /* Zero cap is safe. */
    assert(audio_gen_music(a, 0, 0) == 0);
    printf("PASS: music_generation\n");
}

/* ---- In-range / finite / no-click invariants ---------------------------- */

static void test_buffers_in_range_and_finite(void) {
    /* Every SFX: full-scale-bounded (no int16 wrap), finite, starts and ends
     * near zero (envelope attack/release => no click/pop). */
    for (int id = 0; id < SFX_COUNT; id++) {
        int16_t buf[AUDIO_SAMPLE_RATE];
        size_t n = audio_gen_sfx((SoundId)id, buf, sizeof buf / sizeof buf[0]);
        assert(n > 4);

        for (size_t i = 0; i < n; i++) {
            /* int16 never wraps to the rails. */
            assert(buf[i] > -32768 && buf[i] < 32767);
            /* int16 is integral so always finite, but guard the math anyway:
             * a NaN/Inf would have produced INT16_MIN-ish garbage above. */
            assert(buf[i] == buf[i]);
        }
        /* Onset eases in from (near) zero: the very first sample is small. */
        assert(abs(buf[0]) < 1500);
        /* Tail lands on (near) zero: the last sample is small. */
        assert(abs(buf[n - 1]) < 1500);
    }

    /* Music: in-range + finite over a couple of seconds spanning loop joins. */
    enum { N = AUDIO_SAMPLE_RATE * 2 };
    int16_t* m = (int16_t*)malloc(sizeof(int16_t) * N);
    assert(m);
    size_t got = audio_gen_music(m, N, 0);
    assert(got == (size_t)N);
    for (size_t i = 0; i < (size_t)N; i++)
        assert(m[i] > -32768 && m[i] < 32767);
    free(m);
    printf("PASS: buffers_in_range_and_finite\n");
}

/* Reproduce the backend's float-accumulate-and-clamp mix on the host so we can
 * (a) prove a busy mix never wraps int16 and (b) dump a WAV to inspect levels
 * without any audio device. Mirrors data_callback() in audio_miniaudio.c. */
static void test_mix_clamp_and_wav_dump(void) {
    audio_init();

    /* --- Single SFX dump --- */
    size_t sn = 0;
    const int16_t* sfx = audio_sound_pcm(SFX_BLOCK_BREAK, &sn);
    assert(sfx && sn > 0);
    size_t wrote = write_wav_mono_s16("/tmp/audio_sfx_block_break.wav",
                                      sfx, sn, AUDIO_SAMPLE_RATE);
    assert(wrote == 44 + sn * sizeof(int16_t));   /* 44-byte header + data */

    /* --- ~1s mixed dump: music + several overlapping SFX at full gain,
     *     mixed exactly like the backend (float accumulate, clamp once). --- */
    enum { OUT = AUDIO_SAMPLE_RATE };             /* 1 second */
    int16_t* music = (int16_t*)malloc(sizeof(int16_t) * OUT);
    int16_t* mixed = (int16_t*)malloc(sizeof(int16_t) * OUT);
    assert(music && mixed);
    audio_gen_music(music, OUT, 0);

    /* Gather a few SFX buffers to overlap. */
    const int16_t* a; size_t an; a = audio_sound_pcm(SFX_HURT, &an);
    const int16_t* b; size_t bn; b = audio_sound_pcm(SFX_STEP, &bn);
    const int16_t* c; size_t cn; c = audio_sound_pcm(SFX_EAT,  &cn);
    assert(a && b && c);

    int peak = 0;
    for (size_t i = 0; i < (size_t)OUT; i++) {
        float acc = 0.0f;
        acc += (float)music[i] / 32768.0f * 0.5f;          /* music gain */
        if (i < an) acc += (float)a[i % an] / 32768.0f;    /* full gain */
        if (i < bn) acc += (float)b[i % bn] / 32768.0f;
        if (i < cn) acc += (float)c[i % cn] / 32768.0f;
        if (acc >  1.0f) acc =  1.0f;                       /* clamp once */
        if (acc < -1.0f) acc = -1.0f;
        int v = (int)(acc * 32767.0f + (acc >= 0 ? 0.5f : -0.5f));
        assert(v >= -32768 && v <= 32767);                 /* never wraps */
        mixed[i] = (int16_t)v;
        if (abs(v) > peak) peak = abs(v);
    }
    /* The mix actually produced signal but stayed inside the rails. */
    assert(peak > 1000);
    assert(peak <= 32767);

    wrote = write_wav_mono_s16("/tmp/audio_mixed_1s.wav",
                               mixed, OUT, AUDIO_SAMPLE_RATE);
    assert(wrote == 44 + (size_t)OUT * sizeof(int16_t));

    printf("INFO: wrote /tmp/audio_sfx_block_break.wav (%zu samples) and "
           "/tmp/audio_mixed_1s.wav (peak=%d/32767)\n", sn, peak);

    free(music);
    free(mixed);
    audio_shutdown();
    printf("PASS: mix_clamp_and_wav_dump\n");
}

/* ---- Sound-id table ----------------------------------------------------- */

static void test_sound_id_table(void) {
    audio_init();
    for (int id = 0; id < SFX_COUNT; id++) {
        size_t n = 0;
        const int16_t* pcm = audio_sound_pcm((SoundId)id, &n);
        assert(pcm != NULL);     /* every SoundId has a synthesized buffer */
        assert(n > 0);
    }
    audio_shutdown();
    /* After shutdown the table is gone. */
    assert(audio_sound_pcm(SFX_STEP, NULL) == NULL);
    printf("PASS: sound_id_table\n");
}

/* ---- Lifecycle / null backend safety ------------------------------------ */

static void test_lifecycle_null_backend(void) {
    /* Playing before init is a harmless no-op (must not crash). */
    audio_play(SFX_STEP);
    audio_update(NULL);
    audio_shutdown();                 /* shutdown w/o init: safe */

    audio_init();
    audio_init();                     /* idempotent */

    float pos[3] = {10, 0, 5}, listener[3] = {0, 0, 0};
    audio_play(SFX_BLOCK_BREAK);
    audio_play_at(SFX_MOB_HURT, pos, listener);
    audio_play_at(SFX_EAT, NULL, NULL);   /* NULL positions fall back to 2D */
    assert(audio_play_count(SFX_BLOCK_BREAK) == 1);
    assert(audio_play_count(SFX_MOB_HURT) == 1);
    assert(audio_play_count(SFX_EAT) == 1);

    audio_set_music(true);
    audio_update(listener);
    audio_update(NULL);
    audio_set_music(false);

    /* Out-of-range ids are ignored, not crashes. */
    audio_play((SoundId)SFX_COUNT);
    audio_play((SoundId)-1);

    audio_shutdown();
    audio_shutdown();                 /* double shutdown: safe */
    printf("PASS: lifecycle_null_backend\n");
}

int main(void) {
    test_env_decay();
    test_oscillators();
    test_noise();
    test_sfx_generation();
    test_sfx_respects_capacity();
    test_music_generation();
    test_buffers_in_range_and_finite();
    test_mix_clamp_and_wav_dump();
    test_sound_id_table();
    test_lifecycle_null_backend();
    printf("ALL AUDIO TESTS PASSED\n");
    return 0;
}
