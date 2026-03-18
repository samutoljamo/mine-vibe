#include "texture.h"
#include "renderer.h"
#include "assets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ */
/*  Image layout transitions                                          */
/* ------------------------------------------------------------------ */

static void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                    VkImageLayout old_layout,
                                    VkImageLayout new_layout,
                                    uint32_t mip_levels)
{
    VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = mip_levels,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else {
        fprintf(stderr, "Unsupported layout transition\n");
        return;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

static void generate_mipmaps(VkCommandBuffer cmd, VkImage image,
                              int32_t width, int32_t height, uint32_t mip_levels)
{
    VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    int32_t mip_w = width;
    int32_t mip_h = height;

    for (uint32_t i = 1; i < mip_levels; i++) {
        /* Transition mip i-1 from TRANSFER_DST to TRANSFER_SRC */
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);

        int32_t next_w = mip_w > 1 ? mip_w / 2 : 1;
        int32_t next_h = mip_h > 1 ? mip_h / 2 : 1;

        VkImageBlit blit = {
            .srcSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = i - 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .srcOffsets = { { 0, 0, 0 }, { mip_w, mip_h, 1 } },
            .dstSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = i,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .dstOffsets = { { 0, 0, 0 }, { next_w, next_h, 1 } },
        };

        vkCmdBlitImage(cmd,
                       image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        /* Transition mip i-1 to SHADER_READ_ONLY */
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);

        mip_w = next_w;
        mip_h = next_h;
    }

    /* Transition the last mip level to SHADER_READ_ONLY */
    barrier.subresourceRange.baseMipLevel = mip_levels - 1;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
}

/* ------------------------------------------------------------------ */
/*  Atlas creation                                                    */
/* ------------------------------------------------------------------ */

bool texture_create_atlas(Renderer* r)
{
    const uint8_t* pixels = g_atlas_pixels;
    VkDeviceSize image_size = ATLAS_SIZE * ATLAS_SIZE * 4;

    /* Create staging buffer */
    VkBufferCreateInfo staging_buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };

    VmaAllocationCreateInfo staging_alloc_ci = {
        .usage = VMA_MEMORY_USAGE_CPU_ONLY,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };

    VkBuffer staging_buf;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;

    if (vmaCreateBuffer(r->allocator, &staging_buf_ci, &staging_alloc_ci,
                        &staging_buf, &staging_alloc, &staging_info) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create staging buffer for atlas\n");
        return false;
    }

    memcpy(staging_info.pMappedData, pixels, image_size);

    /* Create GPU image */
    VkImageCreateInfo image_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_SRGB,
        .extent        = { ATLAS_SIZE, ATLAS_SIZE, 1 },
        .mipLevels     = ATLAS_MIP_LEVELS,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VmaAllocationCreateInfo img_alloc_ci = {
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
    };

    if (vmaCreateImage(r->allocator, &image_ci, &img_alloc_ci,
                       &r->atlas_image, &r->atlas_alloc, NULL) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create atlas image\n");
        vmaDestroyBuffer(r->allocator, staging_buf, staging_alloc);
        return false;
    }

    /* Transfer via single-shot command buffer */
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);

    /* Transition all mip levels to TRANSFER_DST */
    transition_image_layout(cmd, r->atlas_image,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            ATLAS_MIP_LEVELS);

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { ATLAS_SIZE, ATLAS_SIZE, 1 },
    };

    vkCmdCopyBufferToImage(cmd, staging_buf, r->atlas_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* Generate mipmaps (also transitions all levels to SHADER_READ_ONLY) */
    generate_mipmaps(cmd, r->atlas_image,
                     ATLAS_SIZE, ATLAS_SIZE, ATLAS_MIP_LEVELS);

    renderer_end_single_cmd(r, cmd);

    /* Destroy staging buffer */
    vmaDestroyBuffer(r->allocator, staging_buf, staging_alloc);

    /* Create image view */
    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = r->atlas_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = ATLAS_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    if (vkCreateImageView(r->device, &view_ci, NULL, &r->atlas_view) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create atlas image view\n");
        vmaDestroyImage(r->allocator, r->atlas_image, r->atlas_alloc);
        return false;
    }

    /* Create sampler */
    VkSamplerCreateInfo sampler_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_NEAREST,
        .minFilter    = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .mipLodBias   = 0.0f,
        .minLod       = 0.0f,
        .maxLod       = (float)(ATLAS_MIP_LEVELS - 1),
    };

    if (vkCreateSampler(r->device, &sampler_ci, NULL, &r->atlas_sampler) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create atlas sampler\n");
        vkDestroyImageView(r->device, r->atlas_view, NULL);
        vmaDestroyImage(r->allocator, r->atlas_image, r->atlas_alloc);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Player skin texture                                               */
/* ------------------------------------------------------------------ */

bool texture_create_player_skin(Renderer* r)
{
    const uint8_t* pixels = g_player_skin_pixels;
    VkDeviceSize image_size = SKIN_WIDTH * SKIN_HEIGHT * 4;

    /* Staging buffer */
    VkBufferCreateInfo staging_buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo staging_alloc_ci = {
        .usage = VMA_MEMORY_USAGE_CPU_ONLY,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer staging_buf;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;
    if (vmaCreateBuffer(r->allocator, &staging_buf_ci, &staging_alloc_ci,
                        &staging_buf, &staging_alloc, &staging_info) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create staging buffer for player skin\n");
        return false;
    }
    memcpy(staging_info.pMappedData, pixels, image_size);

    /* GPU image (1 mip level, NEAREST sampled) */
    VkImageCreateInfo image_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_SRGB,
        .extent        = { SKIN_WIDTH, SKIN_HEIGHT, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo img_alloc_ci = { .usage = VMA_MEMORY_USAGE_GPU_ONLY };
    if (vmaCreateImage(r->allocator, &image_ci, &img_alloc_ci,
                       &r->player_skin_image, &r->player_skin_alloc, NULL) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create player skin image\n");
        vmaDestroyBuffer(r->allocator, staging_buf, staging_alloc);
        return false;
    }

    VkCommandBuffer cmd = renderer_begin_single_cmd(r);
    transition_image_layout(cmd, r->player_skin_image,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);

    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .layerCount = 1 },
        .imageExtent = { SKIN_WIDTH, SKIN_HEIGHT, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging_buf, r->player_skin_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* Transition directly to SHADER_READ_ONLY (no mipmaps) */
    transition_image_layout(cmd, r->player_skin_image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    renderer_end_single_cmd(r, cmd);
    vmaDestroyBuffer(r->allocator, staging_buf, staging_alloc);

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = r->player_skin_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(r->device, &view_ci, NULL, &r->player_skin_view) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create player skin image view\n");
        vmaDestroyImage(r->allocator, r->player_skin_image, r->player_skin_alloc);
        return false;
    }

    /* Sampler: NEAREST, no mipmaps */
    VkSamplerCreateInfo sampler_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_NEAREST,
        .minFilter    = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .minLod       = 0.0f,
        .maxLod       = 0.0f,
    };
    if (vkCreateSampler(r->device, &sampler_ci, NULL, &r->player_skin_sampler) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create player skin sampler\n");
        vkDestroyImageView(r->device, r->player_skin_view, NULL);
        vmaDestroyImage(r->allocator, r->player_skin_image, r->player_skin_alloc);
        return false;
    }
    return true;
}

void texture_write_player_skin_descriptors(Renderer* r, VkDescriptorSet sets[2])
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo image_info = {
            .sampler     = r->player_skin_sampler,
            .imageView   = r->player_skin_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = sets[i],
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &image_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }
}

/* ------------------------------------------------------------------ */
/*  Descriptor writes                                                 */
/* ------------------------------------------------------------------ */

void texture_write_descriptors(Renderer* r)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo image_info = {
            .sampler     = r->atlas_sampler,
            .imageView   = r->atlas_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = r->descriptor_sets[i],
            .dstBinding      = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &image_info,
        };

        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }
}
