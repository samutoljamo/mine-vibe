#include "lighting.h"
#include "chunk.h"
#include <stdlib.h>  /* realloc/free for pending delta growth */
#include <stdio.h>   /* fprintf for OOM diagnostics */

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

_Static_assert((LIGHT_QUEUE_CAP & (LIGHT_QUEUE_CAP - 1)) == 0,
               "LIGHT_QUEUE_CAP must be a power of two for masking");
_Static_assert(LIGHT_QUEUE_CAP >= CHUNK_X * CHUNK_Y * CHUNK_Z,
               "LIGHT_QUEUE_CAP must hold the worst-case full-chunk seeding");

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

/* Append a boundary delta to the neighbor's pending queue. Grows on demand. */
static void push_boundary_delta(Chunk* nb_chunk, uint8_t face,
                                uint8_t axis_coord, uint16_t y, uint8_t new_light)
{
    if (nb_chunk->pending_delta_count >= nb_chunk->pending_delta_cap) {
        uint16_t new_cap = nb_chunk->pending_delta_cap == 0
            ? 64 : (uint16_t)(nb_chunk->pending_delta_cap * 2);
        BoundaryDelta* tmp = realloc(nb_chunk->pending_deltas,
                                     new_cap * sizeof(BoundaryDelta));
        if (!tmp) return; /* drop delta on OOM — eventual consistency loss */
        nb_chunk->pending_deltas    = tmp;
        nb_chunk->pending_delta_cap = new_cap;
    }
    BoundaryDelta* d = &nb_chunk->pending_deltas[nb_chunk->pending_delta_count++];
    d->face       = face;
    d->axis_coord = axis_coord;
    d->y          = y;
    d->new_light  = new_light;
    nb_chunk->needs_relight = true;
}

static void horizontal_bfs(Chunk* c, const LightingNeighbors* nb)
{
    LightQueue* q = malloc(sizeof(LightQueue));
    if (!q) {
        fprintf(stderr, "lighting: out of memory for BFS queue\n");
        return;
    }
    lq_init(q);

    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                uint8_t s = chunk_get_skylight(c, x, y, z);
                if (s > 0) lq_push(q, x, y, z, s);
            }
        }
    }

    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(q)) {
        LightCell cell = lq_pop(q);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;

            /* Cross-chunk: write a delta to the neighbor's pending queue. */
            Chunk* target = c;
            int    tx = nx_, tz = nz_;
            uint8_t out_face = 0xFF;

            if (nx_ < 0) {
                target = nb ? nb->neg_x : NULL;
                tx = CHUNK_X - 1;
                out_face = 1; /* into neighbor's east face */
            } else if (nx_ >= CHUNK_X) {
                target = nb ? nb->pos_x : NULL;
                tx = 0;
                out_face = 0;
            } else if (nz_ < 0) {
                target = nb ? nb->neg_z : NULL;
                tz = CHUNK_Z - 1;
                out_face = 3;
            } else if (nz_ >= CHUNK_Z) {
                target = nb ? nb->pos_z : NULL;
                tz = 0;
                out_face = 2;
            }

            BlockID nb_block;
            if (target == c) {
                nb_block = chunk_get_block(c, tx, ny_, tz);
            } else if (target) {
                nb_block = chunk_get_block(target, tx, ny_, tz);
            } else {
                continue; /* edge of world: nothing to propagate into */
            }
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            if (target == c) {
                uint8_t cur = chunk_get_skylight(c, tx, ny_, tz);
                if (new_sky > cur) {
                    chunk_set_skylight(c, tx, ny_, tz, new_sky);
                    lq_push(q, tx, ny_, tz, new_sky);
                }
            } else {
                /* Record on neighbor's pending queue. axis_coord is the
                 * coordinate along the boundary (z for ±X, x for ±Z). */
                uint8_t axis = (out_face == 0 || out_face == 1)
                             ? (uint8_t)tz : (uint8_t)tx;
                uint8_t cur  = chunk_get_skylight(target, tx, ny_, tz);
                if (new_sky > cur) {
                    push_boundary_delta(target, out_face, axis,
                                        (uint16_t)ny_, new_sky);
                }
            }
        }
    }
    free(q);
}

void lighting_initial_pass(Chunk* c, const LightingNeighbors* nb)
{
    sky_column_pass(c);
    horizontal_bfs(c, nb);
}

