#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/dungeon.h"
#include "../src/loot.h"

#define SEED 1337u

/* dungeon_cell_at must be a pure function of (cell, seed). */
static void test_deterministic(void) {
    for (int i = -200; i < 200; i++) {
        int cgx = i, cgz = i * 3 - 11;
        DungeonRoom a = dungeon_cell_at(cgx, cgz, SEED);
        DungeonRoom b = dungeon_cell_at(cgx, cgz, SEED);
        assert(a.present == b.present);
        assert(a.x0 == b.x0 && a.y0 == b.y0 && a.z0 == b.z0);
        assert(a.w == b.w && a.h == b.h && a.d == b.d);
        assert(a.chest_x == b.chest_x && a.chest_y == b.chest_y &&
               a.chest_z == b.chest_z);
        assert(a.seed == b.seed);
    }
    printf("PASS: deterministic\n");
}

/* Cell index is correct floor-division, including negatives, and matches the
 * cell that a room's origin actually lands in. */
static void test_cell_index(void) {
    assert(dungeon_cell_index(0) == 0);
    assert(dungeon_cell_index(DUNGEON_CELL_BLOCKS - 1) == 0);
    assert(dungeon_cell_index(DUNGEON_CELL_BLOCKS) == 1);
    assert(dungeon_cell_index(-1) == -1);
    assert(dungeon_cell_index(-DUNGEON_CELL_BLOCKS) == -1);
    assert(dungeon_cell_index(-DUNGEON_CELL_BLOCKS - 1) == -2);

    /* A room's full footprint must stay within its own cell so neighbours can
     * never overlap. */
    for (int cgx = -50; cgx < 50; cgx++)
        for (int cgz = -50; cgz < 50; cgz++) {
            DungeonRoom r = dungeon_cell_at(cgx, cgz, SEED);
            int x1 = r.x0 + r.w - 1;
            int z1 = r.z0 + r.d - 1;
            assert(dungeon_cell_index(r.x0) == cgx);
            assert(dungeon_cell_index(r.z0) == cgz);
            assert(dungeon_cell_index(x1) == cgx);
            assert(dungeon_cell_index(z1) == cgz);
        }
    printf("PASS: cell_index\n");
}

/* Cross-chunk continuity: a room is a function of its cell + seed, so the SAME
 * room is reported regardless of which world coordinate (i.e. which chunk) is
 * used to derive its cell. We probe every world voxel inside a room's footprint
 * and confirm dungeon_voxel_role is identical, which is what a neighbouring
 * chunk would compute. */
static void test_cross_chunk_identical(void) {
    DungeonRoom r = { 0 };
    /* Find a present room. */
    for (int cgx = 0; cgx < 1000 && !r.present; cgx++)
        for (int cgz = 0; cgz < 1000 && !r.present; cgz++) {
            DungeonRoom c = dungeon_cell_at(cgx, cgz, SEED);
            if (c.present) r = c;
        }
    assert(r.present);

    /* Re-derive the same room from any coordinate within its footprint, exactly
     * as an overlapping chunk would (cell index from a world coord). */
    for (int wx = r.x0; wx < r.x0 + r.w; wx++)
        for (int wz = r.z0; wz < r.z0 + r.d; wz++) {
            int cgx = dungeon_cell_index(wx);
            int cgz = dungeon_cell_index(wz);
            DungeonRoom r2 = dungeon_cell_at(cgx, cgz, SEED);
            assert(r2.present);
            assert(r2.x0 == r.x0 && r2.y0 == r.y0 && r2.z0 == r.z0);
            assert(r2.w == r.w && r2.h == r.h && r2.d == r.d);
            /* Same voxel role from either derivation. */
            for (int wy = r.y0; wy < r.y0 + r.h; wy++) {
                assert(dungeon_voxel_role(&r, wx, wy, wz) ==
                       dungeon_voxel_role(&r2, wx, wy, wz));
            }
        }
    printf("PASS: cross_chunk_identical\n");
}

/* Rooms are vertically banded and sensibly sized. */
static void test_band_and_size(void) {
    for (int cgx = -100; cgx < 100; cgx++)
        for (int cgz = -100; cgz < 100; cgz++) {
            DungeonRoom r = dungeon_cell_at(cgx, cgz, SEED);
            assert(r.y0 >= DUNGEON_MIN_Y);
            assert(r.y0 + r.h - 1 <= DUNGEON_MAX_Y + DUNGEON_ROOM_H);
            assert(r.y0 <= DUNGEON_MAX_Y);
            assert(r.w >= DUNGEON_MIN_W && r.w <= DUNGEON_MAX_W);
            assert(r.d >= DUNGEON_MIN_D && r.d <= DUNGEON_MAX_D);
            assert(r.h == DUNGEON_ROOM_H);
            /* y0 must keep the whole shell above bedrock mix band (y>=10). */
            assert(r.y0 >= 10);
        }
    printf("PASS: band_and_size\n");
}

