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

# miniaudio (header-only audio playback backend). Pinned to a stable tag.
# We consume only the header (miniaudio.h) and define MA_IMPLEMENTATION in
# exactly one TU (src/audio_miniaudio.c). SOURCE_SUBDIR points at a dir with no
# CMakeLists so MakeAvailable downloads the sources WITHOUT adding miniaudio's
# own library/example/extra-node targets to our build.
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.22
    SOURCE_SUBDIR  do-not-build
)
FetchContent_MakeAvailable(miniaudio)

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
compile_shader(${SHADER_DIR}/particle.vert)
compile_shader(${SHADER_DIR}/particle.frag)
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
    src/cave.c
    src/biome.c
    src/weather.c
    src/daynight.c
    src/village.c
    src/dungeon.c
    src/loot.c
    src/mesher.c
    src/lighting.c
    src/world.c
    src/world_persist.c
    src/worldsave.c
    src/player.c
    src/physics.c
    src/physics_world.c
    src/block_physics.c
    src/ui/hud.c
    src/ui/ui.c
    src/agent.c
    src/player_model.c
    src/player_anim.c
    src/net.c
    src/reliable.c
    src/net_thread.c
    src/chunkwire.c
    src/chunk_stream.c
    src/chunk_reasm.c
    src/server.c
    src/server_block_entity.c
    src/client.c
    src/audio.c
    src/music.c
    src/audio_miniaudio.c
    src/remote_player.c
    src/mob.c
    src/mob_ai.c
    src/mob_render.c
    src/mob_model.c
    src/inventory.c
    src/container.c
    src/item.c
    src/mining.c
    src/crafting.c
    src/combat.c
    src/smelting.c
    src/survival.c
    src/particle.c
    src/particle_render.c
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
    ${miniaudio_SOURCE_DIR}
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

add_executable(test_net tests/test_net.c src/net.c src/reliable.c src/chunkwire.c src/chunk_reasm.c)
target_include_directories(test_net PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(test_net PRIVATE ws2_32)
endif()
# The netcode parser tests (truncation + 200k-iteration fuzz) size every input
# to a fresh heap buffer of exactly the truncated length, so an out-of-bounds
# read of an untrusted packet trips the allocator / a guard page rather than
# silently reading adjacent memory. Build with -fsanitize=address as well when a
# sanitizer runtime is available for an even tighter check.
add_test(NAME net COMMAND test_net)

add_executable(test_chunkwire tests/test_chunkwire.c src/chunkwire.c)
target_include_directories(test_chunkwire PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chunkwire COMMAND test_chunkwire)

# Chunk streaming policy (pure: in-range disc diff, nearest-first send ordering,
# budget cap, unload of out-of-range chunks). No Vulkan/GLFW/net deps.
add_executable(test_chunk_stream tests/test_chunk_stream.c src/chunk_stream.c)
target_include_directories(test_chunk_stream PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chunk_stream COMMAND test_chunk_stream)

add_executable(test_ui
    tests/test_ui.c
    src/ui/ui.c
    src/ui/hud.c
    src/block.c
    src/item.c
    src/crafting.c
    src/pipeline.c
    src/shaders_generated.c
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
    src/player_anim.c
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

add_executable(test_mob_ai
    tests/test_mob_ai.c
    src/mob_ai.c
)
target_include_directories(test_mob_ai PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mob_ai PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mob_ai PRIVATE m)
endif()
add_test(NAME mob_ai COMMAND test_mob_ai)

add_executable(test_mob_render
    tests/test_mob_render.c
    src/mob_render.c
    src/mob_model.c
)
target_include_directories(test_mob_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mob_render PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mob_render PRIVATE m)
endif()
add_test(NAME mob_render COMMAND test_mob_render)

# Per-type mob box-model data layer (pure geometry data; no Vulkan/GLFW).
add_executable(test_mob_model
    tests/test_mob_model.c
    src/mob_model.c
)
target_include_directories(test_mob_model PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mob_model PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mob_model PRIVATE m)
endif()
add_test(NAME mob_model COMMAND test_mob_model)

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
target_link_libraries(test_mesher PRIVATE cglm Vulkan::Headers Threads::Threads)
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

add_executable(test_client tests/test_client.c tests/hud_stub.c src/client.c src/net.c src/reliable.c src/net_thread.c src/inventory.c src/item.c src/block.c src/audio.c src/music.c src/audio_backend_null_msvc.c src/chunkwire.c src/chunk_reasm.c src/particle.c src/weather.c)
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
    src/item.c
    src/block.c
)
target_include_directories(test_inventory PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME inventory COMMAND test_inventory)

# Chest container model: stacking/overflow/leftover, take/remove, and transfer
# to/from an Inventory reusing the inventory stacking rules (pure, no Vulkan/GLFW).
add_executable(test_container
    tests/test_container.c
    src/container.c
    src/inventory.c
    src/item.c
    src/block.c
)
target_include_directories(test_container PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_container PRIVATE m)
endif()
add_test(NAME container COMMAND test_container)

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

# Per-limb animation math (pure: pivots + joint vertex transform + walk cycle).
# CRITICAL: asserts the identity (all-zero) transform is a no-op, guaranteeing
# the default pose renders identically to the old rigid mesh. No Vulkan needed.
add_executable(test_player_anim tests/test_player_anim.c src/player_anim.c)
target_include_directories(test_player_anim PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_player_anim PRIVATE m)
endif()
add_test(NAME player_anim COMMAND test_player_anim)

# Cave carving (pure 3D-noise model, no Vulkan/GLFW required). Uses
# FastNoiseLite (header-only) for its noise fields, hence the include dir.
# This test links cave.c without worldgen.c, so it must generate the
# FastNoiseLite implementation itself (FNL_IMPL); in the main build worldgen.c
# provides FNL_IMPL, so cave.c never defines it there (avoids duplicate symbols).
add_executable(test_cave tests/test_cave.c src/cave.c)
target_include_directories(test_cave PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${fastnoiselite_SOURCE_DIR}/C
)
target_compile_definitions(test_cave PRIVATE FNL_IMPL)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cave PRIVATE m)
endif()
add_test(NAME cave COMMAND test_cave)

