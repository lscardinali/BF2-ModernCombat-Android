// The native_gpu.h entry points on Vulkan: the PC port's D3D11 renderer (renderer.cpp) with its render state, EDRAM and
// resolve logic kept, for a phone GPU:
// - Shaders: Xenos microcode -> HLSL (the port's xenos_hlsl.cpp) -> SPIR-V with glslang, cached on disk.
// - Vertex data: no 512 MB mirror. The ranges a draw's fetch constants name are copied into a per-frame ring, or stay in
//   an arena once they have been stable for a while, and the fetch constants are rebased to where they are.
// - No geometry shaders on Mali: point and rectangle lists expand in the vertex shader, which reads its guest indices
//   from the ring and runs the translated shader for each of them.
// - Uploads go in a command buffer that runs before the frame's, so they split no render pass.
// - BC textures become RGBA8 on the CPU where the GPU lacks BC.
#include <SDL3/SDL.h>
#include <unistd.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <thread>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "bc_decode.h"
#include "native_gpu/native_gpu.h"
#include "native_gpu/textures.h"
#include "native_gpu/write_watch.h"
#include "native_gpu/xenos_hlsl.h"
#include "pc_settings.h"
#include "vk_context.h"
#include "vk_renderer.h"
#include "vk_shaders.h"

namespace bf2n {
namespace {

using vk::Get;

constexpr uint32_t kPhysicalSize = 0x20000000u;
constexpr VkDeviceSize kVertexRing = 32u << 20;   // per frame in flight
constexpr VkDeviceSize kVertexArena = 96u << 20;  // ranges that stopped changing
constexpr VkDeviceSize kUniformRing = 16u << 20;  // per frame in flight
constexpr VkDeviceSize kIndexRing = 16u << 20;    // per frame in flight
constexpr uint32_t kStableFrames = 30;             // unchanged this long: moved to the arena
constexpr uint32_t kWatchAfterFrames = 30;

auto* Mem() { return rex::system::kernel_state()->memory(); }
inline uint8_t* Virt(uint32_t a) {
  static uint8_t* const base = Mem()->virtual_membase();
  return base + a + rex::memory::detail::PhysicalHostOffset(a);
}
inline uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline uint16_t Be16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline void SwapCopy32(uint32_t* dst, const uint8_t* src, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) dst[i] = Be32(src + i * 4);
}
inline uint32_t LoadV(uint32_t a) { return Be32(Virt(a)); }
inline float AsFloat(uint32_t v) { return std::bit_cast<float>(v); }
uint32_t PhysOf(uint32_t virt) {
  if (virt < kPhysicalSize) return virt;
  uint32_t p = Mem()->GetPhysicalAddress(virt);
  return p == UINT32_MAX ? (virt & 0x1FFFFFFFu) : p;
}

// Register shadow inside the D3D device.
uint32_t Reg(uint32_t device, uint32_t r) {
  static const struct {
    uint32_t first, count, offset;
  } kBlocks[] = {{0x2000, 0x10, 0x2CC0}, {0x2100, 0x15, 0x2D0C}, {0x2180, 0x05, 0x2D60},
                 {0x2200, 0x0C, 0x2D74}, {0x2280, 0x15, 0x2DA4}, {0x2300, 0x26, 0x2DF8},
                 {0x2380, 0x08, 0x2E90}};
  for (const auto& b : kBlocks) {
    if (r >= b.first && r < b.first + b.count) return LoadV(device + b.offset + (r - b.first) * 4);
  }
  return 0;
}
float RegF(uint32_t device, uint32_t r) { return AsFloat(Reg(device, r)); }

void NormalizeFetch(TexFetch& f) {
  if (uint32_t base = f.d[1] & 0xFFFFF000u; base >= kPhysicalSize) {
    f.d[1] = (f.d[1] & 0xFFFu) | (PhysOf(base) & 0x1FFFF000u);
  }
  if (uint32_t mip = f.d[5] & 0xFFFFF000u; mip >= kPhysicalSize) {
    f.d[5] = (f.d[5] & 0xFFFu) | (PhysOf(mip) & 0x1FFFF000u);
  }
}
TexFetch ReadFetch(uint32_t addr) {
  TexFetch f;
  for (int i = 0; i < 6; ++i) f.d[i] = LoadV(addr + i * 4);
  NormalizeFetch(f);
  return f;
}

// --- Utility shaders ---------------------------------------------------------------------------

const char kUtilHlsl[] = R"(
[[vk::push_constant]] cbuffer Util { float4 u2; float4 u3; };
struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FsOut vs_fs(uint id : SV_VertexID) {
  FsOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.uv = t;
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
float4 ps_copy(FsOut i) : SV_Target {
  int2 p = int2(i.pos.xy) + int2(u2.xy);
  float4 v = src.Load(int3(p, 0));
  return u2.z != 0.0 ? v.bgra : v;
}
float4 ps_present(FsOut i) : SV_Target {
  float4 v = src.SampleLevel(lin, i.uv * u3.xy, 0.0);
  return float4(u3.z != 0.0 ? v.bgr : v.rgb, 1.0);
}
)";

// The entry points that take the place of the port's geometry shaders (gs_rect, gs_point). The translated vertex shader
// is renamed xvmain; xsys[6].y is where the draw's guest indices start in xvmem.
const char kRectEntry[] = R"(
XVSOut xventry(uint id) {
  uint xbase = asuint(xsys[6].y) + (id / 6u) * 12u;
  XVSOut v[3];
  v[0] = xvmain(xvmem.Load(xbase));
  v[1] = xvmain(xvmem.Load(xbase + 4u));
  v[2] = xvmain(xvmem.Load(xbase + 8u));
  float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
  float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
  int c = 2, a = 0, b = 1;
  if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
  else if (d20 >= d01) { c = 1; a = 2; b = 0; }
  uint k = id % 6u;
  if (k == 0u) return v[c];
  if (k == 1u || k == 4u) return v[a];
  if (k == 2u || k == 3u) return v[b];
  XVSOut d;
  d.pos = v[a].pos + v[b].pos - v[c].pos;
  for (int i = 0; i < 16; ++i) d.o[i] = v[a].o[i] + v[b].o[i] - v[c].o[i];
  d.pts = v[a].pts + v[b].pts - v[c].pts;
  return d;
}
)";
const char kPointEntry[] = R"(
XVSOut xventry(uint id) {
  XVSOut o = xvmain(xvmem.Load(asuint(xsys[6].y) + (id / 6u) * 4u));
  float2 size = o.pts.x > 0.0 ? o.pts.xx : xsys[5].zw;
  float2 h = size * 0.5 * xsys[1].zw * o.pos.w;
  uint k = id % 6u;
  uint q = k == 3u ? 2u : (k == 4u ? 1u : (k == 5u ? 3u : k));
  float2 c = float2((q & 1u) ? 1.0 : -1.0, (q & 2u) ? -1.0 : 1.0);
  o.pos.xy += c * h;
  o.pts.zw = c * float2(0.5, -0.5) + 0.5;
  return o;
}
)";

// The varyings a pixel shader reads: Xenos shaders pass 16 registers, and a tile-based GPU stores and interpolates every
// one of them. The registers of a pixel shader start out as its inputs, so anything above its highest register is unused.
uint32_t PixelInputs(const std::string& hlsl) {
  if (hlsl.find("r[(") != std::string::npos) return 16;  // relative addressing
  size_t body = hlsl.find("float ps = 0.0;");
  uint32_t top = 0;
  for (size_t at = hlsl.find("r[", body); at != std::string::npos; at = hlsl.find("r[", at + 2)) {
    if (at > 0 && (std::isalnum(uint8_t(hlsl[at - 1])) || hlsl[at - 1] == '_')) continue;
    top = std::max<uint32_t>(top, uint32_t(std::atoi(hlsl.c_str() + at + 2)));
  }
  return std::clamp<uint32_t>(top + 1, 1, 16);
}

// Xenos multiplies with 0 * x = 0 even for inf and NaN: four compares and selects per multiply, the bulk of many shaders.
std::string PlainMultiply(std::string hlsl) {
  const std::pair<const char*, const char*> kFrom[] = {
      {"float xmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }", "float xmul(float a, float b) { return a * b; }"},
      {"float4 xmul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }", "float4 xmul(float4 a, float4 b) { return a * b; }"}};
  for (const auto& [from, to] : kFrom) {
    size_t at = hlsl.find(from);
    if (at != std::string::npos) hlsl.replace(at, std::strlen(from), to);
  }
  return hlsl;
}

std::string TrimPixelInputs(std::string hlsl, uint32_t inputs) {
  const std::string decl = "float4 o[16] : TEXCOORD0;\n  float4 pts : TEXCOORD16;";
  size_t at = hlsl.find(decl);
  if (at == std::string::npos) return hlsl;
  hlsl.replace(at, decl.size(), "float4 o[" + std::to_string(inputs) + "] : TEXCOORD0;\n  float4 pts : TEXCOORD" + std::to_string(inputs) + ";");
  at = hlsl.find("for (int ii = 0; ii < ");
  if (at != std::string::npos) {
    size_t end = hlsl.find(';', at + 22);
    hlsl.replace(at + 22, end - (at + 22), std::to_string(inputs));
  }
  return hlsl;
}

// --- Renderer state ----------------------------------------------------------------------------

struct XState {
  uint32_t xb[8];
  uint32_t xl[32];
  uint32_t xf[192];
  uint32_t xsys[kSysFloat4Count * 4];
};
static_assert(sizeof(XState) % 16 == 0);

enum class Kind { kPoints, kLines, kTris, kRects };
enum class VsVariant { kNormal, kRect, kPoint };

struct View {
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  bool linear = true;  // the format can be filtered
  uint32_t skip = 0;   // guest mip levels left out of the image
};

struct HostRT {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;        // attachment
  VkImageView view_gamma = VK_NULL_HANDLE;  // 8_8_8_8_GAMMA view of an 8_8_8_8 target
  VkImageView srv = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, host_width = 0, host_height = 0;
  uint32_t guest_format = 0;
  bool depth = false, blendable = true;
  float scale = 1.0f;
  // a whole-target clear becomes the load op of the next pass
  bool clear_color = false, clear_depth = false, clear_stencil = false;
  VkClearValue clear{};
};

struct Resolved {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView srv = VK_NULL_HANDLE;     // 2D array or cube
  VkImageView srv0 = VK_NULL_HANDLE;    // 2D, slice 0
  std::vector<VkImageView> rtvs;        // per array slice
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, slices = 1, host_width = 0, host_height = 0;
  uint32_t guest_format = 0;
  bool cube = false, linear = true;
  float scale = 1.0f;
  uint32_t used_width = 0, used_height = 0;
  bool bgra = false;
  uint64_t signature = 0;
  uint32_t checked_frame = 0;
};

struct HostTex {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint64_t mem_hash = 0;
  uint32_t checked_frame = 0;
  bool failed = false, linear = true;
  uint32_t levels = 1;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool decoded_bc = false;
  uint32_t skip = 0;  // top guest levels left out (size cap)
  VkDeviceSize bytes = 0;
  TexFetch fetch;
  uint32_t changed_frame = 0;
  bool watched = false;
  uint32_t watch_stamp = 0;
};

struct VsEntry {
  VkShaderModule module[3][17] = {};  // by VsVariant and varyings handed on, made when first used
  std::string hlsl;
  ShaderInfo info;
  uint64_t key = 0;
};
struct PsEntry {
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderInfo info;
  uint64_t key = 0;
  uint32_t inputs = 16;  // varyings it reads
};

struct Targets {
  HostRT* color[4] = {};
  bool gamma[4] = {};
  HostRT* depth = nullptr;
  uint32_t width = 0, height = 0;
  float scale = 1.0f;
};

// Everything of a pipeline that is not dynamic state. Hashed as bytes: always zeroed before it is filled.
struct PipelineKey {
  VkShaderModule vs, ps;
  VkFormat color[4], depth;
  struct Attachment {
    uint8_t enable, src, dst, op, src_a, dst_a, op_a, mask;
  } blend[4];
  uint8_t a2c, topology, restart, cull, front, fill, bias, clamp;
  uint8_t depth_test, depth_write, depth_op, stencil;
  uint8_t f_op, f_fail, f_pass, f_zfail, b_op, b_fail, b_pass, b_zfail;
  uint8_t has_depth_format, pad[3];
};

struct VertexRange {
  uint64_t hash = 0;
  uint32_t seen_frame = 0, ring_offset = 0, changed_frame = 0, captures = 0;
  uint32_t arena_offset = UINT32_MAX;
};

VkFormat VkFormatOf(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case DXGI_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
    case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case DXGI_FORMAT_BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case DXGI_FORMAT_BC4_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
    case DXGI_FORMAT_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case DXGI_FORMAT_BC5_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
  }
}
bool IsBc(DXGI_FORMAT f) { return f >= DXGI_FORMAT_BC1_UNORM && f <= DXGI_FORMAT_BC5_SNORM; }
uint32_t BlockBytes(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_BC1_UNORM || f == DXGI_FORMAT_BC4_UNORM || f == DXGI_FORMAT_BC4_SNORM ? 8 : 16;
}
uint32_t TexelBytes(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM: return 2;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM: return 1;
    default: return 4;
  }
}

