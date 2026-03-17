#include "texture.h"
#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_SIZE 256
#define TILE_SIZE  16
#define TILES_PER_ROW (ATLAS_SIZE / TILE_SIZE)

/* ------------------------------------------------------------------ */
/*  Procedural atlas generation                                       */
/* ------------------------------------------------------------------ */

static void fill_tile(uint8_t* pixels, int tile_index,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int tile_x = tile_index % TILES_PER_ROW;
    int tile_y = tile_index / TILES_PER_ROW;
    int base_x = tile_x * TILE_SIZE;
    int base_y = tile_y * TILE_SIZE;

    for (int py = 0; py < TILE_SIZE; py++) {
        for (int px = 0; px < TILE_SIZE; px++) {
            int x = base_x + px;
            int y = base_y + py;
            int idx = (y * ATLAS_SIZE + x) * 4;

            /* XOR-based variation */
            uint8_t v = (uint8_t)((px ^ py) & 0x0F);
            int vr = (int)r + v - 8;
            int vg = (int)g + v - 8;
            int vb = (int)b + v - 8;
            if (vr < 0) vr = 0; if (vr > 255) vr = 255;
            if (vg < 0) vg = 0; if (vg > 255) vg = 255;
            if (vb < 0) vb = 0; if (vb > 255) vb = 255;

            pixels[idx + 0] = (uint8_t)vr;
            pixels[idx + 1] = (uint8_t)vg;
            pixels[idx + 2] = (uint8_t)vb;
            pixels[idx + 3] = a;
        }
    }
}

static void fill_tile_grass_side(uint8_t* pixels, int tile_index)
{
    int tile_x = tile_index % TILES_PER_ROW;
    int tile_y = tile_index / TILES_PER_ROW;
    int base_x = tile_x * TILE_SIZE;
    int base_y = tile_y * TILE_SIZE;

    for (int py = 0; py < TILE_SIZE; py++) {
        for (int px = 0; px < TILE_SIZE; px++) {
            int x = base_x + px;
            int y = base_y + py;
            int idx = (y * ATLAS_SIZE + x) * 4;

            uint8_t v = (uint8_t)((px ^ py) & 0x0F);
            uint8_t r, g, b;

            if (py < 4) {
                /* Green strip on top 4 pixels */
                r = 76;  g = 153; b = 0;
            } else {
                /* Brown body */
                r = 139; g = 105; b = 60;
            }

            int vr = (int)r + v - 8;
            int vg = (int)g + v - 8;
            int vb = (int)b + v - 8;
            if (vr < 0) vr = 0; if (vr > 255) vr = 255;
            if (vg < 0) vg = 0; if (vg > 255) vg = 255;
            if (vb < 0) vb = 0; if (vb > 255) vb = 255;

            pixels[idx + 0] = (uint8_t)vr;
            pixels[idx + 1] = (uint8_t)vg;
            pixels[idx + 2] = (uint8_t)vb;
            pixels[idx + 3] = 255;
        }
    }
}

static uint8_t* generate_atlas_pixels(void)
{
    size_t size = ATLAS_SIZE * ATLAS_SIZE * 4;
    uint8_t* pixels = malloc(size);
    if (!pixels) return NULL;

    /* Default: magenta for unmapped tiles (debug) */
    for (size_t i = 0; i < size; i += 4) {
        pixels[i + 0] = 255;
        pixels[i + 1] = 0;
        pixels[i + 2] = 255;
        pixels[i + 3] = 255;
    }

    /* tile 0: stone */
    fill_tile(pixels, 0, 120, 120, 120, 255);
    /* tile 1: dirt */
    fill_tile(pixels, 1, 139, 90, 43, 255);
    /* tile 2: grass top */
    fill_tile(pixels, 2, 76, 153, 0, 255);
    /* tile 3: grass side */
    fill_tile_grass_side(pixels, 3);
    /* tile 4: sand */
    fill_tile(pixels, 4, 210, 190, 140, 255);
    /* tile 5: wood top */
    fill_tile(pixels, 5, 160, 120, 60, 255);
    /* tile 6: wood side */
    fill_tile(pixels, 6, 120, 80, 40, 255);
    /* tile 7: leaves */
    fill_tile(pixels, 7, 40, 120, 20, 200);
    /* tile 16: water */
    fill_tile(pixels, 16, 30, 80, 180, 200);
    /* tile 17: bedrock */
    fill_tile(pixels, 17, 60, 60, 60, 255);

    return pixels;
}

/* ------------------------------------------------------------------ */
/*  Image layout transitions                                          */
/* ------------------------------------------------------------------ */

static void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                    VkImageLayout old_layout,
                                    VkImageLayout new_layout)
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
            .levelCount     = 1,
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

/* ------------------------------------------------------------------ */
/*  Atlas creation                                                    */
/* ------------------------------------------------------------------ */

bool texture_create_atlas(Renderer* r)
{
    /* Generate pixels */
    uint8_t* pixels = generate_atlas_pixels();
    if (!pixels) {
        fprintf(stderr, "Failed to generate atlas pixels\n");
        return false;
    }

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
        free(pixels);
        return false;
    }

    memcpy(staging_info.pMappedData, pixels, image_size);
    free(pixels);

    /* Create GPU image */
    VkImageCreateInfo image_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_SRGB,
        .extent        = { ATLAS_SIZE, ATLAS_SIZE, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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

    transition_image_layout(cmd, r->atlas_image,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

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

    transition_image_layout(cmd, r->atlas_image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    if (vkCreateImageView(r->device, &view_ci, NULL, &r->atlas_view) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create atlas image view\n");
        return false;
    }

    /* Create sampler */
    VkSamplerCreateInfo sampler_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_NEAREST,
        .minFilter    = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .mipLodBias   = 0.0f,
        .minLod       = 0.0f,
        .maxLod       = 0.0f,
    };

    if (vkCreateSampler(r->device, &sampler_ci, NULL, &r->atlas_sampler) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create atlas sampler\n");
        return false;
    }

    return true;
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