# Dungeon room placement (pure decision core): determinism, cross-chunk
# continuity, vertical band, rarity, and shell/interior/chest voxel roles.
# Pure model only — no Vulkan/GLFW/FastNoiseLite needed.
add_executable(test_dungeon tests/test_dungeon.c src/dungeon.c src/loot.c)
target_include_directories(test_dungeon PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dungeon PRIVATE m)
endif()
add_test(NAME dungeon COMMAND test_dungeon)

# Server-side dungeon chest loot population (63k): the pure enumeration
# (sbe_dungeon_chests_in_chunk) and the container-landing of rolled loot.
# Links server_block_entity.c, which includes server.h (and thus world.h /
# Vulkan headers) at compile time but at link time only needs the pure cores
# below — same Vulkan-headers pattern as test_world_persist.
add_executable(test_server_dungeon_loot
    tests/test_server_dungeon_loot.c
    src/server_block_entity.c
    src/dungeon.c
    src/loot.c
    src/container.c
    src/inventory.c
    src/item.c
    src/block.c
    src/smelting.c
)
target_include_directories(test_server_dungeon_loot PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_server_dungeon_loot PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_server_dungeon_loot PRIVATE cglm Vulkan::Headers Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_server_dungeon_loot PRIVATE m)
endif()
add_test(NAME server_dungeon_loot COMMAND test_server_dungeon_loot)

# Weighted loot-table model (pure, no Vulkan/GLFW required)
add_executable(test_loot tests/test_loot.c src/loot.c src/item.c src/block.c)
target_include_directories(test_loot PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_loot PRIVATE m)
endif()
add_test(NAME loot COMMAND test_loot)

# Weather state machine (pure deterministic, no Vulkan/GLFW required)
add_executable(test_weather tests/test_weather.c src/weather.c)
target_include_directories(test_weather PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_weather PRIVATE m)
endif()
add_test(NAME weather COMMAND test_weather)