VkFormat ColorFormat(uint32_t f) {
  switch (f) {
    case 0:
    case 1: return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:
    case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 4:
    case 6: return VK_FORMAT_R16G16_SFLOAT;
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}
uint32_t CanonicalColorFormat(uint32_t f) {
  switch (f) {
    case 1: return 0;
    case 10: return 2;
    case 12: return 3;
    default: return f;
  }
}
DXGI_FORMAT ResolveFormat(uint32_t tf) {
  switch (tf) {
    case 7:
    case 54: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 22:
    case 23:
    case 33:
    case 36: return DXGI_FORMAT_R32_FLOAT;
    case 27:
    case 30: return DXGI_FORMAT_R16_FLOAT;
    case 28:
    case 31: return DXGI_FORMAT_R16G16_FLOAT;
    case 29:
    case 32: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 13:
    case 25: return DXGI_FORMAT_R16G16_UNORM;
    case 21:
    case 26: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 34:
    case 37: return DXGI_FORMAT_R32G32_FLOAT;
    case 35:
    case 38: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 2: return DXGI_FORMAT_R8_UNORM;
    case 10: return DXGI_FORMAT_R8G8_UNORM;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

VkBlendFactor BlendFactor(uint32_t f, bool alpha) {
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 9: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12:
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13:
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 16: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;
  }
}
VkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}
// D3D11's COMPARISON_FUNC and STENCIL_OP values less one, as the registers hold them
VkCompareOp CompareOp(uint32_t v) { return VkCompareOp(v & 7); }
VkStencilOp StencilOp(uint32_t v) {
  static const VkStencilOp k[8] = {VK_STENCIL_OP_KEEP,    VK_STENCIL_OP_ZERO,      VK_STENCIL_OP_REPLACE,
                                   VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                   VK_STENCIL_OP_INVERT,  VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                   VK_STENCIL_OP_DECREMENT_AND_WRAP};
  return k[v & 7];
}

bool ConvertPrimitives(uint32_t prim, const std::vector<uint32_t>& in, std::vector<uint32_t>& out, Kind& kind) {
  out.clear();
  switch (prim) {
    case 0x01: kind = Kind::kPoints; break;
    case 0x02: case 0x03: case 0x0C: kind = Kind::kLines; break;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x0D: case 0x0E: case 0x0F: kind = Kind::kTris; break;
    case 0x08: kind = Kind::kRects; break;
    default: return false;
  }
  size_t seg_start = 0;
  for (size_t i = 0; i <= in.size(); ++i) {
    if (i < in.size() && in[i] != UINT32_MAX) continue;
    const uint32_t* s = in.data() + seg_start;
    size_t n = i - seg_start;
    seg_start = i + 1;
    switch (prim) {
      case 0x01: out.insert(out.end(), s, s + n); break;
      case 0x02: out.insert(out.end(), s, s + (n & ~size_t(1))); break;
      case 0x03:
      case 0x0C:
        for (size_t k = 0; k + 1 < n; ++k) {
          out.push_back(s[k]);
          out.push_back(s[k + 1]);
        }
        if (prim == 0x0C && n > 2) {
          out.push_back(s[n - 1]);
          out.push_back(s[0]);
        }
        break;
      case 0x04:
      case 0x07:
      case 0x08: out.insert(out.end(), s, s + n / 3 * 3); break;
      case 0x05:
      case 0x0F:
        for (size_t k = 1; k + 1 < n; ++k) {
          out.push_back(s[0]);
          out.push_back(s[k]);
          out.push_back(s[k + 1]);
        }
        break;
      case 0x06:
        for (size_t k = 0; k + 2 < n; ++k) {
          if (k & 1) {
            out.push_back(s[k + 1]);
            out.push_back(s[k]);
          } else {
            out.push_back(s[k]);
            out.push_back(s[k + 1]);
          }
          out.push_back(s[k + 2]);
        }
        break;
      case 0x0D:
        for (size_t k = 0; k + 3 < n; k += 4) {
          const uint32_t q[6] = {s[k], s[k + 1], s[k + 2], s[k], s[k + 2], s[k + 3]};
          out.insert(out.end(), q, q + 6);
        }
        break;
      case 0x0E:
        for (size_t k = 0; k + 3 < n; k += 2) {
          const uint32_t q[6] = {s[k], s[k + 1], s[k + 3], s[k], s[k + 3], s[k + 2]};
          out.insert(out.end(), q, q + 6);
        }
        break;
    }
  }
  return !out.empty();
}

template <typename T>
uint64_t HashOf(const T& v) {
  return XXH3_64bits(&v, sizeof(T));
}

class Renderer {
 public:
  bool Init();
  void Draw(uint32_t device, uint32_t prim, std::vector<uint32_t>& vids, const UpDraw* up);
  void Clear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
  void Resolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture, uint32_t dest_point,
               uint32_t level, uint32_t slice, uint32_t clear_color, float clear_z, uint32_t clear_stencil);
  void Swap(uint32_t device, uint32_t front_texture);
  std::mutex mutex;

 private:
  VkDevice dev() const { return Get().device; }
  void BeginFrame();
  VkCommandBuffer Cmd() { return Get().Current().cmd; }
  VkCommandBuffer Pre() { return Get().Current().pre; }
  void AllBarrier(VkCommandBuffer cmd);
  void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    uint32_t levels = VK_REMAINING_MIP_LEVELS, uint32_t layers = VK_REMAINING_ARRAY_LAYERS);
  VkDeviceMemory AllocImage(VkImage image);
  VkFormatFeatureFlags FormatFeatures(VkFormat f);
  void DestroyLater(VkImage image, VkDeviceMemory memory, std::vector<VkImageView> views);

  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map);
  void CreateFallbacks();
  bool CreateLayouts();
  bool CreateUtility();
  VkShaderModule Module(const std::string& hlsl, vk::Stage stage, const char* entry, const char* name);

  VsEntry* GetVertexShader(uint32_t object);
  VsEntry* GetVertexShaderSlow(uint32_t object);
  PsEntry* GetPixelShader(uint32_t object, uint32_t param_gen_reg);
  PsEntry* GetPixelShaderSlow(uint32_t object, uint32_t param_gen_reg);
  // shader object -> entry, for this frame: spares hashing the microcode on every draw
  std::unordered_map<uint32_t, VsEntry*> vs_memo_;
  std::unordered_map<uint64_t, PsEntry*> ps_memo_;
  // fetch constant + binding -> views and sampler, until the next resolve or frame
  struct BindingKey {
    uint32_t d[6];
    uint32_t binding, aniso;
    bool operator==(const BindingKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
  };
  struct BindingKeyHash {
    size_t operator()(const BindingKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
  };
  struct BindingMemo {
    View srv, stacked;
    VkSampler sampler;
  };
  std::unordered_map<BindingKey, BindingMemo, BindingKeyHash> binding_memo_;
  bool ReadUcode(uint32_t object, bool pixel, std::vector<uint32_t>& out);
  VkShaderModule VsModule(VsEntry* e, VsVariant v, uint32_t varyings);

  HostRT* GetRT(uint32_t base, uint32_t format, uint32_t w, uint32_t h, bool depth);
  bool CurrentTargets(uint32_t device, Targets& t);
  void EndPass();
  void BeginPass(const Targets& t);
  void FlushClears(HostRT* rt);

  uint32_t UploadVertices(uint32_t address, uint32_t bytes, bool force);
  uint32_t UniformAlloc(const void* data, uint32_t size, uint32_t reserve);
  void LateSyncVertices();

  View GetTexture(const TexFetch& f, TexDim want);
  bool GetResolvedView(const TexFetch& f, TexDim want, View& out);
  VkSampler GetSampler(const TexFetch& f, const TextureBinding& b, bool linear, uint32_t skip);
  void EvictTextures();
  uint32_t max_texture_ = 0;           // largest side of a mipmapped texture; 0: no cap
  VkDeviceSize texture_budget_ = 0;    // above this, textures unused for a while go
  VkDeviceSize texture_bytes_ = 0;
  uint32_t evicted_ = 0;
  Resolved* GetResolved(const TexFetch& f, float scale);
  uint64_t GuestSignature(const TexFetch& f);
  void Upload(HostTex& t, const HostTexture& ht, uint32_t levels, bool first);
  void RefreshTextures();
  void WatchMiss(const char* what, uint32_t address);
  bool WatchClean(bool watched, uint32_t address, uint32_t size, uint32_t stamp) const {
    return watched && watch_.active() && watch_.Unchanged(address, size, stamp);
  }

  VkPipeline GetPipeline(const PipelineKey& k);
  VkPipeline UtilPipeline(VkShaderModule ps, VkFormat format);
  VkDescriptorSet AllocSet(VkDescriptorSetLayout layout, uint32_t slot);
  // What the presenting thread needs of a finished frame. Mali's present waits for the GPU to finish the frame: on its
  // own thread that wait does not hold the game, and a frame costs the longer of CPU and GPU instead of their sum.
  struct PresentJob {
    uint32_t slot = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t used_w = 0, used_h = 0, tex_w = 1, tex_h = 1;
    bool bgra = false, linear = true;
  };
  void Present(const PresentJob& job);
  void PresentThread();
  void QueuePresent(const PresentJob& job);
  void WaitPresented(uint32_t slot);
  void WaitAllPresented();
  std::thread present_thread_;
  std::mutex present_mutex_;
  std::condition_variable present_cv_, presented_cv_;
  std::vector<PresentJob> present_queue_;
  bool present_pending_[vk::kFramesInFlight] = {};
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  bool low_quality_ = false;  // no anisotropy, nearest mips, the game's multiply without its 0 * x rule
  void SavePipelineCache();
  // user/capture holds a frame number: that frame's draws are logged and its targets written to user/capture_*.bin
  void CaptureCopies(VkCommandBuffer cmd);
  void CaptureWrite();
  uint32_t capture_frame_ = 0;
  bool Capturing() const { return frame_ == capture_frame_; }
  struct CaptureCopy {
    std::string name;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t* map;
    VkDeviceSize size;
  };
  std::vector<CaptureCopy> capture_copies_;
  std::vector<HostTex*> capture_textures_;
  bool dump_hlsl_ = false;

  std::filesystem::path cache_dir_;
  bool bc_supported_ = false;
  VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
  float scale_ = 1.0f;
  uint32_t frame_ = 1;
  uint32_t frame_draws_ = 0, frame_skipped_ = 0, frame_passes_ = 0;
  uint32_t last_front_base_ = 0;
  uint32_t pipelines_saved_ = 0;
  double wait_ms_ = 0;  // waiting for the GPU and the display, since the last log
  // user/gpu_times: every 150 frames, what each pass of a frame took on the GPU
  struct PassTime {
    uint32_t width, height, draws, format;
    bool depth;
  };
  static constexpr uint32_t kMaxTimedPasses = 62;
  VkQueryPool gpu_queries_[vk::kFramesInFlight] = {};
  std::vector<PassTime> gpu_passes_[vk::kFramesInFlight];
  uint32_t gpu_frame_[vk::kFramesInFlight] = {};
  bool gpu_times_ = false;
  bool timing_this_frame_ = false;
  uint32_t pass_draws_ = 0;
  uint32_t pass_format_ = 0;
  bool pass_depth_ = false;
  void GpuTimesRead(uint32_t slot);

  // host-visible rings: slot frame_index of each
  VkBuffer vertex_ = VK_NULL_HANDLE, uniform_ = VK_NULL_HANDLE, index_ = VK_NULL_HANDLE;
  VkDeviceMemory vertex_mem_ = VK_NULL_HANDLE, uniform_mem_ = VK_NULL_HANDLE, index_mem_ = VK_NULL_HANDLE;
  uint8_t* vertex_map_ = nullptr;
  uint8_t* uniform_map_ = nullptr;
  uint8_t* index_map_ = nullptr;
  VkDeviceSize vertex_size_ = 0;
  uint32_t vertex_used_ = 0, uniform_used_ = 0, index_used_ = 0, arena_used_ = 0;
  uint32_t ubo_align_ = 256;
  std::unordered_map<uint64_t, VertexRange> ranges_;
  std::vector<uint64_t> frame_ranges_;
  // what the ring holds of the last draw's constants
  std::array<uint8_t, 4096> vs_raw_{}, ps_raw_{};
  uint32_t vs_raw_count_ = 0, ps_raw_count_ = 0, vs_offset_ = 0, ps_offset_ = 0;
  XState last_state_{};
  bool state_valid_ = false;
  uint32_t state_offset_ = 0;

  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE, util_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE, util_layout_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> pools_[vk::kFramesInFlight];
  uint32_t pool_index_[vk::kFramesInFlight] = {};
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  VkShaderModule util_vs_ = VK_NULL_HANDLE, util_copy_ = VK_NULL_HANDLE, util_present_ = VK_NULL_HANDLE;
  VkSampler linear_clamp_ = VK_NULL_HANDLE, nearest_clamp_ = VK_NULL_HANDLE;

  VkImage black_image_[3] = {};
  VkDeviceMemory black_memory_[3] = {};
  VkImageView black_2d_ = VK_NULL_HANDLE, black_3d_ = VK_NULL_HANDLE, black_cube_ = VK_NULL_HANDLE;

  std::unordered_map<uint64_t, std::unique_ptr<VsEntry>> vs_cache_;
  std::unordered_map<uint64_t, std::unique_ptr<PsEntry>> ps_cache_;
  std::unordered_map<uint64_t, HostRT> rts_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  std::unordered_map<uint64_t, HostTex> textures_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  std::vector<uint64_t> frame_textures_;
  WriteWatch watch_;

  // the pass being recorded
  bool pass_open_ = false;
  VkImageView pass_views_[5] = {};
  uint32_t pass_w_ = 0, pass_h_ = 0;
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet bound_set_ = VK_NULL_HANDLE;

  struct SetKey {
    VkImageView views[16 + 16];
    VkImageLayout layouts[16 + 16];
    VkSampler samplers[16];
    bool operator==(const SetKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
  };
  SetKey last_vs_{}, last_ps_{};
  VkDescriptorSet last_set_ = VK_NULL_HANDLE;
};

Renderer* g_renderer = nullptr;
SDL_Window* g_window = nullptr;
std::filesystem::path g_user_dir;

// --- Initialization ----------------------------------------------------------------------------

VkFormatFeatureFlags Renderer::FormatFeatures(VkFormat f) {
  VkFormatProperties p;
  vkGetPhysicalDeviceFormatProperties(Get().gpu, f, &p);
  return p.optimalTilingFeatures;
}

bool Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory,
                            uint8_t*& map) {
  auto& c = Get();
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  if (vkCreateBuffer(c.device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(c.device, buffer, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = c.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(c.device, &mai, nullptr, &memory) != VK_SUCCESS) return false;
  VK_CHECK(vkBindBufferMemory(c.device, buffer, memory, 0));
  VK_CHECK(vkMapMemory(c.device, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&map)));
  return true;
}

bool Renderer::Init() {
  auto& c = Get();
  const long ram_mb = sysconf(_SC_PHYS_PAGES) / 1024 * sysconf(_SC_PAGESIZE) / 1024;
  const bool low_memory = ram_mb <= 4096;
  scale_ = low_memory ? 0.5f : 1.0f;
  max_texture_ = low_memory ? 256 : 1024;
  low_quality_ = low_memory;
  texture_budget_ = VkDeviceSize(low_memory ? 192 : 768) << 20;
  if (std::ifstream f(g_user_dir / "scale"); f) f >> scale_;
  if (std::ifstream f(g_user_dir / "max_texture"); f) f >> max_texture_;
  REXLOG_INFO("bf2native: {} MB of memory: render scale {}, textures up to {} (over {} MB unused ones go)", ram_mb, scale_, max_texture_,
              texture_budget_ >> 20);
  bc_supported_ = c.features.textureCompressionBC;
  const auto ds = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  depth_format_ = (FormatFeatures(VK_FORMAT_D32_SFLOAT_S8_UINT) & ds) == ds ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
  const auto& lim = c.props.limits;
  ubo_align_ = uint32_t(std::max<VkDeviceSize>(lim.minUniformBufferOffsetAlignment, 16));
  vertex_size_ = std::min<VkDeviceSize>(kVertexRing * vk::kFramesInFlight + kVertexArena, lim.maxStorageBufferRange);
  REXLOG_INFO("vulkan: BC textures {}, depth format {}, storage range {} MB, sampled images per stage {}, outputs {}",
              bc_supported_ ? "yes" : "no (decoded on the CPU)", int(depth_format_), lim.maxStorageBufferRange >> 20,
              lim.maxPerStageDescriptorSampledImages, lim.maxVertexOutputComponents);

  if (!CreateBuffer(vertex_size_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vertex_, vertex_mem_, vertex_map_) ||
      !CreateBuffer(kUniformRing * vk::kFramesInFlight, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniform_, uniform_mem_, uniform_map_) ||
      !CreateBuffer(kIndexRing * vk::kFramesInFlight, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_, index_mem_, index_map_)) {
    REXLOG_ERROR("vulkan: the ring buffers failed");
    return false;
  }
  cache_dir_ = g_user_dir / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
  if (!CreateLayouts() || !CreateUtility()) return false;

  // pipelines from the last run
  std::vector<char> blob;
  if (std::ifstream f(cache_dir_ / "pipelines.bin", std::ios::binary); f) {
    blob.assign(std::istreambuf_iterator<char>(f), {});
  }
  VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  pc.initialDataSize = blob.size();
  pc.pInitialData = blob.data();
  VK_CHECK(vkCreatePipelineCache(c.device, &pc, nullptr, &pipeline_cache_));

  if (std::ifstream f(g_user_dir / "capture"); f) f >> capture_frame_;
  if (capture_frame_) REXLOG_INFO("bf2native: capturing frame {}", capture_frame_);
  dump_hlsl_ = capture_frame_ != 0;
  if (watch_.Start()) REXLOG_INFO("bf2native: write watch on");
  present_pipeline_ = UtilPipeline(util_present_, c.swap_format);
  gpu_times_ = std::filesystem::exists(g_user_dir / "gpu_times", ec);
  if (gpu_times_) {
    VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = kMaxTimedPasses + 2;
    for (auto& q : gpu_queries_) VK_CHECK(vkCreateQueryPool(c.device, &qp, nullptr, &q));
  }
  present_thread_ = std::thread([this] { PresentThread(); });
  BeginFrame();
  CreateFallbacks();
  REXLOG_INFO("bf2native: Vulkan renderer on {}", c.props.deviceName);
  return true;
}

void Renderer::QueuePresent(const PresentJob& job) {
  {
    std::lock_guard lock(present_mutex_);
    present_pending_[job.slot] = true;
    present_queue_.push_back(job);
  }
  present_cv_.notify_one();
}

void Renderer::WaitPresented(uint32_t slot) {
  std::unique_lock lock(present_mutex_);
  presented_cv_.wait(lock, [&] { return !present_pending_[slot]; });
}

void Renderer::WaitAllPresented() {
  for (uint32_t i = 0; i < vk::kFramesInFlight; ++i) WaitPresented(i);
}

void Renderer::PresentThread() {
  for (;;) {
    PresentJob job;
    {
      std::unique_lock lock(present_mutex_);
      present_cv_.wait(lock, [&] { return !present_queue_.empty(); });
      job = present_queue_.front();
      present_queue_.erase(present_queue_.begin());
    }
    Present(job);
    {
      std::lock_guard lock(present_mutex_);
      present_pending_[job.slot] = false;
    }
    presented_cv_.notify_all();
  }
}

void Renderer::GpuTimesRead(uint32_t slot) {
  auto& passes = gpu_passes_[slot];
  if (passes.empty()) return;
  uint64_t stamps[kMaxTimedPasses + 2] = {};
  const uint32_t n = uint32_t(passes.size()) + 1;
  if (vkGetQueryPoolResults(dev(), gpu_queries_[slot], 0, n, sizeof(stamps), stamps, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    passes.clear();
    return;
  }
  const double ns = Get().props.limits.timestampPeriod;
  std::string line;
  for (uint32_t i = 0; i < passes.size(); ++i) {
    const PassTime& k = passes[i];
    line += fmt::format(" {}x{}{}f{} {}d {:.1f}ms |", k.width, k.height, k.depth ? "z" : "", k.format, k.draws, double(stamps[i + 1] - stamps[i]) * ns / 1e6);
  }
  REXLOG_INFO("gpu frame {}: total {:.1f} ms:{}", gpu_frame_[slot], double(stamps[n - 1] - stamps[0]) * ns / 1e6, line);
  passes.clear();
}

void Renderer::BeginFrame() {
  auto& c = Get();
  const auto wait_start = std::chrono::steady_clock::now();
  WaitPresented(c.frame_index);
  c.Begin();
  if (gpu_times_) {
    GpuTimesRead(c.frame_index);
    timing_this_frame_ = frame_ % 150 == 0;
    if (timing_this_frame_) {
      VkCommandBuffer pre = c.Current().pre;
      vkCmdResetQueryPool(pre, gpu_queries_[c.frame_index], 0, kMaxTimedPasses + 2);
      vkCmdWriteTimestamp(c.Current().cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpu_queries_[c.frame_index], 0);
      gpu_frame_[c.frame_index] = frame_;
    }
  }
  wait_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
  uint32_t fi = c.frame_index;
  pool_index_[fi] = 0;
  for (VkDescriptorPool p : pools_[fi]) vkResetDescriptorPool(c.device, p, 0);
  vertex_used_ = uniform_used_ = index_used_ = 0;
  vs_memo_.clear();
  ps_memo_.clear();
  binding_memo_.clear();
  vs_raw_count_ = ps_raw_count_ = 0;
  state_valid_ = false;
  bound_pipeline_ = VK_NULL_HANDLE;
  bound_set_ = VK_NULL_HANDLE;
  last_set_ = VK_NULL_HANDLE;
}

void Renderer::AllBarrier(VkCommandBuffer cmd) {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void Renderer::ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                            uint32_t levels, uint32_t layers) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, levels, 0, layers};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkDeviceMemory Renderer::AllocImage(VkImage image) {
  auto& c = Get();
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(c.device, image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = c.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory m = VK_NULL_HANDLE;
  VkResult r = vkAllocateMemory(c.device, &mai, nullptr, &m);
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("vulkan: image memory {} KB failed ({})", req.size >> 10, int(r));
    return VK_NULL_HANDLE;
  }
  VK_CHECK(vkBindImageMemory(c.device, image, m, 0));
  return m;
}

void Renderer::DestroyLater(VkImage image, VkDeviceMemory memory, std::vector<VkImageView> views) {
  VkDevice d = dev();
  Get().Defer([d, image, memory, views = std::move(views)] {
    for (VkImageView v : views)
      if (v) vkDestroyImageView(d, v, nullptr);
    if (image) vkDestroyImage(d, image, nullptr);
    if (memory) vkFreeMemory(d, memory, nullptr);
  });
}

void Renderer::CreateFallbacks() {
  auto& c = Get();
  struct {
    VkImageType type;
    VkImageViewType view;
    uint32_t layers;
    VkImageCreateFlags flags;
    VkImageView* out;
  } kinds[3] = {{VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 0, &black_2d_},
                {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0, &black_3d_},
                {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &black_cube_}};
  for (int i = 0; i < 3; ++i) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = kinds[i].flags;
    ici.imageType = kinds[i].type;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {1, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = kinds[i].layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateImage(c.device, &ici, nullptr, &black_image_[i]));
    black_memory_[i] = AllocImage(black_image_[i]);
    ImageBarrier(Pre(), black_image_[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(Pre(), black_image_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    ImageBarrier(Pre(), black_image_[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = black_image_[i];
    vi.viewType = kinds[i].view;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kinds[i].layers};
    VK_CHECK(vkCreateImageView(c.device, &vi, nullptr, kinds[i].out));
  }
}

bool Renderer::CreateLayouts() {
  auto& c = Get();
  std::vector<VkDescriptorSetLayoutBinding> b;
  auto add = [&](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding x{};
    x.binding = binding;
    x.descriptorType = type;
    x.descriptorCount = 1;
    x.stageFlags = stages;
    b.push_back(x);
  };
  add(vk::kBindVsUbo, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
  add(vk::kBindVsUbo + 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
  add(vk::kBindPsUbo, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
  add(vk::kBindPsUbo + 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
  add(vk::kBindSsbo, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
  for (uint32_t i = 1; i <= 32; ++i) add(vk::kBindVsTex + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_VERTEX_BIT);
  for (uint32_t i = 1; i <= 32; ++i) add(vk::kBindPsTex + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
  for (uint32_t i = 0; i < 16; ++i) add(vk::kBindVsSmp + i, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_VERTEX_BIT);
  for (uint32_t i = 0; i < 16; ++i) add(vk::kBindPsSmp + i, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sl.bindingCount = uint32_t(b.size());
  sl.pBindings = b.data();
  VK_CHECK(vkCreateDescriptorSetLayout(c.device, &sl, nullptr, &set_layout_));
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &set_layout_;
  VK_CHECK(vkCreatePipelineLayout(c.device, &pl, nullptr, &layout_));

  b.clear();
  // t0 and s0 of a pixel shader, where CompileHlsl shifts them
  add(vk::kBindPsTex, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
  add(vk::kBindPsSmp, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
  sl.bindingCount = uint32_t(b.size());
  sl.pBindings = b.data();
  VK_CHECK(vkCreateDescriptorSetLayout(c.device, &sl, nullptr, &util_set_layout_));
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
  pl.pSetLayouts = &util_set_layout_;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &push;
  VK_CHECK(vkCreatePipelineLayout(c.device, &pl, nullptr, &util_layout_));
  return true;
}

VkDescriptorSet Renderer::AllocSet(VkDescriptorSetLayout layout, uint32_t slot) {
  auto& c = Get();
  const uint32_t fi = slot;
  for (;;) {
    if (pool_index_[fi] == pools_[fi].size()) {
      VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 * 2048},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
                                       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * 2048},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, 16 * 2048}};
      VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      dpi.maxSets = 2048;
      dpi.poolSizeCount = 4;
      dpi.pPoolSizes = sizes;
      VkDescriptorPool p;
      VK_CHECK(vkCreateDescriptorPool(c.device, &dpi, nullptr, &p));
      pools_[fi].push_back(p);
    }
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pools_[fi][pool_index_[fi]];
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &layout;
    VkDescriptorSet s;
    if (vkAllocateDescriptorSets(c.device, &dai, &s) == VK_SUCCESS) return s;
    ++pool_index_[fi];
  }
}

// SPIR-V kept on disk by the hash of its HLSL: compiling it takes the phone longer than loading a level.
VkShaderModule Renderer::Module(const std::string& hlsl, vk::Stage stage, const char* entry, const char* name) {
  // the entry point counts: the utility shaders share one source
  uint64_t key = XXH3_64bits(hlsl.data(), hlsl.size()) ^ (uint64_t(stage) << 63) ^ XXH3_64bits(entry, std::strlen(entry));
  char file[32];
  snprintf(file, sizeof(file), "%016llx.spv", static_cast<unsigned long long>(key));
  std::filesystem::path path = cache_dir_ / file;
  if (dump_hlsl_) std::ofstream(cache_dir_ / (std::string(file) + ".hlsl"), std::ios::binary) << hlsl;
  std::vector<uint32_t> spirv;
  if (std::ifstream f(path, std::ios::binary | std::ios::ate); f) {
    spirv.resize(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(spirv.data()), std::streamsize(spirv.size() * 4));
  }
  if (spirv.empty()) {
    std::string log;
    if (!vk::CompileHlsl(hlsl, stage, entry, spirv, log)) {
      if (log.size() > 900) log.resize(900);
      REXLOG_ERROR("bf2native: shader {} failed: {}", name, log);
      std::ofstream(cache_dir_ / (std::string(file) + ".failed.hlsl"), std::ios::binary) << hlsl;
      return VK_NULL_HANDLE;
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  VkShaderModule m = VK_NULL_HANDLE;
  VK_CHECK(vkCreateShaderModule(dev(), &ci, nullptr, &m));
  return m;
}

bool Renderer::CreateUtility() {
  util_vs_ = Module(kUtilHlsl, vk::Stage::Vertex, "vs_fs", "util vs");
  util_copy_ = Module(kUtilHlsl, vk::Stage::Fragment, "ps_copy", "util copy");
  util_present_ = Module(kUtilHlsl, vk::Stage::Fragment, "ps_present", "util present");
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(dev(), &si, nullptr, &linear_clamp_));
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  VK_CHECK(vkCreateSampler(dev(), &si, nullptr, &nearest_clamp_));
  return util_vs_ && util_copy_ && util_present_;
}

// --- Shaders -----------------------------------------------------------------------------------

bool Renderer::ReadUcode(uint32_t object, bool pixel, std::vector<uint32_t>& out) {
  if (!object) return false;
  uint32_t ptr = LoadV(object + (pixel ? 0x0C : 0x28));
  uint32_t bytes = LoadV(object + (pixel ? 0x3C : 0x258));
  if (!ptr || !bytes || bytes > 0x10000 || (bytes & 3)) return false;
  const uint8_t* src = ptr < kPhysicalSize ? Mem()->TranslatePhysical(ptr) : Virt(ptr);
  if (!pixel) {
    // The flush may have loaded a patched copy instead (see ImmediateVertexShader).
    const auto& imm = LoadedImmediateVertexShader();
    if (imm.shader == object && imm.code.size() == bytes) {
      uint32_t decl = LoadV(imm.device + dev::kVertexDeclaration);
      if (decl == imm.declaration && LoadV(decl + 32) == imm.declaration_id) src = imm.code.data();
    }
  }
  out.resize(bytes / 4);
  for (uint32_t i = 0; i < bytes / 4; ++i) out[i] = Be32(src + i * 4);
  return true;
}

VsEntry* Renderer::GetVertexShader(uint32_t object) {
  // The flush may substitute a patched copy per declaration: that one is looked up every time.
  const bool immediate = LoadedImmediateVertexShader().shader == object;
  if (!immediate) {
    auto memo = vs_memo_.find(object);
    if (memo != vs_memo_.end()) return memo->second;
  }
  VsEntry* r = GetVertexShaderSlow(object);
  if (!immediate && r) vs_memo_[object] = r;
  return r;
}

VsEntry* Renderer::GetVertexShaderSlow(uint32_t object) {
  static std::vector<uint32_t> ucode;
  if (!ReadUcode(object, false, ucode)) return nullptr;
  uint64_t key = XXH3_64bits(ucode.data(), ucode.size() * 4);
  auto it = vs_cache_.find(key);
  if (it != vs_cache_.end()) return it->second.get();
  auto e = std::make_unique<VsEntry>();
  e->key = key;
  e->hlsl = TranslateXenosShader(ucode.data(), uint32_t(ucode.size()), false, {}, e->info);
  if (low_quality_) e->hlsl = PlainMultiply(std::move(e->hlsl));
  if (dump_hlsl_) {
    char name[64];
    snprintf(name, sizeof(name), "vs_%016llx.hlsl", static_cast<unsigned long long>(key));
    std::ofstream(cache_dir_ / name, std::ios::binary) << e->hlsl;
  }
  VsEntry* r = e.get();
  vs_cache_[key] = std::move(e);
  return r;
}

VkShaderModule Renderer::VsModule(VsEntry* e, VsVariant v, uint32_t varyings) {
  VkShaderModule& m = e->module[int(v)][varyings];
  if (m) return m;
  std::string hlsl = e->hlsl;
  const std::string from = "XVSOut main(uint vid : SV_VertexID) {";
  size_t at = hlsl.find(from);
  if (at == std::string::npos) return VK_NULL_HANDLE;
  hlsl.replace(at, from.size(), "XVSOut xvmain(uint vid) {");
  if (v != VsVariant::kNormal) hlsl += v == VsVariant::kRect ? kRectEntry : kPointEntry;
  const std::string n = std::to_string(varyings);
  hlsl += "struct XVSLink {\n  precise float4 pos : SV_Position;\n  float4 o[" + n + "] : TEXCOORD0;\n  float4 pts : TEXCOORD" + n + ";\n};\n";
  hlsl += "XVSLink main(uint id : SV_VertexID) {\n  XVSOut f = ";
  hlsl += v == VsVariant::kNormal ? "xvmain" : "xventry";
  hlsl += "(id);\n  XVSLink l;\n  l.pos = f.pos;\n  for (int i = 0; i < " + n + "; ++i) l.o[i] = f.o[i];\n  l.pts = f.pts;\n  return l;\n}\n";
  m = Module(hlsl, vk::Stage::Vertex, "main", "vertex");
  return m;
}

PsEntry* Renderer::GetPixelShader(uint32_t object, uint32_t param_gen_reg) {
  const uint64_t memo_key = uint64_t(object) | (uint64_t(param_gen_reg) << 32);
  auto memo = ps_memo_.find(memo_key);
  if (memo != ps_memo_.end()) return memo->second;
  PsEntry* r = GetPixelShaderSlow(object, param_gen_reg);
  if (r) ps_memo_[memo_key] = r;
  return r;
}

PsEntry* Renderer::GetPixelShaderSlow(uint32_t object, uint32_t param_gen_reg) {
  static std::vector<uint32_t> ucode;
  if (!ReadUcode(object, true, ucode)) return nullptr;
  uint64_t key = XXH3_64bits(ucode.data(), ucode.size() * 4) ^ (uint64_t(param_gen_reg) << 56);
  auto it = ps_cache_.find(key);
  if (it != ps_cache_.end()) return it->second->module ? it->second.get() : nullptr;
  auto e = std::make_unique<PsEntry>();
  e->key = key;
  TranslateOptions opt;
  opt.ps_param_gen_reg = param_gen_reg;
  std::string hlsl = TranslateXenosShader(ucode.data(), uint32_t(ucode.size()), true, opt, e->info);
  if (low_quality_) hlsl = PlainMultiply(std::move(hlsl));
  if (dump_hlsl_) {
    char name[64];
    snprintf(name, sizeof(name), "ps_%016llx.hlsl", static_cast<unsigned long long>(key));
    std::ofstream(cache_dir_ / name, std::ios::binary) << hlsl;
  }
  e->inputs = PixelInputs(hlsl);
  hlsl = TrimPixelInputs(std::move(hlsl), e->inputs);
  e->module = Module(hlsl, vk::Stage::Fragment, "main", "pixel");
  PsEntry* r = e->module ? e.get() : nullptr;
  ps_cache_[key] = std::move(e);
  return r;
}

// --- Render targets ----------------------------------------------------------------------------

HostRT* Renderer::GetRT(uint32_t base, uint32_t format, uint32_t w, uint32_t h, bool depth) {
  if (!depth) format = CanonicalColorFormat(format);
  w = std::clamp(w, 1u, 8192u);
  h = std::clamp(h, 1u, 8192u);
  uint64_t key = uint64_t(base & 0xFFF) | (uint64_t(format & 0xF) << 12) | (uint64_t(depth) << 16) | (uint64_t(w) << 17) |
                 (uint64_t(h) << 31);
  const float scale = w >= 640 ? scale_ : 1.0f;
  auto it = rts_.find(key);
  if (it != rts_.end() && it->second.scale == scale) return &it->second;
  if (it != rts_.end()) {
    DestroyLater(it->second.image, it->second.memory, {it->second.view, it->second.view_gamma, it->second.srv});
    rts_.erase(it);
  }
  HostRT rt;
  rt.width = w;
  rt.height = h;
  rt.guest_format = format;
  rt.depth = depth;
  rt.scale = scale;
  rt.host_width = std::max<uint32_t>(1, uint32_t(std::ceil(float(w) * scale - 0.001f)));
  rt.host_height = std::max<uint32_t>(1, uint32_t(std::ceil(float(h) * scale - 0.001f)));
  rt.format = depth ? depth_format_ : ColorFormat(format);
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  // 8_8_8_8 and 8_8_8_8_GAMMA are one target: blends in linear through the sRGB view
  const bool rgba8 = rt.format == VK_FORMAT_R8G8B8A8_UNORM;
  ici.flags = rgba8 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
  const VkFormat view_formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
  VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  format_list.viewFormatCount = 2;
  format_list.pViewFormats = view_formats;
  if (rgba8 && Get().format_list) ici.pNext = &format_list;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = rt.format;
  ici.extent = {rt.host_width, rt.host_height, 1};
  ici.mipLevels = ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (vkCreateImage(dev(), &ici, nullptr, &rt.image) != VK_SUCCESS) return nullptr;
  rt.memory = AllocImage(rt.image);
  if (!rt.memory) return nullptr;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rt.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = rt.format;
  const VkImageAspectFlags all = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange = {all, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &rt.view));
  vi.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &rt.srv));
  if (rgba8) {
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &rt.view_gamma));
  }
  rt.blendable = depth || (FormatFeatures(rt.format) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
  ImageBarrier(Pre(), rt.image, all, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  if (depth) {
    rt.clear_depth = rt.clear_stencil = true;
    rt.clear.depthStencil = {1.0f, 0};
  } else {
    rt.clear_color = true;
  }
  REXLOG_INFO("bf2native: {} target base {} fmt {} {}x{}", depth ? "depth" : "color", base & 0xFFF, format, w, h);
  return &(rts_[key] = rt);
}

bool SurfaceSize(uint32_t surface, uint32_t& w, uint32_t& h) {
  if (!surface) return false;
  uint32_t v = LoadV(surface + 24);
  w = (v & 0x1FFF) + 1;
  h = ((v >> 13) & 0x1FFF) + 1;
  return true;
}

bool Renderer::CurrentTargets(uint32_t device, Targets& t) {
  t = Targets();
  uint32_t surfaces[4], depth_surface = LoadV(device + dev::kDepthSurface);
  for (uint32_t i = 0; i < 4; ++i) surfaces[i] = LoadV(device + dev::kColorSurfaces + i * 4);
  uint32_t w = 0, h = 0;
  bool have = false;
  for (uint32_t i = 0; i < 4 && !have; ++i) have = SurfaceSize(surfaces[i], w, h);
  if (!have) have = SurfaceSize(depth_surface, w, h);
  if (!have) return false;
  t.width = std::clamp(w, 1u, 8192u);
  t.height = std::clamp(h, 1u, 8192u);
  t.scale = t.width >= 640 ? scale_ : 1.0f;
  uint32_t edram_mode = Reg(device, 0x2208) & 7;
  static const uint32_t kInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  if (edram_mode != 5) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!surfaces[i]) continue;
      uint32_t info = Reg(device, kInfo[i]);
      uint32_t format = (info >> 16) & 0xF;
      t.color[i] = GetRT(info & 0xFFF, format, t.width, t.height, false);
      t.gamma[i] = format == 1;
    }
  }
  if (depth_surface) {
    uint32_t info = Reg(device, 0x2002);
    t.depth = GetRT(info & 0xFFF, (info >> 16) & 1, t.width, t.height, true);
  }
  return true;
}

void Renderer::EndPass() {
  if (!pass_open_) return;
  vkCmdEndRendering(Cmd());
  AllBarrier(Cmd());
  if (timing_this_frame_) {
    auto& passes = gpu_passes_[Get().frame_index];
    if (passes.size() < kMaxTimedPasses) {
      passes.push_back({pass_w_, pass_h_, pass_draws_, pass_format_, pass_depth_});
      vkCmdWriteTimestamp(Cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_queries_[Get().frame_index], uint32_t(passes.size()));
    }
  }
  pass_open_ = false;
  bound_pipeline_ = VK_NULL_HANDLE;
  bound_set_ = VK_NULL_HANDLE;
}

void Renderer::BeginPass(const Targets& t) {
  VkImageView views[5] = {};
  for (uint32_t i = 0; i < 4; ++i)
    if (t.color[i]) views[i] = t.gamma[i] && t.color[i]->view_gamma ? t.color[i]->view_gamma : t.color[i]->view;
  if (t.depth) views[4] = t.depth->view;
  uint32_t w = 0, h = 0;
  for (uint32_t i = 0; i < 4 && !w; ++i)
    if (t.color[i]) w = t.color[i]->host_width, h = t.color[i]->host_height;
  if (!w && t.depth) w = t.depth->host_width, h = t.depth->host_height;
  if (pass_open_ && pass_w_ == w && pass_h_ == h && std::memcmp(pass_views_, views, sizeof(views)) == 0) return;
  EndPass();
  VkRenderingAttachmentInfo color[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (!t.color[i]) continue;
    color[i].imageView = views[i];
    color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color[i].loadOp = t.color[i]->clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color[i].clearValue = t.color[i]->clear;
    t.color[i]->clear_color = false;
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (t.depth) {
    depth.imageView = views[4];
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = t.depth->clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue = t.depth->clear;
    stencil = depth;
    stencil.loadOp = t.depth->clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    t.depth->clear_depth = t.depth->clear_stencil = false;
  }
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 4;
  ri.pColorAttachments = color;
  ri.pDepthAttachment = t.depth ? &depth : nullptr;
  ri.pStencilAttachment = t.depth ? &stencil : nullptr;
  vkCmdBeginRendering(Cmd(), &ri);
  pass_open_ = true;
  std::memcpy(pass_views_, views, sizeof(views));
  pass_w_ = w;
  pass_h_ = h;
  ++frame_passes_;
  pass_draws_ = 0;
  pass_format_ = t.color[0] ? t.color[0]->guest_format : 99;
  pass_depth_ = t.depth != nullptr;
}

// A target about to be read: its waiting clear has to happen first.
void Renderer::FlushClears(HostRT* rt) {
  if (!rt || !(rt->clear_color || rt->clear_depth || rt->clear_stencil)) return;
  Targets t;
  (rt->depth ? t.depth : t.color[0]) = rt;
  BeginPass(t);
  EndPass();
}

// --- Vertex data -------------------------------------------------------------------------------

uint32_t Renderer::UploadVertices(uint32_t address, uint32_t bytes, bool force) {
  address &= 0x1FFFFFFFu;
  bytes = std::min(bytes, kPhysicalSize - address);
  const uint8_t* src = Mem()->physical_membase() + address;
  const uint64_t key = (uint64_t(address) << 32) | bytes;
  VertexRange& r = ranges_[key];
  const bool first_this_frame = r.seen_frame != frame_;
  if (first_this_frame || force) {
    uint64_t h = XXH3_64bits(src, bytes);
    if (h != r.hash) {
      r.hash = h;
      r.changed_frame = frame_;
      r.arena_offset = UINT32_MAX;  // the GPU may still read the old copy: it stays until the arena is reset
      r.seen_frame = 0;
    }
  }
  if (r.arena_offset != UINT32_MAX) {
    r.seen_frame = frame_;
    return r.arena_offset;
  }
  if (r.seen_frame == frame_) {
    // Same contents already in this frame's ring: another draw of the buffer. Only a range captured with other contents
    // is rehashed (force or changed above resets seen_frame).
    return r.ring_offset;
  }
  const VkDeviceSize ring_end = kVertexRing;
  const uint32_t ring_base = uint32_t(kVertexRing * Get().frame_index);
  // stable for a while: into the arena, copied once
  if (frame_ - r.changed_frame >= kStableFrames && arena_used_ + bytes <= vertex_size_ - kVertexRing * vk::kFramesInFlight) {
    uint32_t offset = uint32_t(kVertexRing * vk::kFramesInFlight) + arena_used_;
    std::memcpy(vertex_map_ + offset, src, bytes);
    arena_used_ += (bytes + 3) & ~3u;
    r.arena_offset = offset;
    r.seen_frame = frame_;
    return offset;
  }
  if (vertex_used_ + bytes > ring_end) {
    static int logged = 0;
    if (logged++ < 10) REXLOG_WARN("bf2native: vertex ring full ({} MB in this frame)", vertex_used_ >> 20);
    return UINT32_MAX;
  }
  uint32_t offset = ring_base + vertex_used_;
  std::memcpy(vertex_map_ + offset, src, bytes);
  vertex_used_ += (bytes + 3) & ~3u;
  r.ring_offset = offset;
  r.seen_frame = frame_;
  ++r.captures;
  frame_ranges_.push_back(key);
  return offset;
}

// Like the console GPU, which reads guest memory when it executes the frame: ranges the game wrote after the draws that
// use them get their final contents. Only ranges that were captured once this frame: a range rewritten between draws
// keeps a capture per draw.
void Renderer::LateSyncVertices() {
  for (uint64_t key : frame_ranges_) {
    VertexRange& r = ranges_[key];
    if (r.captures == 1 && r.seen_frame == frame_ && r.arena_offset == UINT32_MAX) {
      uint32_t address = uint32_t(key >> 32), bytes = uint32_t(key);
      const uint8_t* src = Mem()->physical_membase() + address;
      uint64_t h = XXH3_64bits(src, bytes);
      if (h != r.hash) {
        std::memcpy(vertex_map_ + r.ring_offset, src, bytes);
        r.hash = h;
        r.changed_frame = frame_;
      }
    }
    r.captures = 0;
  }
  frame_ranges_.clear();
}

uint32_t Renderer::UniformAlloc(const void* data, uint32_t size, uint32_t reserve) {
  uint32_t aligned = (reserve + ubo_align_ - 1) & ~(ubo_align_ - 1);
  // the binding's range is always 4096 bytes, however few the shader reads
  if (uniform_used_ + 4096 > kUniformRing) {
    static int logged = 0;
    if (logged++ < 10) REXLOG_WARN("bf2native: uniform ring full");
    uniform_used_ = 0;  // a wrong picture beats no picture
  }
  uint32_t offset = uint32_t(kUniformRing * Get().frame_index) + uniform_used_;
  uniform_used_ += aligned;
  if (data) std::memcpy(uniform_map_ + offset, data, size);
  return offset;
}

// --- Textures ----------------------------------------------------------------------------------

uint64_t Renderer::GuestSignature(const TexFetch& f) {
  const uint8_t* phys = Mem()->physical_membase();
  uint32_t base = f.base_address() & 0x1FFFFFFFu;
  uint32_t extent = ((f.width() + 31) & ~31u) * ((f.height() + 31) & ~31u) * 4;
  uint32_t n = std::min<uint32_t>({64u * 1024, extent, kPhysicalSize - base});
  return XXH3_64bits(phys + base, n);
}

Resolved* Renderer::GetResolved(const TexFetch& f, float scale) {
  uint32_t base = f.base_address() & 0x1FFFFFFFu;
  uint32_t w = f.width(), h = f.height();
  uint32_t dim = f.dimension();
  bool cube = dim == 3;
  uint32_t slices = cube ? 6 : (dim == 1 ? f.depth() : 1);
  auto it = resolved_.find(base);
  if (it != resolved_.end()) {
    Resolved& r = it->second;
    if (r.width == w && r.height == h && r.slices == slices && r.cube == cube && r.guest_format == f.format() && r.scale == scale)
      return &r;
    std::vector<VkImageView> views = r.rtvs;
    views.push_back(r.srv);
    views.push_back(r.srv0);
    DestroyLater(r.image, r.memory, std::move(views));
    resolved_.erase(it);
  }
  Resolved r;
  r.width = w;
  r.height = h;
  r.slices = slices;
  r.cube = cube;
  r.guest_format = f.format();
  r.scale = scale;
  r.host_width = std::max<uint32_t>(1, uint32_t(std::ceil(float(w) * scale - 0.001f)));
  r.host_height = std::max<uint32_t>(1, uint32_t(std::ceil(float(h) * scale - 0.001f)));
  r.format = VkFormatOf(ResolveFormat(f.format()));
  const auto need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if ((FormatFeatures(r.format) & need) != need) {
    REXLOG_WARN("bf2native: resolve format {} is not usable here, RGBA16F instead", int(r.format));
    r.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  }
  r.linear = FormatFeatures(r.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = r.format;
  ici.extent = {r.host_width, r.host_height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = slices;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (vkCreateImage(dev(), &ici, nullptr, &r.image) != VK_SUCCESS) return nullptr;
  r.memory = AllocImage(r.image);
  if (!r.memory) return nullptr;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = r.image;
  vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = r.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, slices};
  VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &r.srv));
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.subresourceRange.layerCount = 1;
  VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &r.srv0));
  for (uint32_t i = 0; i < slices; ++i) {
    vi.subresourceRange.baseArrayLayer = i;
    VkImageView rtv;
    VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &rtv));
    r.rtvs.push_back(rtv);
  }
  ImageBarrier(Pre(), r.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue black{};
  black.float32[3] = 1.0f;
  VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, slices};
  vkCmdClearColorImage(Pre(), r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
  ImageBarrier(Pre(), r.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
  REXLOG_INFO("bf2native: resolve target {:08X} fmt {} {}x{}x{}", base, f.format(), w, h, slices);
  return &(resolved_[base] = std::move(r));
}

bool Renderer::GetResolvedView(const TexFetch& f, TexDim want, View& out) {
  auto it = resolved_.find(f.base_address() & 0x1FFFFFFFu);
  if (it == resolved_.end()) return false;
  Resolved& r = it->second;
  if (r.checked_frame != frame_) {
    r.checked_frame = frame_;
    if (GuestSignature(f) != r.signature) {  // the CPU put something else there
      std::vector<VkImageView> views = r.rtvs;
      views.push_back(r.srv);
      views.push_back(r.srv0);
      DestroyLater(r.image, r.memory, std::move(views));
      resolved_.erase(it);
      return false;
    }
  }
  bool is_cube = want == TexDim::kCube;
  if (is_cube != r.cube || want == TexDim::k3D) return false;
  out = {r.srv, VK_IMAGE_LAYOUT_GENERAL, r.linear};
  return true;
}

void Renderer::Upload(HostTex& t, const HostTexture& ht, uint32_t levels, bool first) {
  auto& c = Get();
  const bool bc = IsBc(ht.format);
  const bool expand = bc && t.decoded_bc;
  const uint32_t array = ht.is_3d ? 1 : ht.depth;
  // what each subresource holds once it is in the staging buffer
  std::vector<std::vector<uint8_t>> converted;
  struct Copy {
    const uint8_t* data;
    size_t size;
    VkBufferImageCopy region;
  };
  std::vector<Copy> copies;
  VkDeviceSize total = 0;
  const uint32_t top_w = bc ? (ht.width + 3) & ~3u : ht.width, top_h = bc ? (ht.height + 3) & ~3u : ht.height;
  for (uint32_t s = 0; s < array; ++s) {
    for (uint32_t l = t.skip; l < levels + t.skip && l < ht.levels; ++l) {
      const auto& sub = ht.subs[size_t(s) * ht.levels + l];
      if (sub.data.empty()) continue;
      uint32_t w = std::max(top_w >> l, 1u), h = std::max(top_h >> l, 1u), d = ht.is_3d ? std::max(ht.depth >> l, 1u) : 1;
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - t.skip, ht.is_3d ? 0 : s, 1};
      const uint8_t* data = sub.data.data();
      size_t size = sub.data.size();
      if (bc) {
        const uint32_t block = BlockBytes(ht.format);
        const uint32_t row_blocks = sub.row_pitch / block;
        const uint32_t rows = sub.slice_pitch / std::max(sub.row_pitch, 1u);
        if (expand) {
          converted.emplace_back();
          DecodeBc(ht.format, data, row_blocks, rows, converted.back());
          if (t.format == VK_FORMAT_A1R5G5B5_UNORM_PACK16) PackTo1555(converted.back());
          data = converted.back().data();
          size = converted.back().size();
          region.bufferRowLength = row_blocks * 4;
          region.bufferImageHeight = rows * 4;
        } else {
          region.bufferRowLength = row_blocks * 4;
          region.bufferImageHeight = rows * 4;
        }
        region.imageExtent = {std::min(w, row_blocks * 4), std::min(h, rows * 4), 1};
      } else {
        const uint32_t texel = TexelBytes(t.format);
        const uint32_t row_texels = sub.row_pitch / texel;
        const uint32_t rows = sub.slice_pitch / std::max(sub.row_pitch, 1u);
        region.bufferRowLength = row_texels;
        region.bufferImageHeight = rows;
        region.imageExtent = {std::min(w, row_texels), std::min(h, rows), d};
      }
      copies.push_back({data, size, region});
      total += (size + 15) & ~size_t(15);
    }
  }
  if (!total) return;
  VkBuffer buffer;
  VkDeviceMemory memory;
  uint8_t* map;
  if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, memory, map)) {
    REXLOG_ERROR("vulkan: {} KB of staging failed", total >> 10);
    return;
  }
  VkDeviceSize at = 0;
  std::vector<VkBufferImageCopy> regions;
  for (Copy& k : copies) {
    std::memcpy(map + at, k.data, k.size);
    k.region.bufferOffset = at;
    regions.push_back(k.region);
    at += (k.size + 15) & ~size_t(15);
  }
  VkCommandBuffer pre = Pre();
  ImageBarrier(pre, t.image, VK_IMAGE_ASPECT_COLOR_BIT,
               first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(pre, buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
  ImageBarrier(pre, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  VkDevice d = c.device;
  c.Defer([d, buffer, memory] {
    vkDestroyBuffer(d, buffer, nullptr);
    vkFreeMemory(d, memory, nullptr);
  });
}

View Renderer::GetTexture(const TexFetch& f, TexDim want) {
  if (f.type() != 2 || !f.base_address()) return {};
  View resolved;
  if (GetResolvedView(f, want, resolved)) return resolved;
  uint32_t dim = f.dimension();
  TexDim have = dim == 2 ? TexDim::k3D : dim == 3 ? TexDim::kCube : TexDim::k2D;
  TexDim want_class = want == TexDim::k1D ? TexDim::k2D : want;
  if (have != want_class) return {};

  // Identity: everything except sampler state (clamp, filters, swizzle, bias, border).
  struct Key {
    uint32_t d0, d1, d2, d4, d5;
  } key{f.d[0] & ~(0x1FFu << 10), f.d[1], f.d[2], f.d[4] & (0xFFu << 2), f.d[5] & ~3u};
  uint64_t k = HashOf(key);
  HostTex& t = textures_[k];
  if (t.view) {  // contents are refreshed at Swap (RefreshTextures)
    if (t.checked_frame != frame_) {
      t.checked_frame = frame_;
      t.fetch = f;
      frame_textures_.push_back(k);
    }
    return {t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t.linear, t.skip};
  }
  if (t.checked_frame == frame_) return {};
  t.checked_frame = frame_;
  const uint8_t* phys = Mem()->physical_membase();
  uint64_t h = HashTextureMemory(phys, f);
  if (t.failed && h == t.mem_hash) return {};
  t.mem_hash = h;
  t.failed = true;
  auto fail = [&](const char* why) -> View {
    static int logged = 0;
    if (logged++ < 60) {
      REXLOG_WARN("bf2native: texture {:08X} fmt {} {}x{}x{} dim {} tiled {}: {}", f.base_address(), f.format(), f.width(),
                  f.height(), f.depth(), f.dimension(), f.tiled(), why);
    }
    return {};
  };
  HostTexture ht;
  if (!DecodeTexture(phys, f, ht)) return fail("decode");
  uint32_t array = ht.is_3d ? 1 : ht.depth;
  uint32_t levels = ht.levels;
  for (uint32_t l = 0; l < ht.levels; ++l) {
    if (ht.subs[l].data.empty()) {
      levels = l;
      break;
    }
  }
  if (!levels) return fail("no levels");
  if (ht.is_cube && array != 6) return fail("cube faces");
  const bool bc = IsBc(ht.format);
  uint32_t w = ht.width, hh = ht.height;
  if (bc) {
    w = (w + 3) & ~3u;
    hh = (hh + 3) & ~3u;
  }
  // Size cap: the top levels of a mipmapped 2D texture stay out of the image. Movie frames and menus have no mips.
  t.skip = 0;
  if (max_texture_ && !ht.is_3d) {
    while (t.skip + 1 < levels && std::max(w >> t.skip, hh >> t.skip) > max_texture_) ++t.skip;
  }
  const uint32_t top_w = std::max(w >> t.skip, 1u), top_h = std::max(hh >> t.skip, 1u);
  const uint32_t host_levels = levels - t.skip;
  t.decoded_bc = bc && !bc_supported_;
  t.format = t.decoded_bc ? BcDecodedFormat(ht.format) : VkFormatOf(ht.format);
  if (t.format == VK_FORMAT_UNDEFINED) return fail("format");
  const VkFormatFeatureFlags features = FormatFeatures(t.format);
  if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return fail("format not sampled here");
  t.linear = features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = ht.is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ici.imageType = ht.is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ici.format = t.format;
  ici.extent = {top_w, top_h, ht.is_3d ? ht.depth : 1};
  ici.mipLevels = host_levels;
  ici.arrayLayers = array;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (vkCreateImage(dev(), &ici, nullptr, &t.image) != VK_SUCCESS) return fail("CreateImage");
  t.memory = AllocImage(t.image);
  if (!t.memory) {
    vkDestroyImage(dev(), t.image, nullptr);
    t.image = VK_NULL_HANDLE;
    return fail("memory");
  }
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = t.image;
  vi.viewType = ht.is_3d ? VK_IMAGE_VIEW_TYPE_3D : ht.is_cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = t.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, host_levels, 0, array};
  VK_CHECK(vkCreateImageView(dev(), &vi, nullptr, &t.view));
  Upload(t, ht, host_levels, true);
  {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev(), t.image, &req);
    t.bytes = req.size;
    texture_bytes_ += req.size;
  }
  t.failed = false;
  t.levels = host_levels;
  t.fetch = f;
  t.changed_frame = frame_;
  t.watched = false;
  frame_textures_.push_back(k);
  return {t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t.linear, t.skip};
}

// Over budget: what has not been drawn for a while goes, oldest first. It comes back from guest memory if it is used again.
void Renderer::EvictTextures() {
  if (!texture_budget_ || texture_bytes_ <= texture_budget_) return;
  std::vector<std::pair<uint32_t, uint64_t>> old;
  for (auto& [k, tex] : textures_) {
    if (tex.image && frame_ - tex.checked_frame > 90) old.push_back({tex.checked_frame, k});
  }
  std::sort(old.begin(), old.end());
  for (auto& [frame, k] : old) {
    if (texture_bytes_ <= texture_budget_ * 3 / 4) break;
    auto it = textures_.find(k);
    HostTex& tex = it->second;
    DestroyLater(tex.image, tex.memory, {tex.view});
    texture_bytes_ -= tex.bytes;
    textures_.erase(it);
    ++evicted_;
  }
}

VkSampler Renderer::GetSampler(const TexFetch& f, const TextureBinding& b, bool linear, uint32_t skip) {
  auto& c = Get();
  uint32_t mag = b.mag != 0xFF ? b.mag : f.mag_filter();
  uint32_t min = b.min != 0xFF ? b.min : f.min_filter();
  uint32_t mip = b.mip != 0xFF ? b.mip : f.mip_filter();
  uint32_t aniso = b.aniso != 0xFF ? b.aniso : f.aniso_filter();
  VkSamplerCreateInfo d{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  if (low_quality_ && mip == 1) mip = 0;
  if (aniso >= 2 && aniso <= 5 && linear && c.features.samplerAnisotropy && !low_quality_) {
    d.magFilter = d.minFilter = VK_FILTER_LINEAR;
    d.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    d.anisotropyEnable = VK_TRUE;
    d.maxAnisotropy = std::min(float(1u << (aniso - 1)), c.props.limits.maxSamplerAnisotropy);
  } else {
    d.magFilter = linear && mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    d.minFilter = linear && min == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    d.mipmapMode = linear && mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  }
  const VkSamplerAddressMode once = c.mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  const VkSamplerAddressMode addr[8] = {VK_SAMPLER_ADDRESS_MODE_REPEAT,        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, once,
                                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, once,
                                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, once};
  d.addressModeU = addr[f.clamp(0)];
  d.addressModeV = addr[f.clamp(1)];
  d.addressModeW = addr[f.clamp(2)];
  d.mipLodBias = std::clamp(f.lod_bias(), -c.props.limits.maxSamplerLodBias, c.props.limits.maxSamplerLodBias);
  d.minLod = float(f.mip_min_level() > skip ? f.mip_min_level() - skip : 0);
  d.maxLod = mip == 2 ? d.minLod : std::max(d.minLod, float(f.mip_max_level() > skip ? f.mip_max_level() - skip : 0));
  d.borderColor = f.border_color() == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                  : f.border_color() == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                                          : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  uint64_t key = XXH3_64bits(reinterpret_cast<const uint8_t*>(&d) + offsetof(VkSamplerCreateInfo, magFilter),
                             sizeof(d) - offsetof(VkSamplerCreateInfo, magFilter));
  VkSampler& s = samplers_[key];
  if (!s) VK_CHECK(vkCreateSampler(c.device, &d, nullptr, &s));
  return s;
}

void Renderer::WatchMiss(const char* what, uint32_t address) {
  REXLOG_WARN("bf2native: {} {:08X} changed without a write callback; write watch disabled", what, address);
  watch_.Disable();
}

// Re-uploads textures sampled this frame whose guest memory the CPU changed since they were decoded (video frames,
// dynamic textures), before the frame's commands run, so the recorded draws see the final contents.
void Renderer::RefreshTextures() {
  const uint8_t* phys = Mem()->physical_membase();
  for (uint64_t k : frame_textures_) {
    auto it = textures_.find(k);
    if (it == textures_.end() || !it->second.image) continue;
    HostTex& t = it->second;
    bool clean = false;
    if (t.watched) {
      TextureMemory r = TextureMemoryRanges(t.fetch);
      clean = WatchClean(true, r.base, r.base_size, t.watch_stamp) && WatchClean(true, r.mip, r.mip_size, t.watch_stamp);
      if (!clean) t.watched = false;
    }
    const bool verify = clean && ((k ^ (k >> 29)) + frame_) % 64 == 0;
    if (clean && !verify) continue;
    uint64_t h = HashTextureMemory(phys, t.fetch);
    if (h == t.mem_hash) {
      if (!t.watched && watch_.active() && frame_ - t.changed_frame >= kWatchAfterFrames) {
        TextureMemory r = TextureMemoryRanges(t.fetch);
        uint32_t stamp = watch_.Arm(r.base, r.base_size);
        if (r.mip_size) watch_.Arm(r.mip, r.mip_size);
        if (HashTextureMemory(phys, t.fetch) == t.mem_hash) {
          t.watched = true;
          t.watch_stamp = stamp;
        }
      }
      continue;
    }
    if (verify) WatchMiss("texture", t.fetch.base_address());
    t.watched = false;
    t.changed_frame = frame_;
    HostTexture ht;
    if (!DecodeTexture(phys, t.fetch, ht)) continue;
    Upload(t, ht, t.levels, false);
    t.mem_hash = h;
  }
  frame_textures_.clear();
}

// --- Pipelines ---------------------------------------------------------------------------------

VkPipeline Renderer::GetPipeline(const PipelineKey& k) {
  uint64_t hash = HashOf(k);
  auto it = pipelines_.find(hash);
  if (it != pipelines_.end()) return it->second;
  VkPipelineShaderStageCreateInfo st[2]{};
  st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, k.vs, "main"};
  st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k.ps, "main"};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(k.topology);
  ia.primitiveRestartEnable = k.restart;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.depthClampEnable = k.clamp;
  rs.polygonMode = k.fill ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  rs.cullMode = k.cull;
  rs.frontFace = VkFrontFace(k.front);
  rs.depthBiasEnable = k.bias;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  ms.alphaToCoverageEnable = k.a2c;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = k.depth_test;
  ds.depthWriteEnable = k.depth_write;
  ds.depthCompareOp = VkCompareOp(k.depth_op);
  ds.stencilTestEnable = k.stencil;
  ds.front.compareOp = VkCompareOp(k.f_op);
  ds.front.failOp = VkStencilOp(k.f_fail);
  ds.front.passOp = VkStencilOp(k.f_pass);
  ds.front.depthFailOp = VkStencilOp(k.f_zfail);
  ds.back.compareOp = VkCompareOp(k.b_op);
  ds.back.failOp = VkStencilOp(k.b_fail);
  ds.back.passOp = VkStencilOp(k.b_pass);
  ds.back.depthFailOp = VkStencilOp(k.b_zfail);
  VkPipelineColorBlendAttachmentState ba[4]{};
  for (int i = 0; i < 4; ++i) {
    const auto& b = k.blend[i];
    ba[i].blendEnable = b.enable;
    ba[i].srcColorBlendFactor = VkBlendFactor(b.src);
    ba[i].dstColorBlendFactor = VkBlendFactor(b.dst);
    ba[i].colorBlendOp = VkBlendOp(b.op);
    ba[i].srcAlphaBlendFactor = VkBlendFactor(b.src_a);
    ba[i].dstAlphaBlendFactor = VkBlendFactor(b.dst_a);
    ba[i].alphaBlendOp = VkBlendOp(b.op_a);
    ba[i].colorWriteMask = b.mask;
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 4;
  cb.pAttachments = ba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                                VK_DYNAMIC_STATE_BLEND_CONSTANTS,    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 7;
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 4;
  ri.pColorAttachmentFormats = k.color;
  ri.depthAttachmentFormat = k.depth;
  ri.stencilAttachmentFormat = k.depth;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &ri;
  gp.stageCount = k.ps ? 2 : 1;
  gp.pStages = st;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dy;
  gp.layout = layout_;
  VkPipeline p = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev(), pipeline_cache_, 1, &gp, nullptr, &p);
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("vulkan: pipeline creation failed ({})", int(r));
    p = VK_NULL_HANDLE;
  }
  pipelines_[hash] = p;
  return p;
}

