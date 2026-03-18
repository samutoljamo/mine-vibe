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
#include "player_model.h"
#include "hud.h"

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

    /* HUD rendering */
    VkRenderPass      hud_render_pass;
    VkPipeline        hud_pipeline;
    VkPipelineLayout  hud_pipeline_layout;
    VkFramebuffer*    hud_framebuffers;   /* one per swapchain image, color-only */
    VkBuffer          hud_vertex_buffer[MAX_FRAMES_IN_FLIGHT];
    VmaAllocation     hud_vertex_alloc[MAX_FRAMES_IN_FLIGHT];
    VkBuffer          hud_index_buffer[MAX_FRAMES_IN_FLIGHT];
    VmaAllocation     hud_index_alloc[MAX_FRAMES_IN_FLIGHT];
    void*             hud_vb_mapped[MAX_FRAMES_IN_FLIGHT];
    void*             hud_ib_mapped[MAX_FRAMES_IN_FLIGHT];

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
    uint32_t                    last_image_index;   /* set each frame for dump_frame */

    /* UBOs (per frame in flight) */
    VkBuffer                    ubo_buffers[MAX_FRAMES_IN_FLIGHT];
    VmaAllocation               ubo_allocs[MAX_FRAMES_IN_FLIGHT];
    void*                       ubo_mapped[MAX_FRAMES_IN_FLIGHT];

    /* Texture atlas */
    VkImage                     atlas_image;
    VmaAllocation               atlas_alloc;
    VkImageView                 atlas_view;
    VkSampler                   atlas_sampler;

    /* Player skin texture */
    VkImage                     player_skin_image;
    VmaAllocation               player_skin_alloc;
    VkImageView                 player_skin_view;
    VkSampler                   player_skin_sampler;

    /* Player pipeline */
    VkPipelineLayout            player_pipeline_layout;
    VkPipeline                  player_pipeline;

    /* Player descriptor sets (per frame) */
    VkDescriptorSet             player_descriptor_sets[MAX_FRAMES_IN_FLIGHT];

    /* Player model (static mesh) */
    PlayerModel                 player_model;

    /* Window */
    GLFWwindow*                 window;
    bool                        framebuffer_resized;

    /* Remote player placeholder mesh */
    VkBuffer      player_vb;
    VmaAllocation player_vb_alloc;
    VkBuffer      player_ib;
    VmaAllocation player_ib_alloc;
    uint32_t      player_index_count;
} Renderer;

bool renderer_init(Renderer* r, GLFWwindow* window);
void renderer_draw_frame(Renderer* r,
                         ChunkMesh* meshes, uint32_t mesh_count,
                         const PlayerRenderState* players, uint32_t player_count,
                         mat4 view, mat4 proj, vec3 sun_dir,
                         const HUD* hud, bool dump_frame, const char* dump_path);
void renderer_cleanup(Renderer* r);
bool renderer_dump_frame(Renderer* r, const char *path);

/* Draw placeholder boxes for remote players.
 * positions: array of count world-space positions (feet).
 * Uses the existing block pipeline with a static unit-cube mesh.
 * Must be called from within an active render pass (i.e. between
 * renderer_draw_frame's begin/end render pass). */
void renderer_init_player_mesh(Renderer* r);
void renderer_draw_remote_players(Renderer* r,
                                   const float (*positions)[3],
                                   uint32_t count,
                                   mat4 view, mat4 proj);

VkCommandBuffer renderer_begin_single_cmd(Renderer* r);
void            renderer_end_single_cmd(Renderer* r, VkCommandBuffer cmd);

#endif
