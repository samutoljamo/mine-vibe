# Minecraft Foundation Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a chunk-based voxel world with Vulkan rendering, terrain generation, greedy meshing, async chunk loading, and first-person camera at 32-chunk render distance.

**Architecture:** Monolithic incremental C project with manual Vulkan initialization (volk for function loading, VMA for GPU memory). Chunks are 16x256x16 blocks stored in a hash map, meshed with greedy meshing on a thread pool, and rendered with per-chunk push constants. Frustum culling keeps draw calls manageable at render distance 32.

**Tech Stack:** C11, Vulkan, volk, VMA, GLFW, cglm, FastNoiseLite, stb_image, CMake 3.20+

**Spec:** `docs/superpowers/specs/2026-03-17-minecraft-foundation-design.md`

---

## File Map

| File | Responsibility | Created in Task |
|------|---------------|-----------------|
| `CMakeLists.txt` | Build config, deps, shader compilation | 1 |
| `src/vertex.h` | Vertex struct, UBO, push constants | 2 |
| `src/block.h` | Block type enum and property table | 2 |
| `src/block.c` | Block property lookup | 2 |
| `src/renderer.h` | Renderer public API | 3 |
| `src/renderer.c` | Vulkan init, frame rendering, cleanup | 3, 7, 10 |
| `src/swapchain.h` | Swapchain API | 4 |
| `src/swapchain.c` | Swapchain create/destroy/recreate | 4 |
| `src/pipeline.h` | Pipeline creation API | 5 |
| `src/pipeline.c` | Shader loading, pipeline, descriptor layout | 5 |
| `src/texture.h` | Texture atlas API | 6 |
| `src/texture.c` | Atlas generation, staging upload, sampler | 6 |
| `src/camera.h` | Camera API | 8 |
| `src/camera.c` | First-person camera, view/proj matrices | 8 |
| `src/frustum.h` | Frustum culling API | 9 |
| `src/frustum.c` | Plane extraction, AABB test | 9 |
| `src/chunk.h` | Chunk data structure | 11 |
| `src/chunk.c` | Chunk create/destroy, block access | 11 |
| `src/chunk_mesh.h` | GPU mesh buffer API | 11 |
| `src/chunk_mesh.c` | VMA buffer upload/destroy | 11 |
| `src/chunk_map.h` | Hash map API | 12 |
| `src/chunk_map.c` | Open-addressing hash map | 12 |
| `src/worldgen.h` | World generation API | 13 |
| `src/worldgen.c` | Perlin terrain, trees, water | 13 |
| `src/mesher.h` | Greedy mesher API | 14 |
| `src/mesher.c` | Greedy meshing algorithm | 14 |
| `src/world.h` | World management API | 15 |
| `src/world.c` | Async worker pool, chunk lifecycle | 15 |
| `src/main.c` | Entry point, game loop | 7, 8, 10, 16 |
| `src/vma_impl.cpp` | VMA implementation (C++ TU) | 3 |
| `shaders/block.vert` | Vertex shader | 5 |
| `shaders/block.frag` | Fragment shader | 5 |

---

## Task 1: Project Scaffolding + CMake

**Files:**
- Create: `CMakeLists.txt`

**Context:** Set up the build system with all dependencies fetched via CMake FetchContent. The project mixes C11 and C++ (one file for VMA). Shader compilation via glslc custom commands.

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(minecraft C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Dependencies via FetchContent ----
include(FetchContent)

# GLFW
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# volk
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 1.4.304
)
FetchContent_MakeAvailable(volk)

# VMA
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.2.1
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# cglm
FetchContent_Declare(cglm
    GIT_REPOSITORY https://github.com/recp/cglm.git
    GIT_TAG v0.9.6
)
set(CGLM_SHARED OFF CACHE BOOL "" FORCE)
set(CGLM_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cglm)

# stb (header-only, just need the repo)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
)
FetchContent_MakeAvailable(stb)

# FastNoiseLite (header-only)
FetchContent_Declare(FastNoiseLite
    GIT_REPOSITORY https://github.com/Auburn/FastNoiseLite.git
    GIT_TAG v1.1.1
)
FetchContent_MakeAvailable(FastNoiseLite)

# ---- Find Vulkan SDK (headers + glslc) ----
find_package(Vulkan REQUIRED COMPONENTS glslc)

# ---- Shader compilation ----
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})

function(compile_shader SHADER_SRC)
    get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
    set(SHADER_SPV ${SHADER_BIN_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SHADER_SPV}
        COMMAND Vulkan::glslc ${SHADER_SRC} -o ${SHADER_SPV}
        DEPENDS ${SHADER_SRC}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} ${SHADER_SPV} PARENT_SCOPE)
endfunction()

compile_shader(${SHADER_DIR}/block.vert)
compile_shader(${SHADER_DIR}/block.frag)
add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})

# ---- Executable ----
add_executable(minecraft
    src/main.c
    src/renderer.c
    src/swapchain.c
    src/pipeline.c
    src/texture.c
    src/camera.c
    src/frustum.c
    src/block.c
    src/chunk.c
    src/chunk_mesh.c
    src/chunk_map.c
    src/worldgen.c
    src/mesher.c
    src/world.c
    src/vma_impl.cpp
)

add_dependencies(minecraft shaders)

target_compile_definitions(minecraft PRIVATE VK_NO_PROTOTYPES)

target_include_directories(minecraft PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${fastnoiselite_SOURCE_DIR}/C
)

target_link_libraries(minecraft PRIVATE
    glfw
    volk
    GPUOpen::VulkanMemoryAllocator
    cglm
    ${CMAKE_DL_LIBS}
)

# Platform-specific threading
find_package(Threads REQUIRED)
target_link_libraries(minecraft PRIVATE Threads::Threads)

# Math lib on Linux
if(UNIX AND NOT APPLE)
    target_link_libraries(minecraft PRIVATE m)
endif()
```

- [ ] **Step 2: Create placeholder source files so CMake can configure**

Create minimal stub files for every source file listed in the executable. Each `.c` file contains just an empty function or nothing. Each `.h` file contains an include guard. `main.c` contains a `main()` that returns 0. `vma_impl.cpp` is empty for now. Create empty shader files `shaders/block.vert` and `shaders/block.frag` with minimal valid GLSL:

`src/main.c`:
```c
int main(void) {
    return 0;
}
```

`src/vma_impl.cpp`:
```cpp
// VMA implementation - populated in Task 3
```

All other `.c` files: empty.
All `.h` files: empty include guard, e.g.:
```c
#ifndef RENDERER_H
#define RENDERER_H
#endif
```

`shaders/block.vert`:
```glsl
#version 450
void main() {
    gl_Position = vec4(0.0);
}
```

`shaders/block.frag`:
```glsl
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(1.0);
}
```

- [ ] **Step 3: Verify CMake configures and builds**

Run:
```bash
cd /var/home/samu/minecraft && cmake -B build 2>&1 | tail -5
cmake --build build 2>&1 | tail -10
```
Expected: Configuration succeeds, all dependencies fetched, executable compiles, shaders compile to .spv.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt src/ shaders/
git commit -m "feat: project scaffolding with CMake and all dependencies"
```

---

## Task 2: Core Type Definitions (vertex.h, block.h/.c)

**Files:**
- Create: `src/vertex.h`, `src/block.h`, `src/block.c`

**Context:** These types are used by nearly every other module. Define them first so downstream code can reference them.

- [ ] **Step 1: Write vertex.h**

```c
#ifndef VERTEX_H
#define VERTEX_H

#include <stdint.h>
#include <cglm/cglm.h>
#include <vulkan/vulkan.h>

typedef struct BlockVertex {
    float    pos[3];
    float    uv[2];
    uint8_t  normal;   // 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
    uint8_t  ao;       // 0-3
    uint8_t  _pad[2];
} BlockVertex;

// 24 bytes total
_Static_assert(sizeof(BlockVertex) == 24, "BlockVertex must be 24 bytes");

typedef struct GlobalUBO {
    mat4  view;
    mat4  proj;
    vec4  sun_direction;
    vec4  sun_color;
    float ambient;
    float _pad[3];
} GlobalUBO;

typedef struct ChunkPushConstants {
    vec4 chunk_offset;
} ChunkPushConstants;

static inline VkVertexInputBindingDescription vertex_binding_desc(void) {
    return (VkVertexInputBindingDescription){
        .binding = 0,
        .stride = sizeof(BlockVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
}

static inline void vertex_attr_descs(VkVertexInputAttributeDescription out[4]) {
    out[0] = (VkVertexInputAttributeDescription){
        .location = 0, .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0,
    };
    out[1] = (VkVertexInputAttributeDescription){
        .location = 1, .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT, .offset = 12,
    };
    out[2] = (VkVertexInputAttributeDescription){
        .location = 2, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 20,
    };
    out[3] = (VkVertexInputAttributeDescription){
        .location = 3, .binding = 0,
        .format = VK_FORMAT_R8_UINT, .offset = 21,
    };
}

#endif
```

- [ ] **Step 2: Write block.h**

```c
#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t BlockID;

enum {
    BLOCK_AIR = 0,
    BLOCK_STONE,
    BLOCK_DIRT,
    BLOCK_GRASS,
    BLOCK_SAND,
    BLOCK_WOOD,
    BLOCK_LEAVES,
    BLOCK_WATER,
    BLOCK_BEDROCK,
    BLOCK_COUNT,
};

typedef struct BlockDef {
    const char* name;
    bool        is_solid;
    bool        is_transparent;
    uint8_t     tex_top;    // atlas tile index
    uint8_t     tex_side;
    uint8_t     tex_bottom;
} BlockDef;

const BlockDef* block_get_def(BlockID id);

static inline bool block_is_solid(BlockID id) {
    return block_get_def(id)->is_solid;
}

static inline bool block_is_transparent(BlockID id) {
    return block_get_def(id)->is_transparent;
}

#endif
```

- [ ] **Step 3: Write block.c**

```c
#include "block.h"

// Atlas tile indices (row-major, 16 tiles per row in 256x256 atlas with 16x16 tiles)
// Row 0: stone=0, dirt=1, grass_top=2, grass_side=3, sand=4, wood_top=5, wood_side=6, leaves=7
// Row 1: water=16, bedrock=17
static const BlockDef block_defs[BLOCK_COUNT] = {
    [BLOCK_AIR]     = { "air",     false, true,  0,  0,  0 },
    [BLOCK_STONE]   = { "stone",   true,  false, 0,  0,  0 },
    [BLOCK_DIRT]    = { "dirt",    true,  false, 1,  1,  1 },
    [BLOCK_GRASS]   = { "grass",   true,  false, 2,  3,  1 },
    [BLOCK_SAND]    = { "sand",    true,  false, 4,  4,  4 },
    [BLOCK_WOOD]    = { "wood",    true,  false, 5,  6,  5 },
    [BLOCK_LEAVES]  = { "leaves",  true,  true,  7,  7,  7 },
    [BLOCK_WATER]   = { "water",   false, true,  16, 16, 16 },
    [BLOCK_BEDROCK] = { "bedrock", true,  false, 17, 17, 17 },
};

const BlockDef* block_get_def(BlockID id) {
    if (id >= BLOCK_COUNT) return &block_defs[BLOCK_AIR];
    return &block_defs[id];
}
```

- [ ] **Step 4: Verify it compiles**

Run:
```bash
cmake --build build 2>&1 | tail -5
```
Expected: Compiles cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/vertex.h src/block.h src/block.c
git commit -m "feat: add vertex format, UBO, and block type definitions"
```

---

## Task 3: Vulkan Initialization (renderer.h/.c, vma_impl.cpp)

**Files:**
- Create: `src/renderer.h`, `src/renderer.c`, `src/vma_impl.cpp`

**Context:** The renderer module owns all core Vulkan state. This task covers initialization only (instance, device, VMA). Swapchain, pipeline, and frame rendering are separate tasks.

- [ ] **Step 1: Write vma_impl.cpp**

```cpp
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <volk.h>
#include <vk_mem_alloc.h>
```

- [ ] **Step 2: Write renderer.h**

```c
#ifndef RENDERER_H
#define RENDERER_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include "swapchain.h"
#include "vertex.h"

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct ChunkMesh ChunkMesh;