VkPipeline Renderer::UtilPipeline(VkShaderModule ps, VkFormat format) {
  struct {
    VkShaderModule ps;
    VkFormat format;
    uint32_t tag;
  } key{ps, format, 0x5D11u};
  uint64_t hash = HashOf(key);
  auto it = pipelines_.find(hash);
  if (it != pipelines_.end()) return it->second;
  VkPipelineShaderStageCreateInfo st[2]{};
  st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, util_vs_, "vs_fs"};
  st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps,
           ps == util_copy_ ? "ps_copy" : "ps_present"};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba{};
  ba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &ri;
  gp.stageCount = 2;
  gp.pStages = st;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dy;
  gp.layout = util_layout_;
  VkPipeline p = VK_NULL_HANDLE;
  VK_CHECK(vkCreateGraphicsPipelines(dev(), pipeline_cache_, 1, &gp, nullptr, &p));
  pipelines_[hash] = p;
  return p;
}

void Renderer::SavePipelineCache() {
  if (pipelines_saved_ == pipelines_.size()) return;
  pipelines_saved_ = uint32_t(pipelines_.size());
  size_t size = 0;
  vkGetPipelineCacheData(dev(), pipeline_cache_, &size, nullptr);
  std::vector<char> blob(size);
  if (vkGetPipelineCacheData(dev(), pipeline_cache_, &size, blob.data()) != VK_SUCCESS) return;
  std::ofstream(cache_dir_ / "pipelines.bin", std::ios::binary).write(blob.data(), std::streamsize(size));
}

