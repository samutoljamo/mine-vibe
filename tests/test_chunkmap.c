#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/chunk_map.h"
#include "../src/chunk.h"

/* The chunk map is plain memory (open-addressing linear-probe hash table keyed
 * on (cx,cz)); no Vulkan/GLFW. We exercise it with real Chunk objects created
 * by chunk_create, since chunk_map_put reads chunk->cx / chunk->cz. */

static Chunk* mk(int32_t cx, int32_t cz) { return chunk_create(cx, cz); }

/* Insert then look up returns the same pointer; a missing key returns NULL. */
static void test_put_get_and_miss(void)
{
    ChunkMap m;
    chunk_map_init(&m, 16);

    Chunk* a = mk(0, 0);
    Chunk* b = mk(1, 0);
    Chunk* c = mk(0, 1);
    Chunk* d = mk(-3, 7);
    chunk_map_put(&m, a);
    chunk_map_put(&m, b);
    chunk_map_put(&m, c);
    chunk_map_put(&m, d);

    assert(chunk_map_get(&m, 0, 0)  == a);
    assert(chunk_map_get(&m, 1, 0)  == b);
    assert(chunk_map_get(&m, 0, 1)  == c);
    assert(chunk_map_get(&m, -3, 7) == d);

    /* (cx,cz) is ordered: (1,0) and (0,1) are distinct keys. */
    assert(chunk_map_get(&m, 0, 0) != chunk_map_get(&m, 1, 0));

    /* Absent keys miss. */
    assert(chunk_map_get(&m, 99, 99) == NULL);
    assert(chunk_map_get(&m, 0, 2)   == NULL);
    assert(chunk_map_get(&m, 3, -7)  == NULL); /* sign of key matters */
    assert(m.count == 4);

    chunk_map_free(&m);
    chunk_destroy(a); chunk_destroy(b); chunk_destroy(c); chunk_destroy(d);
    printf("PASS: put_get_and_miss\n");
}

/* Putting the same (cx,cz) twice overwrites in place: count stays, get returns
 * the latest pointer. */
static void test_overwrite_same_key(void)
{
    ChunkMap m;
    chunk_map_init(&m, 16);

    Chunk* first  = mk(5, 5);
    Chunk* second = mk(5, 5);
    chunk_map_put(&m, first);
    assert(m.count == 1);
    assert(chunk_map_get(&m, 5, 5) == first);

    chunk_map_put(&m, second);
    assert(m.count == 1);                       /* not a second slot */
    assert(chunk_map_get(&m, 5, 5) == second);  /* latest wins */

    chunk_map_free(&m);
    chunk_destroy(first); chunk_destroy(second);
    printf("PASS: overwrite_same_key\n");
}

/* Remove returns the stored pointer and makes the key miss; an absent key
 * removes nothing and returns NULL. */
static void test_remove(void)
{
    ChunkMap m;
    chunk_map_init(&m, 16);

    Chunk* a = mk(2, 3);
    Chunk* b = mk(2, 4);
    chunk_map_put(&m, a);
    chunk_map_put(&m, b);
    assert(m.count == 2);

    assert(chunk_map_remove(&m, 9, 9) == NULL); /* absent: no-op */
    assert(m.count == 2);

    Chunk* got = chunk_map_remove(&m, 2, 3);
    assert(got == a);
    assert(m.count == 1);
    assert(chunk_map_get(&m, 2, 3) == NULL);    /* gone */
    assert(chunk_map_get(&m, 2, 4) == b);       /* sibling survives */

    chunk_map_free(&m);
    chunk_destroy(a); chunk_destroy(b);
    printf("PASS: remove\n");
}

/* Removing a key from the middle of a probe chain must not break lookup of the
 * other keys in that chain (linear-probe back-shift / re-insert correctness).
 * We force a colliding chain by inserting a run of keys into a tiny map and
 * then deleting interior members. */
