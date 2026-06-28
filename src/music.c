/*
 * music.c — "music as code" DSL + sequencer (see music.h).
 *
 * A song is declared as data: a tempo plus a few tracks, each an instrument
 * playing a sequence of notes/rests. music_render() walks every track, turns
 * each note into an oscillator under an ADSR envelope, and sums all tracks into
 * one mono float buffer. The engine renders the whole loop ONCE and loops it,
 * so there is no per-frame synthesis cost.
 *
 * The built-in song is deliberately calm/ambient: A natural-minor, a slow
 * tempo, warm sine/triangle voices, and a soft chord pad following an
 * i-VI-III-VII progression (Am - F - C - G). It spans whole bars so the loop
 * is seamless, and the release tails of the last notes are written back into
 * the head of the buffer (wrap) so the loop join has no click.
 */

#include "music.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float music_semitone_to_freq(int semitone)
{
    /* Equal temperament, A4 = 440 Hz at semitone 0. */
    return 440.0f * powf(2.0f, (float)semitone / 12.0f);
}

size_t music_beats_to_samples(float beats, float bpm, int sample_rate)
{
    if (bpm <= 0.0f || sample_rate <= 0) return 0;
    float seconds = beats * 60.0f / bpm;
    return (size_t)(seconds * (float)sample_rate + 0.5f);
}

float music_osc(Waveform wave, float t, float freq)
{
    float phase = t * freq;
    float frac  = phase - floorf(phase);   /* 0..1 within one period */
    switch (wave) {
        case WAVE_TRIANGLE:
            /* 0->1->0->-1->0 triangle, peak ±1. */
            return 4.0f * fabsf(frac - 0.5f) - 1.0f;
        case WAVE_SQUARE:
            return frac < 0.5f ? 1.0f : -1.0f;
        case WAVE_SINE:
        default:
            return sinf(2.0f * (float)M_PI * frac);
    }
}

/* ADSR amplitude at sample `i` into a note that sounds for `note_samples`
 * samples, with `release` extending past that. Pure. */
static float adsr_at(const ADSR* e, size_t i, size_t note_samples, int sr)
{
    float t   = (float)i / (float)sr;
    float ndur = (float)note_samples / (float)sr;

    if (t < e->attack) {
        return e->attack > 0.0f ? (t / e->attack) : 1.0f;
    }
    float td = t - e->attack;
    if (td < e->decay) {
        float k = e->decay > 0.0f ? (td / e->decay) : 1.0f;
        return 1.0f + (e->sustain - 1.0f) * k;       /* 1 -> sustain */
    }
    if (t < ndur) {
        return e->sustain;                            /* hold */
    }
    /* Release phase (past the held note duration). */
    float tr = t - ndur;
    if (tr < e->release) {
        float k = e->release > 0.0f ? (tr / e->release) : 1.0f;
        return e->sustain * (1.0f - k);               /* sustain -> 0 */
    }
    return 0.0f;
}

size_t music_song_samples(const Song* song, int sample_rate)
{
    if (!song || sample_rate <= 0) return 0;
    size_t max_len = 0;
    for (size_t ti = 0; ti < song->track_count; ti++) {
        const Track* tr = &song->tracks[ti];
        size_t len = 0;
        for (size_t ni = 0; ni < tr->note_count; ni++)
            len += music_beats_to_samples(tr->notes[ni].beats, song->bpm, sample_rate);
        if (len > max_len) max_len = len;
    }
    return max_len;
}

