#include "renderer.h"
#include "pipeline.h"
#include "player_model.h"
#include "texture.h"
#include "frustum.h"
#include "chunk_mesh.h"
#include "assets.h"
#include "hud.h"
#include "agent.h"
#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Debug callback                                                    */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
{
    (void)type;
    (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

/* ------------------------------------------------------------------ */
/*  Validation layer                                                  */
/* ------------------------------------------------------------------ */

static const char* const VALIDATION_LAYERS[] = { "VK_LAYER_KHRONOS_validation" };
static const uint32_t VALIDATION_LAYER_COUNT = 1;

#ifndef NDEBUG
static bool check_validation_support(void)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    VkLayerProperties* props = malloc(count * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&count, props);

    for (uint32_t i = 0; i < VALIDATION_LAYER_COUNT; i++) {
        bool found = false;
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(VALIDATION_LAYERS[i], props[j].layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            free(props);
            return false;
        }
    }
    free(props);
    return true;
}
#endif

/* ------------------------------------------------------------------ */
/*  Physical device selection                                         */
/* ------------------------------------------------------------------ */

typedef struct QueueFamilyIndices {
    uint32_t graphics;
    uint32_t present;
    bool     has_graphics;
    bool     has_present;
} QueueFamilyIndices;

static QueueFamilyIndices find_queue_families(VkPhysicalDevice pd, VkSurfaceKHR surface)
{
    QueueFamilyIndices indices = { .has_graphics = false, .has_present = false };

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
    VkQueueFamilyProperties* props = malloc(count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props);

    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics     = i;
            indices.has_graphics = true;
        }

        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present_support);
        if (present_support) {
            indices.present     = i;
            indices.has_present = true;
        }

        if (indices.has_graphics && indices.has_present)
            break;
    }

    free(props);
    return indices;
}

static bool device_has_swapchain_ext(VkPhysicalDevice pd)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL);
    VkExtensionProperties* exts = malloc(count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, exts);

    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            found = true;
            break;
        }
    }
    free(exts);
    return found;
}

static bool pick_physical_device(Renderer* r)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(r->instance, &count, NULL);
    if (count == 0) {
        fprintf(stderr, "No Vulkan-capable GPU found\n");
        return false;
    }

    VkPhysicalDevice* devices = malloc(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(r->instance, &count, devices);

    /* Score each device, prefer discrete GPU */
    VkPhysicalDevice best       = VK_NULL_HANDLE;
    int              best_score = -1;

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        QueueFamilyIndices idx = find_queue_families(devices[i], r->surface);
        if (!idx.has_graphics || !idx.has_present)
            continue;
        if (!device_has_swapchain_ext(devices[i]))
            continue;

        int score = 1;  /* any suitable device gets at least 1 */
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 100;

        if (score > best_score) {
            best_score = score;
            best       = devices[i];
        }
    }

    free(devices);

    if (best == VK_NULL_HANDLE) {
        fprintf(stderr, "No suitable GPU found\n");
        return false;
    }

    r->physical_device = best;

    QueueFamilyIndices idx = find_queue_families(best, r->surface);
    r->graphics_family = idx.graphics;
    r->present_family  = idx.present;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    fprintf(stderr, "Selected GPU: %s\n", props.deviceName);

    return true;
}

/* ------------------------------------------------------------------ */
/*  Logical device                                                    */
/* ------------------------------------------------------------------ */

