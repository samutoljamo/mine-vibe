#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../src/audio.h"

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
    test_sound_id_table();
    test_lifecycle_null_backend();
    printf("ALL AUDIO TESTS PASSED\n");
    return 0;
}
