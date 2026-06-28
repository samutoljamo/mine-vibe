#ifndef MUSIC_H
#define MUSIC_H

/*
 * music.h — a tiny declarative "music as code" DSL + sequencer.
 *
 * The idea: describe a calm, ambient song as data (tempo + a few tracks of
 * notes/rests, each played by an instrument), then render the whole thing once
 * into a single seamless looping PCM buffer. The engine (audio.c) loops that
 * buffer forever, so playback stays CPU-light.
 *
 * All output is mono float in [-1,1] (audio.c converts to int16). All these
 * functions are PURE — no global state, no OS dependency — so they are fully
 * unit-testable.
 *
 * Pitch is expressed as a semitone offset from a reference (A4 = 440 Hz,
 * MIDI-style: semitone 0 == A4). Durations are in beats (a beat = a quarter
 * note); tempo is BPM. A rest is a note with MUSIC_REST as its pitch.
 */

#include <stdint.h>
#include <stddef.h>

/* Sentinel pitch meaning "rest" (no tone), distinguishable from any real
 * semitone offset we use. */
#define MUSIC_REST INT16_MIN

/* Oscillator waveforms. Sine/triangle are warm and chosen for ambient pads;
 * square is available but intentionally unused by the default calm song. */
typedef enum {
    WAVE_SINE = 0,
    WAVE_TRIANGLE,
    WAVE_SQUARE
} Waveform;

/* ADSR amplitude envelope, all times in seconds, sustain is a level in [0,1]. */
typedef struct {
    float attack;   /* time to ramp 0 -> 1 */
    float decay;    /* time to fall 1 -> sustain */
    float sustain;  /* held level while the note sounds */
    float release;  /* time to fall sustain -> 0 after note end */
} ADSR;

/* An instrument = a waveform + an envelope + an overall gain. */
typedef struct {
    Waveform wave;
    ADSR     env;
    float    gain;   /* per-instrument mix level */
} Instrument;

/* A single event in a track: a pitch (semitones from A4, or MUSIC_REST) held
 * for `beats` beats. */
typedef struct {
    int16_t semitone;   /* relative to A4=440; MUSIC_REST = silence */
    float   beats;      /* duration in beats */
} Note;

/* A track = one instrument playing a sequence of notes. */
typedef struct {
    const Instrument* instrument;
    const Note*       notes;
    size_t            note_count;
} Track;

/* A song = a tempo and a set of tracks rendered together. All tracks should
 * sum to the same total length in beats so the mix loops cleanly. */
typedef struct {
    float        bpm;
    const Track* tracks;
    size_t       track_count;
} Song;

/* ---- Pure helpers ------------------------------------------------------- */

/* Convert a semitone offset (relative to A4 = 440 Hz) to a frequency in Hz,
 * using equal temperament: freq = 440 * 2^(semitone/12). Pure. */
float music_semitone_to_freq(int semitone);

/* Number of samples a duration of `beats` beats occupies at `bpm` and the
 * given sample rate. Pure. */
size_t music_beats_to_samples(float beats, float bpm, int sample_rate);

/* One sample of `wave` at phase time `t` (seconds) and frequency `freq`. Pure
 * (value noise / table free). Returns a value in [-1,1]. */
float music_osc(Waveform wave, float t, float freq);

/* Total length of a song in samples at `sample_rate` (max track length). This
 * is the loop length. Pure. */
size_t music_song_samples(const Song* song, int sample_rate);

/* Render `song` into `out` (float, capacity `cap` samples) at `sample_rate`,
 * starting at absolute loop position `start` (so successive calls tile
 * seamlessly across the loop boundary). Returns samples written. Pure. */
size_t music_render(const Song* song, float* out, size_t cap,
                    uint64_t start, int sample_rate);

/* The built-in calm ambient song (natural-minor, slow tempo, lead+bass+pad
 * over an i-VI-III-VII progression). Stable pointer, valid for program
 * lifetime. */
const Song* music_default_song(void);

#endif /* MUSIC_H */
