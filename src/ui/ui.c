#include "../renderer.h"
#include "ui.h"
#include "../pipeline.h"
#include "font_data.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Atlas constants                                                    */
/* ------------------------------------------------------------------ */

#define ATLAS_W      512
#define ATLAS_H      512
#define ATLAS_BAKE_PX 20.0f
#define GLYPH_FIRST  32
#define GLYPH_COUNT  95   /* ASCII 32-126 */

typedef struct {
    float u0, v0, u1, v1;
    float advance, bearing_x, bearing_y;
    float width, height;
} UiGlyph;

/* ------------------------------------------------------------------ */
/*  Module globals                                                     */
/* ------------------------------------------------------------------ */

static UiGlyph   g_glyphs[GLYPH_COUNT];
static uint8_t   g_atlas_cpu[ATLAS_W * ATLAS_H];  /* R8 bitmap */
static bool      g_font_baked = false;

/* Vulkan objects — populated by ui_init */
static VkDevice          g_device;
static VmaAllocator      g_allocator;
static VkRenderPass      g_render_pass;
static VkPipeline        g_pipeline;
static VkPipelineLayout  g_pipeline_layout;
static VkFramebuffer*    g_framebuffers;      /* one per swapchain image */
static uint32_t          g_framebuffer_count;
static VkBuffer          g_vb[MAX_FRAMES_IN_FLIGHT];
static VmaAllocation     g_vb_alloc[MAX_FRAMES_IN_FLIGHT];
static VkBuffer          g_ib[MAX_FRAMES_IN_FLIGHT];
static VmaAllocation     g_ib_alloc[MAX_FRAMES_IN_FLIGHT];
static void*             g_vb_mapped[MAX_FRAMES_IN_FLIGHT];
static void*             g_ib_mapped[MAX_FRAMES_IN_FLIGHT];
static VkImage           g_atlas_image;
static VmaAllocation     g_atlas_alloc;
static VkImageView       g_atlas_view;
static VkSampler         g_sampler;
static VkDescriptorSetLayout g_dsl;
static VkDescriptorPool  g_desc_pool;
static VkDescriptorSet   g_desc_sets[MAX_FRAMES_IN_FLIGHT];

/* Per-frame draw state */
static UiVertex  g_verts[UI_MAX_VERTS];
static uint32_t  g_indices[UI_MAX_INDICES];
static uint32_t  g_vert_count;
static uint32_t  g_idx_count;
static float     g_screen_w;
static float     g_screen_h;
static VkCommandBuffer g_cmd;
static int       g_frame_index;
static uint32_t  g_image_index;

/* ------------------------------------------------------------------ */
/*  Font baking (no Vulkan)                                            */
/* ------------------------------------------------------------------ */

bool ui_font_bake(void)
{
    if (g_font_baked) return true;

    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, g_atlas_cpu, ATLAS_W, ATLAS_H, ATLAS_W, 1, NULL)) {
        fprintf(stderr, "ui_font_bake: stbtt_PackBegin failed\n");
        return false;
    }

    stbtt_packedchar packed[GLYPH_COUNT];
    stbtt_PackFontRange(&pc, ui_font_data, 0, ATLAS_BAKE_PX,
                        GLYPH_FIRST, GLYPH_COUNT, packed);
    stbtt_PackEnd(&pc);

    /* Set white pixel region (top-left 2×2) AFTER packing — PackBegin zeroes the buffer */
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            g_atlas_cpu[y * ATLAS_W + x] = 0xFF;

    /* Build glyph metric table */
    for (int i = 0; i < GLYPH_COUNT; i++) {
        stbtt_packedchar* p = &packed[i];
        g_glyphs[i].u0       = (float)p->x0 / ATLAS_W;
        g_glyphs[i].v0       = (float)p->y0 / ATLAS_H;
        g_glyphs[i].u1       = (float)p->x1 / ATLAS_W;
        g_glyphs[i].v1       = (float)p->y1 / ATLAS_H;
        g_glyphs[i].width    = (float)(p->x1 - p->x0);
        g_glyphs[i].height   = (float)(p->y1 - p->y0);
        g_glyphs[i].bearing_x = p->xoff;
        g_glyphs[i].bearing_y = p->yoff;
        g_glyphs[i].advance   = p->xadvance;
    }

    g_font_baked = true;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Vulkan init                                                        */
/* ------------------------------------------------------------------ */

