#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/chunkwire.h"

#define N CHUNKWIRE_COLUMN_BLOCKS

/* Deterministic PRNG so the test is reproducible. */
static uint32_t rng_state = 0x1234567u;
static uint32_t xrand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Round-trip: encode then decode must reproduce the input exactly. */
static void roundtrip(const uint8_t* in, const char* name) {
    size_t cap = chunkwire_rle_bound(N);
    uint8_t* enc = malloc(cap);
    assert(enc);
    size_t enc_len = chunkwire_rle_encode(in, N, enc, cap);
    assert(enc_len > 0);              /* bound must be sufficient */
    assert(enc_len <= cap);          /* never exceeds the stated bound */

    uint8_t* dec = malloc(N);
    assert(dec);
    size_t dec_n = 0;
    bool ok = chunkwire_rle_decode(enc, enc_len, dec, N, &dec_n);
    assert(ok);
    assert(dec_n == N);
    assert(memcmp(in, dec, N) == 0);

    printf("  PASS roundtrip[%s] encoded %zu bytes (%.1f%% of raw)\n",
           name, enc_len, 100.0 * (double)enc_len / (double)N);
    free(enc);
    free(dec);
}

static void test_all_air(void) {
    uint8_t* in = calloc(N, 1);
    roundtrip(in, "all-air");
    /* A single uniform run should compress to a tiny payload. */
    uint8_t enc[64];
    size_t len = chunkwire_rle_encode(in, N, enc, sizeof enc);
    assert(len > 0 && len <= 8);
    free(in);
    printf("PASS: all_air\n");
}

static void test_all_stone(void) {
    uint8_t* in = malloc(N);
    memset(in, 5, N);  /* arbitrary non-zero block id */
    roundtrip(in, "all-stone");
    free(in);
    printf("PASS: all_stone\n");
}

static void test_single_block(void) {
    uint8_t* in = calloc(N, 1);
    in[N / 2] = 7;
    roundtrip(in, "single-block");
    free(in);
    printf("PASS: single_block\n");
}

/* Run-heavy: air column above, stone below — the realistic terrain case. */
static void test_run_heavy(void) {
    uint8_t* in = malloc(N);
    for (size_t i = 0; i < N; i++) in[i] = (i < N * 2 / 3) ? 5 : 0;
    roundtrip(in, "run-heavy");
    /* Two runs -> should be far smaller than raw. */
    size_t cap = chunkwire_rle_bound(N);
    uint8_t* enc = malloc(cap);
    size_t len = chunkwire_rle_encode(in, N, enc, cap);
    assert(len < N / 100);
    free(enc);
    free(in);
    printf("PASS: run_heavy\n");
}

/* Worst case: every byte differs from its neighbour (no runs). The bound must
 * still hold and the round-trip must still be exact. */
static void test_alternating(void) {
    uint8_t* in = malloc(N);
    for (size_t i = 0; i < N; i++) in[i] = (uint8_t)(i & 1);
    roundtrip(in, "alternating");
    free(in);
    printf("PASS: alternating\n");
}

static void test_random(void) {
    uint8_t* in = malloc(N);
    for (int iter = 0; iter < 20; iter++) {
        /* Mix of small block-id range (realistic) and full range. */
        int mod = (iter & 1) ? 8 : 256;
        for (size_t i = 0; i < N; i++) in[i] = (uint8_t)(xrand() % mod);
        roundtrip(in, "random");
    }
    free(in);
    printf("PASS: random\n");
}

/* Encoding into an undersized buffer must fail cleanly (return 0), not overrun. */
static void test_encode_too_small(void) {
    uint8_t* in = malloc(N);
    for (size_t i = 0; i < N; i++) in[i] = (uint8_t)(xrand() & 0xFF);
    uint8_t tiny[4];
    size_t len = chunkwire_rle_encode(in, N, tiny, sizeof tiny);
    assert(len == 0);
    free(in);
    printf("PASS: encode_too_small\n");
}

/* Decoding into an undersized output must fail cleanly, not overrun. */
static void test_decode_out_too_small(void) {
    uint8_t* in = malloc(N);
    memset(in, 3, N);
    size_t cap = chunkwire_rle_bound(N);
    uint8_t* enc = malloc(cap);
    size_t len = chunkwire_rle_encode(in, N, enc, cap);
    assert(len > 0);
    uint8_t small[16];
    size_t dec_n = 999;
    bool ok = chunkwire_rle_decode(enc, len, small, sizeof small, &dec_n);
    assert(!ok);
    free(enc);
    free(in);
    printf("PASS: decode_out_too_small\n");
}

/* Garbage / truncated input must fail cleanly. */
static void test_decode_garbage(void) {
    uint8_t buf[3] = { 0xFF, 0xFF, 0xFF };  /* incomplete varint */
    uint8_t out[N];
    size_t dec_n = 0;
    bool ok = chunkwire_rle_decode(buf, sizeof buf, out, N, &dec_n);
    assert(!ok);

    /* Empty RLE input decodes to zero bytes (valid, but not a full column —
     * the column-size check lives in chunkwire_decode_chunk, tested there). */
    dec_n = 999;
    ok = chunkwire_rle_decode(buf, 0, out, N, &dec_n);
    assert(ok);
    assert(dec_n == 0);
    printf("PASS: decode_garbage\n");
}

/* Bound must be a true upper bound for every tested input. */
static void test_bound_is_upper(void) {
    uint8_t* in = malloc(N);
    size_t cap = chunkwire_rle_bound(N);
    uint8_t* enc = malloc(cap);
    for (int iter = 0; iter < 50; iter++) {
        for (size_t i = 0; i < N; i++) in[i] = (uint8_t)(xrand() & 0xFF);
        size_t len = chunkwire_rle_encode(in, N, enc, cap);
        assert(len > 0);       /* bound buffer always sufficient */
        assert(len <= cap);
    }
    free(enc);
    free(in);
    printf("PASS: bound_is_upper\n");
}

/* Full chunk-body serialize/deserialize equivalence (cx, cz, blocks). */
static void test_chunk_serialize(void) {
    uint8_t* blocks = malloc(N);
    for (size_t i = 0; i < N; i++)
        blocks[i] = (i < N / 2) ? 5 : (uint8_t)(i & 7);

    uint8_t* buf = malloc(chunkwire_encode_bound());
    size_t len = chunkwire_encode_chunk(-7, 42, blocks, buf,
                                        chunkwire_encode_bound());
    assert(len > 0);

    int32_t cx = 0, cz = 0;
    uint8_t* out = malloc(N);
    bool ok = chunkwire_decode_chunk(buf, len, &cx, &cz, out, N);
    assert(ok);
    assert(cx == -7);
    assert(cz == 42);
    assert(memcmp(blocks, out, N) == 0);

    /* Truncated body must fail cleanly. */
    ok = chunkwire_decode_chunk(buf, 4, &cx, &cz, out, N);
    assert(!ok);

    free(blocks);
    free(buf);
    free(out);
    printf("PASS: chunk_serialize\n");
}

int main(void) {
    test_all_air();
    test_all_stone();
    test_single_block();
    test_run_heavy();
    test_alternating();
    test_random();
    test_encode_too_small();
    test_decode_out_too_small();
    test_decode_garbage();
    test_bound_is_upper();
    test_chunk_serialize();
    printf("ALL CHUNKWIRE TESTS PASSED\n");
    return 0;
}
