#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/chunk_stream.h"

/* Generous output capacities for the test (a disc of radius 16 fits easily). */
#define CAP 4096

static int coord_in_list(const ChunkCoord* list, size_t n, int32_t cx, int32_t cz) {
    for (size_t i = 0; i < n; i++)
        if (list[i].cx == cx && list[i].cz == cz) return 1;
    return 0;
}

/* Reference disc enumerator (the circular in-range set), nearest-first not
 * required here — used to verify completeness. */
static size_t ref_disc(int32_t ccx, int32_t ccz, int rd, ChunkCoord* out) {
    size_t n = 0;
    for (int dx = -rd; dx <= rd; dx++)
        for (int dz = -rd; dz <= rd; dz++)
            if (dx*dx + dz*dz <= rd*rd) {
                out[n].cx = ccx + dx;
                out[n].cz = ccz + dz;
                n++;
            }
    return n;
}

/* Fresh client (nothing sent) receives the FULL disc, with no budget cap. */
static void test_fresh_full_disc(void) {
    int rd = 8;
    ChunkCoord send[CAP], unload[CAP];
    size_t ns = 0, nu = 0;
    chunk_stream_diff(0, 0, rd, NULL, 0,
                      send, CAP, &ns, /*budget=*/0,
                      unload, CAP, &nu);

    ChunkCoord disc[CAP];
    size_t ndisc = ref_disc(0, 0, rd, disc);
    assert(ns == ndisc);
    assert(nu == 0);
    /* Every disc coord must be present in the send list. */
    for (size_t i = 0; i < ndisc; i++)
        assert(coord_in_list(send, ns, disc[i].cx, disc[i].cz));
    printf("PASS: fresh_full_disc (%zu chunks)\n", ns);
}

/* Nearest-first ordering: send list is non-decreasing in squared distance. */
static void test_nearest_first(void) {
    int rd = 6;
    ChunkCoord send[CAP], unload[CAP];
    size_t ns = 0, nu = 0;
    chunk_stream_diff(10, -3, rd, NULL, 0,
                      send, CAP, &ns, 0, unload, CAP, &nu);
    assert(ns > 0);
    /* First element must be the center itself (distance 0). */
    assert(send[0].cx == 10 && send[0].cz == -3);
    int prev = -1;
    for (size_t i = 0; i < ns; i++) {
        int dx = send[i].cx - 10, dz = send[i].cz - (-3);
        int d = dx*dx + dz*dz;
        assert(d >= prev);
        prev = d;
    }
    printf("PASS: nearest_first\n");
}

/* Budget cap: only `budget` nearest chunks are emitted per call; the rest are
 * left for subsequent calls. Streaming the whole disc across calls (recording
 * what was sent each time) eventually covers it with no duplicates. */
static void test_budget_progressive(void) {
    int rd = 5;
    ChunkCoord disc[CAP];
    size_t ndisc = ref_disc(0, 0, rd, disc);

    ChunkCoord sent[CAP];
    size_t nsent = 0;
    int budget = 7;
    int calls = 0;
    for (;;) {
        ChunkCoord send[CAP], unload[CAP];
        size_t ns = 0, nu = 0;
        chunk_stream_diff(0, 0, rd, sent, nsent,
                          send, CAP, &ns, budget, unload, CAP, &nu);
        assert(nu == 0);
        assert((int)ns <= budget);
        if (ns == 0) break;
        /* nearest-first within this batch */
        int prev = -1;
        for (size_t i = 0; i < ns; i++) {
            int d = send[i].cx*send[i].cx + send[i].cz*send[i].cz;
            assert(d >= prev); prev = d;
            /* must not already be in sent */
            assert(!coord_in_list(sent, nsent, send[i].cx, send[i].cz));
            sent[nsent++] = send[i];
        }
        if (++calls > 1000) { assert(!"budget loop did not terminate"); }
    }
    /* The accumulated sent set equals the full disc. */
    assert(nsent == ndisc);
    for (size_t i = 0; i < ndisc; i++)
        assert(coord_in_list(sent, nsent, disc[i].cx, disc[i].cz));
    printf("PASS: budget_progressive (%d calls, %zu chunks)\n", calls, nsent);
}

/* Stationary + fully streamed: idempotent (no sends, no unloads). */
static void test_idempotent_stationary(void) {
    int rd = 4;
    ChunkCoord disc[CAP];
    size_t ndisc = ref_disc(2, 2, rd, disc);

    ChunkCoord send[CAP], unload[CAP];
    size_t ns = 99, nu = 99;
    chunk_stream_diff(2, 2, rd, disc, ndisc,
                      send, CAP, &ns, 0, unload, CAP, &nu);
    assert(ns == 0);
    assert(nu == 0);
    printf("PASS: idempotent_stationary\n");
}

