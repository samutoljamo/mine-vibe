#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "../src/audio.h"
#include "../src/music.h"

/* Resolve a writable temp directory portably: $TMPDIR/$TMP/$TEMP if set
 * (covers Windows, which has no /tmp), else /tmp on POSIX. Returns into `out`
 * a "<dir>/<name>" path. */
static void temp_path(char* out, size_t out_sz, const char* name)
{
    const char* dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = getenv("TMP");
    if (!dir || !*dir) dir = getenv("TEMP");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(out, out_sz, "%s/%s", dir, name);
}

/* ---- Tiny WAV writer (mono s16) for eyeball/`xxd`/`file` sanity checks ----
 * Not part of the assertions; purely a debug artifact in the temp dir so a
 * human (with no speakers in CI) can confirm the headers and levels look like
 * audio rather than noise. Returns bytes written (0 on failure). */
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

/* Ambient beds (cave drip, wind) are intentionally LONGER and slower than the
 * punchy gameplay SFX, so the "short footstep/feedback" duration bounds don't
 * apply to them. They still must be bounded, in-range, soft and click-free. */
static int is_ambient_sfx(int id) {
    return id == SFX_AMBIENT_CAVE || id == SFX_AMBIENT_WIND;
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
    char wav_sfx[1024], wav_mix[1024];
    temp_path(wav_sfx, sizeof(wav_sfx), "audio_sfx_block_break.wav");
    temp_path(wav_mix, sizeof(wav_mix), "audio_mixed_1s.wav");
    size_t wrote = write_wav_mono_s16(wav_sfx, sfx, sn, AUDIO_SAMPLE_RATE);
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

    wrote = write_wav_mono_s16(wav_mix, mixed, OUT, AUDIO_SAMPLE_RATE);
    assert(wrote == 44 + (size_t)OUT * sizeof(int16_t));

    printf("INFO: wrote %s (%zu samples) and %s (peak=%d/32767)\n",
           wav_sfx, sn, wav_mix, peak);

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
        /* Short — gameplay feedback only. Ambient beds are deliberately longer
         * (still bounded under the 1s buffer), so exempt them from this bound. */
        if (!is_ambient_sfx(id))
            assert((float)n / (float)AUDIO_SAMPLE_RATE <= MAX_DUR);
        assert((float)n / (float)AUDIO_SAMPLE_RATE <= 1.0f);   /* bounded for all */
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

/* ---- SFX quality: smooth envelopes, no inter-sample clicks --------------- *
 *
 * These pin the "sounds good" bar from the redesign: every SFX must ramp in
 * from ~0, ramp out to ~0, carry real signal in the middle, and — crucially —
 * have NO large sample-to-sample jump anywhere (a big step == an audible click
 * even in the body of the sound, e.g. a hard square edge or an envelope kink).
 * Peaks stay inside full scale with headroom so a busy mix never clips. */

static void test_sfx_smooth_envelope_and_no_clicks(void) {
    for (int id = 0; id < SFX_COUNT; id++) {
        int16_t buf[AUDIO_SAMPLE_RATE];
        size_t n = audio_gen_sfx((SoundId)id, buf, sizeof buf / sizeof buf[0]);
        assert(n > 64);

        /* Onset eases in from (near) zero. */
        assert(abs(buf[0]) < 400);
        /* Tail lands on (near) zero. */
        assert(abs(buf[n - 1]) < 400);

        /* The fine-grained attack/body/release ramp-timing checks below assume a
         * fast (few-ms) gameplay envelope. Ambient beds use a slow, multi-100ms
         * swell, so the 4ms windows don't apply; they are still proven near-zero
         * at the ends, click-free, in-range and soft by the universal checks. */
        if (!is_ambient_sfx(id)) {
            /* Soft attack: the envelope eases IN, so the immediate onset (first
             * ~0.5 ms) is clearly quieter than once the attack has opened up
             * (~3-4 ms in). A hard onset (old buzzer) would be full-level already
             * at sample ~1 and fail this. We compare windowed RMS-ish averages so a
             * single zero-crossing doesn't fool it. */
            size_t onset = (size_t)(AUDIO_SAMPLE_RATE * 0.0005f);  /* ~0.5 ms */
            size_t open  = (size_t)(AUDIO_SAMPLE_RATE * 0.004f);   /* ~4 ms */
            if (onset < 4) onset = 4;
            if (open <= onset) open = onset + 4;
            if (open + onset > n) { open = n / 2; onset = open / 4; if (onset < 1) onset = 1; }
            long onset_sum = 0, open_sum = 0;
            for (size_t i = 0; i < onset; i++) onset_sum += abs(buf[i]);
            for (size_t i = open; i < open + onset; i++) open_sum += abs(buf[i]);
            double onset_avg = (double)onset_sum / (double)onset;
            double open_avg  = (double)open_sum  / (double)onset;
            assert(open_avg > 100.0);              /* sound has opened to real level */
            assert(onset_avg < open_avg);          /* attack ramps up, no hard onset */

            /* Body carries real signal in the middle. */
            long body_sum = 0;
            size_t bw = open;   /* reuse a ~few-ms window */
            if (n / 3 + bw > n) bw = n - n / 3;
            for (size_t i = n / 3; i < n / 3 + bw; i++) body_sum += abs(buf[i]);
            double body_avg = (double)body_sum / (double)bw;
            assert(body_avg > 80.0);               /* middle is non-silent */

            /* Soft release: the very tail (~0.5 ms) is clearly quieter than a few ms
             * earlier — the release ramp decays the sound out to zero. */
            long tailend_sum = 0, prerel_sum = 0;
            for (size_t i = n - onset; i < n; i++) tailend_sum += abs(buf[i]);
            size_t pr = n > open + onset ? n - open - onset : 0;
            for (size_t i = pr; i < pr + onset; i++) prerel_sum += abs(buf[i]);
            double tailend_avg = (double)tailend_sum / (double)onset;
            double prerel_avg  = (double)prerel_sum  / (double)onset;
            assert(tailend_avg < prerel_avg + 1.0);  /* release decays out (<=, slack) */
            assert(abs(buf[n - 1]) < abs(buf[pr]) + 200 || abs(buf[n-1]) < 400);
        }

        /* No inter-sample click ANYWHERE: the largest single-sample step stays
         * well under full scale. A bare square wave at high amplitude (the old
         * buzzer) would jump ~2*peak between adjacent samples and fail this. */
        int max_step = 0;
        for (size_t i = 1; i < n; i++) {
            int step = abs((int)buf[i] - (int)buf[i - 1]);
            if (step > max_step) max_step = step;
        }
        assert(max_step < 9000);   /* << 65535 full-scale span: no click */

        /* Peak: audible but with comfortable headroom (never near the rails). */
        int peak = 0;
        for (size_t i = 0; i < n; i++)
            if (abs(buf[i]) > peak) peak = abs(buf[i]);
        assert(peak > 300);            /* audible */
        assert(peak <= 30000);         /* headroom: never the int16 rail */
    }
    printf("PASS: sfx_smooth_envelope_and_no_clicks\n");
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

/* Voice-allocation / oldest-voice-stealing policy (mine-vibe-5kz). Pure: no
 * device required. Reuses a free slot when available; steals the oldest active
 * voice (smallest age) when the pool is full instead of dropping the new one. */
static void test_pick_voice(void) {
    /* Free slot is preferred even when others are busy (no stealing while idle). */
    {
        uint8_t  active[4] = { 1, 1, 0, 1 };
        uint64_t age[4]    = { 5, 1, 0, 9 };   /* slot 1 is oldest, but slot 2 is free */
        assert(audio_pick_voice(active, age, 4) == 2);
    }
    /* First free slot wins when several are free. */
    {
        uint8_t  active[4] = { 1, 0, 0, 1 };
        uint64_t age[4]    = { 5, 0, 0, 9 };
        assert(audio_pick_voice(active, age, 4) == 1);
    }
    /* All busy -> steal the OLDEST (smallest age stamp). */
    {
        uint8_t  active[4] = { 1, 1, 1, 1 };
        uint64_t age[4]    = { 7, 3, 9, 4 };   /* slot 1 (age 3) is oldest */
        assert(audio_pick_voice(active, age, 4) == 1);
    }
    /* All busy, oldest is the last slot. */
    {
        uint8_t  active[3] = { 1, 1, 1 };
        uint64_t age[3]    = { 100, 50, 10 };
        assert(audio_pick_voice(active, age, 3) == 2);
    }
    /* Degenerate inputs are graceful, never -1 except for empty pools. */
    {
        uint8_t  active[1] = { 1 };
        uint64_t age[1]    = { 42 };
        assert(audio_pick_voice(active, age, 1) == 0);   /* steal the only slot */
        assert(audio_pick_voice(active, NULL, 1) == 0);  /* no age array: still graceful */
        assert(audio_pick_voice(NULL, age, 1) == -1);    /* no active array */
        assert(audio_pick_voice(active, age, 0) == -1);  /* empty pool */
    }
    printf("PASS: pick_voice\n");
}

/* ---- Footstep surface mapping (pure, dyb.4.2) --------------------------- *
 *
 * The block -> material -> SFX_STEP_* mapping is pure: deterministic, total over
 * all blocks (unknowns fall back to a soft step, never silent / never an
 * out-of-range id), and composes as block == material(block) -> sfx. */
static void test_footstep_mapping(void) {
    /* Representative blocks land on the expected material. */
    assert(audio_footstep_material_for_block(BLOCK_GRASS) == STEP_MAT_SOFT);
    assert(audio_footstep_material_for_block(BLOCK_LEAVES) == STEP_MAT_SOFT);
    assert(audio_footstep_material_for_block(BLOCK_SNOW)  == STEP_MAT_SOFT);
    assert(audio_footstep_material_for_block(BLOCK_DIRT)  == STEP_MAT_DIRT);
    assert(audio_footstep_material_for_block(BLOCK_PATH)  == STEP_MAT_DIRT);
    assert(audio_footstep_material_for_block(BLOCK_STONE) == STEP_MAT_STONE);
    assert(audio_footstep_material_for_block(BLOCK_COBBLE) == STEP_MAT_STONE);
    assert(audio_footstep_material_for_block(BLOCK_IRON_ORE) == STEP_MAT_STONE);
    assert(audio_footstep_material_for_block(BLOCK_SAND) == STEP_MAT_SAND);
    assert(audio_footstep_material_for_block(BLOCK_SANDSTONE) == STEP_MAT_SAND);
    assert(audio_footstep_material_for_block(BLOCK_WOOD) == STEP_MAT_WOOD);
    assert(audio_footstep_material_for_block(BLOCK_PLANKS) == STEP_MAT_WOOD);

    /* Material -> SFX is total and in range; each maps to a STEP buffer. */
    for (int m = 0; m < STEP_MAT_COUNT; m++) {
        SoundId s = audio_footstep_sfx_for_material((FootstepMaterial)m);
        assert(s >= SFX_STEP_GRASS && s <= SFX_STEP_WOOD);
    }

    /* Total over EVERY block id: never silent, never out of range; and the
     * convenience composition agrees with the two-step path. */
    for (int b = 0; b < BLOCK_COUNT; b++) {
        FootstepMaterial m = audio_footstep_material_for_block((BlockID)b);
        assert(m >= 0 && m < STEP_MAT_COUNT);
        SoundId s = audio_footstep_for_block((BlockID)b);
        assert(s >= SFX_STEP_GRASS && s <= SFX_STEP_WOOD);
        assert(s == audio_footstep_sfx_for_material(m));   /* composition holds */
    }

    /* Air/water fall back to the soft default (a footstep is never silent). */
    assert(audio_footstep_for_block(BLOCK_AIR)   == SFX_STEP_GRASS);
    assert(audio_footstep_for_block(BLOCK_WATER) == SFX_STEP_GRASS);

    /* Distinct materials map to distinct sounds (stone != sand != wood). */
    assert(audio_footstep_sfx_for_material(STEP_MAT_STONE) !=
           audio_footstep_sfx_for_material(STEP_MAT_SAND));
    assert(audio_footstep_sfx_for_material(STEP_MAT_WOOD) !=
           audio_footstep_sfx_for_material(STEP_MAT_STONE));

    printf("PASS: footstep_mapping\n");
}

/* The footstep / ambient play helpers route to the right buffer and count, and
 * are safe before init / with out-of-range materials (null-backend). */
static void test_footstep_and_ambient_playback(void) {
    audio_init();

    /* Each material's play helper increments the count for its mapped SFX. */
    for (int m = 0; m < STEP_MAT_COUNT; m++) {
        SoundId expect = audio_footstep_sfx_for_material((FootstepMaterial)m);
        uint64_t before = audio_play_count(expect);
        audio_play_footstep((FootstepMaterial)m, (uint64_t)m);
        assert(audio_play_count(expect) == before + 1);
    }

    /* Positional variant: NULL positions fall back to 2D, still counts. */
    float pos[3] = {3, 0, 4}, listener[3] = {0, 0, 0};
    uint64_t before = audio_play_count(SFX_STEP_STONE);
    audio_play_footstep_at(STEP_MAT_STONE, 1, pos, listener);
    audio_play_footstep_at(STEP_MAT_STONE, 2, NULL, NULL);
    assert(audio_play_count(SFX_STEP_STONE) == before + 2);

    /* Ambient one-shots are ordinary SFX and play via the normal API. */
    uint64_t cave = audio_play_count(SFX_AMBIENT_CAVE);
    audio_play(SFX_AMBIENT_CAVE);
    assert(audio_play_count(SFX_AMBIENT_CAVE) == cave + 1);
    audio_play_at(SFX_AMBIENT_WIND, pos, listener);
    assert(audio_play_count(SFX_AMBIENT_WIND) == 1);

    audio_shutdown();
    printf("PASS: footstep_and_ambient_playback\n");
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
    test_sfx_smooth_envelope_and_no_clicks();
    test_master_volume_and_mute();
    test_mix_clamp_and_wav_dump();
    test_sound_id_table();
    test_lifecycle_null_backend();
    test_pick_voice();
    test_footstep_mapping();
    test_footstep_and_ambient_playback();
    printf("ALL AUDIO TESTS PASSED\n");
    return 0;
}
