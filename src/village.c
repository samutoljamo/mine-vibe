#include "village.h"
#include "chunk.h"
#include "worldgen.h"

/* ─────────────────────────────────────────────────────────────────────────
 * Deterministic spatial hash (same mixing style as worldgen's hash_pos /
 * ore.c's hash01). Pure and stable across runs/platforms.
 * ───────────────────────────────────────────────────────────────────────── */
static unsigned vhash(int x, int z, int salt)
{
    /* Mix in unsigned arithmetic to avoid signed-overflow UB (matters under
     * -O3, where the optimizer may otherwise miscompile the wraparound). */
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u +
                 (uint32_t)salt * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (unsigned)(h & 0x7FFFFFFFu);
}

/* Floor division (handles negatives correctly for cell index math). */
static int floor_div(int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* ── Pure placement ─────────────────────────────────────────────────────── */

VillageCell village_cell_at(int cgx, int cgz, int world_seed)
{
    VillageCell vc = { false, 0, 0, 0 };

    unsigned present_h = vhash(cgx, cgz, world_seed + VILLAGE_SALT_PRESENT);
    vc.present = (present_h % 100u) < (unsigned)VILLAGE_SPAWN_PCT;

    int cell_x0 = cgx * VILLAGE_CELL_BLOCKS;
    int cell_z0 = cgz * VILLAGE_CELL_BLOCKS;

    /* Jitter the center within the cell interior, keeping it at least
     * VILLAGE_MARGIN from every edge so adjacent cells can never overlap. */
    int span = VILLAGE_CELL_BLOCKS - 2 * VILLAGE_MARGIN;
    unsigned jx = vhash(cgx, cgz, world_seed + VILLAGE_SALT_JITTER);
    unsigned jz = vhash(cgz, cgx, world_seed + VILLAGE_SALT_JITTER + 1);
    vc.wx = cell_x0 + VILLAGE_MARGIN + (int)(jx % (unsigned)span);
    vc.wz = cell_z0 + VILLAGE_MARGIN + (int)(jz % (unsigned)span);

    vc.seed = (int)vhash(vc.wx, vc.wz, world_seed + VILLAGE_SALT_SEED);
    return vc;
}

bool village_is_suitable(int center_surface_h, BlockID center_surface_block,
                         int sampled_min_h, int sampled_max_h)
{
    if (center_surface_h < VILLAGE_SEA_LEVEL + 1) return false;
    if (center_surface_block != BLOCK_GRASS)      return false;
    if (sampled_max_h - sampled_min_h > VILLAGE_MAX_SLOPE) return false;
    return true;
}

/* ── Pure house layout ──────────────────────────────────────────────────── */

int village_house_count(int vseed)
{
    unsigned h = vhash(vseed, 0, 0xA1);
    int range = VILLAGE_MAX_HOUSES - VILLAGE_MIN_HOUSES + 1;
    return VILLAGE_MIN_HOUSES + (int)(h % (unsigned)range);
}

/* Footprint and roof/door for house i from its own raw rolls (independent of
 * placement, so we can compute reach before settling on a position). */
static void house_shape(int vseed, int i, int* w, int* d,
                        bool* peaked, int* door_face)
{
    int range = VILLAGE_HOUSE_MAX - VILLAGE_HOUSE_MIN + 1;
    *w = VILLAGE_HOUSE_MIN + (int)(vhash(vseed, i, 0xB2) % (unsigned)range);
    *d = VILLAGE_HOUSE_MIN + (int)(vhash(vseed, i, 0xC3) % (unsigned)range);
    *peaked    = (vhash(vseed, i, 0xD4) & 1u) != 0;
    *door_face = (int)(vhash(vseed, i, 0xE5) % 4u);
}

static int box_overlap(int ax, int az, int aw, int ad,
                       int bx, int bz, int bw, int bd)
{
    int ax0 = ax - aw / 2, ax1 = ax0 + aw;
    int az0 = az - ad / 2, az1 = az0 + ad;
    int bx0 = bx - bw / 2, bx1 = bx0 + bw;
    int bz0 = bz - bd / 2, bz1 = bz0 + bd;
    /* Require a 1-block gap between footprints. */
    return !(ax1 + 1 <= bx0 || bx1 + 1 <= ax0 ||
             az1 + 1 <= bz0 || bz1 + 1 <= az0);
}

VillageHouse village_house_at(int vseed, int center_wx, int center_wz, int i)
{
    VillageHouse house = { center_wx, center_wz, VILLAGE_HOUSE_MIN,
                           VILLAGE_HOUSE_MIN, false, 0 };
    house_shape(vseed, i, &house.w, &house.d, &house.peaked_roof,
                &house.door_face);

    /* Re-derive the layout deterministically: place houses 0..i in order,
     * rejecting overlaps and positions that exceed VILLAGE_MAX_RADIUS, so the
     * result for house i is a pure function of (vseed, center, i) regardless of
     * which chunk asks. The reroll sequence is seed-driven. */
    int placed_cx[VILLAGE_MAX_HOUSES];
    int placed_cz[VILLAGE_MAX_HOUSES];
    int placed_w[VILLAGE_MAX_HOUSES];
    int placed_d[VILLAGE_MAX_HOUSES];

    for (int k = 0; k <= i; k++) {
        int w, d; bool pk; int df;
        house_shape(vseed, k, &w, &d, &pk, &df);

        /* Max center offset so the whole footprint (plus a 1-block margin)
         * stays within VILLAGE_MAX_RADIUS of the village center. */
        int max_off_x = VILLAGE_MAX_RADIUS - w / 2 - 1;
        int max_off_z = VILLAGE_MAX_RADIUS - d / 2 - 1;
        if (max_off_x < 0) max_off_x = 0;
        if (max_off_z < 0) max_off_z = 0;
        int span_x = 2 * max_off_x + 1;
        int span_z = 2 * max_off_z + 1;

        int cx = center_wx, cz = center_wz;
        /* Up to N reroll attempts; deterministic via attempt-indexed salt. */
        for (int attempt = 0; attempt < 24; attempt++) {
            unsigned hx = vhash(vseed, k * 131 + attempt, 0x1F01);
            unsigned hz = vhash(vseed, k * 131 + attempt, 0x1F02);
            cx = center_wx - max_off_x + (int)(hx % (unsigned)span_x);
            cz = center_wz - max_off_z + (int)(hz % (unsigned)span_z);

            bool ok = true;
            for (int p = 0; p < k; p++) {
                if (box_overlap(cx, cz, w, d,
                                placed_cx[p], placed_cz[p],
                                placed_w[p], placed_d[p])) {
                    ok = false;
                    break;
                }
            }
            if (ok) break;
            /* On final attempt keep the last candidate even if overlapping is
             * impossible to avoid — in practice radius/house sizes leave ample
             * room for <=6 houses, so the test never hits this. */
        }

        placed_cx[k] = cx;
        placed_cz[k] = cz;
        placed_w[k]  = w;
        placed_d[k]  = d;
    }

    house.cx = placed_cx[i];
    house.cz = placed_cz[i];
    return house;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Per-chunk generation
 * ───────────────────────────────────────────────────────────────────────── */

/* Sample suitability inputs around a center column using worldgen_get_height
 * (pure of world seed) so every chunk reaches the same decision. */
static bool village_center_suitable(int wx, int wz, int seed, int* out_platform_y)
{
    int center_h = worldgen_get_height(wx, wz, seed);
    int min_h = center_h, max_h = center_h;
    static const int off[5][2] = {
        {0, 0}, {VILLAGE_MAX_RADIUS, 0}, {-VILLAGE_MAX_RADIUS, 0},
        {0, VILLAGE_MAX_RADIUS}, {0, -VILLAGE_MAX_RADIUS}
    };
    for (int i = 1; i < 5; i++) {
        int h = worldgen_get_height(wx + off[i][0], wz + off[i][1], seed);
        if (h < min_h) min_h = h;
        if (h > max_h) max_h = h;
    }
    /* Center surface block: grass iff above sea level (matches worldgen's
     * surface rule away from beaches). */
    BlockID surf = (center_h >= VILLAGE_SEA_LEVEL + 2) ? BLOCK_GRASS : BLOCK_SAND;
    *out_platform_y = center_h;
    return village_is_suitable(center_h, surf, min_h, max_h);
}

/* Place one block in world coords; out-of-range writes drop harmlessly. */
static void set_world(Chunk* c, int base_x, int base_z,
                      int wx, int wy, int wz, BlockID id)
{
    chunk_set_block(c, wx - base_x, wy, wz - base_z, id);
}

/* Flatten the footprint column at (wx,wz): clear air above the platform up to
 * clear_top, and fill solid ground below py down to py-3 so floors never float
 * over caves/dips. */
static void flatten_column(Chunk* c, int base_x, int base_z,
                           int wx, int wz, int py, int clear_top)
{
    for (int y = py + 1; y <= clear_top; y++)
        set_world(c, base_x, base_z, wx, y, wz, BLOCK_AIR);
    for (int y = py; y >= py - 3 && y > 0; y--)
        set_world(c, base_x, base_z, wx, y, wz, BLOCK_DIRT);
}

static void emit_house(Chunk* c, int base_x, int base_z,
                       VillageHouse h, int py)
{
    int x0 = h.cx - h.w / 2, x1 = x0 + h.w - 1;
    int z0 = h.cz - h.d / 2, z1 = z0 + h.d - 1;
    int wall_top = py + VILLAGE_WALL_H;
    int roof_y   = wall_top + 1;
    int clear_top = roof_y + h.w; /* enough headroom for a peaked roof */

    /* 1. Flatten footprint + floor. */
    for (int wx = x0; wx <= x1; wx++)
        for (int wz = z0; wz <= z1; wz++) {
            flatten_column(c, base_x, base_z, wx, wz, py, clear_top);
            set_world(c, base_x, base_z, wx, py, wz, BLOCK_PLANKS); /* floor */
        }

    /* Door gap center along the chosen wall. */
    int door_x = (h.cx), door_z = (h.cz);

    /* 2. Walls (perimeter), py+1 .. wall_top. Door/window gaps cut after. */
    for (int y = py + 1; y <= wall_top; y++) {
        for (int wx = x0; wx <= x1; wx++) {
            set_world(c, base_x, base_z, wx, y, z0, BLOCK_PLANKS);
            set_world(c, base_x, base_z, wx, y, z1, BLOCK_PLANKS);
        }
        for (int wz = z0; wz <= z1; wz++) {
            set_world(c, base_x, base_z, x0, y, wz, BLOCK_PLANKS);
            set_world(c, base_x, base_z, x1, y, wz, BLOCK_PLANKS);
        }
    }

    /* 3. Windows: GLASS at mid-height in the center of each non-door wall. */
    int win_y = py + 2;
    if (h.door_face != 0)
        set_world(c, base_x, base_z, x1, win_y, h.cz, BLOCK_GLASS);
    if (h.door_face != 1)
        set_world(c, base_x, base_z, x0, win_y, h.cz, BLOCK_GLASS);
    if (h.door_face != 2)
        set_world(c, base_x, base_z, h.cx, win_y, z1, BLOCK_GLASS);
    if (h.door_face != 3)
        set_world(c, base_x, base_z, h.cx, win_y, z0, BLOCK_GLASS);

    /* 4. Door gap (1 wide x 2 tall = air) on the door wall. */
    int door_wx = h.cx, door_wz = h.cz;
    switch (h.door_face) {
        case 0: door_wx = x1; break;
        case 1: door_wx = x0; break;
        case 2: door_wz = z1; break;
        default: door_wz = z0; break;
    }
    set_world(c, base_x, base_z, door_wx, py + 1, door_wz, BLOCK_AIR);
    set_world(c, base_x, base_z, door_wx, py + 2, door_wz, BLOCK_AIR);
    (void)door_x; (void)door_z;

    /* 5. Roof. */
    if (!h.peaked_roof) {
        /* Flat cobble slab over the footprint. */
        for (int wx = x0; wx <= x1; wx++)
            for (int wz = z0; wz <= z1; wz++)
                set_world(c, base_x, base_z, wx, roof_y, wz, BLOCK_COBBLE);
    } else {
        /* Peaked planks roof: gable along X, stepping up toward the center Z. */
        int steps = h.d / 2;
        for (int s = 0; s <= steps; s++) {
            int y = roof_y + s;
            int zlo = z0 + s;
            int zhi = z1 - s;
            for (int wx = x0; wx <= x1; wx++) {
                set_world(c, base_x, base_z, wx, y, zlo, BLOCK_PLANKS);
                set_world(c, base_x, base_z, wx, y, zhi, BLOCK_PLANKS);
            }
        }
    }
}

static void emit_well(Chunk* c, int base_x, int base_z, int cx, int cz, int py)
{
    /* 3x3 cobble rim at platform with a hollow 1x1 water core. */
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) {
            if (dx == 0 && dz == 0) continue;
            set_world(c, base_x, base_z, cx + dx, py, cz + dz, BLOCK_COBBLE);
            set_world(c, base_x, base_z, cx + dx, py + 1, cz + dz,
                      (dx == 0 || dz == 0) ? BLOCK_COBBLE : BLOCK_PLANKS);
        }
    /* Water core, 2 deep. */
    for (int y = py; y >= py - 2 && y > 0; y--)
        set_world(c, base_x, base_z, cx, y, cz, BLOCK_WATER);

    /* Four corner posts up to the canopy. */
    int canopy = py + 4;
    for (int y = py + 1; y < canopy; y++) {
        set_world(c, base_x, base_z, cx - 1, y, cz - 1, BLOCK_PLANKS);
        set_world(c, base_x, base_z, cx + 1, y, cz - 1, BLOCK_PLANKS);
        set_world(c, base_x, base_z, cx - 1, y, cz + 1, BLOCK_PLANKS);
        set_world(c, base_x, base_z, cx + 1, y, cz + 1, BLOCK_PLANKS);
    }
    /* Canopy slab. */
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++)
            set_world(c, base_x, base_z, cx + dx, canopy, cz + dz, BLOCK_COBBLE);
}

