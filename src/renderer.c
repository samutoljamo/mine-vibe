#include "renderer.h"
#include "pipeline.h"
#include "texture.h"
#include "frustum.h"
#include "chunk_mesh.h"
#include "hud.h"
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

static bool create_hud_framebuffers(Renderer* r)
{
    uint32_t n = r->swapchain.image_count;
    r->hud_framebuffers = calloc(n, sizeof(VkFramebuffer));
    if (!r->hud_framebuffers) return false;

    for (uint32_t i = 0; i < n; i++) {
        VkFramebufferCreateInfo fb_ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = r->hud_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &r->swapchain.image_views[i],
            .width           = r->swapchain.extent.width,
            .height          = r->swapchain.extent.height,
            .layers          = 1,
        };
        if (vkCreateFramebuffer(r->device, &fb_ci, NULL,
                                &r->hud_framebuffers[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create HUD framebuffer %u\n", i);
            return false;
        }
    }
    return true;
}

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

    /* --- HUD render pass --- */
    /* HUD render pass: color-only, loads existing image, outputs PRESENT_SRC */
    {
        VkAttachmentDescription hud_color = {
            .format         = r->swapchain.image_format,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };
        VkAttachmentReference hud_color_ref = { .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription hud_subpass = {
            .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &hud_color_ref,
        };
        VkSubpassDependency hud_dep = {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        VkRenderPassCreateInfo hud_rp_ci = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &hud_color,
            .subpassCount    = 1,
            .pSubpasses      = &hud_subpass,
            .dependencyCount = 1,
            .pDependencies   = &hud_dep,
        };
        if (vkCreateRenderPass(r->device, &hud_rp_ci, NULL,
                               &r->hud_render_pass) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create HUD render pass\n");
            return false;
        }
    }
    if (!create_hud_framebuffers(r)) {
        fprintf(stderr, "Failed to create HUD framebuffers\n");
        return false;
    }

    /* HUD pipeline */
    {
        VkShaderModule vert_mod = pipeline_load_shader_module(r->device,
            "build/shaders/hud.vert.spv");
        VkShaderModule frag_mod = pipeline_load_shader_module(r->device,
            "build/shaders/hud.frag.spv");
        if (!vert_mod || !frag_mod) {
            fprintf(stderr, "Failed to load HUD shaders\n");
            if (vert_mod) vkDestroyShaderModule(r->device, vert_mod, NULL);
            if (frag_mod) vkDestroyShaderModule(r->device, frag_mod, NULL);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage  = VK_SHADER_STAGE_VERTEX_BIT,
              .module = vert_mod, .pName = "main" },
            { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = frag_mod, .pName = "main" },
        };

        VkVertexInputBindingDescription bind = {
            .binding = 0, .stride = sizeof(HudVertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        VkVertexInputAttributeDescription attrs[2] = {
            { .location=0, .binding=0,
              .format=VK_FORMAT_R32G32_SFLOAT, .offset=0 },           /* x,y */
            { .location=1, .binding=0,
              .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=8 },     /* r,g,b,a */
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount   = 1, .pVertexBindingDescriptions   = &bind,
            .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs,
        };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn_states,
        };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode    = VK_CULL_MODE_NONE,
            .frontFace   = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth   = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState blend_att = {
            .blendEnable         = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp        = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp        = VK_BLEND_OP_ADD,
            .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo blend = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &blend_att,
        };

        /* No depth/stencil state needed */
        VkPipelineLayoutCreateInfo layout_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        if (vkCreatePipelineLayout(r->device, &layout_ci, NULL,
                                   &r->hud_pipeline_layout) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create HUD pipeline layout\n");
            vkDestroyShaderModule(r->device, vert_mod, NULL);
            vkDestroyShaderModule(r->device, frag_mod, NULL);
            return false;
        }

        VkGraphicsPipelineCreateInfo pipe_ci = {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2,
            .pStages             = stages,
            .pVertexInputState   = &vi,
            .pInputAssemblyState = &ia,
            .pViewportState      = &vp,
            .pRasterizationState = &rs,
            .pMultisampleState   = &ms,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dyn,
            .layout              = r->hud_pipeline_layout,
            .renderPass          = r->hud_render_pass,
            .subpass             = 0,
        };
        if (vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipe_ci, NULL,
                                      &r->hud_pipeline) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create HUD pipeline\n");
            vkDestroyPipelineLayout(r->device, r->hud_pipeline_layout, NULL);
            vkDestroyShaderModule(r->device, vert_mod, NULL);
            vkDestroyShaderModule(r->device, frag_mod, NULL);
            return false;
        }

        vkDestroyShaderModule(r->device, vert_mod, NULL);
        vkDestroyShaderModule(r->device, frag_mod, NULL);
    }

    /* HUD vertex buffer — host-visible, persistently mapped */
    {
        VkBufferCreateInfo vb_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = HUD_MAX_VERTS * sizeof(HudVertex),
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo vb_alloc_ci = {
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo vb_info;
        if (vmaCreateBuffer(r->allocator, &vb_ci, &vb_alloc_ci,
                            &r->hud_vertex_buffer, &r->hud_vertex_alloc, &vb_info) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create HUD vertex buffer\n");
            return false;
        }
        r->hud_vb_mapped = vb_info.pMappedData;

        VkBufferCreateInfo ib_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = HUD_MAX_INDICES * sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationInfo ib_info;
        if (vmaCreateBuffer(r->allocator, &ib_ci, &vb_alloc_ci,
                            &r->hud_index_buffer, &r->hud_index_alloc, &ib_info) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create HUD index buffer\n");
            return false;
        }
        r->hud_ib_mapped = ib_info.pMappedData;
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
                         "build/shaders/block.vert.spv",
                         "build/shaders/block.frag.spv",
                         &r->pipeline_layout, &r->pipeline))
        return false;

    /* --- Descriptor pool --- */
    {
        VkDescriptorPoolSize pool_sizes[] = {
            {
                .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = MAX_FRAMES_IN_FLIGHT,
            },
            {
                .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = MAX_FRAMES_IN_FLIGHT,
            },
        };

        VkDescriptorPoolCreateInfo pool_ci = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets       = MAX_FRAMES_IN_FLIGHT,
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

    /* --- Framebuffer resize callback --- */
    glfwSetWindowUserPointer(window, r);
    glfwSetFramebufferSizeCallback(window, framebuffer_resize_cb);

    return true;
}

/* ------------------------------------------------------------------ */
/*  Swapchain recreation                                              */
/* ------------------------------------------------------------------ */

static void recreate_swapchain(Renderer* r)
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

    /* Rebuild HUD framebuffers against new image views */
    if (r->hud_framebuffers) {
        for (uint32_t i = 0; i < old.image_count; i++)
            vkDestroyFramebuffer(r->device, r->hud_framebuffers[i], NULL);
        free(r->hud_framebuffers);
        r->hud_framebuffers = NULL;
    }
    if (!create_hud_framebuffers(r))
        fprintf(stderr, "Failed to recreate HUD framebuffers\n");
}

