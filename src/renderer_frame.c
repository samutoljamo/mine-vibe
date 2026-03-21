#include "renderer.h"
#include "renderer_internal.h"
#include "chunk_mesh.h"
#include "frustum.h"
#include "player_model.h"
#include "hud.h"
#include "agent.h"
#include "ui/ui.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Public API: draw frame                                            */
/* ------------------------------------------------------------------ */

void renderer_draw_frame(Renderer* r,
                         ChunkMesh* meshes, uint32_t mesh_count,
                         const PlayerRenderState* players, uint32_t player_count,
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
        renderer_recreate_swapchain(r);
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

    /* Draw remote players */
    if (player_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->player_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                r->player_pipeline_layout, 0, 1,
                                &r->player_descriptor_sets[fi], 0, NULL);
        player_model_draw(r, cmd, &r->player_model, players, player_count);
    }

    /* 7. End render pass */
    vkCmdEndRenderPass(cmd);

    /* UI pass — HUD and any screen overlays */
    float sw = (float)r->swapchain.extent.width;
    float sh = (float)r->swapchain.extent.height;
    ui_frame_begin(cmd, image_index, r->current_frame, sw, sh);
    if (hud) hud_build(hud, sw, sh);
    ui_frame_end();

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
        renderer_recreate_swapchain(r);
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swapchain image\n");
    }

    /* 10. Advance current frame */
    r->current_frame = (fi + 1) % MAX_FRAMES_IN_FLIGHT;
}