/* Lay a PATH block at the surface column (wx,wz) on the village platform. */
static void emit_path_block(Chunk* c, int base_x, int base_z,
                            int wx, int wz, int py)
{
    set_world(c, base_x, base_z, wx, py, wz, BLOCK_PATH);
    /* Clear one block of air above so the path is walkable. */
    set_world(c, base_x, base_z, wx, py + 1, wz, BLOCK_AIR);
}

/* Bresenham line in world space from (x0,z0) to (x1,z1), emitting path blocks.
 * Each block is independently clipped to the chunk, so split paths line up. */
static void emit_path(Chunk* c, int base_x, int base_z,
                      int x0, int z0, int x1, int z1, int py)
{
    int dx = x1 - x0, dz = z1 - z0;
    int adx = dx < 0 ? -dx : dx;
    int adz = dz < 0 ? -dz : dz;
    int sx = dx < 0 ? -1 : 1;
    int sz = dz < 0 ? -1 : 1;
    int err = adx - adz;
    int x = x0, z = z0;
    for (;;) {
        emit_path_block(c, base_x, base_z, x, z, py);
        if (x == x1 && z == z1) break;
        int e2 = 2 * err;
        if (e2 > -adz) { err -= adz; x += sx; }
        if (e2 <  adx) { err += adx; z += sz; }
    }
}

