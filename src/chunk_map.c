#include "chunk_map.h"
#include "chunk.h"
#include <stdlib.h>
#include <string.h>

/* splitmix64 hash */
static uint64_t hash_key(int32_t cx, int32_t cz)
{
    uint64_t key = ((uint64_t)(uint32_t)cx) | ((uint64_t)(uint32_t)cz << 32);
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return key;
}

static uint32_t round_up_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v < 16 ? 16 : v;
}

void chunk_map_init(ChunkMap* map, uint32_t capacity)
{
    map->capacity = round_up_pow2(capacity);
    map->count = 0;
    map->entries = calloc(map->capacity, sizeof(ChunkMapEntry));
}

void chunk_map_free(ChunkMap* map)
{
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0;
    map->count = 0;
}

static uint32_t find_slot(ChunkMapEntry* entries, uint32_t capacity,
                          int32_t cx, int32_t cz)
{
    uint32_t mask = capacity - 1;
    uint32_t idx = (uint32_t)(hash_key(cx, cz) & mask);
    for (;;) {
        if (!entries[idx].occupied) return idx;
        if (entries[idx].cx == cx && entries[idx].cz == cz) return idx;
        idx = (idx + 1) & mask;
    }
}

static void rehash(ChunkMap* map)
{
    uint32_t new_cap = map->capacity * 2;
    ChunkMapEntry* new_entries = calloc(new_cap, sizeof(ChunkMapEntry));

    for (uint32_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            uint32_t slot = find_slot(new_entries, new_cap,
                                      map->entries[i].cx, map->entries[i].cz);
            new_entries[slot] = map->entries[i];
        }
    }

    free(map->entries);
    map->entries = new_entries;
    map->capacity = new_cap;
}

Chunk* chunk_map_get(ChunkMap* map, int32_t cx, int32_t cz)
{
    uint32_t mask = map->capacity - 1;
    uint32_t idx = (uint32_t)(hash_key(cx, cz) & mask);
    for (;;) {
        if (!map->entries[idx].occupied) return NULL;
        if (map->entries[idx].cx == cx && map->entries[idx].cz == cz)
            return map->entries[idx].chunk;
        idx = (idx + 1) & mask;
    }
}

void chunk_map_put(ChunkMap* map, Chunk* chunk)
{
    /* Rehash at 70% load */
    if (map->count * 10 >= map->capacity * 7) {
        rehash(map);
    }

    uint32_t slot = find_slot(map->entries, map->capacity, chunk->cx, chunk->cz);
    if (!map->entries[slot].occupied) {
        map->entries[slot].cx = chunk->cx;
        map->entries[slot].cz = chunk->cz;
        map->entries[slot].chunk = chunk;
        map->entries[slot].occupied = true;
        map->count++;
    } else {
        /* Update existing */
        map->entries[slot].chunk = chunk;
    }
}

Chunk* chunk_map_remove(ChunkMap* map, int32_t cx, int32_t cz)
{
    uint32_t mask = map->capacity - 1;
    uint32_t idx = (uint32_t)(hash_key(cx, cz) & mask);

    /* Find the entry */
    for (;;) {
        if (!map->entries[idx].occupied) return NULL;
        if (map->entries[idx].cx == cx && map->entries[idx].cz == cz) break;
        idx = (idx + 1) & mask;
    }

    Chunk* removed = map->entries[idx].chunk;
    map->entries[idx].occupied = false;
    map->count--;

    /* Re-insert displaced entries (linear probing fixup) */
    uint32_t next = (idx + 1) & mask;
    while (map->entries[next].occupied) {
        ChunkMapEntry displaced = map->entries[next];
        map->entries[next].occupied = false;
        map->count--;

        uint32_t slot = find_slot(map->entries, map->capacity,
                                  displaced.cx, displaced.cz);
        map->entries[slot] = displaced;
        map->count++;

        next = (next + 1) & mask;
    }

    return removed;
}

Chunk* chunk_map_iter(ChunkMap* map, uint32_t* idx)
{
    while (*idx < map->capacity) {
        uint32_t i = (*idx)++;
        if (map->entries[i].occupied) {
            return map->entries[i].chunk;
        }
    }
    return NULL;
}
