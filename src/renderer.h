#ifndef RENDERER_H
#define RENDERER_H

#include <volk.h>
#include <vk_mem_alloc.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "swapchain.h"
#include "vertex.h"

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct ChunkMesh ChunkMesh;

typedef struct Renderer {
    /* Core Vulkan */
    VkInstance                  instance;
    VkDebugUtilsMessengerEXT   debug_messenger;
    VkSurfaceKHR               surface;
    VkPhysicalDevice            physical_device;
    VkDevice                    device;

    /* Queues */
    VkQueue                     graphics_queue;
    VkQueue                     present_queue;
    uint32_t                    graphics_family;
    uint32_t                    present_family;

    /* Memory allocator */
    VmaAllocator                allocator;

    /* Swapchain */
    Swapchain                   swapchain;

    /* Render pass */
    VkRenderPass                render_pass;

    /* Pipeline */
    VkPipelineLayout            pipeline_layout;
    VkPipeline                  pipeline;

    /* Descriptors */
    VkDescriptorSetLayout       descriptor_set_layout;
    VkDescriptorPool            descriptor_pool;
    VkDescriptorSet             descriptor_sets[MAX_FRAMES_IN_FLIGHT];

    /* Command pool and buffers */
    VkCommandPool               command_pool;
    VkCommandBuffer             command_buffers[MAX_FRAMES_IN_FLIGHT];

    /* Sync objects */
    VkSemaphore                 image_available_sems[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore                 render_finished_sems[MAX_FRAMES_IN_FLIGHT];
    VkFence                     in_flight_fences[MAX_FRAMES_IN_FLIGHT];
    uint32_t                    current_frame;

    /* UBOs (per frame in flight) */
    VkBuffer                    ubo_buffers[MAX_FRAMES_IN_FLIGHT];
    VmaAllocation               ubo_allocs[MAX_FRAMES_IN_FLIGHT];
    void*                       ubo_mapped[MAX_FRAMES_IN_FLIGHT];

    /* Texture atlas */
    VkImage                     atlas_image;
    VmaAllocation               atlas_alloc;
    VkImageView                 atlas_view;
    VkSampler                   atlas_sampler;

    /* Window */
    GLFWwindow*                 window;
    bool                        framebuffer_resized;
} Renderer;

bool renderer_init(Renderer* r, GLFWwindow* window);
void renderer_draw_frame(Renderer* r, ChunkMesh* meshes, uint32_t mesh_count,
                         mat4 view, mat4 proj, vec3 sun_dir);
void renderer_cleanup(Renderer* r);

VkCommandBuffer renderer_begin_single_cmd(Renderer* r);
void            renderer_end_single_cmd(Renderer* r, VkCommandBuffer cmd);

#endif
