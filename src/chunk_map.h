#ifndef CHUNK_MAP_H
#define CHUNK_MAP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Chunk Chunk;

typedef struct ChunkMapEntry {
    int32_t cx, cz;
    Chunk*  chunk;
    bool    occupied;
} ChunkMapEntry;

typedef struct ChunkMap {
    ChunkMapEntry* entries;
    uint32_t       capacity;
    uint32_t       count;
} ChunkMap;

void   chunk_map_init(ChunkMap* map, uint32_t capacity);
void   chunk_map_free(ChunkMap* map);
Chunk* chunk_map_get(ChunkMap* map, int32_t cx, int32_t cz);
void   chunk_map_put(ChunkMap* map, Chunk* chunk);
Chunk* chunk_map_remove(ChunkMap* map, int32_t cx, int32_t cz);
Chunk* chunk_map_iter(ChunkMap* map, uint32_t* idx);

#endif
