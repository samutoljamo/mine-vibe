#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "../src/audio.h"
#include "../src/music.h"

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

/* ---- Music DSL: semitone math, note math, oscillators ------------------- */

static void test_music_semitone_to_freq(void) {
    /* A4 (semitone 0) is exactly 440 Hz. */
    assert(fabsf(music_semitone_to_freq(0) - 440.0f) < 0.001f);
    /* An octave up doubles; an octave down halves. */
    assert(fabsf(music_semitone_to_freq(12) - 880.0f) < 0.01f);
    assert(fabsf(music_semitone_to_freq(-12) - 220.0f) < 0.01f);
    assert(fabsf(music_semitone_to_freq(24) - 1760.0f) < 0.05f);
    /* Equal temperament: 12 semitone steps multiply back to a clean octave. */
    float f = 440.0f;
    for (int s = 0; s < 12; s++)
        f *= music_semitone_to_freq(1) / 440.0f;  /* ratio per semitone */
    assert(fabsf(f - 880.0f) < 0.5f);
    /* Three semitones up from A is C (~523.25/2 -> 261.63 at A3... check ratio).
     * A + 3 semitones = C: 440 * 2^(3/12) ~= 523.25 ... that's C5. */
    assert(fabsf(music_semitone_to_freq(3) - 523.251f) < 0.1f);
    printf("PASS: music_semitone_to_freq\n");
}

static void test_music_note_math(void) {
    int sr = AUDIO_SAMPLE_RATE;
    /* At 120 BPM, one beat = 0.5s => sr/2 samples. */
    assert(music_beats_to_samples(1.0f, 120.0f, sr) == (size_t)(sr / 2));
    /* Two beats = 1s. */
    assert(music_beats_to_samples(2.0f, 120.0f, sr) == (size_t)sr);
    /* At 60 BPM one beat = 1s. */
    assert(music_beats_to_samples(1.0f, 60.0f, sr) == (size_t)sr);
    /* Half a beat at 120 BPM = 0.25s (allow ±1 for rounding). */
    {
        long got = (long)music_beats_to_samples(0.5f, 120.0f, sr);
        assert(labs(got - (long)(sr / 4)) <= 1);
    }
    /* Oscillators stay in range and are the right shape. */
    for (int i = 0; i < 500; i++) {
        float t = i / (float)sr;
        float s = music_osc(WAVE_SINE, t, 440.0f);
        float tri = music_osc(WAVE_TRIANGLE, t, 440.0f);
        assert(s >= -1.0001f && s <= 1.0001f);
        assert(tri >= -1.0001f && tri <= 1.0001f);
    }
    printf("PASS: music_note_math\n");
}

static void test_music_render_song(void) {
    const Song* song = music_default_song();
    assert(song && song->track_count >= 3);   /* lead + bass + pad at least */

    size_t loop = music_song_samples(song, AUDIO_SAMPLE_RATE);
    assert(loop > AUDIO_SAMPLE_RATE);          /* a few seconds long */
    assert(loop == audio_music_loop_samples()); /* engine agrees on loop len */

    /* Render the whole loop as float; finite + in range + carries signal. */
    float* buf = (float*)malloc(sizeof(float) * loop);
    assert(buf);
    size_t got = music_render(song, buf, loop, 0, AUDIO_SAMPLE_RATE);
    assert(got == loop);
    int nonzero = 0;
    float peak = 0.0f;
    for (size_t i = 0; i < loop; i++) {
        assert(buf[i] == buf[i]);                  /* not NaN */
        assert(buf[i] >= -1.0001f && buf[i] <= 1.0001f);
        if (buf[i] != 0.0f) nonzero = 1;
        if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
    }
    assert(nonzero);
    assert(peak > 0.05f);     /* audible */
    assert(peak <= 1.0f);     /* mixed without clipping */
    free(buf);
    printf("PASS: music_render_song\n");
}

/* The int16 engine music buffer must loop SEAMLESSLY: the sample just after the
 * loop end equals the sample at the loop start (continuity across the join). */
