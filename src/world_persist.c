#include "world_persist.h"
#include "chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  On-disk / wire format                                              */
/* ------------------------------------------------------------------ */
/*
 *  Header (16 bytes, all little-endian):
 *      [0..3]   magic   "MVWP"
 *      [4..7]   version u32 = 1
 *      [8..11]  seed    i32
 *      [12..15] count   u32   number of entries that follow
 *  Then `count` entries, 13 bytes each:
 *      x i32, y i32, z i32, block u8
 */
#define OVERLAY_MAGIC0 'M'
#define OVERLAY_MAGIC1 'V'
#define OVERLAY_MAGIC2 'W'
#define OVERLAY_MAGIC3 'P'
#define OVERLAY_VERSION 1u
#define OVERLAY_HEADER_SIZE 16u
#define OVERLAY_ENTRY_SIZE  13u

#define OVERLAY_INITIAL_CAP 256u   /* power of two */

/* ------------------------------------------------------------------ */
/*  Little-endian byte helpers                                         */
/* ------------------------------------------------------------------ */

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static void put_i32(uint8_t* p, int32_t v) { put_u32(p, (uint32_t)v); }
static int32_t get_i32(const uint8_t* p)    { return (int32_t)get_u32(p); }

/* ------------------------------------------------------------------ */
/*  Hashing / table internals (caller must hold the lock)              */
/* ------------------------------------------------------------------ */

static size_t hash_coord(int32_t x, int32_t y, int32_t z) {
    /* Mix three coords; FNV-ish. */
    uint64_t h = 1469598103934665603ULL;
    uint32_t parts[3] = { (uint32_t)x, (uint32_t)y, (uint32_t)z };
    for (int i = 0; i < 3; i++) {
        h ^= parts[i];
        h *= 1099511628211ULL;
    }
    return (size_t)h;
}

/* Insert into a raw slot array (no locking, no growth). Returns true if a
 * brand-new cell was occupied (caller bumps count), false on overwrite. */
static bool table_put(OverlayEntry* slots, size_t cap,
                      int32_t x, int32_t y, int32_t z, uint8_t block) {
    size_t mask = cap - 1;
    size_t i = hash_coord(x, y, z) & mask;
    for (;;) {
        OverlayEntry* e = &slots[i];
        if (!e->used) {
            e->x = x; e->y = y; e->z = z; e->block = block; e->used = 1;
            return true;
        }
        if (e->x == x && e->y == y && e->z == z) {
            e->block = block;   /* overwrite */
            return false;
        }
        i = (i + 1) & mask;
    }
}

static const OverlayEntry* table_find(const OverlayEntry* slots, size_t cap,
                                      int32_t x, int32_t y, int32_t z) {
    if (cap == 0) return NULL;
    size_t mask = cap - 1;
    size_t i = hash_coord(x, y, z) & mask;
    for (;;) {
        const OverlayEntry* e = &slots[i];
        if (!e->used) return NULL;
        if (e->x == x && e->y == y && e->z == z) return e;
        i = (i + 1) & mask;
    }
}

/* Grow to double capacity. Caller holds the lock. */
static void table_grow(BlockOverlay* ov) {
    size_t new_cap = ov->cap ? ov->cap * 2 : OVERLAY_INITIAL_CAP;
    OverlayEntry* ns = calloc(new_cap, sizeof(OverlayEntry));
    if (!ns) {
        fprintf(stderr, "overlay: out of memory growing table\n");
        abort();
    }
    for (size_t i = 0; i < ov->cap; i++) {
        if (ov->slots[i].used) {
            OverlayEntry* e = &ov->slots[i];
            table_put(ns, new_cap, e->x, e->y, e->z, e->block);
        }
    }
    free(ov->slots);
    ov->slots = ns;
    ov->cap   = new_cap;
}