void ui_init(struct Renderer* r)
{
    g_device    = r->device;
    g_allocator = r->allocator;

    /* Bake font atlas (CPU side) */
    if (!ui_font_bake()) {
        fprintf(stderr, "ui_init: font bake failed\n");
        return;
    }

    /* ---- Atlas texture ---- */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8_UNORM,
        .extent        = { ATLAS_W, ATLAS_H, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo img_alloc_ci = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    vmaCreateImage(r->allocator, &img_ci, &img_alloc_ci,
                   &g_atlas_image, &g_atlas_alloc, NULL);

    /* Upload atlas via staging buffer */
    VkDeviceSize atlas_size = ATLAS_W * ATLAS_H;
    VkBuffer      staging;
    VmaAllocation staging_alloc;
    VkBufferCreateInfo stg_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = atlas_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo stg_alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo stg_info;
    vmaCreateBuffer(r->allocator, &stg_ci, &stg_alloc_ci,
                    &staging, &staging_alloc, &stg_info);
    memcpy(stg_info.pMappedData, g_atlas_cpu, atlas_size);

    /* One-time command buffer for layout transition + copy */
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);

    /* UNDEFINED → TRANSFER_DST_OPTIMAL */
    VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = g_atlas_image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    /* Copy buffer → image */
    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { ATLAS_W, ATLAS_H, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, g_atlas_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL */
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    renderer_end_single_cmd(r, cmd);
    vmaDestroyBuffer(r->allocator, staging, staging_alloc);

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = g_atlas_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCreateImageView(r->device, &view_ci, NULL, &g_atlas_view);

    /* Sampler */
    VkSamplerCreateInfo samp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    vkCreateSampler(r->device, &samp_ci, NULL, &g_sampler);

    /* ---- Descriptor set layout ---- */
    VkDescriptorSetLayoutBinding atlas_bind = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dsl_ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &atlas_bind,
    };
    vkCreateDescriptorSetLayout(r->device, &dsl_ci, NULL, &g_dsl);

    /* ---- Descriptor pool (UI-private) ---- */
    VkDescriptorPoolSize pool_sz = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_FRAMES_IN_FLIGHT,
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = MAX_FRAMES_IN_FLIGHT,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_sz,
    };
    vkCreateDescriptorPool(r->device, &pool_ci, NULL, &g_desc_pool);

    /* ---- Allocate + update descriptor sets ---- */
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) layouts[i] = g_dsl;
    VkDescriptorSetAllocateInfo alloc_i = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = g_desc_pool,
        .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
        .pSetLayouts        = layouts,
    };
    vkAllocateDescriptorSets(r->device, &alloc_i, g_desc_sets);

    VkDescriptorImageInfo img_info = {
        .sampler     = g_sampler,
        .imageView   = g_atlas_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkWriteDescriptorSet w = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_desc_sets[i],
            .dstBinding      = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo      = &img_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &w, 0, NULL);
    }

    /* ---- Render pass (load swapchain image → present) ---- */
    VkAttachmentDescription color_att = {
        .format         = r->swapchain.image_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rp_ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &color_att,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep,
    };
    vkCreateRenderPass(r->device, &rp_ci, NULL, &g_render_pass);

    /* ---- Framebuffers (one per swapchain image) ---- */
    g_framebuffer_count = r->swapchain.image_count;
    g_framebuffers = calloc(g_framebuffer_count, sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < g_framebuffer_count; i++) {
        VkFramebufferCreateInfo fb_ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &r->swapchain.image_views[i],
            .width           = r->swapchain.extent.width,
            .height          = r->swapchain.extent.height,
            .layers          = 1,
        };
        vkCreateFramebuffer(r->device, &fb_ci, NULL, &g_framebuffers[i]);
    }

    /* ---- Pipeline ---- */
    VkShaderModule vert_mod = pipeline_load_shader_module(r->device,
        "build/shaders/ui.vert.spv");
    VkShaderModule frag_mod = pipeline_load_shader_module(r->device,
        "build/shaders/ui.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vert_mod, .pName = "main" },
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = frag_mod, .pName = "main" },
    };

    VkVertexInputBindingDescription bind = {
        .binding   = 0,
        .stride    = sizeof(UiVertex),  /* 32 bytes */
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attrs[3] = {
        { .location=0, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=0  }, /* pos */
        { .location=1, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=8  }, /* uv  */
        { .location=2, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=16 }, /* color */
    };

    VkPipelineVertexInputStateCreateInfo vert_in = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1, .pVertexBindingDescriptions   = &bind,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
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
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states,
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &g_dsl,
    };
    vkCreatePipelineLayout(r->device, &layout_ci, NULL, &g_pipeline_layout);

    VkGraphicsPipelineCreateInfo pipe_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vert_in,
        .pInputAssemblyState = &ia,
        .pViewportState      = &vp,
        .pRasterizationState = &rast,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dyn,
        .layout              = g_pipeline_layout,
        .renderPass          = g_render_pass,
        .subpass             = 0,
    };
    vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipe_ci, NULL, &g_pipeline);

    vkDestroyShaderModule(r->device, vert_mod, NULL);
    vkDestroyShaderModule(r->device, frag_mod, NULL);

    /* ---- Per-frame vertex/index buffers ---- */
    for (int fi = 0; fi < MAX_FRAMES_IN_FLIGHT; fi++) {
        VkBufferCreateInfo vb_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = UI_MAX_VERTS * sizeof(UiVertex),
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        };
        VmaAllocationCreateInfo vb_alloc_ci = {
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo vb_info;
        vmaCreateBuffer(r->allocator, &vb_ci, &vb_alloc_ci,
                        &g_vb[fi], &g_vb_alloc[fi], &vb_info);
        g_vb_mapped[fi] = vb_info.pMappedData;

        VkBufferCreateInfo ib_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = UI_MAX_INDICES * sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        };
        VmaAllocationInfo ib_info;
        vmaCreateBuffer(r->allocator, &ib_ci, &vb_alloc_ci,
                        &g_ib[fi], &g_ib_alloc[fi], &ib_info);
        g_ib_mapped[fi] = ib_info.pMappedData;
    }
}