// --- Draw --------------------------------------------------------------------------------------

void Renderer::Draw(uint32_t device, uint32_t prim, std::vector<uint32_t>& vids, const UpDraw* up) {
  ++frame_draws_;
  auto skip = [&](const char* why) {
    ++frame_skipped_;
    static int logged = 0;
    if (logged++ < 40) REXLOG_WARN("bf2native: draw skipped ({}), prim {}", why, prim);
  };
  static std::vector<uint32_t> indices;
  Kind kind;
  const bool native_strip = prim == 0x06 || prim == 0x03;
  const bool native_list = (prim == 0x04 || prim == 0x01) && std::find(vids.begin(), vids.end(), UINT32_MAX) == vids.end();
  if (native_strip) {
    kind = prim == 0x06 ? Kind::kTris : Kind::kLines;
  } else if (native_list) {
    kind = prim == 0x04 ? Kind::kTris : Kind::kPoints;
    if (prim == 0x04) vids.resize(vids.size() / 3 * 3);
    if (vids.empty()) return skip("primitive");
  } else if (!ConvertPrimitives(prim, vids, indices, kind)) {
    return skip("primitive");
  }
  const auto& host_indices = native_strip || native_list ? vids : indices;
  const uint32_t index_count = uint32_t(host_indices.size());

  uint32_t mode = Reg(device, 0x2205);
  if (kind == Kind::kTris && (mode & 3) == 3) return;  // both faces culled

  VsEntry* vs = GetVertexShader(LoadV(device + dev::kVertexShader));
  if (!vs) return skip("vertex shader");
  uint32_t program_cntl = Reg(device, 0x2180);
  uint32_t param_gen = (program_cntl >> 18) & 1 ? (Reg(device, 0x2181) >> 8) & 0xFF : ~0u;
  uint32_t ps_object = LoadV(device + dev::kPixelShader);
  PsEntry* ps = ps_object ? GetPixelShader(ps_object, param_gen) : nullptr;
  if (ps_object && !ps) return skip("pixel shader");
  const VsVariant variant = kind == Kind::kRects ? VsVariant::kRect : kind == Kind::kPoints ? VsVariant::kPoint : VsVariant::kNormal;
  VkShaderModule vs_module = VsModule(vs, variant, ps ? ps->inputs : 1);
  if (!vs_module) return skip("vertex shader module");

  Targets t;
  if (!CurrentTargets(device, t)) return skip("no render target");

  // --- Constants and state block.
  static XState st;
  uint8_t* d = Virt(device);
  for (int i = 0; i < 8; ++i) st.xb[i] = Be32(d + dev::kBoolLoopConstants + i * 4);
  for (int i = 0; i < 32; ++i) st.xl[i] = Be32(d + dev::kBoolLoopConstants + 32 + i * 4);
  SwapCopy32(st.xf, d + dev::kFetchConstants, 192);
  if (up) {
    // UP draws read their vertices from the command buffer through vertex fetch constant 95.
    uint32_t phys = PhysOf(up->vertices);
    uint32_t bytes = up->vertex_count * up->stride;
    st.xf[190] = (phys & ~3u) | 3u;
    st.xf[191] = (((bytes + 3) / 4) << 2) | 2u;
  }
  for (uint32_t k = 0; k < 96; ++k) {
    if ((st.xf[k / 3 * 6] & 3) == 2) continue;  // slot holds a texture fetch constant
    uint32_t& d0 = st.xf[k * 2];
    if ((d0 & 3) == 3 && (d0 & ~3u) >= kPhysicalSize) d0 = (PhysOf(d0 & ~3u) & 0x1FFFFFFCu) | 3u;
  }
  // --- Vertex data: copies in the ring or arena, and the fetch constants point there.
  bool vertex_ok = true;
  for (uint32_t k = 0; k < 96; ++k) {
    if (!((vs->info.vfetch_used[k >> 6] >> (k & 63)) & 1)) continue;
    uint32_t d0 = st.xf[k * 2], d1 = st.xf[k * 2 + 1];
    if ((d0 & 3) != 3) continue;
    uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
    uint32_t offset = UploadVertices(d0 & 0x1FFFFFFCu, std::min(bytes, 32u << 20), up && k == 95);
    if (offset == UINT32_MAX) {
      vertex_ok = false;
      break;
    }
    st.xf[k * 2] = offset | 3u;
  }
  if (!vertex_ok) return skip("vertex data");

  uint32_t vte = Reg(device, 0x2206);
  float sx = (vte & 1) ? RegF(device, 0x210F) : 1.0f;
  float ox = (vte & 2) ? RegF(device, 0x2110) : 0.0f;
  float sy = (vte & 4) ? RegF(device, 0x2111) : 1.0f;
  float oy = (vte & 8) ? RegF(device, 0x2112) : 0.0f;
  float sz = (vte & 16) ? RegF(device, 0x2113) : 1.0f;
  float oz = (vte & 32) ? RegF(device, 0x2114) : 0.0f;
  float vx = 0, vy = 0, vw = float(t.width), vh = float(t.height);
  if ((vte & 1) && std::fabs(sx) >= 0.5f) {
    vw = 2.0f * std::fabs(sx);
    vx = ox - std::fabs(sx);
  }
  if ((vte & 4) && std::fabs(sy) >= 0.5f) {
    vh = 2.0f * std::fabs(sy);
    vy = oy - std::fabs(sy);
  }
  vw = std::min(vw, 16384.0f);
  vh = std::min(vh, 16384.0f);
  vx = std::clamp(vx, -16384.0f, 16383.0f);
  vy = std::clamp(vy, -16384.0f, 16383.0f);
  std::array<float, kSysFloat4Count * 4> sys{};
  sys[0] = sx; sys[1] = ox; sys[2] = sy; sys[3] = oy;
  sys[4] = sz; sys[5] = oz; sys[6] = 2.0f / vw; sys[7] = 2.0f / vh;
  sys[9] = (Reg(device, 0x2302) & 1) == 0 ? 0.5f : 0.0f;  // D3D9 pixel centers
  uint32_t colorcontrol = Reg(device, 0x2202);
  sys[13] = RegF(device, 0x210E);
  static const uint32_t kInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  for (int i = 0; i < 4; ++i) {
    int32_t bias = int32_t(Reg(device, kInfo[i]) << 6) >> 26;
    sys[16 + i] = std::ldexp(1.0f, bias);
  }
  uint32_t point_size = Reg(device, 0x2280);
  sys[20] = vx;
  sys[21] = vy;
  sys[22] = float(point_size >> 16) * 2.0f / 16.0f;
  sys[23] = float(point_size & 0xFFFF) * 2.0f / 16.0f;
  sys[24] = 1.0f / t.scale;  // host pixels -> guest pixels (pixel position parameter)
  for (int i = 0; i < 4; ++i) {
    if (t.color[i] && t.color[i]->guest_format == 3) sys[28 + i] = 1.0f;  // 7e3 range of 2_10_10_10_FLOAT
  }
  std::memcpy(st.xsys, sys.data(), sizeof(st.xsys));
  st.xsys[8] = ((vte >> 8) & 1) | (((vte >> 9) & 1) << 1) | (((vte >> 10) & 1) << 2);
  st.xsys[12] = (colorcontrol & 8) ? (colorcontrol & 7) : 7u;
  st.xsys[14] = kind == Kind::kPoints ? 2u : kind == Kind::kLines ? 4u : 1u;

  // --- Indices, or for rectangles and points the guest indices the vertex shader reads.
  VkDeviceSize index_offset = 0;
  const bool expanded = variant != VsVariant::kNormal;
  if (expanded) {
    const uint32_t bytes = index_count * 4;
    if (vertex_used_ + bytes > kVertexRing) return skip("rectangle indices");
    uint32_t offset = uint32_t(kVertexRing * Get().frame_index) + vertex_used_;
    std::memcpy(vertex_map_ + offset, host_indices.data(), bytes);
    vertex_used_ += bytes;
    st.xsys[25] = offset;
  } else {
    const uint32_t bytes = index_count * 4;
    if (index_used_ + bytes > kIndexRing) return skip("too many indices");
    index_offset = kIndexRing * Get().frame_index + index_used_;
    std::memcpy(index_map_ + index_offset, host_indices.data(), bytes);
    index_used_ += bytes;
  }
  if (!state_valid_ || std::memcmp(&st, &last_state_, sizeof(st)) != 0) {
    state_offset_ = UniformAlloc(&st, sizeof(st), sizeof(st));
    last_state_ = st;
    state_valid_ = true;
  }
  auto upload_consts = [&](std::array<uint8_t, 4096>& raw, uint32_t& raw_count, uint32_t& offset, const uint8_t* src, uint32_t count) {
    if (!count) return;
    const uint32_t bytes = count * 16;
    if (raw_count >= count && std::memcmp(raw.data(), src, bytes) == 0) return;
    offset = UniformAlloc(nullptr, 0, bytes);
    std::memcpy(raw.data(), src, bytes);
    SwapCopy32(reinterpret_cast<uint32_t*>(uniform_map_ + offset), src, count * 4);
    raw_count = count;
  };
  upload_consts(vs_raw_, vs_raw_count_, vs_offset_, d + dev::kVsConstants, vs->info.const_count);
  if (ps) upload_consts(ps_raw_, ps_raw_count_, ps_offset_, d + dev::kPsConstants, ps->info.const_count);

  // --- Textures and samplers.
  SetKey vs_key{}, ps_key{};
  auto gather = [&](const ShaderInfo& info, SetKey& key) {
    uint32_t n = std::min<uint32_t>(uint32_t(info.textures.size()), 16);
    for (uint32_t i = 0; i < n; ++i) {
      const TextureBinding& b = info.textures[i];
      BindingKey memo_key;
      std::memcpy(memo_key.d, &st.xf[b.slot * 6], 24);
      memo_key.binding = uint32_t(b.dim) | (uint32_t(b.mag) << 8) | (uint32_t(b.min) << 16) | (uint32_t(b.mip) << 24);
      memo_key.aniso = b.aniso;
      if (auto memo = binding_memo_.find(memo_key); memo != binding_memo_.end() && !Capturing()) {
        const BindingMemo& m = memo->second;
        key.views[i] = m.srv.view;
        key.layouts[i] = m.srv.layout;
        key.samplers[i] = m.sampler;
        if (b.dim == TexDim::k3D) {
          key.views[16 + i] = m.stacked.view;
          key.layouts[16 + i] = m.stacked.layout;
        }
        continue;
      }
      TexFetch f;
      std::memcpy(f.d, &st.xf[b.slot * 6], 24);
      NormalizeFetch(f);
      View srv = GetTexture(f, b.dim);
      if (b.dim == TexDim::k3D) {
        // 3D-or-stacked fetch: the shader picks the 3D or the 2D-array view at run time.
        View stacked = GetTexture(f, TexDim::k2D);
        key.views[16 + i] = stacked.view ? stacked.view : black_2d_;
        key.layouts[16 + i] = stacked.view ? stacked.layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!srv.view && stacked.view) srv = {black_3d_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true};
      }
      if (!srv.view) {
        srv = {b.dim == TexDim::k3D ? black_3d_ : b.dim == TexDim::kCube ? black_cube_ : black_2d_,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true};
      }
      key.views[i] = srv.view;
      key.layouts[i] = srv.layout;
      key.samplers[i] = f.type() == 2 ? GetSampler(f, b, srv.linear, srv.skip) : srv.linear ? linear_clamp_ : nearest_clamp_;
      binding_memo_[memo_key] = {srv, {key.views[16 + i], key.layouts[16 + i], true, 0}, key.samplers[i]};
    }
  };
  gather(vs->info, vs_key);
  if (ps) gather(ps->info, ps_key);

  // --- Output merger.
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = vs_module;
  key.ps = ps ? ps->module : VK_NULL_HANDLE;
  uint32_t ps_writes = ps ? ps->info.color_written : 0;
  uint32_t color_mask = Reg(device, 0x2104);
  key.a2c = (colorcontrol >> 4) & 1;
  bool const_color = false, const_alpha = false;
  static const uint32_t kBlendRegs[4] = {0x2201, 0x2209, 0x220A, 0x220B};
  for (uint32_t i = 0; i < 4; ++i) {
    auto& rt = key.blend[i];
    rt.src = rt.src_a = VK_BLEND_FACTOR_ONE;
    rt.dst = rt.dst_a = VK_BLEND_FACTOR_ZERO;
    rt.op = rt.op_a = VK_BLEND_OP_ADD;
    if (!t.color[i]) continue;
    key.color[i] = t.gamma[i] && t.color[i]->view_gamma ? VK_FORMAT_R8G8B8A8_SRGB : t.color[i]->format;
    uint32_t mask = (color_mask >> (4 * i)) & 0xF;
    if (!(ps_writes & (1u << i))) mask = 0;
    rt.mask = uint8_t(mask);
    uint32_t bc = Reg(device, kBlendRegs[i]);
    uint32_t cs = bc & 31, cop = (bc >> 5) & 7, cd = (bc >> 8) & 31;
    uint32_t as = (bc >> 16) & 31, aop = (bc >> 21) & 7, ad = (bc >> 24) & 31;
    bool off = cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0;
    uint32_t gf = t.color[i]->guest_format;
    if (off || gf == 14 || gf == 15 || !t.color[i]->blendable) continue;
    rt.enable = 1;
    rt.src = BlendFactor(cs, false);
    rt.dst = BlendFactor(cd, false);
    rt.op = BlendOp(cop);
    rt.src_a = BlendFactor(as, true);
    rt.dst_a = BlendFactor(ad, true);
    rt.op_a = BlendOp(aop);
    for (uint32_t f : {cs, cd, as, ad}) {
      const_color |= f == 12 || f == 13;
      const_alpha |= f == 14 || f == 15;
    }
  }
  float blend_factor[4] = {RegF(device, 0x2105), RegF(device, 0x2106), RegF(device, 0x2107), RegF(device, 0x2108)};
  if (const_alpha && !const_color) blend_factor[0] = blend_factor[1] = blend_factor[2] = blend_factor[3];

  uint32_t dc = Reg(device, 0x2200);
  uint32_t srm = Reg(device, 0x210D);
  key.depth_op = VK_COMPARE_OP_ALWAYS;
  key.f_op = key.b_op = VK_COMPARE_OP_ALWAYS;
  if (t.depth) {
    key.depth = t.depth->format;
    key.depth_test = (dc >> 1) & 1;
    key.depth_write = key.depth_test && ((dc >> 2) & 1);
    key.depth_op = CompareOp(dc >> 4);
    key.stencil = dc & 1;
    key.f_op = CompareOp(dc >> 8);
    key.f_fail = StencilOp(dc >> 11);
    key.f_pass = StencilOp(dc >> 14);
    key.f_zfail = StencilOp(dc >> 17);
    if ((dc >> 7) & 1) {
      key.b_op = CompareOp(dc >> 20);
      key.b_fail = StencilOp(dc >> 23);
      key.b_pass = StencilOp(dc >> 26);
      key.b_zfail = StencilOp(dc >> 29);
    } else {
      key.b_op = key.f_op;
      key.b_fail = key.f_fail;
      key.b_pass = key.f_pass;
      key.b_zfail = key.f_zfail;
    }
  }

  // --- Rasterizer.
  key.cull = VK_CULL_MODE_NONE;
  if (kind == Kind::kTris) {
    if (mode & 1) key.cull = VK_CULL_MODE_FRONT_BIT;
    else if (mode & 2) key.cull = VK_CULL_MODE_BACK_BIT;
    if (((mode >> 3) & 3) && ((mode >> 5) & 7) == 1 && Get().features.fillModeNonSolid) key.fill = 1;
  }
  key.front = (mode >> 2) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  float bias_constant = 0, bias_slope = 0;
  if (mode & (3u << 11)) {
    bool front = mode & (1u << 11);
    float scale = RegF(device, front ? 0x2380 : 0x2382);
    float offset = RegF(device, front ? 0x2381 : 0x2383);
    bool float24 = (Reg(device, 0x2002) >> 16) & 1;
    bias_constant = std::clamp(offset * (float24 ? 16777216.0f : 16777215.0f), -2.0e9f, 2.0e9f);
    bias_slope = scale / 16.0f;
    key.bias = 1;
  }
  key.clamp = ((Reg(device, 0x2204) >> 16) & 1) && Get().features.depthClamp;
  key.topology = native_strip ? (prim == 0x06 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
                 : kind == Kind::kLines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  key.restart = native_strip;

  // Scissor: screen scissor, the device's scissor rect when enabled, and the target.
  uint32_t tl = Reg(device, 0x200E), br = Reg(device, 0x200F);
  int32_t x0 = int32_t(tl & 0x7FFF), y0 = int32_t((tl >> 16) & 0x7FFF);
  int32_t x1 = int32_t(br & 0x7FFF), y1 = int32_t((br >> 16) & 0x7FFF);
  if (x1 <= x0 || y1 <= y0) {
    x0 = y0 = 0;
    x1 = y1 = 8192;
  }
  if (LoadV(device + 11952)) {
    int32_t sx0 = int32_t(LoadV(device + 12832)), sy0 = int32_t(LoadV(device + 12836));
    int32_t sx1 = int32_t(LoadV(device + 12840)), sy1 = int32_t(LoadV(device + 12844));
    if (sx1 > sx0 && sy1 > sy0 && sx1 <= 8192 && sy1 <= 8192) {
      x0 = std::max(x0, sx0);
      y0 = std::max(y0, sy0);
      x1 = std::min(x1, sx1);
      y1 = std::min(y1, sy1);
    }
  }
  x1 = std::min(x1, int32_t(t.width));
  y1 = std::min(y1, int32_t(t.height));
  if (x1 <= x0 || y1 <= y0) return;

  VkPipeline pipeline = GetPipeline(key);
  if (!pipeline) return skip("pipeline");

  if (Capturing()) {
    REXLOG_INFO("capture: draw {} prim {} n {} vs {:016X} ps {:016X} rt {}x{} c0 {} fmt {} d {} vte {:X} vp {} {} {} {} sc {} {} {} {} mode {:X} dc {:X} cmask {:X} pswr {:X} blend0 {}.{}.{} en {} cc {:X} af {} cull {} front {} up {}",
                frame_draws_, prim, index_count, vs->key, ps ? ps->key : 0, t.width, t.height, t.color[0] ? 1 : 0,
                t.color[0] ? t.color[0]->guest_format : 99, t.depth ? 1 : 0, vte, vx, vy, vw, vh, x0, y0, x1, y1, mode, dc, color_mask,
                ps_writes, key.blend[0].src, key.blend[0].dst, key.blend[0].op, key.blend[0].enable, colorcontrol, st.xsys[12],
                int(key.cull), int(key.front), up != nullptr);
    if (ps) {
      for (size_t i = 0; i < ps->info.textures.size() && i < 16; ++i) {
        const TextureBinding& b = ps->info.textures[i];
        TexFetch f;
        std::memcpy(f.d, &st.xf[b.slot * 6], 24);
        NormalizeFetch(f);
        for (auto& [tk, tex] : textures_) {
          if (tex.view == ps_key.views[i] && std::find(capture_textures_.begin(), capture_textures_.end(), &tex) == capture_textures_.end()) capture_textures_.push_back(&tex);
        }
        REXLOG_INFO("capture:   t{} slot {} dim {} -> type {} base {:08X} fmt {} {}x{} swz {:03X} layout {} black {}", i, b.slot, int(b.dim), f.type(),
                    f.base_address(), f.format(), f.width(), f.height(), f.swizzle(), int(ps_key.layouts[i]),
                    ps_key.views[i] == black_2d_ || ps_key.views[i] == black_3d_ || ps_key.views[i] == black_cube_);
      }
      for (int i = 0; i < 4; ++i) {
        const uint8_t* cr = d + dev::kPsConstants + i * 16;
        REXLOG_INFO("capture:   psc{} {} {} {} {}", i, AsFloat(Be32(cr)), AsFloat(Be32(cr + 4)), AsFloat(Be32(cr + 8)), AsFloat(Be32(cr + 12)));
      }
    }
    const uint8_t* cr = d + dev::kVsConstants;
    REXLOG_INFO("capture:   vsc0 {} {} {} {}", AsFloat(Be32(cr)), AsFloat(Be32(cr + 4)), AsFloat(Be32(cr + 8)), AsFloat(Be32(cr + 12)));
    REXLOG_INFO("capture:   sys {} {} {} {} | {} {} {} {} | vte {} | {} {} | ind {} {} {} {}", sys[0], sys[1], sys[2], sys[3], sys[4], sys[5], sys[6], sys[7], st.xsys[8], sys[9], sys[13],
                host_indices[0], host_indices.size() > 1 ? host_indices[1] : 0, host_indices.size() > 2 ? host_indices[2] : 0, host_indices.size() > 3 ? host_indices[3] : 0);
    for (uint32_t k = 0; k < 96; ++k) {
      if (!((vs->info.vfetch_used[k >> 6] >> (k & 63)) & 1)) continue;
      uint32_t a = st.xf[k * 2] & ~3u;
      const uint8_t* v = vertex_map_ + a;
      REXLOG_INFO("capture:   fetch {} at {:08X} len {} words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", k, a, (st.xf[k * 2 + 1] >> 2) & 0xFFFFFF, Be32(v), Be32(v + 4), Be32(v + 8), Be32(v + 12), Be32(v + 16), Be32(v + 20));
    }
  }
  // --- Record.
  BeginPass(t);
  ++pass_draws_;
  VkCommandBuffer cmd = Cmd();
  if (pipeline != bound_pipeline_) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline_ = pipeline;
  }
  VkDescriptorSet set = last_set_;
  if (!set || !(vs_key == last_vs_) || !(ps_key == last_ps_)) {
    set = AllocSet(set_layout_, Get().frame_index);
    last_set_ = set;
    last_vs_ = vs_key;
    last_ps_ = ps_key;
    std::array<VkDescriptorBufferInfo, 5> buffers{};
    buffers[0] = {uniform_, 0, 4096};
    buffers[1] = {uniform_, 0, sizeof(XState)};
    buffers[2] = {uniform_, 0, 4096};
    buffers[3] = {uniform_, 0, sizeof(XState)};
    buffers[4] = {vertex_, 0, vertex_size_};
    static thread_local std::vector<VkWriteDescriptorSet> writes;
    static thread_local std::vector<VkDescriptorImageInfo> images;
    writes.clear();
    images.clear();
    images.reserve(128);
    for (uint32_t i = 0; i < 5; ++i) {
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = i < 4 ? i : vk::kBindSsbo;
      w.descriptorCount = 1;
      w.descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w.pBufferInfo = &buffers[i];
      writes.push_back(w);
    }
    auto add_stage = [&](const SetKey& k, uint32_t tex_base, uint32_t smp_base) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!k.views[i]) continue;
        images.push_back({VK_NULL_HANDLE, k.views[i], k.layouts[i]});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = tex_base + 1 + i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &images.back();
        writes.push_back(w);
      }
      for (uint32_t i = 0; i < 16; ++i) {
        if (!k.samplers[i]) continue;
        images.push_back({k.samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = smp_base + i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &images.back();
        writes.push_back(w);
      }
    };
    add_stage(vs_key, vk::kBindVsTex, vk::kBindVsSmp);
    add_stage(ps_key, vk::kBindPsTex, vk::kBindPsSmp);
    vkUpdateDescriptorSets(dev(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    bound_set_ = VK_NULL_HANDLE;
  }
  const uint32_t dyn[4] = {vs_offset_, state_offset_, ps_offset_, state_offset_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 4, dyn);

  const float scale = t.scale;
  const auto& lim = Get().props.limits;
  float px = vx * scale, py = vy * scale, pw = vw * scale, ph = vh * scale;
  pw = std::min(pw, float(lim.maxViewportDimensions[0]));
  ph = std::min(ph, float(lim.maxViewportDimensions[1]));
  px = std::clamp(px, lim.viewportBoundsRange[0], lim.viewportBoundsRange[1] - pw);
  py = std::clamp(py, lim.viewportBoundsRange[0], lim.viewportBoundsRange[1] - ph);
  // negative height: D3D's y up clip space, and its winding
  VkViewport viewport{px, py + ph, pw, -ph, 0.0f, 1.0f};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{{int32_t(std::lround(float(x0) * scale)), int32_t(std::lround(float(y0) * scale))},
                   {uint32_t(std::lround(float(x1 - x0) * scale)), uint32_t(std::lround(float(y1 - y0) * scale))}};
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdSetBlendConstants(cmd, blend_factor);
  vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, srm & 0xFF);
  vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (srm >> 8) & 0xFF);
  vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (srm >> 16) & 0xFF);
  vkCmdSetDepthBias(cmd, bias_constant, 0.0f, bias_slope);

  if (expanded) {
    const uint32_t per = variant == VsVariant::kRect ? 3 : 1;
    vkCmdDraw(cmd, index_count / per * 6, 1, 0, 0);
  } else {
    vkCmdBindIndexBuffer(cmd, index_, index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
  }
}