void lighting_consume_pending(Chunk* c, const LightingNeighbors* nb)
{
    if (c->pending_delta_count == 0) {
        c->needs_relight = false;
        return;
    }

    LightQueue* q = malloc(sizeof(LightQueue));
    if (!q) {
        fprintf(stderr, "lighting: out of memory for BFS queue\n");
        return;
    }
    lq_init(q);

    /* Apply each pending delta directly into c->lights, then seed a queue
     * with the changed cells so addition-BFS spreads from them. */
    for (uint16_t i = 0; i < c->pending_delta_count; i++) {
        BoundaryDelta d = c->pending_deltas[i];
        int x, z;
        switch (d.face) {
        case 0: x = 0;            z = d.axis_coord; break;  /* +X face: write at x=0 */
        case 1: x = CHUNK_X - 1;  z = d.axis_coord; break;
        case 2: x = d.axis_coord; z = 0;            break;
        case 3: x = d.axis_coord; z = CHUNK_Z - 1;  break;
        default: continue;
        }
        uint8_t cur = chunk_get_skylight(c, x, d.y, z);
        uint8_t v   = d.new_light & 0x0F;
        if (v > cur) {
            chunk_set_skylight(c, x, d.y, z, v);
            lq_push(q, x, d.y, z, v);
        }
    }

    c->pending_delta_count = 0;
    c->needs_relight       = false;

    /* Re-run the same propagation logic. */
    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(q)) {
        LightCell cell = lq_pop(q);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;

            Chunk* target = c;
            int tx = nx_, tz = nz_;
            uint8_t out_face = 0xFF;
            if (nx_ < 0)             { target = nb ? nb->neg_x : NULL; tx = CHUNK_X - 1; out_face = 1; }
            else if (nx_ >= CHUNK_X) { target = nb ? nb->pos_x : NULL; tx = 0;            out_face = 0; }
            else if (nz_ < 0)        { target = nb ? nb->neg_z : NULL; tz = CHUNK_Z - 1; out_face = 3; }
            else if (nz_ >= CHUNK_Z) { target = nb ? nb->pos_z : NULL; tz = 0;            out_face = 2; }

            if (!target) continue;

            BlockID nb_block = chunk_get_block(target, tx, ny_, tz);
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            if (target == c) {
                uint8_t cur = chunk_get_skylight(c, tx, ny_, tz);
                if (new_sky > cur) {
                    chunk_set_skylight(c, tx, ny_, tz, new_sky);
                    lq_push(q, tx, ny_, tz, new_sky);
                }
            } else {
                uint8_t axis = (out_face == 0 || out_face == 1)
                             ? (uint8_t)tz : (uint8_t)tx;
                uint8_t cur = chunk_get_skylight(target, tx, ny_, tz);
                if (new_sky > cur) {
                    push_boundary_delta(target, out_face, axis,
                                        (uint16_t)ny_, new_sky);
                }
            }
        }
    }
    free(q);
}

/* Removal-BFS: visit cells reachable from (x,y,z) whose only support was a
 * value <= the cell's old contribution. Zero them and queue brighter
 * surviving neighbors as re-propagation seeds. The caller-supplied
 * re_propagate queue receives those seeds. The caller-supplied removal
 * queue is the working queue for this BFS. */
static void removal_bfs(Chunk* c, const LightingNeighbors* nb,
                        int x, int y, int z,
                        uint8_t old_value,
                        LightQueue* rq,
                        LightQueue* re_propagate)
{
    lq_push(rq, x, y, z, old_value);

    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(rq)) {
        LightCell cell = lq_pop(rq);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;
            if (nx_ < 0 || nx_ >= CHUNK_X) continue;  /* spec 1: local only */
            if (nz_ < 0 || nz_ >= CHUNK_Z) continue;

            uint8_t nb_sky = chunk_get_skylight(c, nx_, ny_, nz_);
            if (nb_sky == 0) continue;

            /* If this neighbor's light could have been sustained by us
             * (cell.light - cost == nb_sky), zero it and continue removal.
             * Otherwise it's brighter than we contributed — queue it as a
             * re-propagation seed. */
            if (nb_sky < cell.light) {
                chunk_set_skylight(c, nx_, ny_, nz_, 0);
                lq_push(rq, nx_, ny_, nz_, nb_sky);
            } else {
                lq_push(re_propagate, nx_, ny_, nz_, nb_sky);
            }
        }
    }

    (void)nb; /* cross-chunk removal is bounded; see Risks in spec */
}

/* Addition-BFS: relax outward from a queue of cells. Same code shape as
 * the inner loop of horizontal_bfs but operates on a caller-supplied queue. */
