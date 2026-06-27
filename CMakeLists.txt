cmake_minimum_required(VERSION 3.20)

# Default to an optimized Release build when no build type is given (e.g. a
# bare `cmake -B build`). Release implies -O3 -DNDEBUG on GCC/Clang and /O2
# on MSVC; NDEBUG also compiles out the Vulkan validation layers and debug
# messenger (guarded by #ifndef NDEBUG in renderer.c), so release builds run
# without validation overhead. Multi-config generators (VS, Ninja Multi-
# Config) pick the type at build time, so only set it for single-config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug/Release/RelWithDebInfo/MinSizeRel)" FORCE)
endif()

project(minecraft C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC needs /std:c11 + /experimental:c11atomics to enable <stdatomic.h>.
# Applied per-target below via msvc_enable_c11_atomics(). Each target that
# this is called on must be pure C (no .cpp sources) — the C++ compiler
# rejects /std:c11. The VMA C++ glue is in its own static lib (vma_glue)
# for exactly this reason.
function(msvc_enable_c11_atomics target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /std:c11 /experimental:c11atomics)
    endif()
endfunction()

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
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()
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
find_package(Vulkan REQUIRED COMPONENTS glslangValidator)

# ---- Shader compilation ----
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})

function(compile_shader SHADER_SRC)
    get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
    set(SHADER_SPV ${SHADER_BIN_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SHADER_SPV}
        COMMAND Vulkan::glslangValidator -V ${SHADER_SRC} -o ${SHADER_SPV}
        DEPENDS ${SHADER_SRC}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} ${SHADER_SPV} PARENT_SCOPE)
endfunction()

compile_shader(${SHADER_DIR}/block.vert)
compile_shader(${SHADER_DIR}/block.frag)
compile_shader(${SHADER_DIR}/player.vert)
compile_shader(${SHADER_DIR}/player.frag)
compile_shader(${SHADER_DIR}/hud.vert)
compile_shader(${SHADER_DIR}/hud.frag)
compile_shader(${SHADER_DIR}/ui.vert)
compile_shader(${SHADER_DIR}/ui.frag)
compile_shader(${SHADER_DIR}/outline.vert)
compile_shader(${SHADER_DIR}/outline.frag)
add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})

# ---- Executable ----
# VMA glue is C++. Put it in its own static lib so the main target stays
# pure C — this lets MSVC C-only flags (/std:c11, /experimental:c11atomics)
# be applied via target_compile_options without a COMPILE_LANGUAGE genex,
# which is unreliable on the Visual Studio generator for mixed-language
# targets.
add_library(vma_glue STATIC src/vma_impl.cpp)
target_link_libraries(vma_glue PUBLIC volk GPUOpen::VulkanMemoryAllocator)

add_executable(minecraft
    src/main.c
    src/renderer.c
    src/renderer_frame.c
    src/renderer_frame_dump.c
    src/renderer_player_mesh.c
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
    src/ore.c
    src/daynight.c
    src/village.c
    src/mesher.c
    src/lighting.c
    src/world.c
    src/world_persist.c
    src/player.c
    src/physics.c
    src/block_physics.c
    src/ui/hud.c
    src/ui/ui.c
    src/agent.c
    src/player_model.c
    src/net.c
    src/reliable.c
    src/net_thread.c
    src/server.c
    src/client.c
    src/remote_player.c
    src/mob.c
    src/inventory.c
    src/survival.c
    src/raycast.c
    src/assets_generated.c
    src/shaders_generated.c
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
    vma_glue
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

# WinSock on Windows
if(WIN32)
    target_link_libraries(minecraft PRIVATE ws2_32)
endif()

# ---- Unit tests (no Vulkan dependency) ----
enable_testing()
add_executable(test_block_physics
    src/test_block_physics.c
    src/block_physics.c
    src/block.c
)
target_include_directories(test_block_physics PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${fastnoiselite_SOURCE_DIR}/C
)
target_link_libraries(test_block_physics PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_block_physics PRIVATE m)
endif()
add_test(NAME block_physics COMMAND test_block_physics)

# Agent JSON unit tests (no Vulkan/GLFW required)
add_executable(test_agent tests/test_agent_json.c src/agent.c)
target_include_directories(test_agent PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_agent PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_agent PRIVATE m)
endif()
add_test(NAME agent_json COMMAND test_agent)

add_executable(test_net tests/test_net.c src/net.c src/reliable.c)
target_include_directories(test_net PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(test_net PRIVATE ws2_32)
endif()
add_test(NAME net COMMAND test_net)

add_executable(test_ui
    tests/test_ui.c
    src/ui/ui.c
    src/block.c
    src/pipeline.c
    tests/renderer_stubs.c
)
target_include_directories(test_ui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${cglm_SOURCE_DIR}/include
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_ui PRIVATE VK_NO_PROTOTYPES UNDEBUG)
target_link_libraries(test_ui PRIVATE glfw volk vma_glue cglm Vulkan::Headers ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ui PRIVATE m)
endif()
add_test(NAME ui COMMAND test_ui)

add_executable(test_remote_player
    tests/test_remote_player.c
    src/remote_player.c
)
target_include_directories(test_remote_player PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_remote_player PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_remote_player PRIVATE m)
endif()
add_test(NAME remote_player COMMAND test_remote_player)

add_executable(test_mob
    tests/test_mob.c
    src/mob.c
)
target_include_directories(test_mob PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mob PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mob PRIVATE m)
endif()
add_test(NAME mob COMMAND test_mob)

add_executable(test_mesher
    tests/test_mesher.c
    src/mesher.c
    src/chunk.c
    src/block.c
)
target_include_directories(test_mesher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_mesher PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_mesher PRIVATE cglm Vulkan::Headers)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mesher PRIVATE m)
endif()
add_test(NAME mesher COMMAND test_mesher)

add_executable(test_lighting
    tests/test_lighting.c
    src/block.c
    src/chunk.c
    src/lighting.c
)
target_include_directories(test_lighting PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_lighting PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_lighting PRIVATE cglm Vulkan::Headers)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_lighting PRIVATE m)
endif()
add_test(NAME lighting COMMAND test_lighting)

