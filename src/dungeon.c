#include "dungeon.h"

/* Deterministic spatial hash, same mixing style as worldgen's hash_pos /
 * village.c's vhash. Pure and stable across runs/platforms; all arithmetic in
 * uint32 to avoid signed-overflow UB under optimization. */
static unsigned dhash(int x, int z, unsigned salt)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u +
                 salt * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (unsigned)(h & 0x7FFFFFFFu);
}

/* Floor division (correct for negative coordinates). */
static int floor_div(int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

int dungeon_cell_index(int world_coord)
{
    return floor_div(world_coord, DUNGEON_CELL_BLOCKS);
}

DungeonRoom dungeon_cell_at(int cgx, int cgz, uint32_t seed)
{
    DungeonRoom r;
    r.present = false;

    /* Room dimensions: width/depth in [MIN..MAX], height fixed. Rolled before
     * jitter so the footprint margin can account for the actual size. */
    unsigned hw = dhash(cgx, cgz, seed + DUNGEON_SALT_DIMS);
    unsigned hd = dhash(cgz, cgx, seed + DUNGEON_SALT_DIMS + 1u);
    r.w = DUNGEON_MIN_W + (int)(hw % (unsigned)(DUNGEON_MAX_W - DUNGEON_MIN_W + 1));
    r.d = DUNGEON_MIN_D + (int)(hd % (unsigned)(DUNGEON_MAX_D - DUNGEON_MIN_D + 1));
    r.h = DUNGEON_ROOM_H;

    /* Presence roll. */
    unsigned present_h = dhash(cgx, cgz, seed + DUNGEON_SALT_PRESENT);
    r.present = (present_h % 100u) < (unsigned)DUNGEON_SPAWN_PCT;

    int cell_x0 = cgx * DUNGEON_CELL_BLOCKS;
    int cell_z0 = cgz * DUNGEON_CELL_BLOCKS;

    /* Jitter the origin inside the cell so the whole footprint stays within the
     * cell (1-block guard on each side). This guarantees rooms from adjacent
     * cells never overlap, so a chunk need only consult its 3x3 cell neighbours. */
    int span_x = DUNGEON_CELL_BLOCKS - r.w - 1;
    int span_z = DUNGEON_CELL_BLOCKS - r.d - 1;
    if (span_x < 1) span_x = 1;
    if (span_z < 1) span_z = 1;
    unsigned jx = dhash(cgx, cgz, seed + DUNGEON_SALT_JITTER);
    unsigned jz = dhash(cgz, cgx, seed + DUNGEON_SALT_JITTER + 1u);
    r.x0 = cell_x0 + 1 + (int)(jx % (unsigned)span_x);
    r.z0 = cell_z0 + 1 + (int)(jz % (unsigned)span_z);

    /* Vertical placement within the buried band. */
    int span_y = DUNGEON_MAX_Y - DUNGEON_MIN_Y + 1;
    unsigned jy = dhash(cgx + 7919, cgz - 104729, seed + DUNGEON_SALT_JITTER + 2u);
    r.y0 = DUNGEON_MIN_Y + (int)(jy % (unsigned)span_y);

    /* Chest on the interior floor against the -x/-z corner wall. Interior floor
     * is one block above the floor shell (y0 + 1); the corner interior voxel is
     * (x0+1, z0+1). */
    r.chest_x = r.x0 + 1;
    r.chest_y = r.y0 + 1;
    r.chest_z = r.z0 + 1;

    r.seed = (int)dhash(r.x0, r.z0, seed + DUNGEON_SALT_SEED);
    return r;
}

int dungeon_voxel_role(const DungeonRoom *room, int wx, int wy, int wz)
{
    int x1 = room->x0 + room->w - 1;
    int y1 = room->y0 + room->h - 1;
    int z1 = room->z0 + room->d - 1;

    if (wx < room->x0 || wx > x1 ||
        wy < room->y0 || wy > y1 ||
        wz < room->z0 || wz > z1)
        return 0; /* outside bounding box */

    /* On any of the six faces => wall/floor/ceiling shell. */
    if (wx == room->x0 || wx == x1 ||
        wy == room->y0 || wy == y1 ||
        wz == room->z0 || wz == z1)
        return 1; /* shell */

    return 2; /* hollow interior */
}