/* ------------------------------------------------------------------ */
/*  Public API: draw frame                                            */
/* ------------------------------------------------------------------ */

void renderer_draw_frame(Renderer* r, ChunkMesh* meshes, uint32_t mesh_count,
                         mat4 view, mat4 proj, vec3 sun_dir,
                         const HUD* hud, bool dump_frame, const char* dump_path)
{
    uint32_t fi = r->current_frame;

    /* 1. Wait for current frame's fence */
    vkWaitForFences(r->device, 1, &r->in_flight_fences[fi], VK_TRUE, UINT64_MAX);

    /* 2. Acquire next swapchain image */
    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(r->device, r->swapchain.swapchain,
                                            UINT64_MAX,
                                            r->image_available_sems[fi],
                                            VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(r);
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swapchain image\n");
        return;
    }
    r->last_image_index = image_index;

    /* 3. Reset fence (only after we know we will submit work) */
    vkResetFences(r->device, 1, &r->in_flight_fences[fi]);

    /* 4. Update UBO */
    GlobalUBO ubo;
    glm_mat4_copy(view, ubo.view);
    glm_mat4_copy(proj, ubo.proj);
    ubo.sun_direction[0] = sun_dir[0];
    ubo.sun_direction[1] = sun_dir[1];
    ubo.sun_direction[2] = sun_dir[2];
    ubo.sun_direction[3] = 0.0f;
    ubo.sun_color[0] = 1.0f;
    ubo.sun_color[1] = 1.0f;
    ubo.sun_color[2] = 1.0f;
    ubo.sun_color[3] = 1.0f;
    ubo.ambient = 0.3f;
    memcpy(r->ubo_mapped[fi], &ubo, sizeof(ubo));

    /* 5. Record command buffer */
    VkCommandBuffer cmd = r->command_buffers[fi];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    /* Dynamic viewport and scissor — hoisted so HUD pass can reuse them */
    VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float)r->swapchain.extent.width,
        .height   = (float)r->swapchain.extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = r->swapchain.extent,
    };

    VkClearValue clear_values[2] = {
        { .color = { .float32 = { 0.53f, 0.81f, 0.92f, 1.0f } } },
        { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
    };

    VkRenderPassBeginInfo rp_info = {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass  = r->render_pass,
        .framebuffer = r->swapchain.framebuffers[image_index],
        .renderArea  = {
            .offset = { 0, 0 },
            .extent = r->swapchain.extent,
        },
        .clearValueCount = 2,
        .pClearValues    = clear_values,
    };

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    /* Bind descriptor set */
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_layout, 0, 1,
                            &r->descriptor_sets[fi], 0, NULL);

    /* 6. Draw loop: frustum-culled chunk meshes */
    if (meshes && mesh_count > 0) {
        /* Extract frustum from view-projection matrix */
        mat4 vp;
        glm_mat4_mul(proj, view, vp);
        Frustum frustum;
        frustum_extract(vp, &frustum);

        for (uint32_t i = 0; i < mesh_count; i++) {
            ChunkMesh* m = &meshes[i];

            if (!m->uploaded || m->index_count == 0)
                continue;

            /* Frustum cull */
            if (!frustum_test_aabb(&frustum, m->aabb_min, m->aabb_max))
                continue;

            /* Push chunk offset */
            ChunkPushConstants pc;
            pc.chunk_offset[0] = m->chunk_origin[0];
            pc.chunk_offset[1] = m->chunk_origin[1];
            pc.chunk_offset[2] = m->chunk_origin[2];
            pc.chunk_offset[3] = 0.0f;
            vkCmdPushConstants(cmd, r->pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(ChunkPushConstants), &pc);

            /* Bind vertex and index buffers */
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);

            /* Draw */
            vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        }
    }

    /* 7. End render pass */
    vkCmdEndRenderPass(cmd);

    /* HUD renderpass */
    if (hud) {
        HudVertex hud_verts[HUD_MAX_VERTS];
        uint32_t  hud_idx[HUD_MAX_INDICES];
        float sw = (float)r->swapchain.extent.width;
        float sh = (float)r->swapchain.extent.height;
        uint32_t vc = hud_build(hud, sw, sh, hud_verts, hud_idx);
        uint32_t ic = vc / 4 * 6;   /* 4 verts → 6 indices per quad */

        memcpy(r->hud_vb_mapped, hud_verts, vc * sizeof(HudVertex));
        memcpy(r->hud_ib_mapped, hud_idx,   ic * sizeof(uint32_t));

        VkRenderPassBeginInfo hud_rp = {
            .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass  = r->hud_render_pass,
            .framebuffer = r->hud_framebuffers[image_index],
            .renderArea  = { .offset = {0,0}, .extent = r->swapchain.extent },
            .clearValueCount = 0,
        };
        vkCmdBeginRenderPass(cmd, &hud_rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->hud_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (vc > 0) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &r->hud_vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, r->hud_index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, ic, 1, 0, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
    }

    vkEndCommandBuffer(cmd);

    /* 8. Submit */
    VkSemaphore wait_sems[]   = { r->image_available_sems[fi] };
    VkSemaphore signal_sems[] = { r->render_finished_sems[fi] };
    VkPipelineStageFlags wait_stages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    };

    VkSubmitInfo submit_info = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = wait_stages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = signal_sems,
    };

    if (vkQueueSubmit(r->graphics_queue, 1, &submit_info,
                      r->in_flight_fences[fi]) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to submit draw command buffer\n");
        return;
    }

    /* Handle frame dump (after submit: image is in PRESENT_SRC_KHR; before present) */
    if (dump_frame && dump_path && dump_path[0] != '\0') {
        if (renderer_dump_frame(r, dump_path))
            agent_emit_frame_saved(dump_path);
        else
            agent_emit_error("frame capture failed");
    }

    /* 9. Present */
    VkPresentInfoKHR present_info = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = signal_sems,
        .swapchainCount     = 1,
        .pSwapchains        = &r->swapchain.swapchain,
        .pImageIndices      = &image_index,
    };

    result = vkQueuePresentKHR(r->present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        r->framebuffer_resized)
    {
        r->framebuffer_resized = false;
        recreate_swapchain(r);
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swapchain image\n");
    }

    /* 10. Advance current frame */
    r->current_frame = (fi + 1) % MAX_FRAMES_IN_FLIGHT;
}

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

/* ------------------------------------------------------------------ */
/*  Public API: cleanup                                               */
/* ------------------------------------------------------------------ */

void renderer_cleanup(Renderer* r)
{
    vkDeviceWaitIdle(r->device);

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

    /* HUD pipeline and buffers */
    if (r->hud_pipeline)
        vkDestroyPipeline(r->device, r->hud_pipeline, NULL);
    if (r->hud_pipeline_layout)
        vkDestroyPipelineLayout(r->device, r->hud_pipeline_layout, NULL);
    if (r->hud_vertex_buffer)
        vmaDestroyBuffer(r->allocator, r->hud_vertex_buffer, r->hud_vertex_alloc);
    if (r->hud_index_buffer)
        vmaDestroyBuffer(r->allocator, r->hud_index_buffer, r->hud_index_alloc);

    /* HUD renderpass and framebuffers */
    if (r->hud_framebuffers) {
        for (uint32_t i = 0; i < r->swapchain.image_count; i++)
            vkDestroyFramebuffer(r->device, r->hud_framebuffers[i], NULL);
        free(r->hud_framebuffers);
        r->hud_framebuffers = NULL;
    }
    if (r->hud_render_pass)
        vkDestroyRenderPass(r->device, r->hud_render_pass, NULL);

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