typedef struct Renderer {
    // Vulkan core
    VkInstance               instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkSurfaceKHR             surface;
    VkPhysicalDevice         physical_device;
    VkDevice                 device;
    VkQueue                  graphics_queue;
    VkQueue                  present_queue;
    uint32_t                 graphics_family;
    uint32_t                 present_family;
    VmaAllocator             allocator;

    // Swapchain
    Swapchain                swapchain;

    // Pipeline
    VkRenderPass             render_pass;
    VkPipelineLayout         pipeline_layout;
    VkPipeline               pipeline;
    VkDescriptorSetLayout    desc_set_layout;
    VkDescriptorPool         desc_pool;
    VkDescriptorSet          desc_sets[MAX_FRAMES_IN_FLIGHT];

    // Per-frame sync
    VkCommandPool            cmd_pool;
    VkCommandBuffer          cmd_bufs[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore              image_available[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore              render_finished[MAX_FRAMES_IN_FLIGHT];
    VkFence                  in_flight[MAX_FRAMES_IN_FLIGHT];
    uint32_t                 current_frame;

    // Uniform buffers
    VkBuffer                 ubo_bufs[MAX_FRAMES_IN_FLIGHT];
    VmaAllocation            ubo_allocs[MAX_FRAMES_IN_FLIGHT];
    void*                    ubo_mapped[MAX_FRAMES_IN_FLIGHT];

    // Texture atlas
    VkImage                  atlas_image;
    VmaAllocation            atlas_alloc;
    VkImageView              atlas_view;
    VkSampler                atlas_sampler;

    // Window
    GLFWwindow*              window;
    bool                     framebuffer_resized;
} Renderer;

bool renderer_init(Renderer* r, GLFWwindow* window);
void renderer_draw_frame(Renderer* r, ChunkMesh* meshes, uint32_t mesh_count,
                         mat4 view, mat4 proj, vec3 sun_dir);
void renderer_cleanup(Renderer* r);

// Utility: single-shot command buffer for uploads
VkCommandBuffer renderer_begin_single_cmd(Renderer* r);
void renderer_end_single_cmd(Renderer* r, VkCommandBuffer cmd);

#endif
```

- [ ] **Step 3: Write renderer.c — Vulkan init only**

Write `renderer.c` with the full `renderer_init()` implementation covering:

1. `volkInitialize()`
2. Instance creation with GLFW extensions + `VK_EXT_debug_utils`
3. `volkLoadInstance()`
4. Debug messenger setup (severity >= WARNING)
5. Surface creation via GLFW
6. Physical device selection (enumerate, find graphics+present queue families, prefer discrete)
7. Logical device creation with `VK_KHR_swapchain` extension
8. `volkLoadDevice()`
9. VMA allocator creation with volk function pointers

Also implement `renderer_cleanup()` (destroys everything in reverse order) and the single-shot command buffer helpers.

Leave `renderer_draw_frame()` as an empty stub for now.

```c
#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Debug callback ----
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
{
    (void)type; (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

// ---- Physical device selection helpers ----
typedef struct QueueFamilies {
    uint32_t graphics;
    uint32_t present;
    bool     found_graphics;
    bool     found_present;
} QueueFamilies;

static QueueFamilies find_queue_families(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    QueueFamilies qf = {0};
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
    VkQueueFamilyProperties* props = malloc(sizeof(VkQueueFamilyProperties) * count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props);

    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qf.graphics = i;
            qf.found_graphics = true;
        }
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present_support);
        if (present_support) {
            qf.present = i;
            qf.found_present = true;
        }
        if (qf.found_graphics && qf.found_present) break;
    }
    free(props);
    return qf;
}

static bool check_device_extensions(VkPhysicalDevice pd) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL);
    VkExtensionProperties* exts = malloc(sizeof(VkExtensionProperties) * count);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, exts);

    bool has_swapchain = false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            has_swapchain = true;
            break;
        }
    }
    free(exts);
    return has_swapchain;
}

static bool pick_physical_device(Renderer* r) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(r->instance, &count, NULL);
    if (count == 0) return false;

    VkPhysicalDevice* devices = malloc(sizeof(VkPhysicalDevice) * count);
    vkEnumeratePhysicalDevices(r->instance, &count, devices);

    // Prefer discrete GPU
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; i++) {
        if (!check_device_extensions(devices[i])) continue;
        QueueFamilies qf = find_queue_families(devices[i], r->surface);
        if (!qf.found_graphics || !qf.found_present) continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            r->physical_device = devices[i];
            r->graphics_family = qf.graphics;
            r->present_family = qf.present;
            free(devices);
            fprintf(stderr, "Selected GPU: %s\n", props.deviceName);
            return true;
        }
        if (fallback == VK_NULL_HANDLE) {
            fallback = devices[i];
            r->graphics_family = qf.graphics;
            r->present_family = qf.present;
        }
    }

    if (fallback != VK_NULL_HANDLE) {
        r->physical_device = fallback;
        free(devices);
        return true;
    }
    free(devices);
    return false;
}

// ---- Init ----
bool renderer_init(Renderer* r, GLFWwindow* window) {
    memset(r, 0, sizeof(*r));
    r->window = window;

    // 1. Init volk
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "Failed to initialize volk\n");
        return false;
    }

    // 2. Create instance
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Minecraft",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "Custom",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    uint32_t glfw_ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

    // Add debug utils extension
    uint32_t ext_count = glfw_ext_count + 1;
    const char** extensions = malloc(sizeof(char*) * ext_count);
    memcpy(extensions, glfw_exts, sizeof(char*) * glfw_ext_count);
    extensions[glfw_ext_count] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    const char* validation_layer = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo inst_ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = extensions,
#ifndef NDEBUG
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &validation_layer,
#endif
    };

    VkResult res = vkCreateInstance(&inst_ci, NULL, &r->instance);
    free(extensions);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance: %d\n", res);
        return false;
    }

    // 3. Load instance functions
    volkLoadInstance(r->instance);

    // 4. Debug messenger
#ifndef NDEBUG
    VkDebugUtilsMessengerCreateInfoEXT dbg_ci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
    };
    vkCreateDebugUtilsMessengerEXT(r->instance, &dbg_ci, NULL, &r->debug_messenger);
#endif

    // 5. Surface
    if (glfwCreateWindowSurface(r->instance, window, NULL, &r->surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create window surface\n");
        return false;
    }

    // 6. Physical device
    if (!pick_physical_device(r)) {
        fprintf(stderr, "Failed to find a suitable GPU\n");
        return false;
    }

    // 7. Logical device
    float queue_priority = 1.0f;
    uint32_t unique_families[2] = { r->graphics_family, r->present_family };
    uint32_t family_count = (r->graphics_family == r->present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo queue_cis[2];
    for (uint32_t i = 0; i < family_count; i++) {
        queue_cis[i] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i],
            .queueCount = 1,
            .pQueuePriorities = &queue_priority,
        };
    }

    const char* device_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dev_ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = family_count,
        .pQueueCreateInfos = queue_cis,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &device_ext,
    };

    if (vkCreateDevice(r->physical_device, &dev_ci, NULL, &r->device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        return false;
    }

    // 8. Load device functions
    volkLoadDevice(r->device);

    vkGetDeviceQueue(r->device, r->graphics_family, 0, &r->graphics_queue);
    vkGetDeviceQueue(r->device, r->present_family, 0, &r->present_queue);

    // 9. VMA allocator
    VmaVulkanFunctions vma_funcs = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory = vkAllocateMemory,
        .vkFreeMemory = vkFreeMemory,
        .vkMapMemory = vkMapMemory,
        .vkUnmapMemory = vkUnmapMemory,
        .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory = vkBindBufferMemory,
        .vkBindImageMemory = vkBindImageMemory,
        .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
        .vkCreateBuffer = vkCreateBuffer,
        .vkDestroyBuffer = vkDestroyBuffer,
        .vkCreateImage = vkCreateImage,
        .vkDestroyImage = vkDestroyImage,
        .vkCmdCopyBuffer = vkCmdCopyBuffer,
    };

    VmaAllocatorCreateInfo alloc_ci = {
        .physicalDevice = r->physical_device,
        .device = r->device,
        .instance = r->instance,
        .pVulkanFunctions = &vma_funcs,
        .vulkanApiVersion = VK_API_VERSION_1_1,
    };

    if (vmaCreateAllocator(&alloc_ci, &r->allocator) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create VMA allocator\n");
        return false;
    }

    return true;
}

// ---- Single-shot command buffer ----
VkCommandBuffer renderer_begin_single_cmd(Renderer* r) {
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(r->device, &ai, &cmd);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void renderer_end_single_cmd(Renderer* r, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(r->graphics_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(r->graphics_queue);
    vkFreeCommandBuffers(r->device, r->cmd_pool, 1, &cmd);
}

// ---- Stub draw frame (implemented in Task 10) ----
void renderer_draw_frame(Renderer* r, ChunkMesh* meshes, uint32_t mesh_count,
                         mat4 view, mat4 proj, vec3 sun_dir)
{
    (void)r; (void)meshes; (void)mesh_count; (void)view; (void)proj; (void)sun_dir;
}

// ---- Cleanup ----
void renderer_cleanup(Renderer* r) {
    if (r->device) vkDeviceWaitIdle(r->device);

    // Texture
    if (r->atlas_sampler) vkDestroySampler(r->device, r->atlas_sampler, NULL);
    if (r->atlas_view) vkDestroyImageView(r->device, r->atlas_view, NULL);
    if (r->atlas_image) vmaDestroyImage(r->allocator, r->atlas_image, r->atlas_alloc);

    // UBOs
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (r->ubo_bufs[i]) vmaDestroyBuffer(r->allocator, r->ubo_bufs[i], r->ubo_allocs[i]);
    }

    // Sync
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (r->in_flight[i]) vkDestroyFence(r->device, r->in_flight[i], NULL);
        if (r->render_finished[i]) vkDestroySemaphore(r->device, r->render_finished[i], NULL);
        if (r->image_available[i]) vkDestroySemaphore(r->device, r->image_available[i], NULL);
    }

    if (r->cmd_pool) vkDestroyCommandPool(r->device, r->cmd_pool, NULL);
    if (r->desc_pool) vkDestroyDescriptorPool(r->device, r->desc_pool, NULL);
    if (r->pipeline) vkDestroyPipeline(r->device, r->pipeline, NULL);
    if (r->pipeline_layout) vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);
    if (r->desc_set_layout) vkDestroyDescriptorSetLayout(r->device, r->desc_set_layout, NULL);
    if (r->render_pass) vkDestroyRenderPass(r->device, r->render_pass, NULL);

    swapchain_destroy(r->device, r->allocator, &r->swapchain);

    if (r->allocator) vmaDestroyAllocator(r->allocator);
    if (r->device) vkDestroyDevice(r->device, NULL);
    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
#ifndef NDEBUG
    if (r->debug_messenger)
        vkDestroyDebugUtilsMessengerEXT(r->instance, r->debug_messenger, NULL);
#endif
    if (r->instance) vkDestroyInstance(r->instance, NULL);
}
```

- [ ] **Step 4: Update main.c to test Vulkan init**

```c
#include <stdio.h>
#include <stdbool.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "renderer.h"

