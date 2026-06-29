#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dungeon.h"            /* dungeon_roll_chest, DungeonRoom, container ItemStack */
#include "../src/container.h"          /* Container, CHEST_SLOTS */
#include "../src/loot.h"
#include "../src/chunk.h"              /* CHUNK_X / CHUNK_Z */
#include "../src/server_block_entity.h"/* sbe_dungeon_chests_in_chunk (pure) */

#define SEED 1337u

/* Mirror of sbe_fill_chest_loot's container-landing step (which lives in
 * server_block_entity.c behind the container.h/crafting.h typedef firewall).
 * We assert the SAME copy here so the test stays a pure container test. */
static void fill_container_from_roll(Container* c, uint32_t chest_seed) {
    container_init(c);
    ItemStack rolled[CHEST_SLOTS];
    int got = dungeon_roll_chest(chest_seed, rolled, CHEST_SLOTS);
    for (int i = 0; i < got; i++) c->slots[i] = rolled[i];
}

/* dungeon_roll_chest output must land into a Container's leading slots, one
 * stack per slot, with every stack non-empty and within container bounds. */
static void test_roll_lands_in_container(void) {
    for (uint32_t s = 1; s < 400; s++) {
        ItemStack rolled[CHEST_SLOTS];
        int got = dungeon_roll_chest(s, rolled, CHEST_SLOTS);
        assert(got >= DUNGEON_CHEST_MIN_STACKS && got <= DUNGEON_CHEST_MAX_STACKS);

        Container c;
        fill_container_from_roll(&c, s);

        /* Leading `got` slots match the roll exactly. */
        for (int i = 0; i < got; i++) {
            assert(c.slots[i].item == rolled[i].item);
            assert(c.slots[i].count == rolled[i].count);
            assert(c.slots[i].count >= 1 &&
                   c.slots[i].count <= CONTAINER_STACK_MAX);
        }
        /* Remaining slots stay empty. */
        for (int i = got; i < CHEST_SLOTS; i++)
            assert(c.slots[i].count == 0);
    }
    printf("PASS: roll lands in container\n");
}

/* The fill is deterministic in the chest seed: identical seeds -> identical
 * container contents (so a re-roll at chunk-load matches the first roll). */
static void test_fill_deterministic(void) {
    Container a, b;
    fill_container_from_roll(&a, 0xC0FFEEu);
    fill_container_from_roll(&b, 0xC0FFEEu);
    assert(memcmp(&a, &b, sizeof(Container)) == 0);

    Container d;
    fill_container_from_roll(&d, 0xC0FFEFu);
    assert(memcmp(&a, &d, sizeof(Container)) != 0);  /* different seed differs */
    printf("PASS: fill deterministic\n");
}

/* sbe_dungeon_chests_in_chunk must agree with the dungeon placement model:
 * for every present room, its chest is reported by exactly the chunk whose
 * extent contains the chest's world (x,z), with the room's derived loot seed. */
static void test_enumeration_matches_placement(void) {
    int found_any = 0;
    for (int cgx = -40; cgx < 40; cgx++)
        for (int cgz = -40; cgz < 40; cgz++) {
            DungeonRoom r = dungeon_cell_at(cgx, cgz, SEED);
            if (!r.present) continue;
            if (r.chest_y <= 0 || r.chest_y >= CHUNK_Y) continue;

            /* Chunk that owns the chest column (floor-div by 16). */
            int cx = (r.chest_x < 0) ? (r.chest_x - 15) / 16 : r.chest_x / 16;
            int cz = (r.chest_z < 0) ? (r.chest_z - 15) / 16 : r.chest_z / 16;

            int xs[SBE_DUNGEON_CHESTS_PER_CHUNK];
            int ys[SBE_DUNGEON_CHESTS_PER_CHUNK];
            int zs[SBE_DUNGEON_CHESTS_PER_CHUNK];
            uint32_t seeds[SBE_DUNGEON_CHESTS_PER_CHUNK];
            int n = sbe_dungeon_chests_in_chunk(cx, cz, SEED, xs, ys, zs, seeds,
                                                SBE_DUNGEON_CHESTS_PER_CHUNK);
            int hit = 0;
            for (int i = 0; i < n; i++) {
                if (xs[i] == r.chest_x && ys[i] == r.chest_y &&
                    zs[i] == r.chest_z) {
                    assert(seeds[i] == (uint32_t)r.seed);
                    hit = 1;
                    found_any = 1;
                }
                /* Every reported chest must actually fall inside the chunk. */
                int hcx = (xs[i] < 0) ? (xs[i] - 15) / 16 : xs[i] / 16;
                int hcz = (zs[i] < 0) ? (zs[i] - 15) / 16 : zs[i] / 16;
                assert(hcx == cx && hcz == cz);
            }
            assert(hit && "chest's owning chunk must report it");
        }
    assert(found_any && "expected at least one dungeon chest in the scan range");
    printf("PASS: enumeration matches placement\n");
}

/* A chunk with no dungeon chest reports none. (Far enough that we expect at
 * least some empty chunks; we just assert the count never exceeds the bound.) */
static void test_enumeration_bounded(void) {
    for (int cx = -8; cx < 8; cx++)
        for (int cz = -8; cz < 8; cz++) {
            int xs[SBE_DUNGEON_CHESTS_PER_CHUNK];
            int ys[SBE_DUNGEON_CHESTS_PER_CHUNK];
            int zs[SBE_DUNGEON_CHESTS_PER_CHUNK];
            uint32_t seeds[SBE_DUNGEON_CHESTS_PER_CHUNK];
            int n = sbe_dungeon_chests_in_chunk(cx, cz, SEED, xs, ys, zs, seeds,
                                                SBE_DUNGEON_CHESTS_PER_CHUNK);
            assert(n >= 0 && n <= SBE_DUNGEON_CHESTS_PER_CHUNK);
        }
    /* max_slots <= 0 yields nothing. */
    int dummy[1]; uint32_t ds[1];
    assert(sbe_dungeon_chests_in_chunk(0, 0, SEED, dummy, dummy, dummy, ds, 0) == 0);
    printf("PASS: enumeration bounded\n");
}

int main(void) {
    test_roll_lands_in_container();
    test_fill_deterministic();
    test_enumeration_matches_placement();
    test_enumeration_bounded();
    printf("ALL PASS\n");
    return 0;
}