void ui_cleanup(struct Renderer* r)
{
    vkDeviceWaitIdle(r->device);
    for (uint32_t i = 0; i < g_framebuffer_count; i++)
        vkDestroyFramebuffer(r->device, g_framebuffers[i], NULL);
    free(g_framebuffers); g_framebuffers = NULL;
    for (int fi = 0; fi < MAX_FRAMES_IN_FLIGHT; fi++) {
        vmaDestroyBuffer(r->allocator, g_vb[fi], g_vb_alloc[fi]);
        vmaDestroyBuffer(r->allocator, g_ib[fi], g_ib_alloc[fi]);
    }
    vkDestroyPipeline(r->device, g_pipeline, NULL);
    vkDestroyPipelineLayout(r->device, g_pipeline_layout, NULL);
    vkDestroyRenderPass(r->device, g_render_pass, NULL);
    vkDestroyDescriptorPool(r->device, g_desc_pool, NULL);
    vkDestroyDescriptorSetLayout(r->device, g_dsl, NULL);
    vkDestroyImageView(r->device, g_atlas_view, NULL);
    vkDestroySampler(r->device, g_sampler, NULL);
    vmaDestroyImage(r->allocator, g_atlas_image, g_atlas_alloc);
}

void ui_on_swapchain_recreate(struct Renderer* r)
{
    for (uint32_t i = 0; i < g_framebuffer_count; i++)
        vkDestroyFramebuffer(r->device, g_framebuffers[i], NULL);
    free(g_framebuffers);
    g_framebuffer_count = r->swapchain.image_count;
    g_framebuffers = calloc(g_framebuffer_count, sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < g_framebuffer_count; i++) {
        VkFramebufferCreateInfo fb_ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &r->swapchain.image_views[i],
            .width           = r->swapchain.extent.width,
            .height          = r->swapchain.extent.height,
            .layers          = 1,
        };
        vkCreateFramebuffer(r->device, &fb_ci, NULL, &g_framebuffers[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Frame lifecycle                                                    */
/* ------------------------------------------------------------------ */

void ui_frame_begin(VkCommandBuffer cmd, uint32_t image_index,
                    int frame_index, float sw, float sh)
{
    g_cmd         = cmd;
    g_image_index = image_index;
    g_frame_index = frame_index;
    g_screen_w    = sw;
    g_screen_h    = sh;
    g_vert_count  = 0;
    g_idx_count   = 0;
}

void ui_frame_end(void)
{
    if (!g_vb_mapped[0]) return;  /* ui_init not completed */
    int fi = g_frame_index;

    /* Upload geometry */
    memcpy(g_vb_mapped[fi], g_verts,   g_vert_count * sizeof(UiVertex));
    memcpy(g_ib_mapped[fi], g_indices, g_idx_count  * sizeof(uint32_t));

    VkRenderPassBeginInfo rp_begin = {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass  = g_render_pass,
        .framebuffer = g_framebuffers[g_image_index],
        .renderArea  = { .offset = {0,0},
                         .extent = { (uint32_t)g_screen_w, (uint32_t)g_screen_h } },
        .clearValueCount = 0,
    };
    vkCmdBeginRenderPass(g_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    VkViewport viewport = { 0, 0, g_screen_w, g_screen_h, 0.0f, 1.0f };
    VkRect2D   scissor  = { {0,0}, { (uint32_t)g_screen_w, (uint32_t)g_screen_h } };
    vkCmdSetViewport(g_cmd, 0, 1, &viewport);
    vkCmdSetScissor(g_cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_pipeline_layout, 0, 1, &g_desc_sets[fi], 0, NULL);

    if (g_vert_count > 0) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_vb[fi], &offset);
        vkCmdBindIndexBuffer(g_cmd, g_ib[fi], 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g_cmd, g_idx_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(g_cmd);
}

/* ------------------------------------------------------------------ */
/*  Input                                                              */
/* ------------------------------------------------------------------ */

void ui_set_input(float mx, float my, bool pressed, bool released)
{
    /* TODO: Plan 2 */
    (void)mx; (void)my; (void)pressed; (void)released;
}

/* ------------------------------------------------------------------ */
/*  Draw primitives                                                    */
/* ------------------------------------------------------------------ */

static void emit_quad(float px, float py, float pw, float ph,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a)
{
    if (g_vert_count + 4 > UI_MAX_VERTS)   return;
    if (g_idx_count  + 6 > UI_MAX_INDICES) return;

    float x0 = (px        / g_screen_w) * 2.0f - 1.0f;
    float y0 = (py        / g_screen_h) * 2.0f - 1.0f;
    float x1 = ((px + pw) / g_screen_w) * 2.0f - 1.0f;
    float y1 = ((py + ph) / g_screen_h) * 2.0f - 1.0f;

    uint32_t base = g_vert_count;
    UiVertex verts[4] = {
        {x0, y0, u0, v0, r, g, b, a},
        {x1, y0, u1, v0, r, g, b, a},
        {x1, y1, u1, v1, r, g, b, a},
        {x0, y1, u0, v1, r, g, b, a},
    };
    memcpy(g_verts + g_vert_count, verts, sizeof(verts));
    g_vert_count += 4;

    uint32_t idx[6] = {base, base+1, base+2, base+2, base+3, base};
    memcpy(g_indices + g_idx_count, idx, sizeof(idx));
    g_idx_count += 6;
}

void ui_rect(float x, float y, float w, float h, vec4 color)
{
    /* UV = center of 2x2 white pixel region = (1/512, 1/512) */
    float wp = 1.0f / ATLAS_W;
    emit_quad(x, y, w, h, wp, wp, wp, wp,
              color[0], color[1], color[2], color[3]);
}

void ui_text(float x, float y, float size, const char* text, vec4 color)
{
    if (!g_font_baked) return;
    float scale = size / ATLAS_BAKE_PX;
    float cx = x;
    for (const char* p = text; *p; p++) {
        int ci = (unsigned char)*p - GLYPH_FIRST;
        if (ci < 0 || ci >= GLYPH_COUNT) {
            cx += 8.0f * scale;  /* advance for unknown chars */
            continue;
        }
        UiGlyph* g = &g_glyphs[ci];
        float gx = cx + g->bearing_x * scale;
        float gy = y  + g->bearing_y * scale;
        float gw = g->width  * scale;
        float gh = g->height * scale;
        emit_quad(gx, gy, gw, gh,
                  g->u0, g->v0, g->u1, g->v1,
                  color[0], color[1], color[2], color[3]);
        cx += g->advance * scale;
    }
}

float ui_text_width(const char* text, float size)
{
    if (!g_font_baked) return 0.0f;
    float scale = size / ATLAS_BAKE_PX;
    float w = 0.0f;
    for (const char* p = text; *p; p++) {
        int ci = (unsigned char)*p - GLYPH_FIRST;
        if (ci < 0 || ci >= GLYPH_COUNT) { w += 8.0f * scale; continue; } /* match ui_text fallback */
        w += g_glyphs[ci].advance * scale;
    }
    return w;
}

/* ------------------------------------------------------------------ */
/*  Layout stubs (Plan 2)                                             */
/* ------------------------------------------------------------------ */

void ui_flex_begin(UiAnchor a, float w, float h, UiDir d,
                   UiJustify j, UiAlign al, float gap, float pad)
{
    (void)a; (void)w; (void)h; (void)d;
    (void)j; (void)al; (void)gap; (void)pad;
}
void  ui_flex_end(void)     {}
bool  ui_button(int id, const char* l, float f) { (void)id;(void)l;(void)f; return false; }
void  ui_label(const char* t, float s, vec4 c, float f) { (void)t;(void)s;(void)c;(void)f; }
void  ui_spacer(float f) { (void)f; }
