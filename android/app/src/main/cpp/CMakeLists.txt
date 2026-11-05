# libmain.so for SDL3's SDLActivity: the recompiled game, the PC port's hooks and audio (cmake/bf2mc.cmake), the shader
# translator and texture decoder, and the Vulkan renderer. libSDL3 lives inside librexruntime.so, which the SDK builds.
cmake_minimum_required(VERSION 3.25)
project(bf2mc_app C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../.. ABSOLUTE)
set(BF2_APP_LIB_DIR ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
include(${ROOT}/cmake/bf2mc.cmake)
# the SDK sends its output to its own folder: Gradle packages what lands here
set_target_properties(rexruntime PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${BF2_APP_LIB_DIR})

add_library(main SHARED app_main.cpp vk_context.cpp vk_shaders.cpp vk_renderer.cpp vk_loader13.cpp
    ${BF2_PORT}/native_gpu/textures.cpp ${BF2_PORT}/native_gpu/xenos_hlsl.cpp)
bf2mc_add_game(main)
target_include_directories(main PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${ROOT}/third_party/rexglue-sdk/thirdparty/glslang)
target_compile_definitions(main PRIVATE ENABLE_HLSL)  # ShaderLang.h hides the HLSL options without it
target_sources(main PRIVATE ${ROOT}/third_party/rexglue-sdk/thirdparty/glslang/StandAlone/ResourceLimits.cpp)
target_link_libraries(main PRIVATE android vulkan glslang SPIRV Vulkan::Headers)
target_link_options(main PRIVATE -rdynamic)
