#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/workqueue.h"

/*
 * Tests the pure intrusive FIFO extracted from world.c's worker work queue.
 * The real WorkItem embeds `next` as its first member; we mimic that with a
 * test node whose first field is the link pointer.
 */
typedef struct Node {
    void* next;   /* MUST be first — workqueue links through it */
    int   id;
} Node;

static Node* mknode(int id) {
    Node* n = calloc(1, sizeof(Node));
    n->id = id;
    return n;
}

/* Empty queue: count 0, empty true, pop returns NULL. */
static void test_empty(void) {
    WorkQueue q;
    workqueue_init(&q);
    assert(workqueue_empty(&q));
    assert(workqueue_count(&q) == 0);
    assert(workqueue_pop(&q) == NULL);
    /* Popping an empty queue repeatedly stays well-defined. */
    assert(workqueue_pop(&q) == NULL);
    assert(workqueue_count(&q) == 0);
    printf("PASS: empty\n");
}

/* FIFO ordering: items dequeue in the exact order they were enqueued (this is
 * the load-order guarantee world.c relies on for center-outward chunk loads). */
static void test_fifo_order(void) {
    WorkQueue q;
    workqueue_init(&q);
    const int N = 100;
    for (int i = 0; i < N; i++) {
        workqueue_push(&q, mknode(i));
        assert(workqueue_count(&q) == (size_t)(i + 1));
    }
    assert(!workqueue_empty(&q));
    for (int i = 0; i < N; i++) {
        Node* n = (Node*)workqueue_pop(&q);
        assert(n != NULL);
        assert(n->id == i);                       /* same order as pushed */
        assert(workqueue_count(&q) == (size_t)(N - i - 1));
        free(n);
    }
    assert(workqueue_empty(&q));
    printf("PASS: fifo_order\n");
}

/* Single element: head==tail bookkeeping is correct on the 1-item boundary. */
static void test_single_element(void) {
    WorkQueue q;
    workqueue_init(&q);
    Node* a = mknode(42);
    workqueue_push(&q, a);
    assert(workqueue_count(&q) == 1);
    assert(!workqueue_empty(&q));
    Node* got = (Node*)workqueue_pop(&q);
    assert(got == a && got->id == 42);
    assert(workqueue_empty(&q));
    assert(workqueue_count(&q) == 0);
    /* After draining to empty, tail must be reset so the next push works. */
    Node* b = mknode(7);
    workqueue_push(&q, b);
    assert(workqueue_count(&q) == 1);
    Node* got2 = (Node*)workqueue_pop(&q);
    assert(got2 == b && got2->id == 7);
    free(a); free(b);
    printf("PASS: single_element\n");
}

/* Interleaved push/pop: the queue stays a correct FIFO under mixed traffic and
 * never loses or duplicates a node. This is the world.c worker pattern (one
 * thread pushing while another pops), exercised single-threaded for ordering. */
static void test_interleaved(void) {
    WorkQueue q;
    workqueue_init(&q);

    int next_push = 0;   /* next id to enqueue */
    int next_pop  = 0;   /* next id we expect to dequeue */
    int live      = 0;   /* items currently in queue */

    /* Deterministic pseudo-random schedule. */
    unsigned seed = 12345;
    for (int step = 0; step < 10000; step++) {
        seed = seed * 1103515245u + 12345u;
        int do_push = (seed >> 16) & 1;
        if (do_push || live == 0) {
            workqueue_push(&q, mknode(next_push++));
            live++;
        } else {
            Node* n = (Node*)workqueue_pop(&q);
            assert(n != NULL);
            assert(n->id == next_pop);   /* strict FIFO preserved */
            next_pop++;
            live--;
            free(n);
        }
        assert(workqueue_count(&q) == (size_t)live);
    }
    /* Drain the rest, still in order. */
    while (!workqueue_empty(&q)) {
        Node* n = (Node*)workqueue_pop(&q);
        assert(n->id == next_pop++);
        free(n);
    }
    assert(next_pop == next_push);   /* every pushed item was popped exactly once */
    printf("PASS: interleaved  (%d items)\n", next_push);
}

/* Drain-then-refill many cycles: there is no fixed-capacity ring, so there is
 * no wraparound bug class — the queue grows/shrinks via the intrusive links.
 * We assert it survives repeated full drains without corrupting head/tail. */
static void test_drain_refill_cycles(void) {
    WorkQueue q;
    workqueue_init(&q);
    for (int cycle = 0; cycle < 50; cycle++) {
        for (int i = 0; i < 64; i++)
            workqueue_push(&q, mknode(cycle * 1000 + i));
        assert(workqueue_count(&q) == 64);
        for (int i = 0; i < 64; i++) {
            Node* n = (Node*)workqueue_pop(&q);
            assert(n->id == cycle * 1000 + i);
            free(n);
        }
        assert(workqueue_empty(&q));
    }
    printf("PASS: drain_refill_cycles\n");
}

int main(void) {
    test_empty();
    test_fifo_order();
    test_single_element();
    test_interleaved();
    test_drain_refill_cycles();
    printf("ALL WORLDQUEUE TESTS PASSED\n");
    return 0;
}