// --- Clear -------------------------------------------------------------------------------------

void Renderer::Clear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  Targets t;
  if (!CurrentTargets(device, t)) return;
  VkClearValue c{};
  c.color.float32[0] = float((color >> 16) & 0xFF) / 255.0f;
  c.color.float32[1] = float((color >> 8) & 0xFF) / 255.0f;
  c.color.float32[2] = float(color & 0xFF) / 255.0f;
  c.color.float32[3] = float(color >> 24) / 255.0f;
  VkClearValue dsv{};
  dsv.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
  const bool whole = !count || !rects;
  if (whole) {
    // Nothing recorded on it in this pass: it becomes the next pass's load op.
    bool open_on = false;
    if (pass_open_) {
      for (uint32_t i = 0; i < 4; ++i) open_on |= t.color[i] && (t.color[i]->view == pass_views_[i] || t.color[i]->view_gamma == pass_views_[i]);
      open_on |= t.depth && t.depth->view == pass_views_[4];
    }
    if (open_on) EndPass();
    for (uint32_t i = 0; i < 4; ++i) {
      if (t.color[i] && (flags & (1u << i))) {
        t.color[i]->clear_color = true;
        t.color[i]->clear = c;
      }
    }
    if (t.depth && (flags & 0x30)) {
      // a waiting clear of the other aspect keeps its value
      if (flags & 0x10) {
        t.depth->clear_depth = true;
        t.depth->clear.depthStencil.depth = dsv.depthStencil.depth;
      }
      if (flags & 0x20) {
        t.depth->clear_stencil = true;
        t.depth->clear.depthStencil.stencil = dsv.depthStencil.stencil;
      }
    }
    return;
  }
  BeginPass(t);
  std::vector<VkClearAttachment> attachments;
  for (uint32_t i = 0; i < 4; ++i) {
    if (t.color[i] && (flags & (1u << i))) attachments.push_back({VK_IMAGE_ASPECT_COLOR_BIT, i, c});
  }
  if (t.depth && (flags & 0x30)) {
    VkImageAspectFlags a = ((flags & 0x10) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 0x20) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    attachments.push_back({a, 0, dsv});
  }
  if (attachments.empty()) return;
  for (uint32_t i = 0; i < std::min(count, 64u); ++i) {
    uint32_t r = rects + i * 16;
    int32_t left = std::max(int32_t(LoadV(r)), 0), top = std::max(int32_t(LoadV(r + 4)), 0);
    int32_t right = std::min(int32_t(LoadV(r + 8)), int32_t(t.width)), bottom = std::min(int32_t(LoadV(r + 12)), int32_t(t.height));
    if (right <= left || bottom <= top) continue;
    VkClearRect rect{};
    rect.rect = {{int32_t(std::lround(float(left) * t.scale)), int32_t(std::lround(float(top) * t.scale))},
                 {uint32_t(std::lround(float(right - left) * t.scale)), uint32_t(std::lround(float(bottom - top) * t.scale))}};
    rect.layerCount = 1;
    vkCmdClearAttachments(Cmd(), uint32_t(attachments.size()), attachments.data(), 1, &rect);
  }
}

