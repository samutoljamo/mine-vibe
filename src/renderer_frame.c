#include "renderer.h"
#include "renderer_internal.h"
#include "chunk_mesh.h"
#include "frustum.h"
#include "player_model.h"
#include "ui/hud.h"
#include "inventory.h"
#include "raycast.h"
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
                         float day_brightness,
                         vec3 sky_color,
                         const Inventory* inventory,
                         int player_health,
                         const RaycastHit* target,
                         float underwater,
                         bool dump_frame, const char* dump_path)
{
    uint32_t fi = r->current_frame;

    /* 1. Wait for current frame's fence. A non-success here (notably
     * VK_ERROR_DEVICE_LOST) means we can't safely reuse this frame's
     * resources — log and skip the frame rather than racing the GPU. */
    {
        VkResult wr = vkWaitForFences(r->device, 1, &r->in_flight_fences[fi],
                                      VK_TRUE, UINT64_MAX);
        if (wr != VK_SUCCESS) {
            fprintf(stderr, "vkWaitForFences failed (%d); skipping frame\n",
                    (int)wr);
            return;
        }
    }

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
    if (vkResetFences(r->device, 1, &r->in_flight_fences[fi]) != VK_SUCCESS) {
        fprintf(stderr, "vkResetFences failed; skipping frame\n");
        return;
    }

    /* Per-frame outline state reset. Must happen before any
     * renderer_outline_emit_block call this frame. */
    r->outline_vert_count = 0;
    /* Per-frame perf counters (read back by the caller for the stats HUD). */
    r->stat_visible_chunks = 0;
    r->stat_culled_chunks  = 0;
    r->stat_total_chunks   = 0;
    r->stat_draw_calls     = 0;
    if (target && target->hit) {
        renderer_outline_emit_block(r, target->x, target->y, target->z);
    }

    /* 4. Update UBO */
    GlobalUBO ubo;
    glm_mat4_copy(view, ubo.view);
    glm_mat4_copy(proj, ubo.proj);
    ubo.sun_direction[0] = sun_dir[0];
    ubo.sun_direction[1] = sun_dir[1];
    ubo.sun_direction[2] = sun_dir[2];
    ubo.sun_direction[3] = 0.0f;
    /* Day/night drive: block.frag multiplies baked sky-light by sun_color,
     * so scaling sun_color by the daylight factor uniformly darkens the world
     * at night without touching baked lighting. Ambient floors the darkness so
     * caves/night never go fully black. */
    ubo.sun_color[0] = day_brightness;
    ubo.sun_color[1] = day_brightness;
    ubo.sun_color[2] = day_brightness;
    ubo.sun_color[3] = 1.0f;
    ubo.ambient = 0.06f + 0.24f * day_brightness;
    ubo.underwater = underwater;
    memcpy(r->ubo_mapped[fi], &ubo, sizeof(ubo));

    /* 5. Record command buffer */
    VkCommandBuffer cmd = r->command_buffers[fi];
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
        fprintf(stderr, "vkResetCommandBuffer failed; skipping frame\n");
        return;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        fprintf(stderr, "vkBeginCommandBuffer failed; skipping frame\n");
        return;
    }

    /* The 3D scene renders either directly into the swapchain image (legacy,
     * render_scale == 1.0) or into a smaller offscreen target that is then
     * upscale-blitted into the swapchain image. scene_extent / scene_framebuffer
     * drive the world pass; the swapchain extent always drives the UI pass. */
    bool        scale_active = r->scale_active && r->scene_framebuffer != VK_NULL_HANDLE;
    VkExtent2D  scene_extent = scale_active ? r->scene_extent : r->swapchain.extent;
    VkFramebuffer scene_fb   = scale_active ? r->scene_framebuffer
                                            : r->swapchain.framebuffers[image_index];

    /* Viewport/scissor for the 3D world pass, sized to the scene target. */
    VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float)scene_extent.width,
        .height   = (float)scene_extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = scene_extent,
    };

    /* Lerp clear color between sky and deep-water based on underwater
     * factor so the seam between geometry-tinted water and the cleared
     * sky doesn't pop as the eye dips below the surface. clearValues[2]
     * (resolve target, MSAA only) has DONT_CARE loadOp — present for
     * indexing. */
    /* Lerp the day/night sky color toward deep-water in that order, so an
     * underwater-at-night view reads dark (both effects stack). */
    float t = underwater;
    float clear_r = sky_color[0] * (1.0f - t) + 0.06f * t;
    float clear_g = sky_color[1] * (1.0f - t) + 0.18f * t;
    float clear_b = sky_color[2] * (1.0f - t) + 0.40f * t;
    VkClearValue clear_values[3] = {
        { .color = { .float32 = { clear_r, clear_g, clear_b, 1.0f } } },
        { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
        { 0 },
    };
    bool msaa = (r->sample_count != VK_SAMPLE_COUNT_1_BIT);

    VkRenderPassBeginInfo rp_info = {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass  = r->render_pass,
        .framebuffer = scene_fb,
        .renderArea  = {
            .offset = { 0, 0 },
            .extent = scene_extent,
        },
        .clearValueCount = msaa ? 3u : 2u,
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
            if (!frustum_test_aabb(&frustum, m->aabb_min, m->aabb_max)) {
                r->stat_culled_chunks++;   /* candidate rejected by frustum */
                continue;
            }

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
            r->stat_visible_chunks++;
            r->stat_draw_calls++;
        }
    }

    /* Draw remote players */
    if (player_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->player_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                r->player_pipeline_layout, 0, 1,
                                &r->player_descriptor_sets[fi], 0, NULL);
        player_model_draw(r, cmd, &r->player_model, players, player_count);
        r->stat_draw_calls += player_count;   /* one instanced/per-player draw each */
    }

    /* Block outline (semi-transparent black wireframe on the targeted block).
     * Drawn inside the world renderpass so depth test against world geometry
     * works naturally, but with depthWrite=false to avoid affecting later
     * depth-aware passes. */
    if (r->outline_vert_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->outline_pipeline);
        /* Reuse the world's bound descriptor set 0 — same layout, same UBO. */
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                r->outline_pipeline_layout, 0, 1,
                                &r->descriptor_sets[fi], 0, NULL);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &r->outline_vb[fi], &off);
        vkCmdDraw(cmd, r->outline_vert_count, 1, 0, 0);
        r->stat_draw_calls++;
    }

    /* 7. End render pass */
    vkCmdEndRenderPass(cmd);

    /* 7b. Render-scale upscale: when the 3D scene was rendered into the smaller
     * offscreen target, linearly blit it up into the full-res swapchain image.
     * The HUD/UI pass below then loads that swapchain image and draws on top at
     * full resolution, so only the 3D scene is downsampled. At render_scale 1.0
     * this whole block is skipped and the world pass wrote the swapchain image
     * directly (byte-for-byte the legacy path). */
    if (scale_active) {
        VkImage swap_img = r->swapchain.images[image_index];

        /* scene_color leaves the world pass in COLOR_ATTACHMENT_OPTIMAL
         * (render-pass finalLayout) -> TRANSFER_SRC for the blit. */
        VkImageMemoryBarrier to_src = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = r->scene_color_image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        /* Swapchain image has no meaningful prior contents (we overwrite the
         * full image via blit) -> UNDEFINED to TRANSFER_DST. */
        VkImageMemoryBarrier to_dst = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = 0,
            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = swap_img,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkImageMemoryBarrier pre[2] = { to_src, to_dst };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 2, pre);

        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = {
                { 0, 0, 0 },
                { (int32_t)scene_extent.width, (int32_t)scene_extent.height, 1 },
            },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = {
                { 0, 0, 0 },
                { (int32_t)r->swapchain.extent.width,
                  (int32_t)r->swapchain.extent.height, 1 },
            },
        };
        vkCmdBlitImage(cmd,
            r->scene_color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swap_img,             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        /* Hand the swapchain image to the UI pass, which expects
         * COLOR_ATTACHMENT_OPTIMAL (initialLayout, LOAD_OP_LOAD) so the blitted
         * scene is preserved underneath the HUD. */
        VkImageMemoryBarrier to_color = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = swap_img,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, NULL, 0, NULL, 1, &to_color);
    }

    /* UI pass — HUD and any screen overlays */
    float sw = (float)r->swapchain.extent.width;
    float sh = (float)r->swapchain.extent.height;
    ui_frame_begin(cmd, image_index, r->current_frame, sw, sh);
    hud_build(inventory, player_health, sw, sh);   /* hud_build draws crosshair unconditionally; hotbar only if inv non-null */
    /* Performance overlay (top-left). Mirror this frame's renderer counters
     * into the caller-owned PerfStats so the FPS/frametime rolling average and
     * the chunk/draw counts render together. */
    r->stat_total_chunks = r->stat_visible_chunks + r->stat_culled_chunks;
    if (r->show_stats && r->stats_overlay) {
        r->stats_overlay->visible_chunks = r->stat_visible_chunks;
        r->stats_overlay->culled_chunks  = r->stat_culled_chunks;
        r->stats_overlay->total_chunks   = r->stat_total_chunks;
        r->stats_overlay->draw_calls     = r->stat_draw_calls;
        hud_draw_stats(r->stats_overlay, sw, sh);
    }
    /* World-space block outline emission is handled at the top of this
     * function via renderer_outline_emit_block; the draw command is recorded
     * inside the world renderpass above. */
    ui_frame_end();

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fprintf(stderr, "vkEndCommandBuffer failed; skipping frame\n");
        return;
    }

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
