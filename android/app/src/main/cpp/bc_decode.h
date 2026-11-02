#pragma once
// BC1..BC5 blocks to plain texels, for GPUs without BC (most Android ones). BC1-3 become RGBA8, BC4 R8, BC5 RG8.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "native_gpu/textures.h"

namespace bf2n {

inline VkFormat BcDecodedFormat(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_BC4_UNORM: return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_BC4_SNORM: return VK_FORMAT_R8_SNORM;
    case DXGI_FORMAT_BC5_UNORM: return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_BC5_SNORM: return VK_FORMAT_R8G8_SNORM;
    case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;  // half the memory, and all DXT1 can hold
    default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

namespace bc_detail {

// BC4's 3-bit interpolated channel. dst steps by `stride`; four rows of `pitch` bytes.
inline void Channel(const uint8_t* b, bool is_signed, uint8_t* dst, uint32_t stride, uint32_t pitch) {
  int v[8];
  if (is_signed) {
    const int a = int8_t(b[0]), c = int8_t(b[1]);
    v[0] = a;
    v[1] = c;
    if (a > c) {
      for (int i = 0; i < 6; ++i) v[2 + i] = ((6 - i) * a + (1 + i) * c) / 7;
    } else {
      for (int i = 0; i < 4; ++i) v[2 + i] = ((4 - i) * a + (1 + i) * c) / 5;
      v[6] = -127;
      v[7] = 127;
    }
  } else {
    const int a = b[0], c = b[1];
    v[0] = a;
    v[1] = c;
    if (a > c) {
      for (int i = 0; i < 6; ++i) v[2 + i] = ((6 - i) * a + (1 + i) * c) / 7;
    } else {
      for (int i = 0; i < 4; ++i) v[2 + i] = ((4 - i) * a + (1 + i) * c) / 5;
      v[6] = 0;
      v[7] = 255;
    }
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x, bits >>= 3) dst[y * pitch + x * stride] = uint8_t(v[bits & 7]);
}

// BC1's colours. BC2 and BC3 always use the four-colour mode and ignore its alpha.
inline void Color(const uint8_t* b, bool has_alpha_mode, uint8_t* dst, uint32_t pitch) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t p[4][4];
  auto expand = [](uint16_t c, uint8_t* o) {
    o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    o[2] = uint8_t((c & 31) * 255 / 31);
    o[3] = 255;
  };
  expand(c0, p[0]);
  expand(c1, p[1]);
  if (c0 > c1 || !has_alpha_mode) {
    for (int i = 0; i < 3; ++i) {
      p[2][i] = uint8_t((2 * p[0][i] + p[1][i]) / 3);
      p[3][i] = uint8_t((p[0][i] + 2 * p[1][i]) / 3);
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      p[2][i] = uint8_t((p[0][i] + p[1][i]) / 2);
      p[3][i] = 0;
    }
    p[2][3] = 255;
    p[3][3] = 0;
  }
  uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x, bits >>= 2) std::memcpy(dst + y * pitch + x * 4, p[bits & 3], 4);
}

}  // namespace bc_detail

// RGBA8 texels to A1R5G5B5, in place.
inline void PackTo1555(std::vector<uint8_t>& texels) {
  const size_t n = texels.size() / 4;
  auto* out = reinterpret_cast<uint16_t*>(texels.data());
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = texels.data() + i * 4;
    const uint32_t r = p[0], g = p[1], b = p[2], a = p[3];
    out[i] = uint16_t(((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
  }
  texels.resize(n * 2);
}

// Returns the bytes per row of `out`, which holds row_blocks * 4 by rows * 4 texels.
inline uint32_t DecodeBc(DXGI_FORMAT f, const uint8_t* src, uint32_t row_blocks, uint32_t rows, std::vector<uint8_t>& out) {
  using namespace bc_detail;
  const uint32_t texel = f == DXGI_FORMAT_BC4_UNORM || f == DXGI_FORMAT_BC4_SNORM ? 1
                         : f == DXGI_FORMAT_BC5_UNORM || f == DXGI_FORMAT_BC5_SNORM ? 2 : 4;
  const uint32_t block = f == DXGI_FORMAT_BC1_UNORM || texel == 1 ? 8 : 16;
  const uint32_t pitch = row_blocks * 4 * texel;
  out.resize(size_t(pitch) * rows * 4);
  for (uint32_t by = 0; by < rows; ++by) {
    for (uint32_t bx = 0; bx < row_blocks; ++bx) {
      const uint8_t* b = src + (size_t(by) * row_blocks + bx) * block;
      uint8_t* d = out.data() + size_t(by) * 4 * pitch + size_t(bx) * 4 * texel;
      switch (f) {
        case DXGI_FORMAT_BC1_UNORM:
          Color(b, true, d, pitch);
          break;
        case DXGI_FORMAT_BC2_UNORM:
          Color(b + 8, false, d, pitch);
          for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) d[y * pitch + x * 4 + 3] = uint8_t(((b[y * 2 + (x >> 1)] >> ((x & 1) * 4)) & 15) * 17);
          break;
        case DXGI_FORMAT_BC3_UNORM:
          Color(b + 8, false, d, pitch);
          Channel(b, false, d + 3, 4, pitch);
          break;
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
          Channel(b, f == DXGI_FORMAT_BC4_SNORM, d, 1, pitch);
          break;
        default:
          Channel(b, f == DXGI_FORMAT_BC5_SNORM, d, 2, pitch);
          Channel(b + 8, f == DXGI_FORMAT_BC5_SNORM, d + 1, 2, pitch);
          break;
      }
    }
  }
  return pitch;
}

}  // namespace bf2n