// --- Resolve -----------------------------------------------------------------------------------

void Renderer::Resolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture, uint32_t dest_point,
                       uint32_t level, uint32_t slice, uint32_t clear_color, float clear_z, uint32_t clear_stencil) {
  Targets t;
  if (!CurrentTargets(device, t)) return;
  uint32_t source = flags & 7;
  HostRT* src = source == 4 ? t.depth : (source < 4 ? t.color[source] : nullptr);
  EndPass();
  binding_memo_.clear();  // resolve destinations may now back the fetches

  if (dest_texture && level == 0) {
    TexFetch f = ReadFetch(dest_texture + 16);
    if (f.base_address()) {
      if (Resolved* r = GetResolved(f, t.scale)) {
        int32_t sx0 = 0, sy0 = 0, sx1 = int32_t(t.width), sy1 = int32_t(t.height);
        if (src_rect) {
          sx0 = int32_t(LoadV(src_rect));
          sy0 = int32_t(LoadV(src_rect + 4));
          sx1 = int32_t(LoadV(src_rect + 8));
          sy1 = int32_t(LoadV(src_rect + 12));
        }
        int32_t dx = sx0, dy = sy0;
        if (dest_point) {
          dx = int32_t(LoadV(dest_point));
          dy = int32_t(LoadV(dest_point + 4));
        } else if (!src_rect) {
          dx = dy = 0;
        }
        int32_t w = std::min(sx1 - sx0, int32_t(r->width) - dx);
        int32_t h = std::min(sy1 - sy0, int32_t(r->height) - dy);
        uint32_t s = std::min(slice, r->slices - 1);
        if (src && w > 0 && h > 0) {
          FlushClears(src);
          VkCommandBuffer cmd = Cmd();
          VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          ca.imageView = r->rtvs[s];
          ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          const float sc = t.scale;
          auto coord = [sc](int32_t v) { return int32_t(std::lround(float(v) * sc)); };
          VkRect2D area{{coord(dx), coord(dy)}, {uint32_t(coord(dx + w) - coord(dx)), uint32_t(coord(dy + h) - coord(dy))}};
          VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
          ri.renderArea = area;
          ri.layerCount = 1;
          ri.colorAttachmentCount = 1;
          ri.pColorAttachments = &ca;
          vkCmdBeginRendering(cmd, &ri);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, UtilPipeline(util_copy_, r->format));
          VkDescriptorSet set = AllocSet(util_set_layout_, Get().frame_index);
          VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, src->srv, VK_IMAGE_LAYOUT_GENERAL},
                                         {linear_clamp_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
          VkWriteDescriptorSet wr[2];
          for (uint32_t i = 0; i < 2; ++i) {
            wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr[i].dstSet = set;
            wr[i].dstBinding = i ? vk::kBindPsSmp : vk::kBindPsTex;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            wr[i].pImageInfo = &ii[i];
          }
          vkUpdateDescriptorSets(dev(), 2, wr, 0, nullptr);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, util_layout_, 0, 1, &set, 0, nullptr);
          // The resolve stores 8_8_8_8 in memory as BGRA: games sample it with a swizzle that swaps red and blue back.
          bool swap = source < 4 && src->guest_format <= 1 && (f.format() == 6 || f.format() == 62);
          float push[8] = {float(coord(sx0) - coord(dx)), float(coord(sy0) - coord(dy)), swap ? 1.0f : 0.0f, 1.0f, 0, 0, 0, 0};
          vkCmdPushConstants(cmd, util_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
          VkViewport vp{float(area.offset.x), float(area.offset.y) + float(area.extent.height), float(area.extent.width),
                        -float(area.extent.height), 0.0f, 1.0f};
          vkCmdSetViewport(cmd, 0, 1, &vp);
          vkCmdSetScissor(cmd, 0, 1, &area);
          vkCmdDraw(cmd, 3, 1, 0, 0);
          vkCmdEndRendering(cmd);
          AllBarrier(cmd);
          r->bgra = swap;
          r->used_width = uint32_t(dx + w);
          r->used_height = uint32_t(dy + h);
        }
        r->signature = GuestSignature(f);
        r->checked_frame = frame_;
      }
    }
  }

  // Optional clears of the source surfaces after the copy.
  if ((flags & 0x100) && clear_color) {
    HostRT* rt = source < 4 ? t.color[source] : t.color[0];
    if (rt) {
      for (int i = 0; i < 4; ++i) rt->clear.color.float32[i] = AsFloat(LoadV(clear_color + i * 4));
      rt->clear_color = true;
    }
  }
  if ((flags & 0x200) && t.depth) {
    t.depth->clear.depthStencil = {std::clamp(clear_z, 0.0f, 1.0f), clear_stencil & 0xFF};
    t.depth->clear_depth = t.depth->clear_stencil = true;
  }
}

