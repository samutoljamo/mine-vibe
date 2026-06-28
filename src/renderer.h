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
#include "ui/hud.h"

struct Inventory;       /* forward decl, keep includes lean */
struct RaycastHit;

/* Frames the CPU may be ahead of the GPU. Set to 1 to keep CPU and GPU
 * lock-stepped: lower latency, but no CPU/GPU overlap — if either side runs
 * longer than the frame budget the framerate halves. Raise to 2 to get back
 * the overlap at the cost of one frame of input lag. */
#define MAX_FRAMES_IN_FLIGHT 1

typedef struct ChunkMesh ChunkMesh;

/* Runtime graphics settings populated from CLI flags in main.c. Tuned for
 * low-end / integrated GPUs by default; higher quality is opt-in via flags.
 *   render_distance : chunk load radius (plumbed into world_create).
 *   msaa            : requested MSAA sample count (1|2|4|8...); clamped to
 *                     device caps at init. 1 = MSAA off (no resolve pass).
 *   aniso           : requested anisotropic-filtering level (1|4|8|16);
 *                     clamped to device caps. 1 = anisotropy off.
 *   present         : requested present mode; FIFO by default (iGPU-friendly),
 *                     falls back to FIFO if unavailable.
 *   render_scale    : 3D-scene resolution factor (0.25..1.0). 1.0 = render the
 *                     world directly into the swapchain image (the byte-for-byte
 *                     legacy path). Below 1.0 the 3D scene is rendered into an
 *                     offscreen target sized floor(extent*scale) then linearly
 *                     upscale-blitted into the full-res swapchain image; the
 *                     HUD/UI is always drawn at full resolution on top. A big
 *                     fill-rate win on integrated GPUs. */
typedef struct RenderSettings {
    int             render_distance;
    int             msaa;
    int             aniso;
    PresentModePref present;
    float           render_scale;
} RenderSettings;

/* Sensible defaults for an integrated GPU. */
static inline RenderSettings render_settings_default(void) {
    RenderSettings s = { .render_distance = 12, .msaa = 1, .aniso = 4,
                         .present = PRESENT_PREF_FIFO, .render_scale = 1.0f };
    return s;
}

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

    /* MSAA sample count. Picked once at init from physical-device caps —
     * 4 if supported, else 1. Shared between render pass, swapchain (depth
     * + MSAA color images), and all pipelines that draw into the world
     * render pass. The UI pass remains single-sampled. */
    VkSampleCountFlagBits       sample_count;

    /* Requested present mode (from RenderSettings.present). Stashed at init so
     * swapchain recreation (resize) reuses the same preference. Resolved to an
     * actually-supported mode inside swapchain_create (FIFO fallback). */
    PresentModePref             present_pref;

    /* Anisotropic-filtering level for the atlas sampler, resolved from the
     * requested RenderSettings.aniso clamped to device caps. 1.0 = off. */
    float                       max_anisotropy;

    /* Swapchain */
    Swapchain                   swapchain;

    /* Dynamic render-scale (resolution downsampling). When render_scale < 1.0
     * the 3D scene is rendered into an offscreen target at scaled resolution,
     * then linearly upscale-blitted into the full-res swapchain image before the
     * HUD/UI pass. At render_scale == 1.0 none of this is allocated and the
     * legacy direct-to-swapchain path is used unchanged.
     *
     * The offscreen color image is a single-sample blit source (matches the
     * swapchain format). With MSAA, scene_msaa_* is the multisampled color
     * attachment that resolves into scene_color (mirrors the swapchain layout);
     * without MSAA scene_color is the direct color attachment. scene_depth is
     * sized to the scaled extent. scene_framebuffer is built against the same
     * world render_pass (render-pass-compatible: same formats + samples). */
    float                       render_scale;     /* clamped 0.25..1.0 at init */
    bool                        scale_active;      /* render_scale < 1.0 */
    VkExtent2D                  scene_extent;      /* scaled render extent */
    VkImage                     scene_color_image;
    VmaAllocation               scene_color_alloc;
    VkImageView                 scene_color_view;
    VkImage                     scene_depth_image;
    VmaAllocation               scene_depth_alloc;
    VkImageView                 scene_depth_view;
    VkImage                     scene_msaa_image;  /* MSAA only */
    VmaAllocation               scene_msaa_alloc;
    VkImageView                 scene_msaa_view;
    VkFramebuffer               scene_framebuffer;

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

    /* Block-outline pipeline (drawn in the world renderpass after world
     * geometry, before the renderpass ends). */
    VkPipeline       outline_pipeline;
    VkPipelineLayout outline_pipeline_layout;
    VkBuffer         outline_vb[MAX_FRAMES_IN_FLIGHT];
    VmaAllocation    outline_vb_alloc[MAX_FRAMES_IN_FLIGHT];
    void*            outline_vb_mapped[MAX_FRAMES_IN_FLIGHT];
    uint32_t         outline_vert_count;   /* reset to 0 per frame; one block emits 24 verts */

    /* Per-frame perf counters, written by renderer_draw_frame each frame and
     * read back by the caller for the stats overlay. visible_chunks counts the
     * chunk meshes that survived frustum culling and were drawn; draw_calls
     * counts every draw command (chunks, players, outline) issued this frame.
     * culled_chunks counts the uploaded chunk meshes rejected by the frustum
     * test; total_chunks = visible + culled (the candidate set considered this
     * frame), so the overlay can show how effective frustum culling is. */
    uint32_t         stat_visible_chunks;
    uint32_t         stat_culled_chunks;
    uint32_t         stat_total_chunks;
    uint32_t         stat_draw_calls;

    /* Stats overlay control, set by the caller (main.c) before each draw.
     * When show_stats is true and stats_overlay is non-NULL, the per-frame
     * counters above are mirrored into it and hud_draw_stats() renders the
     * overlay in the UI pass. Kept out of the draw-frame signature so the call
     * site stays merge-friendly. */
    bool             show_stats;
    PerfStats*       stats_overlay;
} Renderer;

bool renderer_init(Renderer* r, GLFWwindow* window, RenderSettings settings);
void renderer_draw_frame(Renderer* r,
                         ChunkMesh* meshes, uint32_t mesh_count,
                         const PlayerRenderState* players, uint32_t player_count,
                         mat4 view, mat4 proj, vec3 sun_dir,
                         float day_brightness,  /* 0..1 daylight factor */
                         vec3 sky_color,        /* RGB clear/sky color */
                         const struct Inventory* inventory,
                         int player_health,            /* <0 = no hearts */
                         const struct RaycastHit* target,
                         float underwater,  /* 0..1 fade factor */
                         bool dump_frame, const char* dump_path);
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

/* Emit a wireframe outline for the given block-cell into the current frame's
 * outline vertex buffer. No-op if the per-frame buffer would overflow.
 * Must be called between the per-frame reset (start of renderer_draw_frame)
 * and the world renderpass draw of the outline pipeline. */
void renderer_outline_emit_block(Renderer* r, int x, int y, int z);

#endif
