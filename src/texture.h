#ifndef TEXTURE_H
#define TEXTURE_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdbool.h>

typedef struct Renderer Renderer;

bool texture_create_atlas(Renderer* r);
void texture_write_descriptors(Renderer* r);

bool texture_create_player_skin(Renderer* r);
void texture_write_player_skin_descriptors(Renderer* r, VkDescriptorSet sets[2]);

#endif