static void addition_bfs(Chunk* c, const LightingNeighbors* nb, LightQueue* q)
{
    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };

    while (!lq_empty(q)) {
        LightCell cell = lq_pop(q);
        for (int f = 0; f < 6; f++) {
            int nx_ = cell.x + dx[f];
            int ny_ = cell.y + dy[f];
            int nz_ = cell.z + dz[f];
            if (ny_ < 0 || ny_ >= CHUNK_Y) continue;

            Chunk* target = c;
            int tx = nx_, tz = nz_;
            uint8_t out_face = 0xFF;
            if (nx_ < 0)             { target = nb ? nb->neg_x : NULL; tx = CHUNK_X - 1; out_face = 1; }
            else if (nx_ >= CHUNK_X) { target = nb ? nb->pos_x : NULL; tx = 0;            out_face = 0; }
            else if (nz_ < 0)        { target = nb ? nb->neg_z : NULL; tz = CHUNK_Z - 1; out_face = 3; }
            else if (nz_ >= CHUNK_Z) { target = nb ? nb->pos_z : NULL; tz = 0;            out_face = 2; }

            if (!target) continue;

            BlockID nb_block  = chunk_get_block(target, tx, ny_, tz);
            uint8_t nb_absorb = block_get_def(nb_block)->light_absorb;
            uint8_t new_sky   = step_light(cell.light, nb_absorb);
            if (new_sky == 0) continue;

            if (target == c) {
                uint8_t cur = chunk_get_skylight(c, tx, ny_, tz);
                if (new_sky > cur) {
                    chunk_set_skylight(c, tx, ny_, tz, new_sky);
                    lq_push(q, tx, ny_, tz, new_sky);
                }
            } else {
                uint8_t axis = (out_face == 0 || out_face == 1)
                             ? (uint8_t)tz : (uint8_t)tx;
                uint8_t cur = chunk_get_skylight(target, tx, ny_, tz);
                if (new_sky > cur) {
                    push_boundary_delta(target, out_face, axis,
                                        (uint16_t)ny_, new_sky);
                }
            }
        }
    }
}

void lighting_on_block_changed(
    Chunk* c, const LightingNeighbors* nb,
    int x, int y, int z, BlockID old_id, BlockID new_id)
{
    uint8_t old_absorb = block_get_def(old_id)->light_absorb;
    uint8_t new_absorb = block_get_def(new_id)->light_absorb;
    uint8_t cur_sky    = chunk_get_skylight(c, x, y, z);

    /* Heap-allocate both queues to avoid 512 KiB stack frames. */
    LightQueue* removal_q = malloc(sizeof(LightQueue));
    LightQueue* add_q     = malloc(sizeof(LightQueue));
    if (!removal_q || !add_q) {
        fprintf(stderr, "lighting: out of memory in on_block_changed\n");
        free(removal_q);
        free(add_q);
        return;
    }
    lq_init(removal_q);
    lq_init(add_q);

    if (new_absorb > old_absorb) {
        /* Block became more opaque: removal-BFS from the cell's old value. */
        chunk_set_skylight(c, x, y, z, 0);
        removal_bfs(c, nb, x, y, z, cur_sky, removal_q, add_q);
    }

    /* Re-evaluate this column's sky exposure. Walk down from CHUNK_Y-1 and
     * re-run sky_column_pass for column (x,z). Cheap — single column. */
    {
        uint8_t sky = 15;
        for (int yy = CHUNK_Y - 1; yy >= 0; yy--) {
            BlockID b = chunk_get_block(c, x, yy, z);
            uint8_t a = block_get_def(b)->light_absorb;
            if (a > 0) sky = step_light(sky, a);
            uint8_t was = chunk_get_skylight(c, x, yy, z);
            if (sky > was) {
                chunk_set_skylight(c, x, yy, z, sky);
                lq_push(add_q, x, yy, z, sky);
            } else if (sky < was) {
                /* Cell darkened due to block change above: zero it here.
                 * Horizontal removal will be handled by re-enforcing the
                 * column constraint after addition_bfs. */
                chunk_set_skylight(c, x, yy, z, sky);
            }
        }
    }

    /* Also seed addition from the changed cell's 6 neighbors so light
     * flows back in around a removed opaque. */
    static const int dx[6] = { 1, -1,  0,  0,  0,  0 };
    static const int dy[6] = { 0,  0,  1, -1,  0,  0 };
    static const int dz[6] = { 0,  0,  0,  0,  1, -1 };
    for (int f = 0; f < 6; f++) {
        int nx_ = x + dx[f], ny_ = y + dy[f], nz_ = z + dz[f];
        if (ny_ < 0 || ny_ >= CHUNK_Y) continue;
        if (nx_ < 0 || nx_ >= CHUNK_X) continue;
        if (nz_ < 0 || nz_ >= CHUNK_Z) continue;
        uint8_t s = chunk_get_skylight(c, nx_, ny_, nz_);
        if (s > 0) lq_push(add_q, nx_, ny_, nz_, s);
    }

    addition_bfs(c, nb, add_q);

    /* Re-enforce the column sky constraint after BFS: lateral BFS may have
     * pushed light into column cells that are now below an opaque block.
     * Walk the column once more and clamp each cell to the true column value. */
    {
        uint8_t sky = 15;
        for (int yy = CHUNK_Y - 1; yy >= 0; yy--) {
            BlockID b = chunk_get_block(c, x, yy, z);
            uint8_t a = block_get_def(b)->light_absorb;
            if (a > 0) sky = step_light(sky, a);
            uint8_t cur = chunk_get_skylight(c, x, yy, z);
            if (cur > sky) {
                chunk_set_skylight(c, x, yy, z, sky);
            }
        }
    }

    free(removal_q);
    free(add_q);
}
