#ifndef SWAPCHAIN_H
#define SWAPCHAIN_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdbool.h>

/* Requested swapchain present mode. Plumbed from --present and resolved
 * against device support at swapchain creation (FIFO is always supported and
 * is the graceful fallback).
 *   FIFO      : vsync, queued — lowest memory/power, best for integrated GPUs.
 *   MAILBOX   : low-latency vsync (triple-buffer) — extra image/memory/power.
 *   IMMEDIATE : no vsync, may tear. */
typedef enum PresentModePref {
    PRESENT_PREF_FIFO = 0,
    PRESENT_PREF_MAILBOX,
    PRESENT_PREF_IMMEDIATE,
} PresentModePref;

typedef struct Swapchain {
    VkSwapchainKHR        swapchain;
    VkFormat              image_format;
    VkExtent2D            extent;
    uint32_t              image_count;
    VkImage*              images;
    VkImageView*          image_views;
    /* MSAA color image (multisampled). Null when sample_count == 1. */
    VkImage               msaa_color_image;
    VmaAllocation         msaa_color_alloc;
    VkImageView           msaa_color_view;
    /* Depth image; multisampled when sample_count > 1, single-sampled otherwise. */
    VkImage               depth_image;
    VmaAllocation         depth_alloc;
    VkImageView           depth_view;
    VkFormat              depth_format;
    VkFramebuffer*        framebuffers;
    VkSampleCountFlagBits sample_count;
} Swapchain;

bool swapchain_create(VkPhysicalDevice pd, VkDevice device, VmaAllocator allocator,
                      VkSurfaceKHR surface, uint32_t graphics_family,
                      uint32_t present_family, VkRenderPass render_pass,
                      VkSampleCountFlagBits sample_count,
                      PresentModePref present_pref,
                      int width, int height, VkSwapchainKHR old_swapchain,
                      Swapchain* out);

void swapchain_destroy(VkDevice device, VmaAllocator allocator, Swapchain* sc);

#endif