static bool create_logical_device(Renderer* r)
{
    float priority = 1.0f;

    /* Determine unique queue families */
    uint32_t unique_families[2] = { r->graphics_family, r->present_family };
    uint32_t unique_count       = (r->graphics_family == r->present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo queue_cis[2];
    for (uint32_t i = 0; i < unique_count; i++) {
        queue_cis[i] = (VkDeviceQueueCreateInfo){
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i],
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
    }

    const char* device_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkPhysicalDeviceFeatures features = { 0 };

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = unique_count,
        .pQueueCreateInfos       = queue_cis,
        .enabledExtensionCount   = 1,
        .ppEnabledExtensionNames = device_exts,
        .pEnabledFeatures        = &features,
    };

    if (vkCreateDevice(r->physical_device, &ci, NULL, &r->device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        return false;
    }

    volkLoadDevice(r->device);

    vkGetDeviceQueue(r->device, r->graphics_family, 0, &r->graphics_queue);
    vkGetDeviceQueue(r->device, r->present_family,  0, &r->present_queue);

    return true;
}

/* ------------------------------------------------------------------ */
/*  VMA allocator                                                     */
/* ------------------------------------------------------------------ */

static bool create_allocator(Renderer* r)
{
    VmaVulkanFunctions vk_funcs = {
        .vkGetInstanceProcAddr                  = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr                    = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties          = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties    = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory                       = vkAllocateMemory,
        .vkFreeMemory                           = vkFreeMemory,
        .vkMapMemory                            = vkMapMemory,
        .vkUnmapMemory                          = vkUnmapMemory,
        .vkFlushMappedMemoryRanges              = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges         = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory                     = vkBindBufferMemory,
        .vkBindImageMemory                      = vkBindImageMemory,
        .vkGetBufferMemoryRequirements          = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements           = vkGetImageMemoryRequirements,
        .vkCreateBuffer                         = vkCreateBuffer,
        .vkDestroyBuffer                        = vkDestroyBuffer,
        .vkCreateImage                          = vkCreateImage,
        .vkDestroyImage                         = vkDestroyImage,
        .vkCmdCopyBuffer                        = vkCmdCopyBuffer,
        /* Vulkan 1.1 core functions required by VMA.
         * Volk loads the core (non-KHR) variants via volkLoadDevice;
         * the KHR aliases may be NULL if the driver only advertises core 1.1.
         * The signatures are identical, so casting is safe. */
        .vkGetBufferMemoryRequirements2KHR       = (PFN_vkGetBufferMemoryRequirements2KHR)vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR        = (PFN_vkGetImageMemoryRequirements2KHR)vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR                  = (PFN_vkBindBufferMemory2KHR)vkBindBufferMemory2,
        .vkBindImageMemory2KHR                   = (PFN_vkBindImageMemory2KHR)vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = (PFN_vkGetPhysicalDeviceMemoryProperties2KHR)vkGetPhysicalDeviceMemoryProperties2,
    };

    VmaAllocatorCreateInfo ci = {
        .physicalDevice   = r->physical_device,
        .device           = r->device,
        .instance         = r->instance,
        .pVulkanFunctions = &vk_funcs,
        .vulkanApiVersion = VK_API_VERSION_1_1,
    };

    if (vmaCreateAllocator(&ci, &r->allocator) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create VMA allocator\n");
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Render pass                                                       */
/* ------------------------------------------------------------------ */

static bool create_render_pass(Renderer* r)
{
    VkAttachmentDescription color_att = {
        .format         = r->swapchain.image_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentDescription depth_att = {
        .format         = r->swapchain.depth_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentDescription attachments[] = { color_att, depth_att };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
    };

    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments    = attachments,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep,
    };

    if (vkCreateRenderPass(r->device, &ci, NULL, &r->render_pass) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create render pass\n");
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Sync objects                                                      */
/* ------------------------------------------------------------------ */

static bool create_sync_objects(Renderer* r)
{
    /* Command pool */
    VkCommandPoolCreateInfo pool_ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = r->graphics_family,
    };

    if (vkCreateCommandPool(r->device, &pool_ci, NULL, &r->command_pool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create command pool\n");
        return false;
    }

    /* Command buffers */
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = r->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT,
    };

    if (vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate command buffers\n");
        return false;
    }

    /* Semaphores and fences */
    VkSemaphoreCreateInfo sem_ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(r->device, &sem_ci, NULL, &r->image_available_sems[i]) != VK_SUCCESS ||
            vkCreateSemaphore(r->device, &sem_ci, NULL, &r->render_finished_sems[i]) != VK_SUCCESS ||
            vkCreateFence(r->device, &fence_ci, NULL, &r->in_flight_fences[i]) != VK_SUCCESS)
        {
            fprintf(stderr, "Failed to create sync objects\n");
            return false;
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  UBOs                                                              */
/* ------------------------------------------------------------------ */

static bool create_ubos(Renderer* r)
{
    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = sizeof(GlobalUBO),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    };

    VmaAllocationCreateInfo alloc_ci = {
        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VmaAllocationInfo alloc_info;
        if (vmaCreateBuffer(r->allocator, &buf_ci, &alloc_ci,
                            &r->ubo_buffers[i], &r->ubo_allocs[i],
                            &alloc_info) != VK_SUCCESS)
        {
            fprintf(stderr, "Failed to create UBO %d\n", i);
            return false;
        }
        r->ubo_mapped[i] = alloc_info.pMappedData;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Framebuffer resize callback                                       */
/* ------------------------------------------------------------------ */

static void framebuffer_resize_cb(GLFWwindow* window, int width, int height)
{
    (void)width;
    (void)height;
    Renderer* r = (Renderer*)glfwGetWindowUserPointer(window);
    if (r) r->framebuffer_resized = true;
}

/* ------------------------------------------------------------------ */
/*  Public API: init                                                  */
/* ------------------------------------------------------------------ */

bool renderer_init(Renderer* r, GLFWwindow* window)
{
    memset(r, 0, sizeof(*r));
    r->window = window;

    /* --- volk --- */
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "Failed to initialize volk\n");
        return false;
    }

    /* --- Instance --- */
    {
        VkApplicationInfo app_info = {
            .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName   = "Minecraft",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName        = "No Engine",
            .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion         = VK_API_VERSION_1_1,
        };

        uint32_t glfw_ext_count = 0;
        const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

        /* Build extension list: GLFW extensions + debug utils */
        uint32_t ext_count = glfw_ext_count;
#ifndef NDEBUG
        ext_count += 1; /* VK_EXT_DEBUG_UTILS_EXTENSION_NAME */
#endif
        const char** extensions = malloc(ext_count * sizeof(const char*));
        for (uint32_t i = 0; i < glfw_ext_count; i++)
            extensions[i] = glfw_exts[i];
#ifndef NDEBUG
        extensions[glfw_ext_count] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
#endif

        VkInstanceCreateInfo ci = {
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo        = &app_info,
            .enabledExtensionCount   = ext_count,
            .ppEnabledExtensionNames = extensions,
        };

#ifndef NDEBUG
        if (check_validation_support()) {
            ci.enabledLayerCount   = VALIDATION_LAYER_COUNT;
            ci.ppEnabledLayerNames = VALIDATION_LAYERS;
        }
#endif

        VkResult result = vkCreateInstance(&ci, NULL, &r->instance);
        free(extensions);

        if (result != VK_SUCCESS) {
            fprintf(stderr, "Failed to create Vulkan instance\n");
            return false;
        }
    }

    volkLoadInstance(r->instance);

    /* --- Debug messenger --- */
#ifndef NDEBUG
    {
        VkDebugUtilsMessengerCreateInfoEXT ci = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
        };
        vkCreateDebugUtilsMessengerEXT(r->instance, &ci, NULL, &r->debug_messenger);
    }
#endif

    /* --- Surface --- */
    if (glfwCreateWindowSurface(r->instance, window, NULL, &r->surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create window surface\n");
        return false;
    }

    /* --- Physical device --- */
    if (!pick_physical_device(r))
        return false;

    /* --- Logical device --- */
    if (!create_logical_device(r))
        return false;

    /* --- VMA allocator --- */
    if (!create_allocator(r))
        return false;

    /* --- Swapchain + render pass --- */
    {
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);

        /* First pass: create swapchain without framebuffers to get format */
        Swapchain temp_sc;
        if (!swapchain_create(r->physical_device, r->device, r->allocator,
                              r->surface, r->graphics_family, r->present_family,
                              VK_NULL_HANDLE, w, h, VK_NULL_HANDLE, &temp_sc))
        {
            fprintf(stderr, "Failed to create initial swapchain\n");
            return false;
        }

        /* Store format info for render pass, then store into renderer for
           create_render_pass to read */
        r->swapchain = temp_sc;

        /* Create render pass */
        if (!create_render_pass(r)) {
            swapchain_destroy(r->device, r->allocator, &r->swapchain);
            return false;
        }

        /* Destroy temporary swapchain */
        VkSwapchainKHR old_sc = temp_sc.swapchain;
        swapchain_destroy(r->device, r->allocator, &r->swapchain);

        /* Recreate swapchain WITH render pass for framebuffers */
        if (!swapchain_create(r->physical_device, r->device, r->allocator,
                              r->surface, r->graphics_family, r->present_family,
                              r->render_pass, w, h, VK_NULL_HANDLE, &r->swapchain))
        {
            fprintf(stderr, "Failed to create swapchain with framebuffers\n");
            /* old_sc was already destroyed inside swapchain_destroy above */
            (void)old_sc;
            return false;
        }
    }

    /* --- Sync objects (cmd pool, cmd buffers, semaphores, fences) --- */
    if (!create_sync_objects(r))
        return false;

    /* --- UBOs --- */
    if (!create_ubos(r))
        return false;

    /* --- Descriptor set layout --- */
    if (!pipeline_create_descriptor_layout(r->device, &r->descriptor_set_layout))
        return false;

    /* --- Graphics pipeline --- */
    if (!pipeline_create(r->device, r->render_pass, r->descriptor_set_layout,
                         g_block_vert_spv, g_block_vert_spv_size,
                         g_block_frag_spv, g_block_frag_spv_size,
                         &r->pipeline_layout, &r->pipeline))
        return false;

    /* --- Descriptor pool --- */
    {
        VkDescriptorPoolSize pool_sizes[] = {
            {
                .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = MAX_FRAMES_IN_FLIGHT * 2,
            },
            {
                .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = MAX_FRAMES_IN_FLIGHT * 2,
            },
        };

        VkDescriptorPoolCreateInfo pool_ci = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets       = MAX_FRAMES_IN_FLIGHT * 2,
            .poolSizeCount = 2,
            .pPoolSizes    = pool_sizes,
        };

        if (vkCreateDescriptorPool(r->device, &pool_ci, NULL, &r->descriptor_pool) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create descriptor pool\n");
            return false;
        }
    }

    /* --- Allocate descriptor sets --- */
    {
        VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            layouts[i] = r->descriptor_set_layout;

        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = r->descriptor_pool,
            .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
            .pSetLayouts        = layouts,
        };

        if (vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets) != VK_SUCCESS) {
            fprintf(stderr, "Failed to allocate descriptor sets\n");
            return false;
        }
    }

    /* --- Write UBO descriptors (binding 0) --- */
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo buf_info = {
            .buffer = r->ubo_buffers[i],
            .offset = 0,
            .range  = sizeof(GlobalUBO),
        };

        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = r->descriptor_sets[i],
            .dstBinding      = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &buf_info,
        };

        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }

    /* --- Texture atlas --- */
    if (!texture_create_atlas(r))
        return false;

    /* --- Write texture descriptors (binding 1) --- */
    texture_write_descriptors(r);

    /* --- Player skin texture --- */
    if (!texture_create_player_skin(r))
        return false;

    /* --- Player descriptor sets --- */
    {
        VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            layouts[i] = r->descriptor_set_layout;

        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = r->descriptor_pool,
            .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
            .pSetLayouts        = layouts,
        };
        if (vkAllocateDescriptorSets(r->device, &alloc_info,
                                     r->player_descriptor_sets) != VK_SUCCESS) {
            fprintf(stderr, "Failed to allocate player descriptor sets\n");
            return false;
        }
    }

    /* Write UBO binding 0 into player sets */
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo buf_info = {
            .buffer = r->ubo_buffers[i], .offset = 0, .range = sizeof(GlobalUBO),
        };
        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = r->player_descriptor_sets[i],
            .dstBinding      = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &buf_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }

    /* Write skin sampler binding 1 into player sets */
    texture_write_player_skin_descriptors(r, r->player_descriptor_sets);

    /* --- Player pipeline --- */
    if (!player_pipeline_create(r->device, r->render_pass, r->descriptor_set_layout,
                                g_player_vert_spv, g_player_vert_spv_size,
                                g_player_frag_spv, g_player_frag_spv_size,
                                &r->player_pipeline_layout, &r->player_pipeline))
        return false;

    /* --- Player model (static mesh) --- */
    if (!player_model_init(r, &r->player_model))
        return false;

    /* --- Framebuffer resize callback --- */
    glfwSetWindowUserPointer(window, r);
    glfwSetFramebufferSizeCallback(window, framebuffer_resize_cb);

    /* --- UI system --- */
    ui_init(r);

    return true;
}