static void test_music_loops_seamlessly(void) {
    size_t loop = audio_music_loop_samples();
    assert(loop > 0);

    /* First samples of the loop. */
    int16_t head[256];
    audio_gen_music(head, 256, 0);
    /* Samples straddling the loop boundary: the last few of one loop and the
     * first few of the next must be continuous (gen tiles by absolute pos). */
    int16_t join[256];
    audio_gen_music(join, 256, loop - 128);

    /* join[128] is the sample at absolute position `loop`, which is position 0
     * of the next loop -> must equal head[0]. join[255] == head[127]. */
    assert(join[128] == head[0]);
    assert(join[255] == head[127]);

    /* And the step across the boundary is small (no discontinuity/click): the
     * jump from the last sample of the loop to the first must be modest. */
    int16_t boundary = join[127];   /* abs pos loop-1 */
    int step = abs((int)head[0] - (int)boundary);
    assert(step < 4000);            /* << full scale: continuous, click-free */
    printf("PASS: music_loops_seamlessly\n");
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

/* ---- SFX are soft + short + click-free ---------------------------------- */

static void test_sfx_soft_and_short(void) {
    /* Soft: peak amplitude well below full scale (~0.3 target, allow margin).
     * Short: each SFX is under ~0.25s. Click-free: starts and ends near zero. */
    const float MAX_DUR = 0.26f;   /* seconds */
    const int   SOFT_PEAK = (int)(0.45f * 32767.0f);  /* generous ceiling */
    for (int id = 0; id < SFX_COUNT; id++) {
        int16_t buf[AUDIO_SAMPLE_RATE];
        size_t n = audio_gen_sfx((SoundId)id, buf, sizeof buf / sizeof buf[0]);
        assert(n > 4);
        /* Short. */
        assert((float)n / (float)AUDIO_SAMPLE_RATE <= MAX_DUR);
        /* Soft peak. */
        int peak = 0;
        for (size_t i = 0; i < n; i++)
            if (abs(buf[i]) > peak) peak = abs(buf[i]);
        assert(peak <= SOFT_PEAK);
        assert(peak > 200);   /* but still audible */
        /* Click-free onset + tail (ramped to/from zero). */
        assert(abs(buf[0]) < 800);
        assert(abs(buf[n - 1]) < 800);
    }
    printf("PASS: sfx_soft_and_short\n");
}

/* ---- Music is a render-once looping voice, NOT a per-frame stream -------- *
 *
 * The P0 bug was: audio_update() generated and submitted a ~33ms music chunk
 * EVERY frame, so above 30fps music played faster than real-time and the
 * overlapping chunks piled up into clipping. The fix renders the whole loop
 * once and hands it to the backend as a single looping voice; audio_update()
 * no longer touches music at all. These tests pin that contract. */

static void test_music_render_once_not_per_frame(void) {
    audio_init();   /* music defaults ON -> the loop is submitted exactly once */

    /* Turning music on submits the looping voice exactly once. */
    uint64_t base = audio_music_submit_count();
    assert(audio_music_is_playing());

    /* Hammer audio_update() at a high "frame rate": it must NOT submit any
     * more music (the whole point of the fix). */
    float listener[3] = {0, 0, 0};
    for (int i = 0; i < 1000; i++) audio_update(listener);
    assert(audio_music_submit_count() == base);     /* update never submits music */
    assert(audio_music_is_playing());               /* still playing */

    /* Toggling music off stops the loop; on re-submits exactly one voice. */
    audio_set_music(false);
    assert(!audio_music_is_playing());
    for (int i = 0; i < 100; i++) audio_update(listener);
    uint64_t after_off = audio_music_submit_count();

    audio_set_music(true);
    assert(audio_music_is_playing());
    assert(audio_music_submit_count() == after_off + 1);   /* exactly one (re)submit */

    /* And update still doesn't add submissions after re-enabling. */
    uint64_t after_on = audio_music_submit_count();
    for (int i = 0; i < 100; i++) audio_update(listener);
    assert(audio_music_submit_count() == after_on);

    audio_shutdown();
    printf("PASS: music_render_once_not_per_frame\n");
}

/* The single looping music buffer is the full song loop, in-range, finite, and
 * carries signal (it's the ONLY music voice now — no overlap, so no clipping). */
static void test_music_loop_buffer(void) {
    audio_init();

    size_t n = 0;
    const int16_t* loop = audio_music_loop_pcm(&n);
    assert(loop != NULL);
    /* Length matches the song loop length exactly (not time-compressed). */
    assert(n == audio_music_loop_samples());
    assert(n > AUDIO_SAMPLE_RATE);     /* multiple seconds */

    int nonzero = 0, peak = 0;
    for (size_t i = 0; i < n; i++) {
        assert(loop[i] > -32768 && loop[i] < 32767);   /* in range, no wrap */
        assert(loop[i] == loop[i]);                    /* finite */
        if (loop[i] != 0) nonzero = 1;
        if (abs(loop[i]) > peak) peak = abs(loop[i]);
    }
    assert(nonzero);
    assert(peak > 200);                /* audible */

    /* Seamless wrap: the stored loop matches audio_gen_music head, and the
     * join across the boundary is small (the looping voice wraps cleanly). */
    int16_t head[256];
    audio_gen_music(head, 256, 0);
    assert(memcmp(loop, head, sizeof head) == 0);      /* buffer == generator */
    int step = abs((int)loop[0] - (int)loop[n - 1]);
    assert(step < 4000);               /* continuous across the loop join */

    audio_shutdown();
    printf("PASS: music_loop_buffer\n");
}

/* Mute / master volume still scale music: since the loop is submitted once, the
 * effective music gain must track the CURRENT volume/mute live (no re-submit
 * artifacts). We verify the gain the engine pushes to the backend for music. */
static void test_music_respects_volume_and_mute(void) {
    audio_init();
    audio_set_muted(false);

    audio_set_master_volume(1.0f);
    float full = audio_music_effective_gain();
    assert(full > 0.0f);

    /* Halving master volume halves the music gain. */
    audio_set_master_volume(0.5f);
    float half = audio_music_effective_gain();
    assert(fabsf(half - full * 0.5f) < 1e-4f);

    /* Mute => truly silent music. */
    audio_set_muted(true);
    assert(audio_music_effective_gain() == 0.0f);

    /* Unmute restores. */
    audio_set_muted(false);
    assert(fabsf(audio_music_effective_gain() - half) < 1e-4f);

    audio_set_master_volume(0.6f);
    audio_shutdown();
    printf("PASS: music_respects_volume_and_mute\n");
}

/* ---- Master volume + mute ----------------------------------------------- */

static void test_master_volume_and_mute(void) {
    /* Default master volume is ~0.6. */
    float def = audio_get_master_volume();
    assert(def > 0.55f && def < 0.65f);

    /* Effective gain scales the base gain by the master volume. */
    audio_set_master_volume(0.5f);
    assert(fabsf(audio_get_master_volume() - 0.5f) < 1e-6f);
    assert(fabsf(audio_effective_gain(1.0f) - 0.5f) < 1e-6f);   /* ~half */
    assert(fabsf(audio_effective_gain(0.5f) - 0.25f) < 1e-6f);

    /* Out-of-range volume clamps to [0,1]. */
    audio_set_master_volume(5.0f);
    assert(fabsf(audio_get_master_volume() - 1.0f) < 1e-6f);
    audio_set_master_volume(-1.0f);
    assert(fabsf(audio_get_master_volume() - 0.0f) < 1e-6f);

    /* Mute forces effective gain to silence regardless of volume. */
    audio_set_master_volume(0.8f);
    audio_set_muted(true);
    assert(audio_get_muted());
    assert(audio_effective_gain(1.0f) == 0.0f);   /* silent */
    assert(audio_effective_gain(0.5f) == 0.0f);
    /* Unmuting restores the scaled gain. */
    audio_set_muted(false);
    assert(!audio_get_muted());
    assert(fabsf(audio_effective_gain(1.0f) - 0.8f) < 1e-6f);

    /* Restore default for any later tests. */
    audio_set_master_volume(0.6f);
    printf("PASS: master_volume_and_mute\n");
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
    test_music_semitone_to_freq();
    test_music_note_math();
    test_music_render_song();
    test_music_loops_seamlessly();
    test_music_render_once_not_per_frame();
    test_music_loop_buffer();
    test_music_respects_volume_and_mute();
    test_buffers_in_range_and_finite();
    test_sfx_soft_and_short();
    test_master_volume_and_mute();
    test_mix_clamp_and_wav_dump();
    test_sound_id_table();
    test_lifecycle_null_backend();
    printf("ALL AUDIO TESTS PASSED\n");
    return 0;
}
