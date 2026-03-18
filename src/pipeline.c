#include "pipeline.h"
#include "vertex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static uint8_t* read_file(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open file: %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fprintf(stderr, "File is empty or error: %s\n", path);
        fclose(f);
        return NULL;
    }

    uint8_t* buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (read != (size_t)len) {
        fprintf(stderr, "Failed to read file: %s\n", path);
        free(buf);
        return NULL;
    }

    *out_size = (size_t)len;
    return buf;
}

static VkShaderModule create_shader_module(VkDevice device,
                                           const uint8_t* code, size_t size)
{
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = (const uint32_t*)code,
    };

    VkShaderModule module;
    if (vkCreateShaderModule(device, &ci, NULL, &module) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create shader module\n");
        return VK_NULL_HANDLE;
    }

    return module;
}

VkShaderModule pipeline_load_shader_module(VkDevice device, const char* path)
{
    size_t size;
    uint8_t* code = read_file(path, &size);
    if (!code) return VK_NULL_HANDLE;
    VkShaderModule mod = create_shader_module(device, code, size);
    free(code);
    return mod;
}

/* ------------------------------------------------------------------ */
/*  Descriptor set layout                                             */
/* ------------------------------------------------------------------ */

bool pipeline_create_descriptor_layout(VkDevice device, VkDescriptorSetLayout* out)
{
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
    };

    if (vkCreateDescriptorSetLayout(device, &ci, NULL, out) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create descriptor set layout\n");
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Graphics pipeline                                                 */
/* ------------------------------------------------------------------ */

bool pipeline_create(VkDevice device, VkRenderPass render_pass,
                     VkDescriptorSetLayout desc_layout,
                     const char* vert_path, const char* frag_path,
                     VkPipelineLayout* out_layout, VkPipeline* out_pipeline)
{
    /* Load shader SPIR-V */
    size_t vert_size = 0, frag_size = 0;
    uint8_t* vert_code = read_file(vert_path, &vert_size);
    if (!vert_code) return false;

    uint8_t* frag_code = read_file(frag_path, &frag_size);
    if (!frag_code) { free(vert_code); return false; }

    VkShaderModule vert_mod = create_shader_module(device, vert_code, vert_size);
    VkShaderModule frag_mod = create_shader_module(device, frag_code, frag_size);
    free(vert_code);
    free(frag_code);

    if (vert_mod == VK_NULL_HANDLE || frag_mod == VK_NULL_HANDLE) {
        if (vert_mod) vkDestroyShaderModule(device, vert_mod, NULL);
        if (frag_mod) vkDestroyShaderModule(device, frag_mod, NULL);
        return false;
    }

    /* Shader stages */
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_mod,
            .pName  = "main",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_mod,
            .pName  = "main",
        },
    };

    /* Vertex input */
    VkVertexInputBindingDescription binding = vertex_binding_desc();
    VkVertexInputAttributeDescription attrs[4];
    vertex_attr_descs(attrs);

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions    = attrs,
    };

    /* Input assembly */
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    /* Viewport state (dynamic) */
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };

    /* Rasterizer */
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode             = VK_POLYGON_MODE_FILL,
        .cullMode                = VK_CULL_MODE_BACK_BIT,
        .frontFace               = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable         = VK_FALSE,
        .lineWidth               = 1.0f,
    };

    /* Multisampling */
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable  = VK_FALSE,
    };

    /* Depth stencil */
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable       = VK_TRUE,
        .depthWriteEnable      = VK_TRUE,
        .depthCompareOp        = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable     = VK_FALSE,
    };

    /* Color blending */
    VkPipelineColorBlendAttachmentState color_blend_att = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable   = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments    = &color_blend_att,
    };

    /* Dynamic state */
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states,
    };

    /* Push constant range */
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(ChunkPushConstants),
    };

    /* Pipeline layout */
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range,
    };

    if (vkCreatePipelineLayout(device, &layout_ci, NULL, out_layout) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create pipeline layout\n");
        vkDestroyShaderModule(device, vert_mod, NULL);
        vkDestroyShaderModule(device, frag_mod, NULL);
        return false;
    }

    /* Graphics pipeline */
    VkGraphicsPipelineCreateInfo pipeline_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState   = &multisampling,
        .pDepthStencilState  = &depth_stencil,
        .pColorBlendState    = &color_blend,
        .pDynamicState       = &dynamic_state,
        .layout              = *out_layout,
        .renderPass          = render_pass,
        .subpass             = 0,
    };

    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                &pipeline_ci, NULL, out_pipeline);

    vkDestroyShaderModule(device, vert_mod, NULL);
    vkDestroyShaderModule(device, frag_mod, NULL);

    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipeline\n");
        vkDestroyPipelineLayout(device, *out_layout, NULL);
        *out_layout = VK_NULL_HANDLE;
        return false;
    }

    return true;
}