int main(void) {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        fprintf(stderr, "Failed to init renderer\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    fprintf(stderr, "Vulkan initialized successfully!\n");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }

    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 5: Verify it compiles and runs**

Run:
```bash
cmake --build build 2>&1 | tail -10
./build/minecraft
```
Expected: Window opens, "Vulkan initialized successfully!" printed, "Selected GPU: ..." printed. Close window to exit cleanly.

- [ ] **Step 6: Commit**

```bash
git add src/renderer.h src/renderer.c src/vma_impl.cpp src/main.c
git commit -m "feat: Vulkan initialization with volk and VMA"
```

---

## Task 4: Swapchain (swapchain.h/.c)

**Files:**
- Create: `src/swapchain.h`, `src/swapchain.c`

**Context:** Swapchain manages the presentable images, depth buffer, and framebuffers. Must handle recreation on window resize.

- [ ] **Step 1: Write swapchain.h**

```c
#ifndef SWAPCHAIN_H
#define SWAPCHAIN_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdbool.h>

typedef struct Swapchain {
    VkSwapchainKHR  swapchain;
    VkFormat        image_format;
    VkExtent2D      extent;
    uint32_t        image_count;
    VkImage*        images;
    VkImageView*    image_views;
    VkImage         depth_image;
    VmaAllocation   depth_alloc;
    VkImageView     depth_view;
    VkFormat        depth_format;
    VkFramebuffer*  framebuffers;
} Swapchain;

bool swapchain_create(VkPhysicalDevice pd, VkDevice device, VmaAllocator allocator,
                      VkSurfaceKHR surface, uint32_t graphics_family,
                      uint32_t present_family, VkRenderPass render_pass,
                      int width, int height, VkSwapchainKHR old_swapchain,
                      Swapchain* out);

void swapchain_destroy(VkDevice device, VmaAllocator allocator, Swapchain* sc);

#endif
```

- [ ] **Step 2: Write swapchain.c**

```c
#include "swapchain.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static VkSurfaceFormatKHR choose_format(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, NULL);
    VkSurfaceFormatKHR* formats = malloc(sizeof(VkSurfaceFormatKHR) * count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, formats);

    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            break;
        }
    }
    free(formats);
    return chosen;
}

static VkPresentModeKHR choose_present_mode(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, NULL);
    VkPresentModeKHR* modes = malloc(sizeof(VkPresentModeKHR) * count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, modes);

    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosen = modes[i];
            break;
        }
    }
    free(modes);
    return chosen;
}

static uint32_t clamp_u32(uint32_t val, uint32_t lo, uint32_t hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

bool swapchain_create(VkPhysicalDevice pd, VkDevice device, VmaAllocator allocator,
                      VkSurfaceKHR surface, uint32_t graphics_family,
                      uint32_t present_family, VkRenderPass render_pass,
                      int width, int height, VkSwapchainKHR old_swapchain,
                      Swapchain* out)
{
    memset(out, 0, sizeof(*out));

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);

    VkSurfaceFormatKHR fmt = choose_format(pd, surface);
    VkPresentModeKHR mode = choose_present_mode(pd, surface);

    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = clamp_u32((uint32_t)width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp_u32((uint32_t)height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };

    uint32_t families[2] = { graphics_family, present_family };
    if (graphics_family != present_family) {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(device, &ci, NULL, &out->swapchain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swapchain\n");
        return false;
    }

    out->image_format = fmt.format;
    out->extent = extent;

    // Get images
    vkGetSwapchainImagesKHR(device, out->swapchain, &out->image_count, NULL);
    out->images = malloc(sizeof(VkImage) * out->image_count);
    vkGetSwapchainImagesKHR(device, out->swapchain, &out->image_count, out->images);

    // Image views
    out->image_views = malloc(sizeof(VkImageView) * out->image_count);
    for (uint32_t i = 0; i < out->image_count; i++) {
        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = out->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = out->image_format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        vkCreateImageView(device, &vi, NULL, &out->image_views[i]);
    }

    // Depth buffer
    out->depth_format = VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = out->depth_format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    };
    VmaAllocationCreateInfo dai = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    };
    vmaCreateImage(allocator, &dci, &dai, &out->depth_image, &out->depth_alloc, NULL);

    VkImageViewCreateInfo dvi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = out->depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCreateImageView(device, &dvi, NULL, &out->depth_view);

    // Framebuffers (only if render_pass provided)
    if (render_pass != VK_NULL_HANDLE) {
        out->framebuffers = malloc(sizeof(VkFramebuffer) * out->image_count);
        for (uint32_t i = 0; i < out->image_count; i++) {
            VkImageView attachments[2] = { out->image_views[i], out->depth_view };
            VkFramebufferCreateInfo fci = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = render_pass,
                .attachmentCount = 2,
                .pAttachments = attachments,
                .width = extent.width,
                .height = extent.height,
                .layers = 1,
            };
            vkCreateFramebuffer(device, &fci, NULL, &out->framebuffers[i]);
        }
    }

    return true;
}

void swapchain_destroy(VkDevice device, VmaAllocator allocator, Swapchain* sc) {
    if (!sc->swapchain) return;

    if (sc->framebuffers) {
        for (uint32_t i = 0; i < sc->image_count; i++)
            vkDestroyFramebuffer(device, sc->framebuffers[i], NULL);
        free(sc->framebuffers);
    }

    if (sc->depth_view) vkDestroyImageView(device, sc->depth_view, NULL);
    if (sc->depth_image) vmaDestroyImage(allocator, sc->depth_image, sc->depth_alloc);

    if (sc->image_views) {
        for (uint32_t i = 0; i < sc->image_count; i++)
            vkDestroyImageView(device, sc->image_views[i], NULL);
        free(sc->image_views);
    }

    free(sc->images);
    vkDestroySwapchainKHR(device, sc->swapchain, NULL);
    memset(sc, 0, sizeof(*sc));
}
```

- [ ] **Step 3: Integrate swapchain creation into renderer_init**

Add to `renderer_init()` after VMA creation:

1. Create render pass (color + depth attachments)
2. Create swapchain with render pass
3. Create command pool + command buffers
4. Create sync objects (semaphores + fences, fences created SIGNALED)
5. Create UBOs (VMA, persistently mapped)

Add render pass creation as a static function in `renderer.c`:

```c
static bool create_render_pass(Renderer* r) {
    VkAttachmentDescription attachments[2] = {
        { // Color
            .format = r->swapchain.image_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        { // Depth
            .format = VK_FORMAT_D32_SFLOAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };

    VkAttachmentReference color_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
    };

    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo rp_ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };

    return vkCreateRenderPass(r->device, &rp_ci, NULL, &r->render_pass) == VK_SUCCESS;
}

static bool create_sync_objects(Renderer* r) {
    VkCommandPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = r->graphics_family,
    };
    if (vkCreateCommandPool(r->device, &pool_ci, NULL, &r->cmd_pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo alloc_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT,
    };
    vkAllocateCommandBuffers(r->device, &alloc_ci, r->cmd_bufs);

    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkCreateSemaphore(r->device, &sem_ci, NULL, &r->image_available[i]);
        vkCreateSemaphore(r->device, &sem_ci, NULL, &r->render_finished[i]);
        vkCreateFence(r->device, &fence_ci, NULL, &r->in_flight[i]);
    }
    return true;
}

static bool create_ubos(Renderer* r) {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(GlobalUBO),
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        };
        VmaAllocationCreateInfo aci = {
            .usage = VMA_MEMORY_USAGE_AUTO,
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                   | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        };
        VmaAllocationInfo info;
        if (vmaCreateBuffer(r->allocator, &bci, &aci,
                            &r->ubo_bufs[i], &r->ubo_allocs[i], &info) != VK_SUCCESS)
            return false;
        r->ubo_mapped[i] = info.pMappedData;
    }
    return true;
}
```

Then append these calls to the end of `renderer_init()`:

```c
    // 10. Render pass (needs swapchain format, so create swapchain first without framebuffers)
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    if (!swapchain_create(r->physical_device, r->device, r->allocator, r->surface,
                          r->graphics_family, r->present_family, VK_NULL_HANDLE,
                          w, h, VK_NULL_HANDLE, &r->swapchain))
        return false;

    if (!create_render_pass(r)) return false;

    // Now create framebuffers with the render pass
    swapchain_destroy(r->device, r->allocator, &r->swapchain);
    if (!swapchain_create(r->physical_device, r->device, r->allocator, r->surface,
                          r->graphics_family, r->present_family, r->render_pass,
                          w, h, VK_NULL_HANDLE, &r->swapchain))
        return false;

    // 11. Sync objects + command pool
    if (!create_sync_objects(r)) return false;

    // 12. UBOs
    if (!create_ubos(r)) return false;
```

Also register the framebuffer resize callback:

```c
    glfwSetWindowUserPointer(window, r);
    glfwSetFramebufferSizeCallback(window, framebuffer_resize_cb);
```

With callback:
```c
static void framebuffer_resize_cb(GLFWwindow* window, int w, int h) {
    (void)w; (void)h;
    Renderer* r = glfwGetWindowUserPointer(window);
    r->framebuffer_resized = true;
}
```

- [ ] **Step 4: Verify it compiles and runs**

Run:
```bash
cmake --build build 2>&1 | tail -10
./build/minecraft
```
Expected: Window opens, Vulkan init + swapchain created. Close cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/swapchain.h src/swapchain.c src/renderer.c src/renderer.h
git commit -m "feat: swapchain, render pass, sync objects, and UBOs"
```

---

## Task 5: Graphics Pipeline + Shaders (pipeline.h/.c, shaders)

**Files:**
- Create: `src/pipeline.h`, `src/pipeline.c`, `shaders/block.vert`, `shaders/block.frag`

**Context:** Pipeline creation with shader loading, descriptor set layout, push constant range. The shaders implement block rendering with directional light and AO.

- [ ] **Step 1: Write shaders/block.vert**

```glsl
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_normal;
layout(location = 3) in uint in_ao;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 sun_direction;
    vec4 sun_color;
    float ambient;
} ubo;

layout(push_constant) uniform PushConstants {
    vec4 chunk_offset;
} pc;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out float frag_light;
layout(location = 2) out float frag_ao;

const vec3 NORMALS[6] = vec3[6](
    vec3( 1, 0, 0),
    vec3(-1, 0, 0),
    vec3( 0, 1, 0),
    vec3( 0,-1, 0),
    vec3( 0, 0, 1),
    vec3( 0, 0,-1)
);

void main() {
    vec3 world_pos = in_pos + pc.chunk_offset.xyz;
    gl_Position = ubo.proj * ubo.view * vec4(world_pos, 1.0);

    vec3 normal = NORMALS[in_normal];
    float ndotl = max(dot(normal, -ubo.sun_direction.xyz), 0.0);
    frag_light = ubo.ambient + (1.0 - ubo.ambient) * ndotl * ubo.sun_color.r;

    frag_uv = in_uv;
    frag_ao = float(in_ao) / 3.0;
}
```

- [ ] **Step 2: Write shaders/block.frag**

```glsl
#version 450

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in float frag_light;
layout(location = 2) in float frag_ao;

layout(set = 0, binding = 1) uniform sampler2D tex_atlas;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(tex_atlas, frag_uv);
    if (tex_color.a < 0.5) discard;

    float ao_factor = 0.4 + 0.6 * frag_ao;
    out_color = vec4(tex_color.rgb * frag_light * ao_factor, 1.0);
}
```

- [ ] **Step 3: Write pipeline.h**

```c
#ifndef PIPELINE_H
#define PIPELINE_H

#include <volk.h>
#include <stdbool.h>

bool pipeline_create(VkDevice device, VkRenderPass render_pass,
                     VkDescriptorSetLayout desc_layout,
                     const char* vert_path, const char* frag_path,
                     VkPipelineLayout* out_layout, VkPipeline* out_pipeline);

bool pipeline_create_descriptor_layout(VkDevice device, VkDescriptorSetLayout* out);

#endif
```

- [ ] **Step 4: Write pipeline.c**

```c
#include "pipeline.h"
#include "vertex.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open shader: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(*size);
    fread(buf, 1, *size, f);
    fclose(f);
    return buf;
}

static VkShaderModule create_shader_module(VkDevice device, const uint8_t* code, size_t size) {
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t*)code,
    };
    VkShaderModule mod;
    if (vkCreateShaderModule(device, &ci, NULL, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
}

bool pipeline_create_descriptor_layout(VkDevice device, VkDescriptorSetLayout* out) {
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    return vkCreateDescriptorSetLayout(device, &ci, NULL, out) == VK_SUCCESS;
}

bool pipeline_create(VkDevice device, VkRenderPass render_pass,
                     VkDescriptorSetLayout desc_layout,
                     const char* vert_path, const char* frag_path,
                     VkPipelineLayout* out_layout, VkPipeline* out_pipeline)
{
    // Load shaders
    size_t vert_size, frag_size;
    uint8_t* vert_code = read_file(vert_path, &vert_size);
    uint8_t* frag_code = read_file(frag_path, &frag_size);
    if (!vert_code || !frag_code) {
        free(vert_code);
        free(frag_code);
        return false;
    }

    VkShaderModule vert_mod = create_shader_module(device, vert_code, vert_size);
    VkShaderModule frag_mod = create_shader_module(device, frag_code, frag_size);
    free(vert_code);
    free(frag_code);

    if (!vert_mod || !frag_mod) {
        if (vert_mod) vkDestroyShaderModule(device, vert_mod, NULL);
        if (frag_mod) vkDestroyShaderModule(device, frag_mod, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_mod,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_mod,
            .pName = "main",
        },
    };

    // Vertex input
    VkVertexInputBindingDescription binding = vertex_binding_desc();
    VkVertexInputAttributeDescription attrs[4];
    vertex_attr_descs(attrs);

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attrs,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };

    VkPipelineColorBlendAttachmentState blend_attach = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attach,
    };

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };

    // Push constants
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ChunkPushConstants),
    };

    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    if (vkCreatePipelineLayout(device, &layout_ci, NULL, out_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert_mod, NULL);
        vkDestroyShaderModule(device, frag_mod, NULL);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipe_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic,
        .layout = *out_layout,
        .renderPass = render_pass,
        .subpass = 0,
    };

    VkResult res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe_ci, NULL, out_pipeline);

    vkDestroyShaderModule(device, vert_mod, NULL);
    vkDestroyShaderModule(device, frag_mod, NULL);

    return res == VK_SUCCESS;
}
```

- [ ] **Step 5: Integrate pipeline creation into renderer_init**

Add after render pass + swapchain creation in `renderer_init()`:

```c
    // 13. Descriptor set layout + pipeline
    if (!pipeline_create_descriptor_layout(r->device, &r->desc_set_layout))
        return false;
    if (!pipeline_create(r->device, r->render_pass, r->desc_set_layout,
                         "shaders/block.vert.spv", "shaders/block.frag.spv",
                         &r->pipeline_layout, &r->pipeline))
        return false;
```

Also create the descriptor pool and allocate descriptor sets:

```c
static bool create_descriptors(Renderer* r) {
    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_FRAMES_IN_FLIGHT },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES_IN_FLIGHT },
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_FRAMES_IN_FLIGHT,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    if (vkCreateDescriptorPool(r->device, &pool_ci, NULL, &r->desc_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) layouts[i] = r->desc_set_layout;

    VkDescriptorSetAllocateInfo alloc_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->desc_pool,
        .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
        .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(r->device, &alloc_ci, r->desc_sets) != VK_SUCCESS)
        return false;

    // Write UBO descriptors (texture written later in Task 6)
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo buf_info = {
            .buffer = r->ubo_bufs[i],
            .offset = 0,
            .range = sizeof(GlobalUBO),
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->desc_sets[i],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &buf_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }
    return true;
}
```

Call after pipeline creation:
```c
    if (!create_descriptors(r)) return false;
