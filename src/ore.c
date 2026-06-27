#include "ore.h"

/* 3D spatial hash (same mixing style as worldgen's hash_pos), returning a
 * pseudo-random value in [0, 1). Pure and stable across runs/platforms for a
 * given (x, y, z, salt). */
static float hash01(int x, int y, int z, int salt)
{
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263 +
                            z * 1610612741 + salt * 1274126177);
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
}

/* Per-stone-block spawn probabilities. Rarity ordering is enforced by these
 * magnitudes: coal > iron > gold > diamond. */
#define P_COAL    0.012f
#define P_IRON    0.007f
#define P_GOLD    0.0022f
#define P_DIAMOND 0.0011f

/* Distinct salts per ore so each rolls independently. */
#define SALT_COAL    11
#define SALT_IRON    22
#define SALT_GOLD    33
#define SALT_DIAMOND 44

BlockID ore_select(int wx, int wy, int wz, int surface_h, int seed)
{
    /* Keep ores buried so they never replace the surface skin. */
    if (surface_h - wy < ORE_MIN_DEPTH)
        return BLOCK_STONE;

    /* Rarest first so a block eligible for several ores becomes the rare one. */
    if (wy <= ORE_DIAMOND_MAX_Y &&
        hash01(wx, wy, wz, seed + SALT_DIAMOND) < P_DIAMOND)
        return BLOCK_DIAMOND_ORE;

    if (wy <= ORE_GOLD_MAX_Y &&
        hash01(wx, wy, wz, seed + SALT_GOLD) < P_GOLD)
        return BLOCK_GOLD_ORE;

    if (wy <= ORE_IRON_MAX_Y &&
        hash01(wx, wy, wz, seed + SALT_IRON) < P_IRON)
        return BLOCK_IRON_ORE;

    if (hash01(wx, wy, wz, seed + SALT_COAL) < P_COAL)
        return BLOCK_COAL_ORE;

    return BLOCK_STONE;
}
