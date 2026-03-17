#include "swapchain.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static VkSurfaceFormatKHR choose_format(const VkSurfaceFormatKHR* formats,
                                         uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format     == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[i];
    }
    return formats[0];
}

static VkPresentModeKHR choose_present_mode(const VkPresentModeKHR* modes,
                                             uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            return modes[i];
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR* caps,
                                int width, int height)
{
    if (caps->currentExtent.width != UINT32_MAX) {
        return caps->currentExtent;
    }
    VkExtent2D extent;
    extent.width  = (uint32_t)width;
    extent.height = (uint32_t)height;

    if (extent.width  < caps->minImageExtent.width)
        extent.width  = caps->minImageExtent.width;
    if (extent.width  > caps->maxImageExtent.width)
        extent.width  = caps->maxImageExtent.width;
    if (extent.height < caps->minImageExtent.height)
        extent.height = caps->minImageExtent.height;
    if (extent.height > caps->maxImageExtent.height)
        extent.height = caps->maxImageExtent.height;

    return extent;
}

/* ------------------------------------------------------------------ */
/*  Depth buffer                                                      */
/* ------------------------------------------------------------------ */

static bool create_depth(VkDevice device, VmaAllocator allocator,
                         VkExtent2D extent, Swapchain* sc)
{
    sc->depth_format = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo img_ci = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = sc->depth_format,
        .extent      = { extent.width, extent.height, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo alloc_ci = {
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
    };

    if (vmaCreateImage(allocator, &img_ci, &alloc_ci,
                       &sc->depth_image, &sc->depth_alloc, NULL) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create depth image\n");
        return false;
    }

    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = sc->depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = sc->depth_format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    if (vkCreateImageView(device, &view_ci, NULL, &sc->depth_view) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image view\n");
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

bool swapchain_create(VkPhysicalDevice pd, VkDevice device, VmaAllocator allocator,
                      VkSurfaceKHR surface, uint32_t graphics_family,
                      uint32_t present_family, VkRenderPass render_pass,
                      int width, int height, VkSwapchainKHR old_swapchain,
                      Swapchain* out)
{
    memset(out, 0, sizeof(*out));

    /* Query surface capabilities */
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps) != VK_SUCCESS) {
        fprintf(stderr, "Failed to query surface capabilities\n");
        return false;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &format_count, NULL);
    VkSurfaceFormatKHR* formats = malloc(format_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &format_count, formats);

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &mode_count, NULL);
    VkPresentModeKHR* modes = malloc(mode_count * sizeof(VkPresentModeKHR));
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &mode_count, modes);

    VkSurfaceFormatKHR fmt  = choose_format(formats, format_count);
    VkPresentModeKHR   mode = choose_present_mode(modes, mode_count);
    VkExtent2D         ext  = choose_extent(&caps, width, height);

    free(formats);
    free(modes);

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;

    uint32_t families[] = { graphics_family, present_family };
    bool same_family = (graphics_family == present_family);

    VkSwapchainCreateInfoKHR ci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = surface,
        .minImageCount    = img_count,
        .imageFormat      = fmt.format,
        .imageColorSpace  = fmt.colorSpace,
        .imageExtent      = ext,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = same_family ? VK_SHARING_MODE_EXCLUSIVE
                                        : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = same_family ? 0 : 2,
        .pQueueFamilyIndices   = same_family ? NULL : families,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = mode,
        .clipped          = VK_TRUE,
        .oldSwapchain     = old_swapchain,
    };

    if (vkCreateSwapchainKHR(device, &ci, NULL, &out->swapchain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swapchain\n");
        return false;
    }

    out->image_format = fmt.format;
    out->extent       = ext;

    /* Get swapchain images */
    vkGetSwapchainImagesKHR(device, out->swapchain, &out->image_count, NULL);
    out->images = malloc(out->image_count * sizeof(VkImage));
    vkGetSwapchainImagesKHR(device, out->swapchain, &out->image_count, out->images);

    /* Create image views */
    out->image_views = malloc(out->image_count * sizeof(VkImageView));
    for (uint32_t i = 0; i < out->image_count; i++) {
        VkImageViewCreateInfo view_ci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = out->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = out->image_format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        if (vkCreateImageView(device, &view_ci, NULL, &out->image_views[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create swapchain image view %u\n", i);
            return false;
        }
    }

    /* Create depth buffer */
    if (!create_depth(device, allocator, ext, out))
        return false;

    /* Create framebuffers (only if render pass is provided) */
    if (render_pass != VK_NULL_HANDLE) {
        out->framebuffers = malloc(out->image_count * sizeof(VkFramebuffer));
        for (uint32_t i = 0; i < out->image_count; i++) {
            VkImageView attachments[] = { out->image_views[i], out->depth_view };
            VkFramebufferCreateInfo fb_ci = {
                .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass      = render_pass,
                .attachmentCount = 2,
                .pAttachments    = attachments,
                .width           = ext.width,
                .height          = ext.height,
                .layers          = 1,
            };
            if (vkCreateFramebuffer(device, &fb_ci, NULL, &out->framebuffers[i]) != VK_SUCCESS) {
                fprintf(stderr, "Failed to create framebuffer %u\n", i);
                return false;
            }
        }
    }

    return true;
}

void swapchain_destroy(VkDevice device, VmaAllocator allocator, Swapchain* sc)
{
    if (!sc || sc->swapchain == VK_NULL_HANDLE)
        return;

    if (sc->framebuffers) {
        for (uint32_t i = 0; i < sc->image_count; i++)
            vkDestroyFramebuffer(device, sc->framebuffers[i], NULL);
        free(sc->framebuffers);
    }

    if (sc->depth_view)
        vkDestroyImageView(device, sc->depth_view, NULL);
    if (sc->depth_image)
        vmaDestroyImage(allocator, sc->depth_image, sc->depth_alloc);

    if (sc->image_views) {
        for (uint32_t i = 0; i < sc->image_count; i++)
            vkDestroyImageView(device, sc->image_views[i], NULL);
        free(sc->image_views);
    }

    free(sc->images);
    vkDestroySwapchainKHR(device, sc->swapchain, NULL);

    memset(sc, 0, sizeof(*sc));
}
