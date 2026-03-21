#ifndef RENDERER_INTERNAL_H
#define RENDERER_INTERNAL_H

/* Internal cross-file declarations for renderer_*.c translation units.
 * Do NOT include from headers or non-renderer source files. */

#include "renderer.h"

/* Defined in renderer.c; called from renderer_frame.c on swapchain invalidation. */
void renderer_recreate_swapchain(Renderer* r);

#endif
