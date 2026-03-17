#ifndef SWAPCHAIN_H
#define SWAPCHAIN_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdbool.h>

typedef struct Swapchain {
    VkSwapchainKHR  swapchain;
    VkFormat        image_format;
    VkExtent2D      extent;
    uint32_t        image_count;
    VkImage*        images;
    VkImageView*    image_views;
    VkImage         depth_image;
    VmaAllocation   depth_alloc;
    VkImageView     depth_view;
    VkFormat        depth_format;
    VkFramebuffer*  framebuffers;
} Swapchain;

bool swapchain_create(VkPhysicalDevice pd, VkDevice device, VmaAllocator allocator,
                      VkSurfaceKHR surface, uint32_t graphics_family,
                      uint32_t present_family, VkRenderPass render_pass,
                      int width, int height, VkSwapchainKHR old_swapchain,
                      Swapchain* out);

void swapchain_destroy(VkDevice device, VmaAllocator allocator, Swapchain* sc);

#endif