size_t music_render(const Song* song, float* out, size_t cap,
                    uint64_t start, int sample_rate)
{
    if (!song || !out || cap == 0 || sample_rate <= 0) return 0;

    size_t loop = music_song_samples(song, sample_rate);
    if (loop == 0) { memset(out, 0, cap * sizeof(float)); return cap; }

    /* For each output sample, determine per track which note is sounding (plus
     * any release tail bleeding in from an earlier note, and the wrapped tail
     * of the final note for a seamless loop) and sum the voices. This is O(cap *
     * tracks * notes); the engine renders the whole loop ONCE and then loops the
     * resulting buffer, so the cost is paid a single time at init. */
    for (size_t k = 0; k < cap; k++) {
        size_t pos = (size_t)((start + k) % loop);
        float acc = 0.0f;

        for (size_t ti = 0; ti < song->track_count; ti++) {
            const Track* tr = &song->tracks[ti];
            const Instrument* inst = tr->instrument;
            if (!inst || tr->note_count == 0) continue;

            /* Walk notes to find the one covering `pos` (and remember the
             * previous note for its release tail). */
            size_t cursor = 0;
            for (size_t ni = 0; ni < tr->note_count; ni++) {
                size_t nlen = music_beats_to_samples(tr->notes[ni].beats,
                                                     song->bpm, sample_rate);
                size_t note_end = cursor + nlen;

                /* Active note body (attack/decay/sustain region). */
                if (pos >= cursor && pos < note_end) {
                    const Note* nt = &tr->notes[ni];
                    if (nt->semitone != MUSIC_REST) {
                        size_t off = pos - cursor;
                        float t  = (float)off / (float)sample_rate;
                        float f  = music_semitone_to_freq(nt->semitone);
                        float a  = adsr_at(&inst->env, off, nlen, sample_rate);
                        acc += music_osc(inst->wave, t, f) * a * inst->gain;
                    }
                }
                /* Release tail of a note bleeding past its end into `pos`. */
                else if (pos >= note_end) {
                    const Note* nt = &tr->notes[ni];
                    size_t rel = (size_t)(inst->env.release * sample_rate + 0.5f);
                    if (nt->semitone != MUSIC_REST &&
                        pos < note_end + rel) {
                        size_t off = pos - cursor;   /* into note incl. release */
                        float t  = (float)off / (float)sample_rate;
                        float f  = music_semitone_to_freq(nt->semitone);
                        float a  = adsr_at(&inst->env, off, nlen, sample_rate);
                        acc += music_osc(inst->wave, t, f) * a * inst->gain;
                    }
                }

                cursor = note_end;
            }

            /* Wrap: a note near the loop end whose release runs PAST the loop
             * should also sound at small `pos` values at the head. Handle by
             * checking the last note's tail wrapping to the front. */
            if (tr->note_count > 0) {
                const Note* last = &tr->notes[tr->note_count - 1];
                if (last->semitone != MUSIC_REST) {
                    size_t rel = (size_t)(inst->env.release * sample_rate + 0.5f);
                    size_t last_len = music_beats_to_samples(last->beats,
                                                song->bpm, sample_rate);
                    /* last note ends at `loop`; its release covers [loop, loop+rel)
                     * which wraps to [0, rel). */
                    if (pos < rel && rel > 0) {
                        size_t off = last_len + pos;   /* into note incl. release */
                        float t  = (float)off / (float)sample_rate;
                        float f  = music_semitone_to_freq(last->semitone);
                        float a  = adsr_at(&inst->env, off, last_len, sample_rate);
                        acc += music_osc(inst->wave, t, f) * a * inst->gain;
                    }
                }
            }
        }

        /* Gentle soft clip to keep within [-1,1] without harsh clamping. */
        if (acc >  1.0f) acc =  1.0f;
        if (acc < -1.0f) acc = -1.0f;
        out[k] = acc;
    }
    return cap;
}

/* ===================================================================== *
 *  The built-in calm song.
 *
 *  Key: A natural minor (A B C D E F G). Semitones relative to A4 = 440:
 *    A=0  B=2  C=3  D=5  E=7  F=8  G=10  (next A=12)
 *  Lower octave for bass: subtract 12; pad sits mid; lead sits up an octave.
 *
 *  Tempo: 72 BPM (slow, breathing). 4/4. 8 bars total.
 *  Progression (2 bars each): i (Am) - VI (F) - III (C) - VII (G).
 * ===================================================================== */

#define SR_REF 22050   /* documentation only; actual sr passed at render */

/* Scale degrees as semitone offsets from A4. */
enum {
    A3 = -12, B3 = -10, C4 = -9, D4 = -7, E4 = -5, F4 = -4, G4 = -2,
    A4 = 0,  B4 = 2,  C5 = 3,  D5 = 5,  E5 = 7,  F5 = 8,  G5 = 10,
    A5 = 12
};

