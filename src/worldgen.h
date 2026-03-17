#ifndef WORLDGEN_H
#define WORLDGEN_H

typedef struct Chunk Chunk;

void worldgen_generate(Chunk* chunk, int seed);

#endif