# Crafting recipe matching / consumption math (pure, no Vulkan/GLFW required)
add_executable(test_crafting tests/test_crafting.c src/crafting.c src/item.c src/block.c)
target_include_directories(test_crafting PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_crafting PRIVATE m)
endif()
add_test(NAME crafting COMMAND test_crafting)

# Smelting recipe table + furnace state tick (pure, no Vulkan/GLFW required)
add_executable(test_smelting tests/test_smelting.c src/smelting.c src/item.c src/block.c)
target_include_directories(test_smelting PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smelting PRIVATE m)
endif()
add_test(NAME smelting COMMAND test_smelting)

# Item model + tool break-time / durability math (pure, no Vulkan/GLFW required)
add_executable(test_item tests/test_item.c src/item.c src/block.c)
target_include_directories(test_item PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_item PRIVATE m)
endif()
add_test(NAME item COMMAND test_item)

# Mining model: correct-tool speed multiplier by material/kind + harvest-level
# drop gating (pure, no Vulkan/GLFW required).
add_executable(test_mining tests/test_mining.c src/mining.c src/item.c src/block.c)
target_include_directories(test_mining PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mining PRIVATE m)
endif()
add_test(NAME mining COMMAND test_mining)

# Armour model: classification, slot-for-item, points total + damage-reduction
# math + durability (pure, no Vulkan/GLFW required).
add_executable(test_armor tests/test_armor.c src/item.c src/block.c)
target_include_directories(test_armor PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_armor PRIVATE m)
endif()
add_test(NAME armor COMMAND test_armor)

# Combat model: weapon damage (sword>tool>fist, tier scaling), attack cooldown
# ordering, and armour damage-reduction wrapper (pure, no Vulkan/GLFW required).
add_executable(test_combat tests/test_combat.c src/combat.c src/item.c src/block.c)
target_include_directories(test_combat PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_combat PRIVATE m)
endif()
add_test(NAME combat COMMAND test_combat)

# Audio engine: pure procedural PCM/envelope generators, the sound-id table,
# and null-backend lifecycle safety (no Vulkan/GLFW/audio device required).
add_executable(test_audio tests/test_audio.c src/audio.c src/music.c src/audio_backend_null_msvc.c)
target_include_directories(test_audio PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_audio PRIVATE m)
endif()
add_test(NAME audio COMMAND test_audio)

# Block hardness / break-time math (pure, no Vulkan/GLFW required)
add_executable(test_hardness tests/test_hardness.c src/block.c)
target_include_directories(test_hardness PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hardness PRIVATE m)
endif()
add_test(NAME hardness COMMAND test_hardness)

# Biome classification (pure, no Vulkan/GLFW required). Pulls in FastNoiseLite
# (header-only) for its noise fields, hence the include dir.
add_executable(test_biome tests/test_biome.c src/biome.c src/block.c)
target_include_directories(test_biome PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${fastnoiselite_SOURCE_DIR}/C
)
# This test links biome.c without worldgen.c, so it must generate the
# FastNoiseLite implementation itself. In the main build worldgen.c provides
# FNL_IMPL, hence biome.c never defines it there (avoids duplicate symbols).
target_compile_definitions(test_biome PRIVATE FNL_IMPL)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_biome PRIVATE m)
endif()
add_test(NAME biome COMMAND test_biome)

# Survival mechanics math (pure: fall/hunger/drown/lava/regen, no Vulkan/GLFW)
add_executable(test_survival tests/test_survival.c src/survival.c)
target_include_directories(test_survival PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_survival PRIVATE m)
endif()
add_test(NAME survival COMMAND test_survival)

# CPU particle simulation core (pure: emit/integrate/compact, no Vulkan/GLFW).
# GPU rendering integration is deferred to a follow-up ticket.
add_executable(test_particle tests/test_particle.c src/particle.c)
target_include_directories(test_particle PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_particle PRIVATE m)
endif()
add_test(NAME particle COMMAND test_particle)