/* ------------------------------------------------------------------ */
/*  Swapchain recreation                                              */
/* ------------------------------------------------------------------ */

void renderer_recreate_swapchain(Renderer* r)
{
    /* Wait for non-zero framebuffer size (window may be minimized) */
    int w = 0, h = 0;
    glfwGetFramebufferSize(r->window, &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(r->window, &w, &h);
    }

    vkDeviceWaitIdle(r->device);

    /* Save old swapchain */
    Swapchain old = r->swapchain;

    /* Create new swapchain, passing old handle */
    Swapchain new_sc;
    if (!swapchain_create(r->physical_device, r->device, r->allocator,
                          r->surface, r->graphics_family, r->present_family,
                          r->render_pass, w, h, old.swapchain, &new_sc))
    {
        fprintf(stderr, "Failed to recreate swapchain\n");
        return;
    }

    /* Destroy old swapchain resources (framebuffers, image views, depth, then handle) */
    if (old.framebuffers) {
        for (uint32_t i = 0; i < old.image_count; i++)
            vkDestroyFramebuffer(r->device, old.framebuffers[i], NULL);
        free(old.framebuffers);
    }
    if (old.image_views) {
        for (uint32_t i = 0; i < old.image_count; i++)
            vkDestroyImageView(r->device, old.image_views[i], NULL);
        free(old.image_views);
    }
    if (old.depth_view)
        vkDestroyImageView(r->device, old.depth_view, NULL);
    if (old.depth_image)
        vmaDestroyImage(r->allocator, old.depth_image, old.depth_alloc);
    free(old.images);
    vkDestroySwapchainKHR(r->device, old.swapchain, NULL);

    r->swapchain = new_sc;

    /* Rebuild UI framebuffers against new image views */
    ui_on_swapchain_recreate(r);
}

