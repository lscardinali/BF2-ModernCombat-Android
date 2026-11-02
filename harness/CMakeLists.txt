# Android arm64 executables run from adb, without the APK: the SDK runtime and, for now, a smoke test of the parts of it
# that depend on the device and need no game files.
#   tools/build_harness.ps1 [-Target smoke]
cmake_minimum_required(VERSION 3.25)
project(bf2mc_harness C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/third_party/rexglue-sdk ${CMAKE_BINARY_DIR}/rexglue-sdk EXCLUDE_FROM_ALL)

add_executable(smoke smoke.cpp)
target_link_libraries(smoke PRIVATE rexruntime)
set_target_properties(smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
