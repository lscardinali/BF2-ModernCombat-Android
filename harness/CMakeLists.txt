# Android arm64 executables run from adb, without the APK: the smoke test of what the SDK needs from the device, and
# the game with a null renderer, its pad scripted.
#   tools/build_harness.ps1 [-Target bf2mc]
cmake_minimum_required(VERSION 3.25)
project(bf2mc_harness C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/bf2mc.cmake)

add_executable(smoke smoke.cpp)
target_link_libraries(smoke PRIVATE rexruntime)
set_target_properties(smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(bf2mc main.cpp null_renderer.cpp)
bf2mc_add_game(bf2mc)
set_target_properties(bf2mc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
target_link_options(bf2mc PRIVATE -rdynamic)
