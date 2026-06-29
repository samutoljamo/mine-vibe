#ifndef PARTICLE_RENDER_H
#define PARTICLE_RENDER_H

#include "particle.h"
#include <stddef.h>

/* Pure CPU helpers that turn the particle pool into renderable billboard
 * geometry. No Vulkan/GLFW here so the math is unit-testable; the renderer
 * just memcpys the produced vertices into a mapped GPU buffer.
 *
 * Each particle becomes a camera-facing quad = 2 triangles = 6 vertices. Each
 * vertex is 9 floats laid out as the particle pipeline's vertex input expects:
 *   [0..2] world position
 *   [3..6] rgba color (alpha already faded by remaining-life ratio)
 *   [7..8] local quad coord in [-1,1]^2 (for the fragment's round falloff)
 */

#define PARTICLE_RENDER_VERT_FLOATS 9
#define PARTICLE_RENDER_VERTS_PER   6

/* Remaining-life -> alpha fade ratio in [0,1]. Fades out over the last portion
 * of life and clamps at full opacity earlier, so particles pop in crisp and
 * dissolve at the end. Safe for max_life <= 0 (returns 0). */
float particle_render_alpha(float life, float max_life);

/* Expand the live pool into billboard vertices written to `out` (must hold at
 * least ps->count * PARTICLE_RENDER_VERTS_PER * PARTICLE_RENDER_VERT_FLOATS
 * floats). `cam_right`/`cam_up` are the camera basis vectors in world space
 * (unit length). Returns the number of vertices written (count * 6). A NULL
 * arg or empty pool writes nothing and returns 0. */
size_t particle_render_build(const ParticleSystem* ps,
                             const float cam_right[3],
                             const float cam_up[3],
                             float* out);

#endif /* PARTICLE_RENDER_H */
