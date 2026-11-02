# What the Android harness, the Android app and the Windows test build share: the SDK, the recompiled game code as an
# object library and the PC port's sources that do not depend on its D3D11 renderer or its settings screen.
#   include(cmake/bf2mc.cmake) adds the bf2mc_game_code object library and the bf2mc_add_game() function
include_guard(GLOBAL)
get_filename_component(BF2_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(BF2_RECOMP ${BF2_ROOT}/out/recomp)
set(BF2_GEN ${BF2_RECOMP}/generated/default)
set(BF2_PORT ${BF2_ROOT}/third_party/bf2-modern-combat-pc/project/src)
if(NOT EXISTS ${BF2_GEN}/sources.cmake)
    message(FATAL_ERROR "no generated game code in ${BF2_GEN}: run tools/recompile.ps1 first")
endif()

if(WIN32)
    # the SDK's prebuilt Windows package, as the PC port builds with (tools/build_windows.ps1)
    find_package(rexglue 0.10.0 REQUIRED CONFIG)
else()
    set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
    set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${BF2_ROOT}/third_party/rexglue-sdk ${CMAKE_BINARY_DIR}/rexglue-sdk EXCLUDE_FROM_ALL)
endif()

# the flags the SDK's own sources build with; the SDK sets them for its directory only
set(BF2_FLAGS -fno-strict-aliasing -ffp-model=strict -fno-char8_t)

# generated code: warnings off. Android: no debug info, ARMv8.2 for LSE atomics instead of the outlined helpers.
# Windows: line tables, so a debugger's stacks show the sub_<guest address> names, and __try in the guest SEH scopes.
if(WIN32)
    set(BF2_GAME_FLAGS -msse4.1 -gline-tables-only -gcodeview -fasync-exceptions)
else()
    set(BF2_GAME_FLAGS -march=armv8.2-a -g0)
endif()
set(CMAKE_CURRENT_SOURCE_DIR_SAVED ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CURRENT_SOURCE_DIR ${BF2_RECOMP})
include(${BF2_GEN}/sources.cmake)
set(CMAKE_CURRENT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR_SAVED})
add_library(bf2mc_game_code OBJECT ${GENERATED_SOURCES})
target_include_directories(bf2mc_game_code PRIVATE ${BF2_RECOMP} ${BF2_GEN})
target_link_libraries(bf2mc_game_code PRIVATE rex::runtime)
target_compile_options(bf2mc_game_code PRIVATE ${BF2_FLAGS} -O2 -w ${BF2_GAME_FLAGS})
target_precompile_headers(bf2mc_game_code PRIVATE ${BF2_GEN}/bf2mc_pch.h)

# a target with the game code, the port's hooks and audio, this project's stand ins for its settings screen and Win32
function(bf2mc_add_game target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:bf2mc_game_code>
        ${BF2_PORT}/hooks.cpp
        ${BF2_PORT}/game_natives.cpp
        ${BF2_PORT}/input_remap.cpp
        ${BF2_PORT}/menu_hooks.cpp
        ${BF2_PORT}/apt_menu.cpp
        ${BF2_PORT}/post_effects.cpp
        ${BF2_PORT}/native_gpu/gpu_hooks.cpp
        ${BF2_PORT}/native_audio/audio_exports.cpp
        ${BF2_PORT}/native_audio/audio_system.cpp
        ${BF2_PORT}/native_audio/command_queue.cpp
        ${BF2_PORT}/native_audio/xma_engine.cpp
        ${BF2_ROOT}/src/android_settings.cpp)
    target_include_directories(${target} PRIVATE
        ${BF2_ROOT}/src ${BF2_PORT} ${BF2_RECOMP} ${BF2_GEN}
        ${BF2_ROOT}/third_party/rexglue-sdk/thirdparty/FFmpeg)
    target_compile_options(${target} PRIVATE ${BF2_FLAGS} -O2 -g)
    target_link_libraries(${target} PRIVATE rex::runtime)
    if(WIN32)
        target_compile_options(${target} PRIVATE -gcodeview)
        target_link_libraries(${target} PRIVATE rex::libavcodec rex::libavutil rex::xxhash winmm)
    else()
        target_sources(${target} PRIVATE ${BF2_ROOT}/src/android_win32.cpp ${BF2_ROOT}/src/thread_dump.cpp)
        target_include_directories(${target} PRIVATE ${BF2_ROOT}/src/compat)
        target_link_libraries(${target} PRIVATE libavcodec libavutil xxHash::xxhash dl log)
    endif()
endfunction()