/* Dungeons are rare but present. */
static void test_rarity(void) {
    long present = 0, total = 0;
    for (int cgx = -160; cgx < 160; cgx++)
        for (int cgz = -160; cgz < 160; cgz++) {
            total++;
            if (dungeon_cell_at(cgx, cgz, SEED).present) present++;
        }
    double pct = 100.0 * (double)present / (double)total;
    printf("  present=%ld total=%ld (%.1f%%)\n", present, total, pct);
    assert(present > 0);          /* findable */
    assert(pct < 35.0);           /* uncommon */
    printf("PASS: rarity\n");
}

/* Seed-sensitive: a different seed changes placement. */
static void test_seed_sensitive(void) {
    int diffs = 0;
    for (int cgx = 0; cgx < 100; cgx++)
        for (int cgz = 0; cgz < 100; cgz++) {
            DungeonRoom a = dungeon_cell_at(cgx, cgz, SEED);
            DungeonRoom b = dungeon_cell_at(cgx, cgz, SEED + 1u);
            if (a.present != b.present || a.x0 != b.x0 || a.y0 != b.y0)
                diffs++;
        }
    assert(diffs > 0);
    printf("PASS: seed_sensitive\n");
}

/* Voxel roles: a present room has a mossy-cobble shell enclosing a hollow
 * interior, with the chest sitting on the interior floor inside the shell. */
static void test_voxel_roles(void) {
    DungeonRoom r = { 0 };
    for (int cgx = 0; cgx < 1000 && !r.present; cgx++)
        for (int cgz = 0; cgz < 1000 && !r.present; cgz++) {
            DungeonRoom c = dungeon_cell_at(cgx, cgz, SEED);
            if (c.present) r = c;
        }
    assert(r.present);

    long shell = 0, interior = 0;
    int x1 = r.x0 + r.w - 1, y1 = r.y0 + r.h - 1, z1 = r.z0 + r.d - 1;
    for (int wx = r.x0; wx <= x1; wx++)
        for (int wy = r.y0; wy <= y1; wy++)
            for (int wz = r.z0; wz <= z1; wz++) {
                int role = dungeon_voxel_role(&r, wx, wy, wz);
                assert(role == 1 || role == 2);
                /* Faces are shell, strictly-inside is interior. */
                bool on_face = (wx == r.x0 || wx == x1 ||
                                wy == r.y0 || wy == y1 ||
                                wz == r.z0 || wz == z1);
                assert(role == (on_face ? 1 : 2));
                if (role == 1) shell++; else interior++;
            }
    assert(shell > 0);
    assert(interior > 0); /* genuinely hollow */

    /* Chest sits strictly inside the shell (interior voxel). */
    assert(dungeon_voxel_role(&r, r.chest_x, r.chest_y, r.chest_z) == 2);
    /* Chest rests on the floor: the voxel below it is the floor shell. */
    assert(dungeon_voxel_role(&r, r.chest_x, r.chest_y - 1, r.chest_z) == 1);

    /* Voxels well outside the box are role 0. */
    assert(dungeon_voxel_role(&r, r.x0 - 5, r.y0, r.z0) == 0);
    assert(dungeon_voxel_role(&r, r.x0, r.y0 - 1, r.z0) == 0);
    assert(dungeon_voxel_role(&r, r.x0, y1 + 99, r.z0) == 0);

    printf("  shell=%ld interior=%ld\n", shell, interior);
    printf("PASS: voxel_roles\n");
}

/* ------------------------------------------------------------------ */
/*  Chest loot roller (ado core)                                        */
/* ------------------------------------------------------------------ */

/* Look up the loot-table entry that produced `item`, or NULL if `item` is not
 * a member of LOOT_DUNGEON_CHEST at all. */
static const LootEntry *find_entry(ItemId item) {
    for (int i = 0; i < LOOT_DUNGEON_CHEST.count; i++)
        if (LOOT_DUNGEON_CHEST.entries[i].item == item)
            return &LOOT_DUNGEON_CHEST.entries[i];
    return NULL;
}

/* Same chest_seed must always yield byte-identical loot. */
static void test_loot_deterministic(void) {
    for (uint32_t s = 1; s < 500; s++) {
        ItemStack a[CHEST_SLOTS], b[CHEST_SLOTS];
        int na = dungeon_roll_chest(s, a, CHEST_SLOTS);
        int nb = dungeon_roll_chest(s, b, CHEST_SLOTS);
        assert(na == nb);
        for (int i = 0; i < na; i++) {
            assert(a[i].item == b[i].item);
            assert(a[i].count == b[i].count);
        }
    }
    printf("PASS: loot_deterministic\n");
}