static void test_remove_preserves_probe_chain(void)
{
    ChunkMap m;
    chunk_map_init(&m, 16);

    /* A column of chunks (cx fixed); whatever their hash slots, repeated
     * removals + lookups must stay consistent. */
    enum { N = 40 };
    Chunk* cs[N];
    for (int i = 0; i < N; i++) {
        cs[i] = mk(7, i);
        chunk_map_put(&m, cs[i]);
    }
    for (int i = 0; i < N; i++)
        assert(chunk_map_get(&m, 7, i) == cs[i]);

    /* Remove every other one, then verify the survivors are still findable
     * and the removed ones miss. */
    for (int i = 0; i < N; i += 2) {
        Chunk* got = chunk_map_remove(&m, 7, i);
        assert(got == cs[i]);
    }
    for (int i = 0; i < N; i++) {
        if (i % 2 == 0) assert(chunk_map_get(&m, 7, i) == NULL);
        else            assert(chunk_map_get(&m, 7, i) == cs[i]);
    }

    chunk_map_free(&m);
    for (int i = 0; i < N; i++) chunk_destroy(cs[i]);
    printf("PASS: remove_preserves_probe_chain\n");
}

/* Inserting past the 70% load factor must trigger an automatic rehash (grow):
 * capacity increases yet every key remains findable with the right pointer. */
static void test_grow_rehash_preserves_all(void)
{
    ChunkMap m;
    chunk_map_init(&m, 16);
    uint32_t start_cap = m.capacity;

    enum { N = 500 };
    Chunk* cs[N];
    for (int i = 0; i < N; i++) {
        cs[i] = mk(i, i * 3 - 1);
        chunk_map_put(&m, cs[i]);
    }

    assert(m.count == N);
    assert(m.capacity > start_cap);             /* it grew */

    /* All survive the rehash with correct pointers. */
    for (int i = 0; i < N; i++)
        assert(chunk_map_get(&m, i, i * 3 - 1) == cs[i]);

    /* Capacity stays a power of two (mask-based probing relies on this). */
    assert((m.capacity & (m.capacity - 1)) == 0);

    chunk_map_free(&m);
    for (int i = 0; i < N; i++) chunk_destroy(cs[i]);
    printf("PASS: grow_rehash_preserves_all\n");
}

/* Negative coordinates pack into the 64-bit key without aliasing positives. */
static void test_negative_coords_distinct(void)
{
    ChunkMap m;
    chunk_map_init(&m, 64);

    Chunk* pp = mk( 4,  9);
    Chunk* nn = mk(-4, -9);
    Chunk* pn = mk( 4, -9);
    Chunk* np = mk(-4,  9);
    chunk_map_put(&m, pp);
    chunk_map_put(&m, nn);
    chunk_map_put(&m, pn);
    chunk_map_put(&m, np);

    assert(chunk_map_get(&m,  4,  9) == pp);
    assert(chunk_map_get(&m, -4, -9) == nn);
    assert(chunk_map_get(&m,  4, -9) == pn);
    assert(chunk_map_get(&m, -4,  9) == np);
    assert(m.count == 4);

    chunk_map_free(&m);
    chunk_destroy(pp); chunk_destroy(nn); chunk_destroy(pn); chunk_destroy(np);
    printf("PASS: negative_coords_distinct\n");
}

/* The iterator visits exactly the occupied entries, each once. */
static void test_iter_visits_all_once(void)
{
    ChunkMap m;
    chunk_map_init(&m, 16);

    enum { N = 30 };
    Chunk* cs[N];
    for (int i = 0; i < N; i++) {
        cs[i] = mk(i * 2, i);
        chunk_map_put(&m, cs[i]);
    }

    int seen[N] = {0};
    uint32_t idx = 0;
    int total = 0;
    Chunk* it;
    while ((it = chunk_map_iter(&m, &idx)) != NULL) {
        total++;
        int found = -1;
        for (int i = 0; i < N; i++) if (cs[i] == it) { found = i; break; }
        assert(found >= 0);        /* iterator returned a chunk we inserted */
        assert(seen[found] == 0);  /* and never twice */
        seen[found] = 1;
    }
    assert(total == N);
    for (int i = 0; i < N; i++) assert(seen[i] == 1);

    chunk_map_free(&m);
    for (int i = 0; i < N; i++) chunk_destroy(cs[i]);
    printf("PASS: iter_visits_all_once\n");
}

int main(void)
{
    test_put_get_and_miss();
    test_overwrite_same_key();
    test_remove();
    test_remove_preserves_probe_chain();
    test_grow_rehash_preserves_all();
    test_negative_coords_distinct();
    test_iter_visits_all_once();
    printf("ALL CHUNKMAP TESTS PASSED\n");
    return 0;
}