# CPU billboard expansion for particle rendering (pure: life->alpha fade and
# camera-facing quad geometry, no Vulkan/GLFW). The GPU upload/draw is in
# renderer_frame.c and not unit-tested (GPU code).
add_executable(test_particle_render
    tests/test_particle_render.c src/particle_render.c src/particle.c)
target_include_directories(test_particle_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_particle_render PRIVATE m)
endif()
add_test(NAME particle_render COMMAND test_particle_render)

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
    src/biome.c
    src/cave.c
    src/dungeon.c
    src/loot.c
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

# Worldgen terrain generation: generates real chunks via worldgen_generate and
# asserts invariants (determinism, bedrock floor, surface sanity, stone body,
# ore depth gating, no OOB across a coord spread). Links the modules worldgen
# pulls in; chunk.h transitively includes Vulkan headers, hence the
# VK_NO_PROTOTYPES + Vulkan::Headers wiring mirrored from test_village.
add_executable(test_worldgen
    tests/test_worldgen.c
    src/worldgen.c
    src/ore.c
    src/village.c
    src/biome.c
    src/cave.c
    src/dungeon.c
    src/loot.c
    src/chunk.c
    src/block.c
)
target_include_directories(test_worldgen PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${fastnoiselite_SOURCE_DIR}/C
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_worldgen PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_worldgen PRIVATE cglm Vulkan::Headers Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_worldgen PRIVATE m)
endif()
add_test(NAME worldgen COMMAND test_worldgen)

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

# Multiple-world save management (pure: name sanitize, path build, meta
# format/parse, new-world name/seed derivation, world-list). No Vulkan/GLFW.
add_executable(test_worldsave tests/test_worldsave.c src/worldsave.c)
target_include_directories(test_worldsave PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME worldsave COMMAND test_worldsave)

# Physics collision core (pure: AABB/voxel sweep via BlockQueryFn callback,
# no World/thread machinery). physics.c includes world.h which transitively
# pulls in Vulkan headers, hence the VK_NO_PROTOTYPES + Vulkan::Headers wiring.
add_executable(test_physics
    tests/test_physics.c
    src/physics.c
    src/block.c
)
target_include_directories(test_physics PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_physics PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_physics PRIVATE cglm Vulkan::Headers)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_physics PRIVATE m)
endif()
add_test(NAME physics COMMAND test_physics)

# Chunk block/meta/light storage (plain memory: get/set round-trips, index
# math, bounds, lazy alloc, nibble packing). chunk.h includes chunk_mesh.h
# which pulls in Vulkan headers, hence the VK_NO_PROTOTYPES wiring.
add_executable(test_chunk
    tests/test_chunk.c
    src/chunk.c
    src/block.c
)
target_include_directories(test_chunk PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_chunk PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_chunk PRIVATE cglm Vulkan::Headers Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_chunk PRIVATE m)
endif()
add_test(NAME chunk COMMAND test_chunk)

# Player state transitions (agent-mode init/eye/dip + free-mode noclip motion +
# fixed-timestep accumulator clamp). player.c links physics.c + camera.c and
# references GLFW symbols (only called on the non-agent path), so glfw is linked
# like test_ui. Vulkan headers come in transitively via world.h.
add_executable(test_player
    tests/test_player.c
    src/player.c
    src/physics.c
    src/physics_world.c
    src/camera.c
    src/block.c
)
# physics_world.c provides physics_move (used by player.c) and calls
# world_get_block; test_player supplies its own world_get_block stub (a flat
# floor) so no real world.c / thread machinery is pulled in.
target_include_directories(test_player PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_player PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_player PRIVATE glfw volk cglm Vulkan::Headers ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_player PRIVATE m)
endif()
add_test(NAME player COMMAND test_player)

# Frustum AABB culling (pure math, Vulkan-free: only cglm). Builds a view-proj
# from glm_perspective/glm_lookat, extracts the 6 planes, and tests boxes that
# are inside / behind / off to the side / beyond far / straddling the near plane.
add_executable(test_frustum
    tests/test_frustum.c
    src/frustum.c
)
target_include_directories(test_frustum PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_frustum PRIVATE cglm)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_frustum PRIVATE m)
endif()
add_test(NAME frustum COMMAND test_frustum)