/* Different chest seeds should (at least sometimes) produce different loot. */
static void test_loot_seed_sensitive(void) {
    int diffs = 0;
    for (uint32_t s = 1; s < 500; s++) {
        ItemStack a[CHEST_SLOTS], b[CHEST_SLOTS];
        int na = dungeon_roll_chest(s, a, CHEST_SLOTS);
        int nb = dungeon_roll_chest(s + 1u, b, CHEST_SLOTS);
        if (na != nb) { diffs++; continue; }
        for (int i = 0; i < na; i++)
            if (a[i].item != b[i].item || a[i].count != b[i].count) { diffs++; break; }
    }
    assert(diffs > 0);
    printf("PASS: loot_seed_sensitive\n");
}

/* Every produced stack is a valid loot-table item with a count inside that
 * entry's [min_count, max_count] band, and within container limits. Stack count
 * is bounded by [DUNGEON_CHEST_MIN_STACKS, DUNGEON_CHEST_MAX_STACKS]. Output
 * slots are distinct and never overflow max_slots. */
static void test_loot_valid_and_bounded(void) {
    for (uint32_t s = 1; s < 2000; s++) {
        ItemStack out[CHEST_SLOTS];
        int n = dungeon_roll_chest(s, out, CHEST_SLOTS);

        assert(n >= DUNGEON_CHEST_MIN_STACKS);
        assert(n <= DUNGEON_CHEST_MAX_STACKS);
        assert(n <= CHEST_SLOTS);

        for (int i = 0; i < n; i++) {
            const LootEntry *e = find_entry(out[i].item);
            assert(e != NULL); /* item is a real loot-table entry */
            assert(out[i].count >= e->min_count);
            assert(out[i].count <= e->max_count);
            assert(out[i].count >= 1);
            assert(out[i].count <= CONTAINER_STACK_MAX);
        }
    }
    printf("PASS: loot_valid_and_bounded\n");
}

/* The roller must honour a tight max_slots: never produce more than max_slots
 * stacks and never write past out[max_slots-1]. Probe with a guard sentinel. */
static void test_loot_respects_max_slots(void) {
    for (int cap = 1; cap <= DUNGEON_CHEST_MAX_STACKS; cap++) {
        for (uint32_t s = 1; s < 300; s++) {
            ItemStack out[CHEST_SLOTS + 1];
            /* Sentinel just past the cap to catch overflow writes. */
            out[cap].item = (ItemId)0xBEEF;
            out[cap].count = 0xAB;
            int n = dungeon_roll_chest(s, out, cap);
            assert(n <= cap);
            assert(out[cap].item == (ItemId)0xBEEF); /* untouched */
            assert(out[cap].count == 0xAB);
        }
    }
    printf("PASS: loot_respects_max_slots\n");
}

/* A zero/negative budget yields nothing and writes nothing. */
static void test_loot_zero_budget(void) {
    ItemStack out[CHEST_SLOTS];
    out[0].item = (ItemId)0xBEEF;
    out[0].count = 0xAB;
    assert(dungeon_roll_chest(1234u, out, 0) == 0);
    assert(out[0].item == (ItemId)0xBEEF);
    assert(dungeon_roll_chest(1234u, out, -5) == 0);
    printf("PASS: loot_zero_budget\n");
}

/* Loot for an actual generated room is reproducible from its derived seed,
 * confirming the deferred server step can re-roll the same contents. */
static void test_loot_from_room_seed(void) {
    DungeonRoom r = { 0 };
    for (int cgx = 0; cgx < 1000 && !r.present; cgx++)
        for (int cgz = 0; cgz < 1000 && !r.present; cgz++) {
            DungeonRoom c = dungeon_cell_at(cgx, cgz, SEED);
            if (c.present) r = c;
        }
    assert(r.present);

    ItemStack a[CHEST_SLOTS], b[CHEST_SLOTS];
    int na = dungeon_roll_chest((uint32_t)r.seed, a, CHEST_SLOTS);
    int nb = dungeon_roll_chest((uint32_t)r.seed, b, CHEST_SLOTS);
    assert(na == nb && na > 0);
    for (int i = 0; i < na; i++) {
        assert(a[i].item == b[i].item && a[i].count == b[i].count);
    }
    printf("PASS: loot_from_room_seed\n");
}

int main(void) {
    test_deterministic();
    test_cell_index();
    test_cross_chunk_identical();
    test_band_and_size();
    test_rarity();
    test_seed_sensitive();
    test_voxel_roles();
    test_loot_deterministic();
    test_loot_seed_sensitive();
    test_loot_valid_and_bounded();
    test_loot_respects_max_slots();
    test_loot_zero_budget();
    test_loot_from_room_seed();
    printf("ALL DUNGEON TESTS PASSED\n");
    return 0;
}