/* ------------------------------------------------------------------ */
/*  Public API: cleanup                                               */
/* ------------------------------------------------------------------ */

void renderer_cleanup(Renderer* r)
{
    /* UI system (calls vkDeviceWaitIdle internally) */
    ui_cleanup(r);

    /* Player placeholder mesh */
    if (r->player_vb)
        vmaDestroyBuffer(r->allocator, r->player_vb, r->player_vb_alloc);
    if (r->player_ib)
        vmaDestroyBuffer(r->allocator, r->player_ib, r->player_ib_alloc);

    /* UBOs */
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (r->ubo_buffers[i])
            vmaDestroyBuffer(r->allocator, r->ubo_buffers[i], r->ubo_allocs[i]);
    }

    /* Atlas (if created) */
    if (r->atlas_sampler)
        vkDestroySampler(r->device, r->atlas_sampler, NULL);
    if (r->atlas_view)
        vkDestroyImageView(r->device, r->atlas_view, NULL);
    if (r->atlas_image)
        vmaDestroyImage(r->allocator, r->atlas_image, r->atlas_alloc);

    /* Player model */
    player_model_destroy(r, &r->player_model);

    /* Player pipeline */
    if (r->player_pipeline)
        vkDestroyPipeline(r->device, r->player_pipeline, NULL);
    if (r->player_pipeline_layout)
        vkDestroyPipelineLayout(r->device, r->player_pipeline_layout, NULL);

    /* Player skin */
    if (r->player_skin_sampler)
        vkDestroySampler(r->device, r->player_skin_sampler, NULL);
    if (r->player_skin_view)
        vkDestroyImageView(r->device, r->player_skin_view, NULL);
    if (r->player_skin_image)
        vmaDestroyImage(r->allocator, r->player_skin_image, r->player_skin_alloc);

    /* Descriptors */
    if (r->descriptor_pool)
        vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    if (r->descriptor_set_layout)
        vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);

    /* Pipeline */
    if (r->pipeline)
        vkDestroyPipeline(r->device, r->pipeline, NULL);
    if (r->pipeline_layout)
        vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);

    /* Sync objects */
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (r->image_available_sems[i])
            vkDestroySemaphore(r->device, r->image_available_sems[i], NULL);
        if (r->render_finished_sems[i])
            vkDestroySemaphore(r->device, r->render_finished_sems[i], NULL);
        if (r->in_flight_fences[i])
            vkDestroyFence(r->device, r->in_flight_fences[i], NULL);
    }

    /* Command pool */
    if (r->command_pool)
        vkDestroyCommandPool(r->device, r->command_pool, NULL);

    /* Render pass */
    if (r->render_pass)
        vkDestroyRenderPass(r->device, r->render_pass, NULL);

    /* Swapchain */
    swapchain_destroy(r->device, r->allocator, &r->swapchain);

    /* VMA */
    if (r->allocator)
        vmaDestroyAllocator(r->allocator);

    /* Device */
    if (r->device)
        vkDestroyDevice(r->device, NULL);

    /* Surface */
    if (r->surface)
        vkDestroySurfaceKHR(r->instance, r->surface, NULL);

    /* Debug messenger */
#ifndef NDEBUG
    if (r->debug_messenger)
        vkDestroyDebugUtilsMessengerEXT(r->instance, r->debug_messenger, NULL);
#endif

    /* Instance */
    if (r->instance)
        vkDestroyInstance(r->instance, NULL);

    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------ */
/*  Single-shot command helpers                                       */
/* ------------------------------------------------------------------ */

VkCommandBuffer renderer_begin_single_cmd(Renderer* r)
{
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = r->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(r->device, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkBeginCommandBuffer(cmd, &begin_info);
    return cmd;
}

void renderer_end_single_cmd(Renderer* r, VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };

    vkQueueSubmit(r->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(r->graphics_queue);

    vkFreeCommandBuffers(r->device, r->command_pool, 1, &cmd);
}