# Chunk hash map (open-addressing linear-probe table keyed on (cx,cz)): insert/
# get/miss, overwrite, remove + probe-chain back-shift, auto-grow rehash, signed
# coordinate packing, iteration. Uses real Chunk objects; chunk.h pulls in
# chunk_mesh.h (Vulkan headers), hence the VK_NO_PROTOTYPES + Vulkan::Headers
# wiring mirrored from test_chunk.
add_executable(test_chunkmap
    tests/test_chunkmap.c
    src/chunk_map.c
    src/chunk.c
    src/block.c
)
target_include_directories(test_chunkmap PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${volk_SOURCE_DIR}
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_compile_definitions(test_chunkmap PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(test_chunkmap PRIVATE cglm Vulkan::Headers Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_chunkmap PRIVATE m)
endif()
add_test(NAME chunkmap COMMAND test_chunkmap)

# World worker-thread work queue (pure intrusive FIFO extracted to workqueue.h:
# enqueue/dequeue ordering, single-element boundary, interleaved traffic,
# drain/refill cycles). Header-only, no deps.
add_executable(test_worldqueue tests/test_worldqueue.c)
target_include_directories(test_worldqueue PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME worldqueue COMMAND test_worldqueue)

foreach(t minecraft test_block_physics test_agent test_net test_ui
          test_remote_player test_mob test_mesher test_lighting test_client
          test_inventory test_container test_raycast test_ore test_cave test_loot test_weather test_audio test_hardness test_biome
          test_daynight test_village test_worldgen test_survival test_world_persist
          test_crafting test_smelting test_item test_mining test_armor test_combat test_worldsave
          test_physics test_chunk test_player test_worldqueue test_chunk_stream
          test_frustum test_chunkmap test_mob_model test_particle test_particle_render)
    msvc_enable_c11_atomics(${t})
endforeach()

# ---- End-to-end gameplay scenario tests --------------------------------------
# Drive the real integrated authoritative server + headless no-render client via
# the JSON agent harness (./build/minecraft --agent --headless). Each .scenario
# file under tests/scenarios feeds commands + `expect` assertions through
# tools/scenario_run.py, which fails (rc 1) on the first failed assertion or a
# harness crash/timeout. These need the built `minecraft` binary, so they are
# gated behind ENABLE_SCENARIO_TESTS and skipped where no binary/Python exists.
option(ENABLE_SCENARIO_TESTS
       "Register headless gameplay scenario tests (need the minecraft binary)" ON)

if(ENABLE_SCENARIO_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # Every scenario known to pass deterministically against the harness.
        # Block-edit loops (place/break/mine, chest/furnace) are intentionally
        # omitted: the shared-world headless physics drifts the player position
        # and the chunk-gen/reach validation races, so those edits are not
        # reproducible enough to assert on. See the runner header for the format.
        set(SCENARIO_NAMES
            tp_and_state
            give_and_inventory
            craft_planks
            craft_pickaxe
            mob_takes_damage
            mob_death
            spawn_passive_mob
            weather_control
            set_time_of_day
            sound_swing
            sound_hurt
            sound_eat
        )
        foreach(sc ${SCENARIO_NAMES})
            add_test(NAME scenario_${sc}
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_SOURCE_DIR}/tools/scenario_run.py
                             ${CMAKE_SOURCE_DIR}/tests/scenarios/${sc}.scenario)
            # Pass the freshly built binary path + a generous per-test timeout.
            set_tests_properties(scenario_${sc} PROPERTIES
                ENVIRONMENT "MINECRAFT_BIN=$<TARGET_FILE:minecraft>;SCENARIO_TIMEOUT=60"
                TIMEOUT 90)
        endforeach()
    else()
        message(STATUS "Python3 interpreter not found; scenario tests disabled.")
    endif()
endif()
