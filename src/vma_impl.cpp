// VMA implementation translation unit (C++)
// Must define these BEFORE including VMA so it doesn't try to load Vulkan functions itself.
// We provide them manually via VmaVulkanFunctions populated from volk.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <volk.h>
#include <vk_mem_alloc.h>
