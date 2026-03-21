/* Minimal stubs so test_ui can link without the full renderer.
 * These functions are never called by the unit tests (which only test
 * CPU-side ui_font_bake / ui_text_width). */
#include <volk.h>
#include "renderer.h"

VkCommandBuffer renderer_begin_single_cmd(Renderer* r)
{
    (void)r;
    return VK_NULL_HANDLE;
}

void renderer_end_single_cmd(Renderer* r, VkCommandBuffer cmd)
{
    (void)r;
    (void)cmd;
}
