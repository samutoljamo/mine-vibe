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
compile_shader(${SHADER_DIR}/hud.vert)
compile_shader(${SHADER_DIR}/hud.frag)
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
    src/player.c
    src/physics.c
    src/block_physics.c
    src/hud.c
    src/agent.c
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
target_link_libraries(test_block_physics PRIVATE cglm m)
add_test(NAME block_physics COMMAND test_block_physics)

add_executable(test_hud tests/test_hud.c src/hud.c src/block.c)
target_include_directories(test_hud PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_hud PRIVATE m)
add_test(NAME hud COMMAND test_hud)

# Agent JSON unit tests (no Vulkan/GLFW required)
add_executable(test_agent tests/test_agent_json.c src/agent.c)
target_include_directories(test_agent PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_agent PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_agent PRIVATE m)
endif()
add_test(NAME agent_json COMMAND test_agent)
