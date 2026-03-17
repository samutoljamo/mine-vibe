#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>
#include <cglm/cglm.h>
#include "chunk_mesh.h"
#include "block.h"

typedef struct Renderer Renderer;
typedef struct World World;

World* world_create(Renderer* renderer, int seed, int render_distance);
void   world_destroy(World* world);
void   world_update(World* world, vec3 player_pos);
void   world_get_meshes(World* world, ChunkMesh** out_meshes, uint32_t* out_count);

uint32_t world_get_ready_count(const World* world);
int      world_get_render_distance(const World* world);

BlockID world_get_block(World* world, int x, int y, int z);

#endif