add_executable(test_client tests/test_client.c tests/hud_stub.c src/client.c src/net.c src/reliable.c src/net_thread.c src/inventory.c)
target_include_directories(test_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME client COMMAND test_client)
target_link_libraries(test_client PRIVATE Threads::Threads cglm)
if(WIN32)
    target_link_libraries(test_client PRIVATE ws2_32)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_client PRIVATE m)
endif()

add_executable(test_inventory
    tests/test_inventory.c
    src/inventory.c
)
target_include_directories(test_inventory PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME inventory COMMAND test_inventory)

add_executable(test_raycast
    tests/test_raycast.c
    src/raycast.c
)
target_include_directories(test_raycast PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_raycast PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_raycast PRIVATE cglm Vulkan::Headers)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_raycast PRIVATE m)
endif()
add_test(NAME raycast COMMAND test_raycast)

# Ore selection (pure, no Vulkan/GLFW required)
add_executable(test_ore tests/test_ore.c src/ore.c src/block.c)
target_include_directories(test_ore PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ore PRIVATE m)
endif()
add_test(NAME ore COMMAND test_ore)

# Survival mechanics math (pure: fall/hunger/drown/lava/regen, no Vulkan/GLFW)
add_executable(test_survival tests/test_survival.c src/survival.c)
target_include_directories(test_survival PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_survival PRIVATE m)
endif()
add_test(NAME survival COMMAND test_survival)

# Day/night cycle math (pure, no Vulkan/GLFW required)
add_executable(test_daynight tests/test_daynight.c src/daynight.c)
target_include_directories(test_daynight PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_daynight PRIVATE m)
endif()
add_test(NAME daynight COMMAND test_daynight)

# Village placement/layout (pure decision core) + per-chunk generation.
# Pulls in chunk.h (via village.c) which transitively includes Vulkan headers,
# hence the VK_NO_PROTOTYPES + Vulkan::Headers wiring mirrored from test_mesher.
add_executable(test_village
    tests/test_village.c
    src/village.c
    src/worldgen.c
    src/ore.c
    src/chunk.c
    src/block.c
)
target_include_directories(test_village PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${fastnoiselite_SOURCE_DIR}/C
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_village PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_village PRIVATE cglm Vulkan::Headers Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_village PRIVATE m)
endif()
add_test(NAME village COMMAND test_village)

# World persistence overlay (pure: serialize/deserialize roundtrip + file I/O).
# Pulls in chunk.h (via world_persist.c) which transitively includes Vulkan
# headers, hence the VK_NO_PROTOTYPES + Vulkan::Headers wiring mirrored from
# test_mesher / test_lighting.
add_executable(test_world_persist
    tests/test_world_persist.c
    src/world_persist.c
    src/block.c
)
target_include_directories(test_world_persist PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_world_persist PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_world_persist PRIVATE cglm Vulkan::Headers Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_world_persist PRIVATE m)
endif()
add_test(NAME world_persist COMMAND test_world_persist)

foreach(t minecraft test_block_physics test_agent test_net test_ui
          test_remote_player test_mob test_mesher test_lighting test_client
          test_inventory test_raycast test_ore test_daynight test_village
          test_world_persist)
    msvc_enable_c11_atomics(${t})
endforeach()
