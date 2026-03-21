#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Public API: dump frame                                            */
/* ------------------------------------------------------------------ */

bool renderer_dump_frame(Renderer* r, const char *path)
{
    uint32_t width  = r->swapchain.extent.width;
    uint32_t height = r->swapchain.extent.height;
    VkDeviceSize buf_size = (VkDeviceSize)width * height * 4;

    /* 1. Allocate host-visible staging buffer */
    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = buf_size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc_ci = {
        .usage = VMA_MEMORY_USAGE_CPU_ONLY,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer      staging_buf;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;
    if (vmaCreateBuffer(r->allocator, &buf_ci, &alloc_ci,
                        &staging_buf, &staging_alloc, &staging_info) != VK_SUCCESS) {
        fprintf(stderr, "renderer_dump_frame: failed to create staging buffer\n");
        return false;
    }

    /* 2. Wait for GPU idle */
    vkQueueWaitIdle(r->graphics_queue);

    /* 3. Submit copy command */
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);
    VkImage src_image = r->swapchain.images[r->last_image_index];

    /* Transition: PRESENT_SRC -> TRANSFER_SRC */
    VkImageMemoryBarrier barrier_to_src = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = src_image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier_to_src);

    /* Copy image -> buffer */
    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, src_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buf, 1, &region);

    /* Transition back: TRANSFER_SRC -> PRESENT_SRC */
    VkImageMemoryBarrier barrier_to_present = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = src_image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier_to_present);

    renderer_end_single_cmd(r, cmd);

    /* 4. Swapchain is B8G8R8A8; swap R↔B so stbi_write_png gets RGBA */
    uint8_t *pixels = (uint8_t *)staging_info.pMappedData;
    for (uint32_t i = 0; i < width * height * 4; i += 4) {
        uint8_t tmp = pixels[i]; pixels[i] = pixels[i+2]; pixels[i+2] = tmp;
    }
    bool ok = stbi_write_png(path, (int)width, (int)height, 4,
                              pixels, (int)(width * 4)) != 0;
    if (!ok)
        fprintf(stderr, "renderer_dump_frame: stbi_write_png failed\n");

    /* 5. Cleanup */
    vmaDestroyBuffer(r->allocator, staging_buf, staging_alloc);
    return ok;
}
