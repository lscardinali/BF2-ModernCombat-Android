# bf2mc.exe, for testing here what runs on the phone: the same sources as the Android app (android/app/src/main/cpp), on
# this PC's Vulkan driver. tools/build_windows.ps1 configures and builds it.
cmake_minimum_required(VERSION 3.25)
project(bf2mc_windows C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
include(${ROOT}/cmake/bf2mc.cmake)

set(APP ${ROOT}/android/app/src/main/cpp)
set(SDK_THIRDPARTY ${ROOT}/third_party/rexglue-sdk/thirdparty)

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # glslang asks for less than CMake 4 still accepts
add_subdirectory(${SDK_THIRDPARTY}/glslang ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)

add_executable(bf2mc ${APP}/app_main.cpp ${APP}/vk_context.cpp ${APP}/vk_shaders.cpp
    ${APP}/vk_renderer.cpp ${APP}/vk_loader13.cpp
    ${BF2_PORT}/native_gpu/textures.cpp ${BF2_PORT}/native_gpu/xenos_hlsl.cpp
    ${SDK_THIRDPARTY}/glslang/StandAlone/ResourceLimits.cpp)
bf2mc_add_game(bf2mc)
target_include_directories(bf2mc PRIVATE ${APP} ${SDK_THIRDPARTY}/glslang ${SDK_THIRDPARTY}/vulkan-headers/include)
target_compile_definitions(bf2mc PRIVATE ENABLE_HLSL)
target_link_libraries(bf2mc PRIVATE glslang SPIRV ${BF2_VULKAN_LIB})
target_link_options(bf2mc PRIVATE -fuse-ld=lld -g)
add_custom_command(TARGET bf2mc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:bf2mc> $<TARGET_FILE_DIR:bf2mc>
    COMMAND_EXPAND_LISTS)