/* Moving by one chunk: sends exactly the new leading column and unloads the
 * trailing one. With a circular disc the counts on each side are equal and
 * nonzero, and no coord appears in both lists. */
static void test_move_one_chunk(void) {
    int rd = 8;
    ChunkCoord disc0[CAP];
    size_t ndisc0 = ref_disc(0, 0, rd, disc0);

    /* Client is fully streamed at center (0,0); now it moves to (1,0). */
    ChunkCoord send[CAP], unload[CAP];
    size_t ns = 0, nu = 0;
    chunk_stream_diff(1, 0, rd, disc0, ndisc0,
                      send, CAP, &ns, 0, unload, CAP, &nu);

    /* Verify against the reference: to_send = disc(1,0) \ disc(0,0);
     * to_unload = disc(0,0) \ disc(1,0). */
    ChunkCoord disc1[CAP];
    size_t ndisc1 = ref_disc(1, 0, rd, disc1);

    size_t expect_send = 0, expect_unload = 0;
    for (size_t i = 0; i < ndisc1; i++)
        if (!coord_in_list(disc0, ndisc0, disc1[i].cx, disc1[i].cz)) expect_send++;
    for (size_t i = 0; i < ndisc0; i++)
        if (!coord_in_list(disc1, ndisc1, disc0[i].cx, disc0[i].cz)) expect_unload++;

    assert(ns == expect_send);
    assert(nu == expect_unload);
    assert(ns > 0 && nu > 0);

    /* No coord in both lists. */
    for (size_t i = 0; i < ns; i++)
        assert(!coord_in_list(unload, nu, send[i].cx, send[i].cz));

    /* Every sent coord is in-range at the new center and was NOT previously
     * sent; every unloaded coord is out-of-range at the new center and WAS
     * previously sent. */
    for (size_t i = 0; i < ns; i++) {
        assert(chunk_stream_in_range(1, 0, send[i].cx, send[i].cz, rd));
        assert(coord_in_list(disc1, ndisc1, send[i].cx, send[i].cz));
        assert(!coord_in_list(disc0, ndisc0, send[i].cx, send[i].cz));
    }
    for (size_t i = 0; i < nu; i++) {
        assert(!chunk_stream_in_range(1, 0, unload[i].cx, unload[i].cz, rd));
        assert(coord_in_list(disc0, ndisc0, unload[i].cx, unload[i].cz));
    }
    printf("PASS: move_one_chunk (send %zu, unload %zu)\n", ns, nu);
}

/* An already-sent chunk far out of range is unloaded even with no center move
 * (e.g. a teleport): unload list captures stale chunks regardless of budget. */
static void test_unload_after_teleport(void) {
    int rd = 3;
    /* Sent a cluster around the old origin. */
    ChunkCoord sent[CAP];
    size_t nsent = ref_disc(0, 0, rd, sent);

    /* Player teleports far away to (100,100). */
    ChunkCoord send[CAP], unload[CAP];
    size_t ns = 0, nu = 0;
    chunk_stream_diff(100, 100, rd, sent, nsent,
                      send, CAP, &ns, 0, unload, CAP, &nu);

    /* Everything previously sent is now out of range -> all unloaded. */
    assert(nu == nsent);
    /* And the new disc (none of it sent) is fully queued to send. */
    ChunkCoord disc[CAP];
    size_t ndisc = ref_disc(100, 100, rd, disc);
    assert(ns == ndisc);
    printf("PASS: unload_after_teleport (unload %zu, send %zu)\n", nu, ns);
}

/* Output capacity clamping: tiny caps never overrun, counts are clamped. */
static void test_capacity_clamp(void) {
    int rd = 8;
    ChunkCoord send[3], unload[3];
    size_t ns = 0, nu = 0;
    chunk_stream_diff(0, 0, rd, NULL, 0,
                      send, 3, &ns, 0, unload, 3, &nu);
    assert(ns <= 3);
    assert(nu == 0);
    /* Still nearest-first within the clamped window. */
    assert(ns >= 1 && send[0].cx == 0 && send[0].cz == 0);
    printf("PASS: capacity_clamp (send clamped to %zu)\n", ns);
}

int main(void) {
    test_fresh_full_disc();
    test_nearest_first();
    test_budget_progressive();
    test_idempotent_stationary();
    test_move_one_chunk();
    test_unload_after_teleport();
    test_capacity_clamp();
    printf("ALL CHUNK_STREAM TESTS PASSED\n");
    return 0;
}