// --- Swap --------------------------------------------------------------------------------------

void Renderer::Present(const PresentJob& job) {
  auto& c = Get();
  VkCommandBuffer cmd = c.frames[job.slot].cmd;
  uint32_t image = 0;
  if (!c.Acquire(job.slot, image)) {
    c.Submit(job.slot, false, 0);
    return;
  }
  ImageBarrier(cmd, c.swap_images[image], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = c.swap_views[image];
  ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, c.swap_extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cmd, &ri);
  if (job.view) {
    const uint32_t gw = job.used_w, gh = job.used_h;
    float sw = float(c.swap_extent.width), sh = float(c.swap_extent.height);
    float aspect = float(gw) / float(gh);
    float dw = sw, dh = sw / aspect;
    if (dh > sh) dh = sh, dw = sh * aspect;
    float x0 = std::floor((sw - dw) / 2), y0 = std::floor((sh - dh) / 2);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
    VkDescriptorSet set = AllocSet(util_set_layout_, job.slot);
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, job.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {job.linear ? linear_clamp_ : nearest_clamp_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet wr[2];
    for (uint32_t i = 0; i < 2; ++i) {
      wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr[i].dstSet = set;
      wr[i].dstBinding = i ? vk::kBindPsSmp : vk::kBindPsTex;
      wr[i].descriptorCount = 1;
      wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev(), 2, wr, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, util_layout_, 0, 1, &set, 0, nullptr);
    float push[8] = {0, 0, 0, 0, float(gw) / float(job.tex_w), float(gh) / float(job.tex_h), job.bgra ? 1.0f : 0.0f, 0};
    vkCmdPushConstants(cmd, util_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    VkViewport vp{x0, y0 + dh, dw, -dh, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, c.swap_extent};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  vkCmdEndRendering(cmd);
  ImageBarrier(cmd, c.swap_images[image], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  c.Submit(job.slot, true, image);
}

void Renderer::CaptureCopies(VkCommandBuffer cmd) {
  auto add = [&](const std::string& name, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageAspectFlags aspect,
                 VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) {
    CaptureCopy c;
    c.name = name + "_" + std::to_string(w) + "x" + std::to_string(h) + "_f" + std::to_string(int(format));
    c.size = VkDeviceSize(w) * h * 16;
    if (!CreateBuffer(c.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, c.buffer, c.memory, c.map)) return;
    ImageBarrier(cmd, image, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {aspect, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.buffer, 1, &copy);
    ImageBarrier(cmd, image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    capture_copies_.push_back(c);
  };
  char name[48];
  for (auto& [key, rt] : rts_) {
    if (rt.depth) continue;
    snprintf(name, sizeof(name), "rt%03X_g%u", uint32_t(key & 0xFFF), rt.guest_format);
    add(name, rt.image, rt.format, rt.host_width, rt.host_height, VK_IMAGE_ASPECT_COLOR_BIT);
  }
  int tex_index = 0;
  for (HostTex* tex : capture_textures_) {
    snprintf(name, sizeof(name), "tex%02d_%08X_g%u", tex_index++, tex->fetch.base_address(), tex->fetch.format());
    add(name, tex->image, tex->format, tex->fetch.width(), tex->fetch.height(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  capture_textures_.clear();
  for (auto& [base, res] : resolved_) {
    if (res.slices != 1 || res.cube) continue;
    snprintf(name, sizeof(name), "res%08X_g%u", base, res.guest_format);
    add(name, res.image, res.format, res.host_width, res.host_height, VK_IMAGE_ASPECT_COLOR_BIT);
  }
}

void Renderer::CaptureWrite() {
  Get().WaitIdle();
  for (CaptureCopy& c : capture_copies_) {
    std::ofstream(g_user_dir / ("capture_" + c.name + ".bin"), std::ios::binary).write(reinterpret_cast<const char*>(c.map), std::streamsize(c.size));
    vkDestroyBuffer(dev(), c.buffer, nullptr);
    vkFreeMemory(dev(), c.memory, nullptr);
  }
  REXLOG_INFO("capture: wrote {} images", capture_copies_.size());
  capture_copies_.clear();
}

void Renderer::Swap(uint32_t device, uint32_t front_texture) {
  (void)device;
  RefreshTextures();
  LateSyncVertices();
  EndPass();
  if (frame_ % 30 == 0) EvictTextures();
  Resolved* r = nullptr;
  if (front_texture) {
    TexFetch f = ReadFetch(front_texture + 16);
    last_front_base_ = f.base_address() & 0x1FFFFFFFu;
  }
  auto it = resolved_.find(last_front_base_);
  if (it != resolved_.end()) r = &it->second;
  if (Capturing()) CaptureCopies(Cmd());
  PresentJob job;
  job.slot = Get().frame_index;
  if (r) {
    job.view = r->srv0;
    job.used_w = r->used_width ? r->used_width : r->width;
    job.used_h = r->used_height ? r->used_height : r->height;
    job.tex_w = r->width;
    job.tex_h = r->height;
    job.bgra = r->bgra;
    job.linear = r->linear;
  }
  const bool capturing = Capturing();
  QueuePresent(job);
  Get().Advance();
  if (capturing) {
    WaitAllPresented();
    CaptureWrite();
  }
  static auto last_log = std::chrono::steady_clock::now();
  static uint32_t frames_at = 0;
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - last_log).count();
  if (frame_ <= 3 || seconds >= 5.0) {
    long rss_pages = 0;
    if (std::ifstream statm("/proc/self/statm"); statm) {
      long size;
      statm >> size >> rss_pages;
    }
    const uint32_t frames_done = std::max<uint32_t>(frame_ - frames_at, 1);
    REXLOG_INFO("bf2native: frame {}: {:.1f} fps, {:.0f} ms/frame of it waiting on the GPU, {} draws ({} skipped), {} passes, {} textures {} MB ({} evicted), {} pipelines, ring {} MB idx {} MB ubo {} MB arena {} MB, rss {} MB",
                frame_, seconds > 0.01 ? double(frame_ - frames_at) / seconds : 0.0, wait_ms_ / frames_done, frame_draws_, frame_skipped_, frame_passes_,
                textures_.size(), texture_bytes_ >> 20, evicted_, pipelines_.size(), vertex_used_ >> 20, index_used_ >> 20, uniform_used_ >> 20,
                arena_used_ >> 20, rss_pages * 4096 >> 20);
    last_log = now;
    frames_at = frame_;
    wait_ms_ = 0;
  }
  frame_draws_ = frame_skipped_ = frame_passes_ = 0;
  if (frame_ % 600 == 0) SavePipelineCache();
  ++frame_;
  BeginFrame();
}

}  // namespace

// --- Entry points ------------------------------------------------------------------------------

void VulkanRendererSetup(SDL_Window* window, const std::filesystem::path& user_dir) {
  g_window = window;
  g_user_dir = user_dir;
}

bool RendererInit(void*) {
  if (g_renderer) return true;
  auto r = std::make_unique<Renderer>();
  if (!r->Init()) return false;
  g_renderer = r.release();
  return true;
}

void RendererDraw(uint32_t device, uint32_t prim, int32_t base_vertex, uint32_t start, uint32_t count, bool indexed) {
  if (!g_renderer || !count) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  static std::vector<uint32_t> vids;
  vids.resize(count);
  if (indexed) {
    uint32_t ib = LoadV(device + dev::kIndexBuffer);
    if (!ib) return;
    bool is32 = LoadV(ib) >> 31;
    uint32_t address = LoadV(ib + 0xC);
    if (!address) return;
    uint32_t mode = Reg(device, 0x2205);
    bool restart = (mode >> 21) & 1;
    const uint32_t mask = is32 ? 0xFFFFFFu : 0xFFFFu;
    uint32_t reset = Reg(device, 0x2103) & mask;
    const uint8_t* src = Virt(address + start * (is32 ? 4 : 2));
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = is32 ? Be32(src + i * 4) : Be16(src + i * 2);
      vids[i] = restart && (v & mask) == reset ? UINT32_MAX : uint32_t((v + uint32_t(base_vertex)) & 0xFFFFFF);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) vids[i] = uint32_t(base_vertex) + i;
  }
  g_renderer->Draw(device, prim, vids, nullptr);
}

void RendererDrawUp(uint32_t device, const UpDraw& up) {
  if (!g_renderer || !up.vertices || !up.stride) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  static std::vector<uint32_t> vids;
  if (up.indexed) {
    if (!up.indices || !up.index_count) return;
    vids.resize(up.index_count);
    const uint8_t* p = Virt(up.indices);
    for (uint32_t i = 0; i < up.index_count; ++i) vids[i] = up.index32 ? Be32(p + i * 4) : Be16(p + i * 2);
  } else {
    if (!up.vertex_count) return;
    vids.resize(up.vertex_count);
    for (uint32_t i = 0; i < up.vertex_count; ++i) vids[i] = i;
  }
  g_renderer->Draw(device, up.prim, vids, &up);
}

void RendererClear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  if (!g_renderer) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  g_renderer->Clear(device, count, rects, flags, color, z, stencil);
}

void RendererResolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture, uint32_t dest_point,
                     uint32_t level, uint32_t slice, uint32_t clear_color, float clear_z, uint32_t clear_stencil) {
  if (!g_renderer) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  g_renderer->Resolve(device, flags, src_rect, dest_texture, dest_point, level, slice, clear_color, clear_z, clear_stencil);
}

void RendererRequestCapture() {}

void RendererSwap(uint32_t device, uint32_t front_texture) {
  if (!g_renderer) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  g_renderer->Swap(device, front_texture);
}

}  // namespace bf2n