/* Insert with auto-grow. Caller holds the lock. */
static void overlay_put_locked(BlockOverlay* ov, int32_t x, int32_t y, int32_t z, uint8_t block) {
    /* Keep load factor under 0.7. */
    if (ov->cap == 0 || (ov->count + 1) * 10 >= ov->cap * 7)
        table_grow(ov);
    if (table_put(ov->slots, ov->cap, x, y, z, block))
        ov->count++;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void overlay_init(BlockOverlay* ov, int32_t seed) {
    ov->seed  = seed;
    ov->cap   = OVERLAY_INITIAL_CAP;
    ov->count = 0;
    ov->slots = calloc(ov->cap, sizeof(OverlayEntry));
    if (!ov->slots) {
        fprintf(stderr, "overlay_init: out of memory\n");
        abort();
    }
    pt_mutex_init(&ov->mutex);
    ov->mutex_init = true;
}

void overlay_free(BlockOverlay* ov) {
    if (!ov) return;
    free(ov->slots);
    ov->slots = NULL;
    ov->cap = 0;
    ov->count = 0;
    if (ov->mutex_init) {
        pt_mutex_destroy(&ov->mutex);
        ov->mutex_init = false;
    }
}

void overlay_record(BlockOverlay* ov, int32_t x, int32_t y, int32_t z, BlockID block) {
    pt_mutex_lock(&ov->mutex);
    overlay_put_locked(ov, x, y, z, (uint8_t)block);
    pt_mutex_unlock(&ov->mutex);
}

bool overlay_get(const BlockOverlay* ov, int32_t x, int32_t y, int32_t z, BlockID* out) {
    BlockOverlay* m = (BlockOverlay*)ov;   /* mutex is logically mutable */
    pt_mutex_lock(&m->mutex);
    const OverlayEntry* e = table_find(ov->slots, ov->cap, x, y, z);
    bool found = (e != NULL);
    if (found && out) *out = (BlockID)e->block;
    pt_mutex_unlock(&m->mutex);
    return found;
}

size_t overlay_count(const BlockOverlay* ov) {
    BlockOverlay* m = (BlockOverlay*)ov;
    pt_mutex_lock(&m->mutex);
    size_t c = ov->count;
    pt_mutex_unlock(&m->mutex);
    return c;
}

int32_t overlay_seed(const BlockOverlay* ov) {
    return ov->seed;
}

void overlay_apply_chunk(const BlockOverlay* ov, Chunk* chunk) {
    if (!ov || !chunk) return;
    BlockOverlay* m = (BlockOverlay*)ov;
    int32_t base_x = chunk->cx * CHUNK_X;
    int32_t base_z = chunk->cz * CHUNK_Z;

    pt_mutex_lock(&m->mutex);
    /* The overlay is typically far larger than one chunk, so iterate the
     * chunk's cells against the table only if the table is small; otherwise
     * scan the table once and apply the entries that land in this chunk. */
    for (size_t i = 0; i < ov->cap; i++) {
        const OverlayEntry* e = &ov->slots[i];
        if (!e->used) continue;
        if (e->x < base_x || e->x >= base_x + CHUNK_X) continue;
        if (e->z < base_z || e->z >= base_z + CHUNK_Z) continue;
        if (e->y < 0 || e->y >= CHUNK_Y) continue;
        chunk_set_block(chunk, e->x - base_x, e->y, e->z - base_z, (BlockID)e->block);
    }
    pt_mutex_unlock(&m->mutex);
}

/* ------------------------------------------------------------------ */
/*  Serialize / deserialize                                            */
/* ------------------------------------------------------------------ */

bool overlay_serialize(const BlockOverlay* ov, uint8_t** out_buf, size_t* out_len) {
    BlockOverlay* m = (BlockOverlay*)ov;
    pt_mutex_lock(&m->mutex);

    size_t count = ov->count;
    size_t len   = OVERLAY_HEADER_SIZE + count * OVERLAY_ENTRY_SIZE;
    uint8_t* buf = malloc(len ? len : 1);
    if (!buf) {
        pt_mutex_unlock(&m->mutex);
        return false;
    }

    buf[0] = OVERLAY_MAGIC0; buf[1] = OVERLAY_MAGIC1;
    buf[2] = OVERLAY_MAGIC2; buf[3] = OVERLAY_MAGIC3;
    put_u32(buf + 4, OVERLAY_VERSION);
    put_i32(buf + 8, ov->seed);
    put_u32(buf + 12, (uint32_t)count);

    size_t off = OVERLAY_HEADER_SIZE;
    for (size_t i = 0; i < ov->cap; i++) {
        const OverlayEntry* e = &ov->slots[i];
        if (!e->used) continue;
        put_i32(buf + off + 0, e->x);
        put_i32(buf + off + 4, e->y);
        put_i32(buf + off + 8, e->z);
        buf[off + 12] = e->block;
        off += OVERLAY_ENTRY_SIZE;
    }

    pt_mutex_unlock(&m->mutex);

    *out_buf = buf;
    *out_len = len;
    return true;
}

bool overlay_deserialize(BlockOverlay* ov, const uint8_t* buf, size_t len) {
    if (!buf || len < OVERLAY_HEADER_SIZE) return false;
    if (buf[0] != OVERLAY_MAGIC0 || buf[1] != OVERLAY_MAGIC1 ||
        buf[2] != OVERLAY_MAGIC2 || buf[3] != OVERLAY_MAGIC3)
        return false;
    if (get_u32(buf + 4) != OVERLAY_VERSION) return false;

    int32_t  seed  = get_i32(buf + 8);
    uint32_t count = get_u32(buf + 12);

    /* Validate the declared count matches the buffer exactly. */
    size_t expected = OVERLAY_HEADER_SIZE + (size_t)count * OVERLAY_ENTRY_SIZE;
    if (expected != len) return false;

    overlay_init(ov, seed);

    size_t off = OVERLAY_HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = get_i32(buf + off + 0);
        int32_t y = get_i32(buf + off + 4);
        int32_t z = get_i32(buf + off + 8);
        uint8_t b = buf[off + 12];
        overlay_put_locked(ov, x, y, z, b);   /* single-threaded here, lock not held but exclusive */
        off += OVERLAY_ENTRY_SIZE;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  File I/O                                                           */
/* ------------------------------------------------------------------ */

bool overlay_save(const BlockOverlay* ov, const char* path) {
    uint8_t* buf = NULL;
    size_t   len = 0;
    /* Snapshot+encode under the lock (inside overlay_serialize), then do the
     * actual disk write here OUTSIDE the lock. */
    if (!overlay_serialize(ov, &buf, &len)) return false;

    /* Write to a temp file then rename for atomicity. */
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) { free(buf); return false; }

    FILE* f = fopen(tmp, "wb");
    if (!f) { free(buf); return false; }
    bool ok = (fwrite(buf, 1, len, f) == len);
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    free(buf);

    if (!ok) { remove(tmp); return false; }
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

bool overlay_load(BlockOverlay* ov, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }

    uint8_t* buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return false; }

    bool ok = overlay_deserialize(ov, buf, (size_t)sz);
    free(buf);
    return ok;
}