void village_generate(Chunk* chunk, int seed, int height_map[16][16])
{
    (void)height_map; /* placement is pure of world seed, not local terrain */

    int base_x = chunk->cx * CHUNK_X;
    int base_z = chunk->cz * CHUNK_Z;

    /* Chunk world AABB. */
    int chunk_x0 = base_x, chunk_x1 = base_x + CHUNK_X - 1;
    int chunk_z0 = base_z, chunk_z1 = base_z + CHUNK_Z - 1;

    /* This chunk's cell, plus 3x3 neighbourhood (a village center can reach up
     * to VILLAGE_MAX_RADIUS into a neighbouring cell). */
    int ccx = floor_div(base_x, VILLAGE_CELL_BLOCKS);
    int ccz = floor_div(base_z, VILLAGE_CELL_BLOCKS);

    for (int dcx = -1; dcx <= 1; dcx++)
        for (int dcz = -1; dcz <= 1; dcz++) {
            VillageCell vc = village_cell_at(ccx + dcx, ccz + dcz, seed);
            if (!vc.present) continue;

            /* Quick AABB reject: village reach vs this chunk. */
            int vx0 = vc.wx - VILLAGE_MAX_RADIUS, vx1 = vc.wx + VILLAGE_MAX_RADIUS;
            int vz0 = vc.wz - VILLAGE_MAX_RADIUS, vz1 = vc.wz + VILLAGE_MAX_RADIUS;
            if (vx1 < chunk_x0 || vx0 > chunk_x1 ||
                vz1 < chunk_z0 || vz0 > chunk_z1)
                continue;

            int py;
            if (!village_center_suitable(vc.wx, vc.wz, seed, &py))
                continue;

            /* Emit houses + a path from each house door to the well, then the
             * well last so the central landmark always wins where a house or
             * path roll happens to land near the center. */
            int n = village_house_count(vc.seed);
            for (int i = 0; i < n; i++) {
                VillageHouse h = village_house_at(vc.seed, vc.wx, vc.wz, i);

                /* Door world position (on the door wall). */
                int hx0 = h.cx - h.w / 2, hx1 = hx0 + h.w - 1;
                int hz0 = h.cz - h.d / 2, hz1 = hz0 + h.d - 1;
                int door_wx = h.cx, door_wz = h.cz;
                switch (h.door_face) {
                    case 0: door_wx = hx1; break;
                    case 1: door_wx = hx0; break;
                    case 2: door_wz = hz1; break;
                    default: door_wz = hz0; break;
                }
                /* Path first so the house overwrites any path under its walls. */
                emit_path(chunk, base_x, base_z,
                          door_wx, door_wz, vc.wx, vc.wz, py);
                emit_house(chunk, base_x, base_z, h, py);
            }

            /* Well at the village center, emitted last. */
            emit_well(chunk, base_x, base_z, vc.wx, vc.wz, py);
        }
}