```

- [ ] **Step 6: Verify shaders compile and project builds**

Run:
```bash
cmake --build build 2>&1 | tail -15
```
Expected: Shaders compile to .spv, project links cleanly.

- [ ] **Step 7: Commit**

```bash
git add src/pipeline.h src/pipeline.c shaders/block.vert shaders/block.frag src/renderer.c
git commit -m "feat: graphics pipeline with block shaders and descriptor sets"
```

---

## Task 6: Texture Atlas (texture.h/.c)

**Files:**
- Create: `src/texture.h`, `src/texture.c`

**Context:** Programmatically generate a colored-square texture atlas for initial testing (no external PNG needed yet). Upload via staging buffer to GPU image.

- [ ] **Step 1: Write texture.h**

```c
#ifndef TEXTURE_H
#define TEXTURE_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <stdbool.h>

typedef struct Renderer Renderer;

// Generate a programmatic atlas and upload to GPU
bool texture_create_atlas(Renderer* r);

// Update descriptor sets with the atlas sampler
void texture_write_descriptors(Renderer* r);

#endif
```

- [ ] **Step 2: Write texture.c**

```c
#include "texture.h"
#include "renderer.h"
#include "block.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ATLAS_SIZE 256
#define TILE_SIZE  16
#define TILES_PER_ROW (ATLAS_SIZE / TILE_SIZE)

// Colors for each tile index (RGBA)
static void get_tile_color(uint8_t tile_idx, uint8_t rgba[4]) {
    // Default gray
    rgba[0] = 128; rgba[1] = 128; rgba[2] = 128; rgba[3] = 255;

    switch (tile_idx) {
        case 0:  rgba[0]=120; rgba[1]=120; rgba[2]=120; break; // stone
        case 1:  rgba[0]=139; rgba[1]=90;  rgba[2]=43;  break; // dirt
        case 2:  rgba[0]=76;  rgba[1]=153; rgba[2]=0;   break; // grass top
        case 3:  rgba[0]=139; rgba[1]=105; rgba[2]=60;  break; // grass side (dirt with green strip)
        case 4:  rgba[0]=210; rgba[1]=190; rgba[2]=140; break; // sand
        case 5:  rgba[0]=160; rgba[1]=120; rgba[2]=60;  break; // wood top
        case 6:  rgba[0]=120; rgba[1]=80;  rgba[2]=40;  break; // wood side
        case 7:  rgba[0]=40;  rgba[1]=120; rgba[2]=20;  rgba[3]=200; break; // leaves (semi-transparent)
        case 16: rgba[0]=30;  rgba[1]=80;  rgba[2]=180; rgba[3]=200; break; // water
        case 17: rgba[0]=60;  rgba[1]=60;  rgba[2]=60;  break; // bedrock
    }
}

static uint8_t* generate_atlas_pixels(void) {
    uint8_t* pixels = calloc(ATLAS_SIZE * ATLAS_SIZE * 4, 1);

    for (int tile_idx = 0; tile_idx < TILES_PER_ROW * TILES_PER_ROW; tile_idx++) {
        uint8_t color[4];
        get_tile_color((uint8_t)tile_idx, color);

        int tx = tile_idx % TILES_PER_ROW;
        int ty = tile_idx / TILES_PER_ROW;

        for (int py = 0; py < TILE_SIZE; py++) {
            for (int px = 0; px < TILE_SIZE; px++) {
                int x = tx * TILE_SIZE + px;
                int y = ty * TILE_SIZE + py;
                int idx = (y * ATLAS_SIZE + x) * 4;

                // Add slight variation for texture
                int var = ((px ^ py) & 1) * 8;
                pixels[idx + 0] = (uint8_t)(color[0] > var ? color[0] - var : 0);
                pixels[idx + 1] = (uint8_t)(color[1] > var ? color[1] - var : 0);
                pixels[idx + 2] = (uint8_t)(color[2] > var ? color[2] - var : 0);
                pixels[idx + 3] = color[3];

                // Grass side: green strip on top 4 pixels
                if (tile_idx == 3 && py < 4) {
                    pixels[idx + 0] = (uint8_t)(76 > var ? 76 - var : 0);
                    pixels[idx + 1] = (uint8_t)(153 > var ? 153 - var : 0);
                    pixels[idx + 2] = 0;
                }
            }
        }
    }
    return pixels;
}

static void transition_image_layout(Renderer* r, VkImage image,
                                     VkImageLayout old_layout, VkImageLayout new_layout) {
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    VkPipelineStageFlags src_stage, dst_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                          0, NULL, 0, NULL, 1, &barrier);

    renderer_end_single_cmd(r, cmd);
}

