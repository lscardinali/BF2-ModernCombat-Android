#pragma once
// HLSL to SPIR-V with glslang's HLSL front end, on the device: the translator (xenos_hlsl.cpp) writes HLSL.
#include <cstdint>
#include <string>
#include <vector>

namespace vk {

enum class Stage { Vertex, Fragment };

// Where the registers land in the renderer's one descriptor set. VS: b0 b1 are 0 1; PS: 2 3. Textures are t1 and up
// (stacked views t17 and up), samplers s0 and up. xvmem is t0 of the vertex shader: glslang shifts a ByteAddressBuffer
// as a texture, so it lands on kBindVsTex.
constexpr uint32_t kBindVsUbo = 0, kBindPsUbo = 2;
constexpr uint32_t kBindVsTex = 8, kBindPsTex = 48;  // + the register: 9.. and 49..
constexpr uint32_t kBindSsbo = kBindVsTex;
constexpr uint32_t kBindVsSmp = 100, kBindPsSmp = 116;

bool CompileHlsl(const std::string& src, Stage stage, const char* entry, std::vector<uint32_t>& spirv, std::string& log);

}  // namespace vk
