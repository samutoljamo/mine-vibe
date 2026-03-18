#ifndef PIPELINE_H
#define PIPELINE_H

#include <volk.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool pipeline_create(VkDevice device, VkRenderPass render_pass,
                     VkDescriptorSetLayout desc_layout,
                     const uint8_t* vert_spv, size_t vert_size,
                     const uint8_t* frag_spv, size_t frag_size,
                     VkPipelineLayout* out_layout, VkPipeline* out_pipeline);

bool player_pipeline_create(VkDevice device, VkRenderPass render_pass,
                             VkDescriptorSetLayout desc_layout,
                             const uint8_t* vert_spv, size_t vert_size,
                             const uint8_t* frag_spv, size_t frag_size,
                             VkPipelineLayout* out_layout, VkPipeline* out_pipeline);

bool pipeline_create_descriptor_layout(VkDevice device, VkDescriptorSetLayout* out);

#endif
