#include "lighting.h"
#include "chunk.h"
#include <stdlib.h>
#include <string.h>

/* Step the light value through a block of given absorption.
 * cost = max(1, absorb): air costs 1 per step, leaves cost 2, etc. */
static inline uint8_t step_light(uint8_t v, uint8_t absorb)
{
    uint8_t cost = absorb < 1 ? 1 : absorb;
    return v <= cost ? 0 : (uint8_t)(v - cost);
}

/* Sky column pass: for each (x,z), walk y from CHUNK_Y-1 down. Track
 * remaining sky light. When remaining > 0, write it; otherwise write 0. */
static void sky_column_pass(Chunk* c)
{
    for (int z = 0; z < CHUNK_Z; z++) {
        for (int x = 0; x < CHUNK_X; x++) {
            uint8_t sky = 15;
            for (int y = CHUNK_Y - 1; y >= 0; y--) {
                BlockID b = chunk_get_block(c, x, y, z);
                uint8_t absorb = block_get_def(b)->light_absorb;
                if (absorb > 0) {
                    sky = step_light(sky, absorb);
                }
                chunk_set_skylight(c, x, y, z, sky);
            }
        }
    }
}

void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb)
{
    (void)nb; /* horizontal BFS + cross-chunk added in later tasks */
    sky_column_pass(c);
}

void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb)
{
    (void)c; (void)nb;
    /* Implemented in Task 5. */
}

void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id)
{
    (void)c; (void)nb; (void)x; (void)y; (void)z;
    (void)old_id; (void)new_id;
    /* Implemented in Task 6. */
}
