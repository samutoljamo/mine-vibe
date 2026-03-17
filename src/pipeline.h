#ifndef PIPELINE_H
#define PIPELINE_H

#include <volk.h>
#include <stdbool.h>

bool pipeline_create(VkDevice device, VkRenderPass render_pass,
                     VkDescriptorSetLayout desc_layout,
                     const char* vert_path, const char* frag_path,
                     VkPipelineLayout* out_layout, VkPipeline* out_pipeline);

bool pipeline_create_descriptor_layout(VkDevice device, VkDescriptorSetLayout* out);

#endif