bool texture_create_atlas(Renderer* r) {
    uint8_t* pixels = generate_atlas_pixels();
    VkDeviceSize image_size = ATLAS_SIZE * ATLAS_SIZE * 4;

    // Staging buffer
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo saci = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer staging;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;
    vmaCreateBuffer(r->allocator, &bci, &saci, &staging, &staging_alloc, &staging_info);
    memcpy(staging_info.pMappedData, pixels, image_size);
    free(pixels);

    // GPU image
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = { ATLAS_SIZE, ATLAS_SIZE, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VmaAllocationCreateInfo iaci = { .usage = VMA_MEMORY_USAGE_AUTO };
    vmaCreateImage(r->allocator, &ici, &iaci, &r->atlas_image, &r->atlas_alloc, NULL);

    // Transfer
    transition_image_layout(r, r->atlas_image,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkCommandBuffer cmd = renderer_begin_single_cmd(r);
    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { ATLAS_SIZE, ATLAS_SIZE, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, r->atlas_image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    renderer_end_single_cmd(r, cmd);

    transition_image_layout(r, r->atlas_image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vmaDestroyBuffer(r->allocator, staging, staging_alloc);

    // Image view
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = r->atlas_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCreateImageView(r->device, &vci, NULL, &r->atlas_view);

    // Sampler
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    vkCreateSampler(r->device, &sci, NULL, &r->atlas_sampler);

    return true;
}

void texture_write_descriptors(Renderer* r) {
    VkDescriptorImageInfo img_info = {
        .sampler = r->atlas_sampler,
        .imageView = r->atlas_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->desc_sets[i],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &img_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }
}
```

- [ ] **Step 3: Integrate into renderer_init**

Add after `create_descriptors()`:
```c
    if (!texture_create_atlas(r)) return false;
    texture_write_descriptors(r);
```

- [ ] **Step 4: Verify it compiles**

Run:
```bash
cmake --build build 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add src/texture.h src/texture.c src/renderer.c
git commit -m "feat: programmatic texture atlas with GPU upload"
```

---

## Task 7: Minimal Rendering Loop (renderer.c draw_frame + main.c)

**Files:**
- Modify: `src/renderer.c` (implement draw_frame)
- Modify: `src/main.c` (render loop with hardcoded test geometry)

**Context:** Get a visible rendering on screen. Use hardcoded triangle/quad data to verify the pipeline works end-to-end before building the mesher.

- [ ] **Step 1: Implement renderer_draw_frame in renderer.c**

Replace the stub with the full frame rendering implementation:

```c
static void recreate_swapchain(Renderer* r) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(r->window, &w, &h);
    while (w == 0 || h == 0) {
        glfwGetFramebufferSize(r->window, &w, &h);
        glfwWaitEvents();
    }
    vkDeviceWaitIdle(r->device);

    VkSwapchainKHR old = r->swapchain.swapchain;
    Swapchain new_sc;
    swapchain_create(r->physical_device, r->device, r->allocator, r->surface,
                     r->graphics_family, r->present_family, r->render_pass,
                     w, h, old, &new_sc);

    // Destroy old (images are freed when old swapchain is destroyed)
    for (uint32_t i = 0; i < r->swapchain.image_count; i++) {
        vkDestroyFramebuffer(r->device, r->swapchain.framebuffers[i], NULL);
        vkDestroyImageView(r->device, r->swapchain.image_views[i], NULL);
    }
    vkDestroyImageView(r->device, r->swapchain.depth_view, NULL);
    vmaDestroyImage(r->allocator, r->swapchain.depth_image, r->swapchain.depth_alloc);
    free(r->swapchain.framebuffers);
    free(r->swapchain.image_views);
    free(r->swapchain.images);
    vkDestroySwapchainKHR(r->device, old, NULL);

    r->swapchain = new_sc;
}

void renderer_draw_frame(Renderer* r, ChunkMesh* meshes, uint32_t mesh_count,
                         mat4 view, mat4 proj, vec3 sun_dir)
{
    uint32_t frame = r->current_frame;

    vkWaitForFences(r->device, 1, &r->in_flight[frame], VK_TRUE, UINT64_MAX);

    uint32_t image_idx;
    VkResult res = vkAcquireNextImageKHR(r->device, r->swapchain.swapchain,
                                          UINT64_MAX, r->image_available[frame],
                                          VK_NULL_HANDLE, &image_idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(r);
        return;
    }

    vkResetFences(r->device, 1, &r->in_flight[frame]);

    // Update UBO
    GlobalUBO ubo;
    glm_mat4_copy(view, ubo.view);
    glm_mat4_copy(proj, ubo.proj);
    glm_vec3_copy(sun_dir, ubo.sun_direction);
    ubo.sun_direction[3] = 0.0f;
    ubo.sun_color[0] = 1.0f; ubo.sun_color[1] = 1.0f;
    ubo.sun_color[2] = 0.9f; ubo.sun_color[3] = 1.0f;
    ubo.ambient = 0.3f;
    memcpy(r->ubo_mapped[frame], &ubo, sizeof(ubo));

    // Record command buffer
    VkCommandBuffer cmd = r->cmd_bufs[frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clears[2] = {
        { .color = {{ 0.53f, 0.81f, 0.92f, 1.0f }} },
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->swapchain.framebuffers[image_idx],
        .renderArea = { .extent = r->swapchain.extent },
        .clearValueCount = 2,
        .pClearValues = clears,
    };
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);

    VkViewport viewport = {
        .width = (float)r->swapchain.extent.width,
        .height = (float)r->swapchain.extent.height,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = { .extent = r->swapchain.extent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             r->pipeline_layout, 0, 1,
                             &r->desc_sets[frame], 0, NULL);

    // Draw chunks
    if (meshes) {
        for (uint32_t i = 0; i < mesh_count; i++) {
            ChunkMesh* m = &meshes[i];
            if (!m->uploaded || m->index_count == 0) continue;

            ChunkPushConstants pc = {0};
            glm_vec3_copy(m->chunk_origin, pc.chunk_offset);
            vkCmdPushConstants(cmd, r->pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &r->image_available[frame],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &r->render_finished[frame],
    };
    vkQueueSubmit(r->graphics_queue, 1, &submit, r->in_flight[frame]);

    // Present
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &r->render_finished[frame],
        .swapchainCount = 1,
        .pSwapchains = &r->swapchain.swapchain,
        .pImageIndices = &image_idx,
    };
    res = vkQueuePresentKHR(r->present_queue, &present);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR
        || r->framebuffer_resized) {
        r->framebuffer_resized = false;
        recreate_swapchain(r);
    }

    r->current_frame = (frame + 1) % MAX_FRAMES_IN_FLIGHT;
}
```

- [ ] **Step 2: Update main.c to render sky-blue background**

```c
#include <stdio.h>
#include <stdbool.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"

int main(void) {
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    mat4 view, proj;
    glm_lookat((vec3){0, 80, 5}, (vec3){0, 80, 0}, (vec3){0, 1, 0}, view);
    glm_perspective(glm_rad(70.0f), 1280.0f / 720.0f, 0.1f, 1000.0f, proj);
    proj[1][1] *= -1.0f; // Vulkan Y-flip

    vec3 sun_dir = { -0.5f, -0.8f, -0.3f };
    glm_vec3_normalize(sun_dir);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        renderer_draw_frame(&renderer, NULL, 0, view, proj, sun_dir);
    }

    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 3: Verify sky-blue window renders**

Run:
```bash
cmake --build build && ./build/minecraft
```
Expected: Window shows solid sky-blue background. No validation errors.

- [ ] **Step 4: Commit**

```bash
git add src/renderer.c src/main.c
git commit -m "feat: rendering loop with sky-blue clear color"
```

---

## Task 8: First-Person Camera (camera.h/.c)

**Files:**
- Create: `src/camera.h`, `src/camera.c`
- Modify: `src/main.c`

- [ ] **Step 1: Write camera.h**

```c
#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>

typedef struct Camera {
    vec3  position;
    float yaw;       // radians
    float pitch;     // radians
    float speed;
    float sensitivity;
    float fov;
    float last_x, last_y;
    bool  first_mouse;
} Camera;

void camera_init(Camera* cam, vec3 start_pos);
void camera_process_mouse(Camera* cam, double xpos, double ypos);
void camera_process_keyboard(Camera* cam, GLFWwindow* window, float dt);
void camera_get_view(Camera* cam, mat4 out);
void camera_get_proj(Camera* cam, float aspect, mat4 out);
void camera_get_front(Camera* cam, vec3 out);

#endif
```

- [ ] **Step 2: Write camera.c**

```c
#include "camera.h"
#include <math.h>

void camera_init(Camera* cam, vec3 start_pos) {
    glm_vec3_copy(start_pos, cam->position);
    cam->yaw = -GLM_PI_2f;  // face -Z
    cam->pitch = 0.0f;
    cam->speed = 20.0f;
    cam->sensitivity = 0.002f;
    cam->fov = glm_rad(70.0f);
    cam->first_mouse = true;
}

void camera_get_front(Camera* cam, vec3 out) {
    out[0] = cosf(cam->pitch) * cosf(cam->yaw);
    out[1] = sinf(cam->pitch);
    out[2] = cosf(cam->pitch) * sinf(cam->yaw);
    glm_vec3_normalize(out);
}

void camera_process_mouse(Camera* cam, double xpos, double ypos) {
    float x = (float)xpos;
    float y = (float)ypos;

    if (cam->first_mouse) {
        cam->last_x = x;
        cam->last_y = y;
        cam->first_mouse = false;
        return;
    }

    float dx = x - cam->last_x;
    float dy = cam->last_y - y;  // inverted Y
    cam->last_x = x;
    cam->last_y = y;

    cam->yaw += dx * cam->sensitivity;
    cam->pitch += dy * cam->sensitivity;

    // Clamp pitch
    if (cam->pitch > GLM_PI_2f - 0.01f) cam->pitch = GLM_PI_2f - 0.01f;
    if (cam->pitch < -GLM_PI_2f + 0.01f) cam->pitch = -GLM_PI_2f + 0.01f;
}

void camera_process_keyboard(Camera* cam, GLFWwindow* window, float dt) {
    float velocity = cam->speed * dt;

    vec3 front;
    camera_get_front(cam, front);

    // Horizontal movement (ignore Y component)
    vec3 forward = { front[0], 0, front[2] };
    glm_vec3_normalize(forward);

    vec3 right;
    vec3 up = { 0, 1, 0 };
    glm_vec3_cross(forward, up, right);
    glm_vec3_normalize(right);

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        glm_vec3_muladds(forward, velocity, cam->position);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        glm_vec3_muladds(forward, -velocity, cam->position);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        glm_vec3_muladds(right, -velocity, cam->position);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        glm_vec3_muladds(right, velocity, cam->position);
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
        cam->position[1] += velocity;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        cam->position[1] -= velocity;
}

void camera_get_view(Camera* cam, mat4 out) {
    vec3 front;
    camera_get_front(cam, front);

    vec3 center;
    glm_vec3_add(cam->position, front, center);

    glm_lookat(cam->position, center, (vec3){0, 1, 0}, out);
}

void camera_get_proj(Camera* cam, float aspect, mat4 out) {
    glm_perspective(cam->fov, aspect, 0.1f, 1000.0f, out);
    out[1][1] *= -1.0f; // Vulkan Y-flip
}
```

- [ ] **Step 3: Update main.c to use camera**

```c
#include <stdio.h>
#include <stdbool.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"
#include "camera.h"

static Camera g_camera;

static void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    camera_process_mouse(&g_camera, xpos, ypos);
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode; (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

int main(void) {
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetKeyCallback(window, key_callback);

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    camera_init(&g_camera, (vec3){0, 80, 0});

    vec3 sun_dir = { -0.5f, -0.8f, -0.3f };
    glm_vec3_normalize(sun_dir);

    double last_time = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        glfwPollEvents();
        camera_process_keyboard(&g_camera, window, dt);

        mat4 view, proj;
        camera_get_view(&g_camera, view);
        float aspect = (float)renderer.swapchain.extent.width
                     / (float)renderer.swapchain.extent.height;
        camera_get_proj(&g_camera, aspect, proj);

        renderer_draw_frame(&renderer, NULL, 0, view, proj, sun_dir);
    }

    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 4: Verify camera works**

Run: `cmake --build build && ./build/minecraft`
Expected: Sky-blue background, mouse look works (view direction changes), WASD moves camera position. Escape closes window.

- [ ] **Step 5: Commit**

```bash
git add src/camera.h src/camera.c src/main.c
git commit -m "feat: first-person camera with WASD movement and mouse look"
```

---

## Task 9: Frustum Culling (frustum.h/.c)

**Files:**
- Create: `src/frustum.h`, `src/frustum.c`

- [ ] **Step 1: Write frustum.h**

```c
#ifndef FRUSTUM_H
#define FRUSTUM_H

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct Frustum {
    vec4 planes[6];
} Frustum;

void frustum_extract(mat4 vp, Frustum* f);
bool frustum_test_aabb(const Frustum* f, vec3 min, vec3 max);

#endif
```

- [ ] **Step 2: Write frustum.c**

```c
#include "frustum.h"

void frustum_extract(mat4 vp, Frustum* f) {
    glm_frustum_planes(vp, f->planes);
}

bool frustum_test_aabb(const Frustum* f, vec3 min, vec3 max) {
    vec3 box[2];
    glm_vec3_copy(min, box[0]);
    glm_vec3_copy(max, box[1]);
    return glm_aabb_frustum(box, (vec4*)f->planes);
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -5`

- [ ] **Step 4: Commit**

```bash
git add src/frustum.h src/frustum.c
git commit -m "feat: frustum culling with AABB test"
```

---

## Task 10: Integrate Frustum Culling into Draw Frame

**Files:**
- Modify: `src/renderer.c`, `src/renderer.h`

- [ ] **Step 1: Add frustum culling to draw loop**

In `renderer_draw_frame()`, before the chunk draw loop, add:

```c
    #include "frustum.h"

    // Extract frustum
    mat4 vp;
    glm_mat4_mul(proj, view, vp);
    Frustum frustum;
    frustum_extract(vp, &frustum);
```

Then in the draw loop, add the cull check:

```c
    if (!frustum_test_aabb(&frustum, m->aabb_min, m->aabb_max)) continue;
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -5`

- [ ] **Step 3: Commit**

```bash
git add src/renderer.c
git commit -m "feat: frustum culling in draw loop"
```

---

## Task 11: Chunk Data Structure + GPU Mesh Buffers (chunk.h/.c, chunk_mesh.h/.c)

**Files:**
- Create: `src/chunk.h`, `src/chunk.c`, `src/chunk_mesh.h`, `src/chunk_mesh.c`

- [ ] **Step 1: Write chunk.h**

```c
#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "block.h"
#include "chunk_mesh.h"

#define CHUNK_X 16
#define CHUNK_Y 256
#define CHUNK_Z 16
#define CHUNK_BLOCKS (CHUNK_X * CHUNK_Y * CHUNK_Z)

typedef enum ChunkState {
    CHUNK_UNLOADED = 0,
    CHUNK_GENERATING,
    CHUNK_GENERATED,
    CHUNK_MESHING,
    CHUNK_MESHED,
    CHUNK_READY,
} ChunkState;

typedef struct Chunk {
    int32_t          cx, cz;        // chunk coordinates
    _Atomic int      state;
    BlockID          blocks[CHUNK_BLOCKS];
    ChunkMesh        mesh;
} Chunk;

Chunk* chunk_create(int32_t cx, int32_t cz);
void   chunk_destroy(Chunk* chunk);

static inline BlockID chunk_get_block(const Chunk* c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
        return BLOCK_AIR;
    return c->blocks[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
}

static inline void chunk_set_block(Chunk* c, int x, int y, int z, BlockID id) {
    if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z) return;
    c->blocks[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z] = id;
}

#endif
```

- [ ] **Step 2: Write chunk.c**

```c
#include "chunk.h"
#include <stdlib.h>
#include <string.h>

Chunk* chunk_create(int32_t cx, int32_t cz) {
    Chunk* c = calloc(1, sizeof(Chunk));
    c->cx = cx;
    c->cz = cz;
    atomic_store(&c->state, CHUNK_UNLOADED);
    return c;
}

void chunk_destroy(Chunk* chunk) {
    // GPU buffers must be freed by caller before this
    free(chunk);
}
```

- [ ] **Step 3: Write chunk_mesh.h**

```c
#ifndef CHUNK_MESH_H
#define CHUNK_MESH_H

#include <volk.h>
#include <vk_mem_alloc.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>
#include "vertex.h"

typedef struct Renderer Renderer;

typedef struct ChunkMesh {
    VkBuffer       vertex_buffer;
    VmaAllocation  vertex_alloc;
    VkBuffer       index_buffer;
    VmaAllocation  index_alloc;
    uint32_t       index_count;
    vec3           aabb_min;
    vec3           aabb_max;
    vec3           chunk_origin;  // world-space origin for push constant
    bool           uploaded;
} ChunkMesh;

bool chunk_mesh_upload(Renderer* r, ChunkMesh* mesh,
                       BlockVertex* vertices, uint32_t vertex_count,
                       uint32_t* indices, uint32_t index_count);

void chunk_mesh_destroy(VmaAllocator allocator, ChunkMesh* mesh);

#endif
```

- [ ] **Step 4: Write chunk_mesh.c**

```c
#include "chunk_mesh.h"
#include "renderer.h"
#include <string.h>
#include <stdio.h>

static bool upload_buffer(Renderer* r, const void* data, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkBuffer* out_buf,
                          VmaAllocation* out_alloc)
{
    // Staging
    VkBufferCreateInfo staging_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VmaAllocationCreateInfo staging_aci = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkBuffer staging;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;
    if (vmaCreateBuffer(r->allocator, &staging_ci, &staging_aci,
                        &staging, &staging_alloc, &staging_info) != VK_SUCCESS)
        return false;

    memcpy(staging_info.pMappedData, data, size);

    // GPU buffer
    VkBufferCreateInfo gpu_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VmaAllocationCreateInfo gpu_aci = { .usage = VMA_MEMORY_USAGE_AUTO };
    if (vmaCreateBuffer(r->allocator, &gpu_ci, &gpu_aci,
                        out_buf, out_alloc, NULL) != VK_SUCCESS) {
        vmaDestroyBuffer(r->allocator, staging, staging_alloc);
        return false;
    }

    // Copy
    VkCommandBuffer cmd = renderer_begin_single_cmd(r);
    VkBufferCopy copy = { .size = size };
    vkCmdCopyBuffer(cmd, staging, *out_buf, 1, &copy);
    renderer_end_single_cmd(r, cmd);

    vmaDestroyBuffer(r->allocator, staging, staging_alloc);
    return true;
}

bool chunk_mesh_upload(Renderer* r, ChunkMesh* mesh,
                       BlockVertex* vertices, uint32_t vertex_count,
                       uint32_t* indices, uint32_t index_count)
{
    if (vertex_count == 0 || index_count == 0) {
        mesh->index_count = 0;
        mesh->uploaded = false;
        return true;
    }

    if (!upload_buffer(r, vertices, sizeof(BlockVertex) * vertex_count,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       &mesh->vertex_buffer, &mesh->vertex_alloc))
        return false;

    if (!upload_buffer(r, indices, sizeof(uint32_t) * index_count,
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       &mesh->index_buffer, &mesh->index_alloc)) {
        vmaDestroyBuffer(r->allocator, mesh->vertex_buffer, mesh->vertex_alloc);
        return false;
    }

    mesh->index_count = index_count;
    mesh->uploaded = true;
    return true;
}

void chunk_mesh_destroy(VmaAllocator allocator, ChunkMesh* mesh) {
    if (mesh->vertex_buffer)
        vmaDestroyBuffer(allocator, mesh->vertex_buffer, mesh->vertex_alloc);
    if (mesh->index_buffer)
        vmaDestroyBuffer(allocator, mesh->index_buffer, mesh->index_alloc);
    memset(mesh, 0, sizeof(*mesh));
}
```

- [ ] **Step 5: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -5`

- [ ] **Step 6: Commit**

```bash
git add src/chunk.h src/chunk.c src/chunk_mesh.h src/chunk_mesh.c
git commit -m "feat: chunk data structure and GPU mesh upload"
```

---

## Task 12: Chunk Hash Map (chunk_map.h/.c)

**Files:**
- Create: `src/chunk_map.h`, `src/chunk_map.c`

- [ ] **Step 1: Write chunk_map.h**

```c
#ifndef CHUNK_MAP_H
#define CHUNK_MAP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Chunk Chunk;

typedef struct ChunkMapEntry {
    int32_t cx, cz;
    Chunk*  chunk;
    bool    occupied;
} ChunkMapEntry;

typedef struct ChunkMap {
    ChunkMapEntry* entries;
    uint32_t       capacity;
    uint32_t       count;
} ChunkMap;

void   chunk_map_init(ChunkMap* map, uint32_t capacity);
void   chunk_map_free(ChunkMap* map);
Chunk* chunk_map_get(ChunkMap* map, int32_t cx, int32_t cz);
void   chunk_map_put(ChunkMap* map, Chunk* chunk);
Chunk* chunk_map_remove(ChunkMap* map, int32_t cx, int32_t cz);

// Iterator: call with idx starting at 0, returns next chunk or NULL when done
Chunk* chunk_map_iter(ChunkMap* map, uint32_t* idx);

#endif
```

- [ ] **Step 2: Write chunk_map.c**

```c
#include "chunk_map.h"
#include "chunk.h"
#include <stdlib.h>
#include <string.h>

static uint64_t hash_key(int32_t cx, int32_t cz) {
    uint64_t h = (uint64_t)(uint32_t)cx | ((uint64_t)(uint32_t)cz << 32);
    // splitmix64
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

void chunk_map_init(ChunkMap* map, uint32_t capacity) {
    // Round up to power of 2
    uint32_t cap = 1;
    while (cap < capacity) cap <<= 1;
    map->entries = calloc(cap, sizeof(ChunkMapEntry));
    map->capacity = cap;
    map->count = 0;
}

void chunk_map_free(ChunkMap* map) {
    free(map->entries);
    memset(map, 0, sizeof(*map));
}

static uint32_t probe(ChunkMap* map, int32_t cx, int32_t cz) {
    uint32_t mask = map->capacity - 1;
    uint32_t idx = (uint32_t)(hash_key(cx, cz) & mask);
    while (map->entries[idx].occupied) {
        if (map->entries[idx].cx == cx && map->entries[idx].cz == cz)
            return idx;
        idx = (idx + 1) & mask;
    }
    return idx;
}

static void rehash(ChunkMap* map) {
    uint32_t old_cap = map->capacity;
    ChunkMapEntry* old = map->entries;

    map->capacity *= 2;
    map->entries = calloc(map->capacity, sizeof(ChunkMapEntry));
    map->count = 0;

    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].occupied) {
            chunk_map_put(map, old[i].chunk);
        }
    }
    free(old);
}

Chunk* chunk_map_get(ChunkMap* map, int32_t cx, int32_t cz) {
    uint32_t idx = probe(map, cx, cz);
    if (map->entries[idx].occupied) return map->entries[idx].chunk;
    return NULL;
}

void chunk_map_put(ChunkMap* map, Chunk* chunk) {
    if (map->count * 10 >= map->capacity * 7) // 70% load
        rehash(map);

    uint32_t idx = probe(map, chunk->cx, chunk->cz);
    if (!map->entries[idx].occupied) {
        map->entries[idx].cx = chunk->cx;
        map->entries[idx].cz = chunk->cz;
        map->entries[idx].chunk = chunk;
        map->entries[idx].occupied = true;
        map->count++;
    } else {
        map->entries[idx].chunk = chunk;
    }
}

Chunk* chunk_map_remove(ChunkMap* map, int32_t cx, int32_t cz) {
    uint32_t mask = map->capacity - 1;
    uint32_t idx = probe(map, cx, cz);
    if (!map->entries[idx].occupied) return NULL;

    Chunk* removed = map->entries[idx].chunk;
    map->entries[idx].occupied = false;
    map->count--;

    // Re-insert displaced entries (linear probing fixup)
    uint32_t next = (idx + 1) & mask;
    while (map->entries[next].occupied) {
        ChunkMapEntry tmp = map->entries[next];
        map->entries[next].occupied = false;
        map->count--;
        chunk_map_put(map, tmp.chunk);
        next = (next + 1) & mask;
    }

    return removed;
}

Chunk* chunk_map_iter(ChunkMap* map, uint32_t* idx) {
    while (*idx < map->capacity) {
        if (map->entries[*idx].occupied) {
            Chunk* c = map->entries[*idx].chunk;
            (*idx)++;
            return c;
        }
        (*idx)++;
    }
    return NULL;
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -5`

- [ ] **Step 4: Commit**

```bash
git add src/chunk_map.h src/chunk_map.c
git commit -m "feat: open-addressing hash map for chunk storage"
```

---

## Task 13: World Generation (worldgen.h/.c)

**Files:**
- Create: `src/worldgen.h`, `src/worldgen.c`

- [ ] **Step 1: Write worldgen.h**

```c
#ifndef WORLDGEN_H
#define WORLDGEN_H

typedef struct Chunk Chunk;

// Generate terrain for a chunk. Sets chunk state to GENERATED.
void worldgen_generate(Chunk* chunk, int seed);

#endif
```

- [ ] **Step 2: Write worldgen.c**

```c
#include "worldgen.h"
#include "chunk.h"
#include "block.h"
#include <string.h>

#define FNL_IMPL
#include "FastNoiseLite.h"

#define SEA_LEVEL 62
#define BASE_HEIGHT 64

void worldgen_generate(Chunk* chunk, int seed) {
    memset(chunk->blocks, BLOCK_AIR, CHUNK_BLOCKS);

    fnl_state noise = fnlCreateState();
    noise.seed = seed;
    noise.noise_type = FNL_NOISE_PERLIN;
    noise.fractal_type = FNL_FRACTAL_FBM;
    noise.octaves = 3;
    noise.frequency = 0.005f;

    // Separate noise for tree placement
    fnl_state tree_noise = fnlCreateState();
    tree_noise.seed = seed + 12345;
    tree_noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    tree_noise.frequency = 0.5f;

    int wx_base = chunk->cx * CHUNK_X;
    int wz_base = chunk->cz * CHUNK_Z;

    // Height map
    int heights[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            float wx = (float)(wx_base + x);
            float wz = (float)(wz_base + z);
            float n = fnlGetNoise2D(&noise, wx, wz); // [-1, 1]
            heights[x][z] = BASE_HEIGHT + (int)(n * 20.0f);
        }
    }

    // Fill blocks
    for (int x = 0; x < CHUNK_X; x++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            int h = heights[x][z];

            for (int y = 0; y < CHUNK_Y; y++) {
                BlockID block = BLOCK_AIR;

                if (y == 0) {
                    block = BLOCK_BEDROCK;
                } else if (y < 10) {
                    // Bedrock with some stone mixed in
                    float bn = fnlGetNoise3D(&noise, (float)(wx_base+x)*10.0f,
                                             (float)y*10.0f, (float)(wz_base+z)*10.0f);
                    block = (bn > 0.3f && y < 5) ? BLOCK_BEDROCK : BLOCK_STONE;
                } else if (y < h - 3) {
                    block = BLOCK_STONE;
                } else if (y < h) {
                    // Near sea level: sand, otherwise dirt
                    block = (h <= SEA_LEVEL + 2) ? BLOCK_SAND : BLOCK_DIRT;
                } else if (y == h) {
                    if (h <= SEA_LEVEL) {
                        block = BLOCK_SAND;
                    } else {
                        block = BLOCK_GRASS;
                    }
                } else if (y <= SEA_LEVEL) {
                    block = BLOCK_WATER;
                }

                chunk_set_block(chunk, x, y, z, block);
            }
        }
    }

    // Trees - only on grass blocks, constrained to [2..13] to avoid cross-chunk
    for (int x = 2; x < CHUNK_X - 2; x++) {
        for (int z = 2; z < CHUNK_Z - 2; z++) {
            float tn = fnlGetNoise2D(&tree_noise,
                                      (float)(wx_base + x),
                                      (float)(wz_base + z));
            if (tn < 0.85f) continue; // ~2% chance

            int h = heights[x][z];
            if (h <= SEA_LEVEL) continue;
            if (chunk_get_block(chunk, x, h, z) != BLOCK_GRASS) continue;

            // Trunk height 4-6
            int trunk_h = 4 + (int)((tn - 0.85f) * 20.0f);
            if (trunk_h > 6) trunk_h = 6;

            for (int ty = h + 1; ty <= h + trunk_h && ty < CHUNK_Y; ty++) {
                chunk_set_block(chunk, x, ty, z, BLOCK_WOOD);
            }

            // Leaf sphere
            int leaf_y = h + trunk_h;
            for (int ly = leaf_y - 1; ly <= leaf_y + 2 && ly < CHUNK_Y; ly++) {
                int r = (ly == leaf_y + 2 || ly == leaf_y - 1) ? 1 : 2;
                for (int lx = -r; lx <= r; lx++) {
                    for (int lz = -r; lz <= r; lz++) {
                        if (lx == 0 && lz == 0 && ly <= leaf_y) continue; // trunk
                        int bx = x + lx, bz = z + lz;
                        if (bx < 0 || bx >= CHUNK_X || bz < 0 || bz >= CHUNK_Z) continue;
                        if (chunk_get_block(chunk, bx, ly, bz) == BLOCK_AIR)
                            chunk_set_block(chunk, bx, ly, bz, BLOCK_LEAVES);
                    }
                }
            }
        }
    }

    atomic_store(&chunk->state, CHUNK_GENERATED);
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -5`

- [ ] **Step 4: Commit**

```bash
git add src/worldgen.h src/worldgen.c
git commit -m "feat: Perlin terrain generation with trees and water"
```

---

## Task 14: Greedy Mesher (mesher.h/.c)

**Files:**
- Create: `src/mesher.h`, `src/mesher.c`

**Context:** The mesher converts a chunk's block data into vertex/index arrays using greedy meshing. It needs access to neighbor chunk boundary data for seamless edges.

- [ ] **Step 1: Write mesher.h**

```c
#ifndef MESHER_H
#define MESHER_H

#include <stdint.h>
#include "vertex.h"
#include "block.h"

typedef struct Chunk Chunk;

typedef struct MeshData {
    BlockVertex* vertices;
    uint32_t*    indices;
    uint32_t     vertex_count;
    uint32_t     index_count;
    uint32_t     vertex_cap;
    uint32_t     index_cap;
} MeshData;

// Neighbor boundary slices (NULL if neighbor not available)
typedef struct ChunkNeighbors {
    const BlockID* pos_x; // chunk to the +X side, slice x=0 (CHUNK_Z * CHUNK_Y)
    const BlockID* neg_x; // chunk to the -X side, slice x=15
    const BlockID* pos_z; // chunk to the +Z side, slice z=0
    const BlockID* neg_z; // chunk to the -Z side, slice z=15
} ChunkNeighbors;

void mesh_data_init(MeshData* md);
void mesh_data_free(MeshData* md);

// Build mesh for a chunk. Neighbors can have NULL slices (treated as air).
void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors, MeshData* out);

// Extract a boundary slice from a chunk for neighbor meshing
// face: 0=x=0, 1=x=15, 2=z=0, 3=z=15
// out must be CHUNK_Z * CHUNK_Y (or CHUNK_X * CHUNK_Y) bytes
void mesher_extract_boundary(const Chunk* chunk, int face, BlockID* out);

#endif
```

- [ ] **Step 2: Write mesher.c**

```c
#include "mesher.h"
#include "chunk.h"
#include <stdlib.h>
#include <string.h>

#define ATLAS_TILES_PER_ROW 16
#define TILE_UV (1.0f / ATLAS_TILES_PER_ROW)

void mesh_data_init(MeshData* md) {
    md->vertex_cap = 4096;
    md->index_cap = 6144;
    md->vertices = malloc(sizeof(BlockVertex) * md->vertex_cap);
    md->indices = malloc(sizeof(uint32_t) * md->index_cap);
    md->vertex_count = 0;
    md->index_count = 0;
}

void mesh_data_free(MeshData* md) {
    free(md->vertices);
    free(md->indices);
    memset(md, 0, sizeof(*md));
}

static void ensure_capacity(MeshData* md, uint32_t extra_verts, uint32_t extra_idx) {
    while (md->vertex_count + extra_verts > md->vertex_cap) {
        md->vertex_cap *= 2;
        md->vertices = realloc(md->vertices, sizeof(BlockVertex) * md->vertex_cap);
    }
    while (md->index_count + extra_idx > md->index_cap) {
        md->index_cap *= 2;
        md->indices = realloc(md->indices, sizeof(uint32_t) * md->index_cap);
    }
}

static void emit_quad(MeshData* md, float positions[4][3], float uvs[4][2],
                      uint8_t normal, uint8_t ao[4]) {
    ensure_capacity(md, 4, 6);
    uint32_t base = md->vertex_count;

    for (int i = 0; i < 4; i++) {
        BlockVertex* v = &md->vertices[md->vertex_count++];
        v->pos[0] = positions[i][0];
        v->pos[1] = positions[i][1];
        v->pos[2] = positions[i][2];
        v->uv[0] = uvs[i][0];
        v->uv[1] = uvs[i][1];
        v->normal = normal;
        v->ao = ao[i];
        v->_pad[0] = 0;
        v->_pad[1] = 0;
    }

    // Two triangles, flip based on AO to avoid anisotropy artifacts
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
        md->indices[md->index_count++] = base + 0;
    } else {
        md->indices[md->index_count++] = base + 1;
        md->indices[md->index_count++] = base + 2;
        md->indices[md->index_count++] = base + 3;
        md->indices[md->index_count++] = base + 3;
        md->indices[md->index_count++] = base + 0;
        md->indices[md->index_count++] = base + 1;
    }
}

static BlockID get_neighbor_block(const Chunk* c, const ChunkNeighbors* n,
                                   int x, int y, int z) {
    if (y < 0 || y >= CHUNK_Y) return BLOCK_AIR;

    if (x < 0) {
        if (!n->neg_x) return BLOCK_AIR;
        return n->neg_x[z * CHUNK_Y + y];
    }
    if (x >= CHUNK_X) {
        if (!n->pos_x) return BLOCK_AIR;
        return n->pos_x[z * CHUNK_Y + y];
    }
    if (z < 0) {
        if (!n->neg_z) return BLOCK_AIR;
        return n->neg_z[x * CHUNK_Y + y];
    }
    if (z >= CHUNK_Z) {
        if (!n->pos_z) return BLOCK_AIR;
        return n->pos_z[x * CHUNK_Y + y];
    }
    return c->blocks[x + z * CHUNK_X + y * CHUNK_X * CHUNK_Z];
}

static bool face_visible(BlockID block, BlockID neighbor) {
    if (block == BLOCK_AIR) return false;
    if (neighbor == BLOCK_AIR) return true;
    if (block_is_transparent(neighbor) && neighbor != block) return true;
    return false;
}

static uint8_t get_tex_for_face(BlockID block, int face_dir) {
    const BlockDef* def = block_get_def(block);
    switch (face_dir) {
        case 2: return def->tex_top;    // +Y
        case 3: return def->tex_bottom; // -Y
        default: return def->tex_side;
    }
}

static void get_tile_uv(uint8_t tile_idx, float u0v0[2], float u1v1[2]) {
    int tx = tile_idx % ATLAS_TILES_PER_ROW;
    int ty = tile_idx / ATLAS_TILES_PER_ROW;
    u0v0[0] = tx * TILE_UV;
    u0v0[1] = ty * TILE_UV;
    u1v1[0] = (tx + 1) * TILE_UV;
    u1v1[1] = (ty + 1) * TILE_UV;
}

void mesher_build(const Chunk* chunk, const ChunkNeighbors* neighbors, MeshData* out) {
    out->vertex_count = 0;
    out->index_count = 0;

    // Simple per-face meshing (not greedy yet — greedy adds complexity;
    // this is correct and can be optimized later)
    for (int y = 0; y < CHUNK_Y; y++) {
        for (int z = 0; z < CHUNK_Z; z++) {
            for (int x = 0; x < CHUNK_X; x++) {
                BlockID block = chunk_get_block(chunk, x, y, z);
                if (block == BLOCK_AIR) continue;

                float fx = (float)x, fy = (float)y, fz = (float)z;

                // Check 6 faces
                struct { int dx, dy, dz; uint8_t normal; } faces[6] = {
                    { 1,  0,  0, 0}, // +X
                    {-1,  0,  0, 1}, // -X
                    { 0,  1,  0, 2}, // +Y
                    { 0, -1,  0, 3}, // -Y
                    { 0,  0,  1, 4}, // +Z
                    { 0,  0, -1, 5}, // -Z
                };

                for (int f = 0; f < 6; f++) {
                    BlockID nb = get_neighbor_block(chunk, neighbors,
                                                    x + faces[f].dx,
                                                    y + faces[f].dy,
                                                    z + faces[f].dz);
                    if (!face_visible(block, nb)) continue;

                    uint8_t tex = get_tex_for_face(block, faces[f].normal);
                    float uv0[2], uv1[2];
                    get_tile_uv(tex, uv0, uv1);

                    float positions[4][3];
                    float uvs[4][2];
                    uint8_t ao[4] = {3, 3, 3, 3}; // no AO for now

                    switch (faces[f].normal) {
                        case 0: // +X
                            positions[0][0]=fx+1; positions[0][1]=fy;   positions[0][2]=fz;
                            positions[1][0]=fx+1; positions[1][1]=fy;   positions[1][2]=fz+1;
                            positions[2][0]=fx+1; positions[2][1]=fy+1; positions[2][2]=fz+1;
                            positions[3][0]=fx+1; positions[3][1]=fy+1; positions[3][2]=fz;
                            break;
                        case 1: // -X
                            positions[0][0]=fx; positions[0][1]=fy;   positions[0][2]=fz+1;
                            positions[1][0]=fx; positions[1][1]=fy;   positions[1][2]=fz;
                            positions[2][0]=fx; positions[2][1]=fy+1; positions[2][2]=fz;
                            positions[3][0]=fx; positions[3][1]=fy+1; positions[3][2]=fz+1;
                            break;
                        case 2: // +Y
                            positions[0][0]=fx;   positions[0][1]=fy+1; positions[0][2]=fz;
                            positions[1][0]=fx+1; positions[1][1]=fy+1; positions[1][2]=fz;
                            positions[2][0]=fx+1; positions[2][1]=fy+1; positions[2][2]=fz+1;
                            positions[3][0]=fx;   positions[3][1]=fy+1; positions[3][2]=fz+1;
                            break;
                        case 3: // -Y
                            positions[0][0]=fx;   positions[0][1]=fy; positions[0][2]=fz+1;
                            positions[1][0]=fx+1; positions[1][1]=fy; positions[1][2]=fz+1;
                            positions[2][0]=fx+1; positions[2][1]=fy; positions[2][2]=fz;
                            positions[3][0]=fx;   positions[3][1]=fy; positions[3][2]=fz;
                            break;
                        case 4: // +Z
                            positions[0][0]=fx+1; positions[0][1]=fy;   positions[0][2]=fz+1;
                            positions[1][0]=fx;   positions[1][1]=fy;   positions[1][2]=fz+1;
                            positions[2][0]=fx;   positions[2][1]=fy+1; positions[2][2]=fz+1;
                            positions[3][0]=fx+1; positions[3][1]=fy+1; positions[3][2]=fz+1;
                            break;
                        case 5: // -Z
                            positions[0][0]=fx;   positions[0][1]=fy;   positions[0][2]=fz;
                            positions[1][0]=fx+1; positions[1][1]=fy;   positions[1][2]=fz;
                            positions[2][0]=fx+1; positions[2][1]=fy+1; positions[2][2]=fz;
                            positions[3][0]=fx;   positions[3][1]=fy+1; positions[3][2]=fz;
                            break;
                    }

                    uvs[0][0] = uv0[0]; uvs[0][1] = uv1[1];
                    uvs[1][0] = uv1[0]; uvs[1][1] = uv1[1];
                    uvs[2][0] = uv1[0]; uvs[2][1] = uv0[1];
                    uvs[3][0] = uv0[0]; uvs[3][1] = uv0[1];

                    emit_quad(out, positions, uvs, faces[f].normal, ao);
                }
            }
        }
    }
}

void mesher_extract_boundary(const Chunk* chunk, int face, BlockID* out) {
    switch (face) {
        case 0: // x=0 slice → CHUNK_Z * CHUNK_Y
            for (int z = 0; z < CHUNK_Z; z++)
                for (int y = 0; y < CHUNK_Y; y++)
                    out[z * CHUNK_Y + y] = chunk_get_block(chunk, 0, y, z);
            break;
        case 1: // x=15
            for (int z = 0; z < CHUNK_Z; z++)
                for (int y = 0; y < CHUNK_Y; y++)
                    out[z * CHUNK_Y + y] = chunk_get_block(chunk, CHUNK_X - 1, y, z);
            break;
        case 2: // z=0 → CHUNK_X * CHUNK_Y
            for (int x = 0; x < CHUNK_X; x++)
                for (int y = 0; y < CHUNK_Y; y++)
                    out[x * CHUNK_Y + y] = chunk_get_block(chunk, x, y, 0);
            break;
        case 3: // z=15
            for (int x = 0; x < CHUNK_X; x++)
                for (int y = 0; y < CHUNK_Y; y++)
                    out[x * CHUNK_Y + y] = chunk_get_block(chunk, x, y, CHUNK_Z - 1);
            break;
    }
}
```

**Note:** This is a simple per-face mesher, not a greedy mesher. It is correct and produces seamless results. Greedy meshing is an optimization that can be added later — per-face meshing is much simpler to get right and debug first.

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -5`

- [ ] **Step 4: Commit**

```bash
git add src/mesher.h src/mesher.c
git commit -m "feat: per-face mesher with neighbor boundary support"
```

---

## Task 15: World Management + Async Worker Pool (world.h/.c)

**Files:**
- Create: `src/world.h`, `src/world.c`

**Context:** The world module owns the chunk map, thread pool, and chunk lifecycle. It coordinates async generation and meshing, and provides the list of renderable chunks to the main loop.

- [ ] **Step 1: Write world.h**

```c
#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <cglm/cglm.h>
#include "chunk_map.h"
#include "chunk_mesh.h"

typedef struct Renderer Renderer;

typedef struct World World;

World* world_create(Renderer* renderer, int seed, int render_distance);
void   world_destroy(World* world);

// Call each frame with player position. Handles load/unload/mesh upload.
void   world_update(World* world, vec3 player_pos);

// Get array of renderable chunk meshes for drawing
void   world_get_meshes(World* world, ChunkMesh** out_meshes, uint32_t* out_count);

#endif
```

- [ ] **Step 2: Write world.c**

```c
#include "world.h"
#include "chunk.h"
#include "chunk_map.h"
#include "chunk_mesh.h"
#include "worldgen.h"
#include "mesher.h"
#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_WORKERS 8
#define MAX_MESH_UPLOADS_PER_FRAME 8
#define MAX_GEN_SUBMITS_PER_FRAME 16
#define UNLOAD_EXTRA 4

typedef enum WorkType {
    WORK_GENERATE,
    WORK_MESH,
    WORK_QUIT,
} WorkType;

typedef struct WorkItem {
    WorkType type;
    Chunk*   chunk;
    // For meshing: boundary slices
    BlockID* boundary_pos_x;
    BlockID* boundary_neg_x;
    BlockID* boundary_pos_z;
    BlockID* boundary_neg_z;
    struct WorkItem* next;
} WorkItem;

typedef struct ResultItem {
    Chunk*    chunk;
    MeshData* mesh_data; // NULL for gen results
    struct ResultItem* next;
} ResultItem;

struct World {
    Renderer*   renderer;
    ChunkMap    chunks;
    int         seed;
    int         render_distance;

    // Thread pool
    pthread_t   workers[MAX_WORKERS];
    int         worker_count;
    atomic_bool shutdown;

    // Work queue
    WorkItem*       work_head;
    pthread_mutex_t work_mutex;
    pthread_cond_t  work_cond;

    // Result queue
    ResultItem*     result_head;
    pthread_mutex_t result_mutex;

    // Renderable mesh array
    ChunkMesh*  render_meshes;
    uint32_t    render_mesh_count;
    uint32_t    render_mesh_cap;
};

static void push_work(World* w, WorkItem* item) {
    pthread_mutex_lock(&w->work_mutex);
    item->next = w->work_head;
    w->work_head = item;
    pthread_cond_signal(&w->work_cond);
    pthread_mutex_unlock(&w->work_mutex);
}

static WorkItem* pop_work(World* w) {
    pthread_mutex_lock(&w->work_mutex);
    while (!w->work_head && !atomic_load(&w->shutdown)) {
        pthread_cond_wait(&w->work_cond, &w->work_mutex);
    }
    WorkItem* item = w->work_head;
    if (item) w->work_head = item->next;
    pthread_mutex_unlock(&w->work_mutex);
    return item;
}

static void push_result(World* w, ResultItem* item) {
    pthread_mutex_lock(&w->result_mutex);
    item->next = w->result_head;
    w->result_head = item;
    pthread_mutex_unlock(&w->result_mutex);
}

static ResultItem* pop_all_results(World* w) {
    pthread_mutex_lock(&w->result_mutex);
    ResultItem* list = w->result_head;
    w->result_head = NULL;
    pthread_mutex_unlock(&w->result_mutex);
    return list;
}

static void* worker_func(void* arg) {
    World* w = arg;

    while (!atomic_load(&w->shutdown)) {
        WorkItem* item = pop_work(w);
        if (!item) break;

        if (item->type == WORK_QUIT) {
            free(item);
            break;
        }

        if (item->type == WORK_GENERATE) {
            worldgen_generate(item->chunk, w->seed);

            ResultItem* res = calloc(1, sizeof(ResultItem));
            res->chunk = item->chunk;
            push_result(w, res);
        }
        else if (item->type == WORK_MESH) {
            ChunkNeighbors neighbors = {
                .pos_x = item->boundary_pos_x,
                .neg_x = item->boundary_neg_x,
                .pos_z = item->boundary_pos_z,
                .neg_z = item->boundary_neg_z,
            };

            MeshData* md = malloc(sizeof(MeshData));
            mesh_data_init(md);
            mesher_build(item->chunk, &neighbors, md);

            ResultItem* res = calloc(1, sizeof(ResultItem));
            res->chunk = item->chunk;
            res->mesh_data = md;
            push_result(w, res);

            free(item->boundary_pos_x);
            free(item->boundary_neg_x);
            free(item->boundary_pos_z);
            free(item->boundary_neg_z);
        }

        free(item);
    }
    return NULL;
}

World* world_create(Renderer* renderer, int seed, int render_distance) {
    World* w = calloc(1, sizeof(World));
    w->renderer = renderer;
    w->seed = seed;
    w->render_distance = render_distance;

    chunk_map_init(&w->chunks, 8192);

    pthread_mutex_init(&w->work_mutex, NULL);
    pthread_cond_init(&w->work_cond, NULL);
    pthread_mutex_init(&w->result_mutex, NULL);

    atomic_store(&w->shutdown, false);

    // Determine worker count
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    w->worker_count = (int)(ncpus > 2 ? ncpus - 2 : 1);
    if (w->worker_count > MAX_WORKERS) w->worker_count = MAX_WORKERS;

    for (int i = 0; i < w->worker_count; i++) {
        pthread_create(&w->workers[i], NULL, worker_func, w);
    }

    w->render_mesh_cap = 4096;
    w->render_meshes = malloc(sizeof(ChunkMesh) * w->render_mesh_cap);
    w->render_mesh_count = 0;

    return w;
}

void world_destroy(World* w) {
    // Signal shutdown
    atomic_store(&w->shutdown, true);

    // Wake all workers
    pthread_mutex_lock(&w->work_mutex);
    pthread_cond_broadcast(&w->work_cond);
    pthread_mutex_unlock(&w->work_mutex);

    for (int i = 0; i < w->worker_count; i++) {
        pthread_join(w->workers[i], NULL);
    }

    // Free remaining work items
    while (w->work_head) {
        WorkItem* item = w->work_head;
        w->work_head = item->next;
        free(item->boundary_pos_x);
        free(item->boundary_neg_x);
        free(item->boundary_pos_z);
        free(item->boundary_neg_z);
        free(item);
    }

    // Free remaining results
    ResultItem* res = pop_all_results(w);
    while (res) {
        ResultItem* next = res->next;
        if (res->mesh_data) {
            mesh_data_free(res->mesh_data);
            free(res->mesh_data);
        }
        free(res);
        res = next;
    }

    // Destroy all chunks
    uint32_t idx = 0;
    Chunk* c;
    while ((c = chunk_map_iter(&w->chunks, &idx))) {
        chunk_mesh_destroy(w->renderer->allocator, &c->mesh);
        chunk_destroy(c);
    }
    chunk_map_free(&w->chunks);

    pthread_mutex_destroy(&w->work_mutex);
    pthread_cond_destroy(&w->work_cond);
    pthread_mutex_destroy(&w->result_mutex);

    free(w->render_meshes);
    free(w);
}

static int dist_sq(int32_t cx, int32_t cz, int32_t pcx, int32_t pcz) {
    int dx = cx - pcx, dz = cz - pcz;
    return dx * dx + dz * dz;
}

static BlockID* extract_boundary_if_exists(World* w, int32_t cx, int32_t cz, int face) {
    Chunk* c = chunk_map_get(&w->chunks, cx, cz);
    if (!c || atomic_load(&c->state) < CHUNK_GENERATED) return NULL;
    int slice_size = CHUNK_Z * CHUNK_Y; // same for both X and Z faces
    BlockID* buf = malloc(slice_size);
    mesher_extract_boundary(c, face, buf);
    return buf;
}

void world_update(World* w, vec3 player_pos) {
    int32_t pcx = (int32_t)floorf(player_pos[0] / CHUNK_X);
    int32_t pcz = (int32_t)floorf(player_pos[2] / CHUNK_Z);
    int rd = w->render_distance;
    int rd_sq = rd * rd;
    int unload_sq = (rd + UNLOAD_EXTRA) * (rd + UNLOAD_EXTRA);

    // 1. Process results
    int uploads = 0;
    ResultItem* results = pop_all_results(w);
    while (results) {
        ResultItem* next = results->next;
        Chunk* c = results->chunk;

        if (results->mesh_data) {
            // Mesh result
            MeshData* md = results->mesh_data;
            if (md->vertex_count > 0 && uploads < MAX_MESH_UPLOADS_PER_FRAME) {
                chunk_mesh_destroy(w->renderer->allocator, &c->mesh);

                c->mesh.chunk_origin[0] = (float)(c->cx * CHUNK_X);
                c->mesh.chunk_origin[1] = 0.0f;
                c->mesh.chunk_origin[2] = (float)(c->cz * CHUNK_Z);

                c->mesh.aabb_min[0] = c->mesh.chunk_origin[0];
                c->mesh.aabb_min[1] = 0.0f;
                c->mesh.aabb_min[2] = c->mesh.chunk_origin[2];
                c->mesh.aabb_max[0] = c->mesh.chunk_origin[0] + CHUNK_X;
                c->mesh.aabb_max[1] = CHUNK_Y;
                c->mesh.aabb_max[2] = c->mesh.chunk_origin[2] + CHUNK_Z;

                chunk_mesh_upload(w->renderer, &c->mesh,
                                  md->vertices, md->vertex_count,
                                  md->indices, md->index_count);
                atomic_store(&c->state, CHUNK_READY);
                uploads++;
            } else if (md->vertex_count == 0) {
                atomic_store(&c->state, CHUNK_READY);
            } else {
                // Couldn't upload this frame, re-mark as meshed
                atomic_store(&c->state, CHUNK_MESHED);
            }
            mesh_data_free(md);
            free(md);
        }
        // Gen results just need state update (already done in worldgen)

        free(results);
        results = next;
    }

    // 2. Unload distant chunks
    uint32_t iter_idx = 0;
    Chunk* c;
    while ((c = chunk_map_iter(&w->chunks, &iter_idx))) {
        if (dist_sq(c->cx, c->cz, pcx, pcz) > unload_sq) {
            int state = atomic_load(&c->state);
            if (state == CHUNK_GENERATING || state == CHUNK_MESHING) continue;

            chunk_map_remove(&w->chunks, c->cx, c->cz);
            chunk_mesh_destroy(w->renderer->allocator, &c->mesh);
            chunk_destroy(c);
            // Reset iterator since map changed
            iter_idx = 0;
        }
    }

    // 3. Load missing chunks + submit generation
    int gen_submitted = 0;
    for (int32_t dz = -rd; dz <= rd && gen_submitted < MAX_GEN_SUBMITS_PER_FRAME; dz++) {
        for (int32_t dx = -rd; dx <= rd && gen_submitted < MAX_GEN_SUBMITS_PER_FRAME; dx++) {
            if (dx * dx + dz * dz > rd_sq) continue;
            int32_t cx = pcx + dx, cz = pcz + dz;

            if (chunk_map_get(&w->chunks, cx, cz)) continue;

            Chunk* nc = chunk_create(cx, cz);
            atomic_store(&nc->state, CHUNK_GENERATING);
            chunk_map_put(&w->chunks, nc);

            WorkItem* item = calloc(1, sizeof(WorkItem));
            item->type = WORK_GENERATE;
            item->chunk = nc;
            push_work(w, item);
            gen_submitted++;
        }
    }

    // 4. Submit meshing for generated chunks
    iter_idx = 0;
    while ((c = chunk_map_iter(&w->chunks, &iter_idx))) {
        if (atomic_load(&c->state) != CHUNK_GENERATED) continue;

        // Check all 4 horizontal neighbors exist and are generated
        Chunk* px = chunk_map_get(&w->chunks, c->cx + 1, c->cz);
        Chunk* nx = chunk_map_get(&w->chunks, c->cx - 1, c->cz);
        Chunk* pz = chunk_map_get(&w->chunks, c->cx, c->cz + 1);
        Chunk* nz = chunk_map_get(&w->chunks, c->cx, c->cz - 1);

        bool neighbors_ready = true;
        if (px && atomic_load(&px->state) < CHUNK_GENERATED) neighbors_ready = false;
        if (nx && atomic_load(&nx->state) < CHUNK_GENERATED) neighbors_ready = false;
        if (pz && atomic_load(&pz->state) < CHUNK_GENERATED) neighbors_ready = false;
        if (nz && atomic_load(&nz->state) < CHUNK_GENERATED) neighbors_ready = false;

        if (!neighbors_ready) continue;

        atomic_store(&c->state, CHUNK_MESHING);

        WorkItem* item = calloc(1, sizeof(WorkItem));
        item->type = WORK_MESH;
        item->chunk = c;

        // Extract boundary slices from neighbors
        item->boundary_pos_x = extract_boundary_if_exists(w, c->cx + 1, c->cz, 0); // x=0 of +X neighbor
        item->boundary_neg_x = extract_boundary_if_exists(w, c->cx - 1, c->cz, 1); // x=15 of -X neighbor
        item->boundary_pos_z = extract_boundary_if_exists(w, c->cx, c->cz + 1, 2); // z=0 of +Z neighbor
        item->boundary_neg_z = extract_boundary_if_exists(w, c->cx, c->cz - 1, 3); // z=15 of -Z neighbor

        push_work(w, item);
    }

    // 5. Build render mesh array
    w->render_mesh_count = 0;
    iter_idx = 0;
    while ((c = chunk_map_iter(&w->chunks, &iter_idx))) {
        if (atomic_load(&c->state) != CHUNK_READY) continue;
        if (!c->mesh.uploaded) continue;

        if (w->render_mesh_count >= w->render_mesh_cap) {
            w->render_mesh_cap *= 2;
            w->render_meshes = realloc(w->render_meshes,
                                        sizeof(ChunkMesh) * w->render_mesh_cap);
        }
        w->render_meshes[w->render_mesh_count++] = c->mesh;
    }
}

void world_get_meshes(World* w, ChunkMesh** out_meshes, uint32_t* out_count) {
    *out_meshes = w->render_meshes;
    *out_count = w->render_mesh_count;
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build 2>&1 | tail -10`

- [ ] **Step 4: Commit**

```bash
git add src/world.h src/world.c
git commit -m "feat: world management with async gen/mesh worker pool"
```

---

## Task 16: Full Integration (main.c game loop)

**Files:**
- Modify: `src/main.c`
- Modify: `src/renderer.c` (add frustum include)

**Context:** Wire everything together: create world, update it each frame, pass meshes to renderer with frustum culling.

- [ ] **Step 1: Update main.c with full game loop**

```c
#include <stdio.h>
#include <stdbool.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include "renderer.h"
#include "camera.h"
#include "world.h"
#include "chunk_mesh.h"

static Camera g_camera;

static void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    camera_process_mouse(&g_camera, xpos, ypos);
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode; (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

int main(void) {
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Minecraft", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetKeyCallback(window, key_callback);

    Renderer renderer;
    if (!renderer_init(&renderer, window)) {
        fprintf(stderr, "Failed to init renderer\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    camera_init(&g_camera, (vec3){0, 80, 0});

    World* world = world_create(&renderer, 42, 32);

    vec3 sun_dir = { -0.5f, -0.8f, -0.3f };
    glm_vec3_normalize(sun_dir);

    double last_time = glfwGetTime();
    int frame_count = 0;
    double fps_timer = last_time;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        glfwPollEvents();
        camera_process_keyboard(&g_camera, window, dt);

        // Update world
        world_update(world, g_camera.position);

        // Get renderable meshes
        ChunkMesh* meshes;
        uint32_t mesh_count;
        world_get_meshes(world, &meshes, &mesh_count);

        // Render
        mat4 view, proj;
        camera_get_view(&g_camera, view);
        float aspect = (float)renderer.swapchain.extent.width
                     / (float)renderer.swapchain.extent.height;
        camera_get_proj(&g_camera, aspect, proj);

        renderer_draw_frame(&renderer, meshes, mesh_count, view, proj, sun_dir);

        // FPS counter
        frame_count++;
        if (now - fps_timer >= 2.0) {
            char title[128];
            snprintf(title, sizeof(title), "Minecraft | FPS: %d | Chunks: %u | Pos: %.0f, %.0f, %.0f",
                     (int)(frame_count / (now - fps_timer)),
                     mesh_count,
                     g_camera.position[0], g_camera.position[1], g_camera.position[2]);
            glfwSetWindowTitle(window, title);
            frame_count = 0;
            fps_timer = now;
        }
    }

    vkDeviceWaitIdle(renderer.device);
    world_destroy(world);
    renderer_cleanup(&renderer);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 2: Build and run**

Run:
```bash
cmake --build build && ./build/minecraft
```
Expected: Window opens. Terrain gradually loads around the spawn point. You see colored blocks: green grass, brown dirt, gray stone, blue water. WASD to fly around, mouse to look. Chunks load as you move. FPS counter in title bar.

- [ ] **Step 3: Fix any issues that arise**

Common issues to watch for:
- Shader path: ensure `./build/minecraft` is run from project root so `shaders/` path works. May need to adjust to use the build directory path for .spv files.
- Validation errors: run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build/minecraft` to check.
- Missing includes or linker errors.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "feat: full game loop with world generation and rendering"
```

---

## Task 17: Polish and Verify

**Files:**
- Potentially any file for bug fixes

- [ ] **Step 1: Run with validation layers**

```bash
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build/minecraft 2>&1 | head -50
```
Expected: No validation errors. Fix any that appear.

- [ ] **Step 2: Test window resize**

Resize the window while running. Expected: No crash, rendering adapts to new size.

- [ ] **Step 3: Test chunk loading at distance**

Fly far from spawn. Expected: New chunks load, distant chunks unload. No gaps between chunks.

- [ ] **Step 4: Check FPS**

Expected: Smooth 60fps (or higher with MAILBOX present mode) on a discrete GPU.

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "fix: polish and validation layer cleanup"
```

---

## Verification Checklist

- [ ] `cmake -B build && cmake --build build` — compiles without errors or warnings
- [ ] Window opens with sky-blue background
- [ ] Camera: WASD movement + mouse look works
- [ ] Terrain: visible blocks with correct colors (green grass, brown dirt, gray stone)
- [ ] Trees: visible wood trunks + leaf canopies
- [ ] Water: blue blocks at sea level
- [ ] Chunk boundaries: seamless, no gaps
- [ ] Chunk loading: new chunks appear as you move
- [ ] Chunk unloading: distant chunks freed
- [ ] Window resize: no crash
- [ ] Validation layers: no errors
- [ ] FPS: 60+ on discrete GPU at render distance 32
