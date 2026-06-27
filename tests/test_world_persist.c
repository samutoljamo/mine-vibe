/* Pure unit tests for the server-side world-persistence block-delta overlay.
 *
 * Mirrors the structure of the other pure unit tests (e.g. test_mob.c):
 * no Vulkan, no GLFW — just the overlay data structure and its
 * serialize/deserialize roundtrip. File I/O is exercised via a tmp file.
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/world_persist.h"
#include "../src/block.h"

/* ------------------------------------------------------------------ */
/*  Overlay basics                                                     */
/* ------------------------------------------------------------------ */

static void test_record_and_get(void) {
    BlockOverlay ov;
    overlay_init(&ov, 12345);

    /* unknown cell -> not present */
    BlockID out;
    assert(overlay_get(&ov, 10, 64, -7, &out) == false);

    overlay_record(&ov, 10, 64, -7, BLOCK_STONE);
    assert(overlay_get(&ov, 10, 64, -7, &out) == true);
    assert(out == BLOCK_STONE);

    /* overwrite same cell -> latest wins, count stays 1 */
    overlay_record(&ov, 10, 64, -7, BLOCK_AIR);
    assert(overlay_get(&ov, 10, 64, -7, &out) == true);
    assert(out == BLOCK_AIR);
    assert(overlay_count(&ov) == 1);

    /* second distinct cell */
    overlay_record(&ov, -100, 5, 200, BLOCK_WOOD);
    assert(overlay_count(&ov) == 2);
    assert(overlay_get(&ov, -100, 5, 200, &out) == true && out == BLOCK_WOOD);

    overlay_free(&ov);
    printf("PASS: record_and_get\n");
}

static void test_grow_many_entries(void) {
    BlockOverlay ov;
    overlay_init(&ov, 7);
    const int N = 5000;   /* forces several rehashes past the initial cap */
    for (int i = 0; i < N; i++)
        overlay_record(&ov, i, i % 256, -i, (BlockID)((i % (BLOCK_COUNT - 1)) + 1));
    assert(overlay_count(&ov) == (size_t)N);
    for (int i = 0; i < N; i++) {
        BlockID out;
        assert(overlay_get(&ov, i, i % 256, -i, &out) == true);
        assert(out == (BlockID)((i % (BLOCK_COUNT - 1)) + 1));
    }
    overlay_free(&ov);
    printf("PASS: grow_many_entries\n");
}

/* ------------------------------------------------------------------ */
/*  Serialize / deserialize roundtrip (the TDD core)                   */
/* ------------------------------------------------------------------ */

static void test_serialize_roundtrip_empty(void) {
    BlockOverlay ov;
    overlay_init(&ov, 999);

    uint8_t* buf = NULL;
    size_t   len = 0;
    assert(overlay_serialize(&ov, &buf, &len) == true);
    assert(buf != NULL && len > 0);

    BlockOverlay ov2;
    assert(overlay_deserialize(&ov2, buf, len) == true);
    assert(overlay_seed(&ov2) == 999);
    assert(overlay_count(&ov2) == 0);

    free(buf);
    overlay_free(&ov);
    overlay_free(&ov2);
    printf("PASS: serialize_roundtrip_empty\n");
}

static void test_serialize_roundtrip_entries(void) {
    BlockOverlay ov;
    overlay_init(&ov, -424242);
    overlay_record(&ov, 0, 0, 0, BLOCK_BEDROCK);
    overlay_record(&ov, 1000000, 200, -1000000, BLOCK_SAND);
    overlay_record(&ov, -5, 250, 5, BLOCK_AIR);
    overlay_record(&ov, -5, 250, 5, BLOCK_LEAVES);   /* overwrite */

    uint8_t* buf = NULL;
    size_t   len = 0;
    assert(overlay_serialize(&ov, &buf, &len) == true);

    BlockOverlay ov2;
    assert(overlay_deserialize(&ov2, buf, len) == true);
    assert(overlay_seed(&ov2) == -424242);
    assert(overlay_count(&ov2) == 3);

    BlockID out;
    assert(overlay_get(&ov2, 0, 0, 0, &out) && out == BLOCK_BEDROCK);
    assert(overlay_get(&ov2, 1000000, 200, -1000000, &out) && out == BLOCK_SAND);
    assert(overlay_get(&ov2, -5, 250, 5, &out) && out == BLOCK_LEAVES);

    free(buf);
    overlay_free(&ov);
    overlay_free(&ov2);
    printf("PASS: serialize_roundtrip_entries\n");
}

static void test_deserialize_rejects_garbage(void) {
    BlockOverlay ov;
    /* too short */
    assert(overlay_deserialize(&ov, (const uint8_t*)"xx", 2) == false);
    /* bad magic, otherwise plausible length */
    uint8_t bad[32] = {0};
    bad[0] = 'X'; bad[1] = 'X'; bad[2] = 'X'; bad[3] = 'X';
    assert(overlay_deserialize(&ov, bad, sizeof(bad)) == false);
    printf("PASS: deserialize_rejects_garbage\n");
}

static void test_deserialize_truncated_entries(void) {
    /* Serialize a real overlay, then lop off the last byte: the declared
     * entry count no longer matches the buffer -> reject. */
    BlockOverlay ov;
    overlay_init(&ov, 1);
    overlay_record(&ov, 7, 8, 9, BLOCK_DIRT);
    uint8_t* buf = NULL; size_t len = 0;
    assert(overlay_serialize(&ov, &buf, &len) == true);

    BlockOverlay ov2;
    assert(overlay_deserialize(&ov2, buf, len - 1) == false);

    free(buf);
    overlay_free(&ov);
    printf("PASS: deserialize_truncated_entries\n");
}

/* ------------------------------------------------------------------ */
/*  File save / load                                                   */
/* ------------------------------------------------------------------ */

static void test_save_load_file(void) {
    const char* path = "test_world_persist_tmp.dat";
    remove(path);

    BlockOverlay ov;
    overlay_init(&ov, 13);
    overlay_record(&ov, 3, 4, 5, BLOCK_GRASS);
    overlay_record(&ov, -9, 130, 42, BLOCK_WATER);
    assert(overlay_save(&ov, path) == true);
    overlay_free(&ov);

    BlockOverlay ld;
    assert(overlay_load(&ld, path) == true);
    assert(overlay_seed(&ld) == 13);
    assert(overlay_count(&ld) == 2);
    BlockID out;
    assert(overlay_get(&ld, 3, 4, 5, &out) && out == BLOCK_GRASS);
    assert(overlay_get(&ld, -9, 130, 42, &out) && out == BLOCK_WATER);
    overlay_free(&ld);

    /* load of a nonexistent file returns false (caller falls back to fresh) */
    BlockOverlay missing;
    assert(overlay_load(&missing, "definitely_does_not_exist_98765.dat") == false);

    remove(path);
    printf("PASS: save_load_file\n");
}

int main(void) {
    test_record_and_get();
    test_grow_many_entries();
    test_serialize_roundtrip_empty();
    test_serialize_roundtrip_entries();
    test_deserialize_rejects_garbage();
    test_deserialize_truncated_entries();
    test_save_load_file();
    printf("All world_persist tests passed.\n");
    return 0;
}
