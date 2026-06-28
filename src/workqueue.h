#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Pure intrusive singly-linked FIFO used by world.c for its chunk work queue.
 *
 * "Intrusive" = the caller's node embeds a `void* next` link; the queue stores
 * no nodes of its own and does no allocation. This is the exact list discipline
 * world.c's worker queue uses (append at tail, pop from head), lifted out so the
 * ordering / capacity / wraparound-free invariants are unit-testable without the
 * surrounding thread + mutex + Vulkan machinery.
 *
 * The queue itself is NOT thread-safe; callers serialize access with their own
 * mutex (world.c holds work_mutex around every call), exactly as before.
 *
 * Node layout contract: each enqueued node must begin with a `void* next`
 * field as its FIRST member, so the queue can read/write the link via a
 * `void**` cast of the node pointer. (world.c's WorkItem is adjusted to put
 * `next` first.)
 */

typedef struct WorkQueue {
    void*  head;
    void*  tail;
    size_t count;
} WorkQueue;

static inline void workqueue_init(WorkQueue* q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

static inline bool workqueue_empty(const WorkQueue* q) {
    return q->head == NULL;
}

static inline size_t workqueue_count(const WorkQueue* q) {
    return q->count;
}

/* Append node at the tail (FIFO order). node->next is cleared. */
static inline void workqueue_push(WorkQueue* q, void* node) {
    *(void**)node = NULL;            /* node->next = NULL */
    if (q->tail == NULL) {
        q->head = node;
        q->tail = node;
    } else {
        *(void**)q->tail = node;     /* tail->next = node */
        q->tail = node;
    }
    q->count++;
}

/* Remove and return the head node, or NULL if empty. */
static inline void* workqueue_pop(WorkQueue* q) {
    void* node = q->head;
    if (!node) return NULL;
    q->head = *(void**)node;         /* head = node->next */
    if (q->head == NULL)
        q->tail = NULL;
    q->count--;
    return node;
}

#endif