/* Instruments ------------------------------------------------------------- */

/* Lead: warm triangle, gentle pluck (medium attack, long-ish decay/release). */
static const Instrument INST_LEAD = {
    .wave = WAVE_TRIANGLE,
    .env  = { .attack = 0.04f, .decay = 0.25f, .sustain = 0.45f, .release = 0.30f },
    .gain = 0.30f,
};

/* Bass: deep sine, slow swell, long sustain — the floor of the mix. */
static const Instrument INST_BASS = {
    .wave = WAVE_SINE,
    .env  = { .attack = 0.06f, .decay = 0.20f, .sustain = 0.70f, .release = 0.40f },
    .gain = 0.34f,
};

/* Pad: soft sine chords, very slow attack/release so they breathe under the
 * melody without any percussive edge. */
static const Instrument INST_PAD = {
    .wave = WAVE_SINE,
    .env  = { .attack = 0.50f, .decay = 0.60f, .sustain = 0.55f, .release = 0.70f },
    .gain = 0.16f,
};

/* Tracks ------------------------------------------------------------------ */
/* Each track totals 8 bars * 4 beats = 32 beats so the loop is whole-bar. */

/* Lead melody — sparse, mostly stepwise within A minor, with rests to breathe.
 * 2 bars (8 beats) per chord region. */
static const Note LEAD_NOTES[] = {
    /* i: Am */
    { E5, 1.0f }, { C5, 1.0f }, { D5, 1.0f }, { E5, 1.0f },
    { A4, 2.0f }, { MUSIC_REST, 2.0f },
    /* VI: F */
    { F5, 1.0f }, { E5, 1.0f }, { C5, 2.0f },
    { D5, 1.0f }, { C5, 1.0f }, { A4, 2.0f },
    /* III: C */
    { G4, 1.0f }, { C5, 1.0f }, { E5, 1.0f }, { G5, 1.0f },
    { E5, 2.0f }, { MUSIC_REST, 2.0f },
    /* VII: G */
    { D5, 1.0f }, { B4, 1.0f }, { G4, 2.0f },
    { B4, 1.0f }, { D5, 1.0f }, { E5, 2.0f },
};

/* Bass — root of each chord, low octave, one long note per bar. */
static const Note BASS_NOTES[] = {
    /* i: Am -> A */
    { A3, 4.0f }, { A3, 4.0f },
    /* VI: F */
    { F4 - 12, 4.0f }, { F4 - 12, 4.0f },
    /* III: C */
    { C4 - 12, 4.0f }, { C4 - 12, 4.0f },
    /* VII: G */
    { G4 - 12, 4.0f }, { G4 - 12, 4.0f },
};

/* Pad — sustained triad roots+fifths, two whole-bar chords per region (held as
 * 4-beat notes). We voice a single mid tone per beat-block; the chord color
 * comes from lead+bass context (keeps the pad uncluttered/soft). */
static const Note PAD_NOTES[] = {
    /* i: Am -> A + E */
    { A4, 4.0f }, { E4, 4.0f },
    /* VI: F -> F + C */
    { F4, 4.0f }, { C4, 4.0f },
    /* III: C -> C + G */
    { C5, 4.0f }, { G4, 4.0f },
    /* VII: G -> G + D */
    { G4, 4.0f }, { D5, 4.0f },
};

static const Track TRACKS[] = {
    { &INST_PAD,  PAD_NOTES,  sizeof(PAD_NOTES)  / sizeof(PAD_NOTES[0])  },
    { &INST_BASS, BASS_NOTES, sizeof(BASS_NOTES) / sizeof(BASS_NOTES[0]) },
    { &INST_LEAD, LEAD_NOTES, sizeof(LEAD_NOTES) / sizeof(LEAD_NOTES[0]) },
};

static const Song DEFAULT_SONG = {
    .bpm         = 72.0f,
    .tracks      = TRACKS,
    .track_count = sizeof(TRACKS) / sizeof(TRACKS[0]),
};

const Song* music_default_song(void)
{
    return &DEFAULT_SONG;
}
