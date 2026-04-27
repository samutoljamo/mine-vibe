#include "lighting.h"
#include "chunk.h"

/* Step the light value through a block of given absorption.
 * cost = max(1, absorb): air costs 1 per step, leaves cost 2, etc. */
static inline uint8_t step_light(uint8_t v, uint8_t absorb)
{
    uint8_t cost = absorb < 1 ? 1 : absorb;
    return v <= cost ? 0 : (uint8_t)(v - cost);
}

/* BFS queue for in-chunk propagation. Cap is a power of two so we can
 * mask instead of mod; sized large enough for worst-case full-chunk BFS. */
typedef struct LightCell {
    int16_t x, y, z;
    uint8_t light;
} LightCell;

#define LIGHT_QUEUE_CAP 65536

typedef struct LightQueue {
    LightCell cells[LIGHT_QUEUE_CAP];
    uint32_t  head;
    uint32_t  tail;
} LightQueue;

static void lq_init(LightQueue* q) { q->head = 0; q->tail = 0; }
static int  lq_empty(const LightQueue* q) { return q->head == q->tail; }
static void lq_push(LightQueue* q, int x, int y, int z, uint8_t light)
{
    LightCell* c = &q->cells[q->tail++ & (LIGHT_QUEUE_CAP - 1)];
    c->x = (int16_t)x; c->y = (int16_t)y; c->z = (int16_t)z; c->light = light;
}
static LightCell lq_pop(LightQueue* q)
{
    return q->cells[q->head++ & (LIGHT_QUEUE_CAP - 1)];
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

/* Horizontal/vertical BFS within a single chunk. Seeds: every cell whose
 * current sky-light value is greater than 0; for each, propagate to 6
 * neighbors with neighbor_sky = max(neighbor_sky, here_sky - cost).
 *
 * Cross-chunk propagation is deferred to Task 5; here we skip neighbors
 * that fall outside chunk bounds. */
static void horizontal_bfs(Chunk* c, const LightingNeighbors* nb)
{
    (void)nb; /* Task 5 adds cross-chunk seeding */

    LightQueue q;
    lq_init(&q);

    /* Seed: every cell whose sky > 0 enters the queue. The BFS will
     * relax 6-neighbors. Many seeds is fine — duplicates are filtered by
     * the "only push when neighbor_sky strictly increases" check. */
    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                uint8_t s = chunk_get_skylight(c, x, y, z);
                if (s > 0) lq_push(&q, x, y, z, s);
            }
        }
    }

    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(&q)) {
        LightCell cell = lq_pop(&q);
        for (int f = 0; f < 6; f++) {
            int nx = cell.x + dx[f];
            int ny = cell.y + dy[f];
            int nz = cell.z + dz[f];
            if (ny < 0 || ny >= CHUNK_Y) continue;
            if (nx < 0 || nx >= CHUNK_X) continue; /* cross-chunk: Task 5 */
            if (nz < 0 || nz >= CHUNK_Z) continue;

            BlockID nb_block  = chunk_get_block(c, nx, ny, nz);
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            uint8_t cur = chunk_get_skylight(c, nx, ny, nz);
            if (new_sky > cur) {
                chunk_set_skylight(c, nx, ny, nz, new_sky);
                lq_push(&q, nx, ny, nz, new_sky);
            }
        }
    }
}

void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb)
{
    sky_column_pass(c);
    horizontal_bfs(c, nb);
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
