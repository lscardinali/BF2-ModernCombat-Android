// The native_gpu.h entry points with no renderer behind them: counts draws and swaps, logs the rate every 5 seconds.
#include <atomic>
#include <chrono>

#include <rex/logging.h>

#include "native_gpu/native_gpu.h"

namespace bf2n {

namespace {
std::atomic<uint64_t> g_draws{0}, g_resolves{0}, g_clears{0};
uint64_t g_frames = 0, g_frames_at = 0, g_draws_at = 0;
std::chrono::steady_clock::time_point g_last;
}  // namespace

bool RendererInit(void*) {
  REXLOG_INFO("null renderer: started");
  g_last = std::chrono::steady_clock::now();
  return true;
}
void RendererDraw(uint32_t, uint32_t, int32_t, uint32_t, uint32_t, bool) { g_draws.fetch_add(1, std::memory_order_relaxed); }
void RendererDrawUp(uint32_t, const UpDraw&) { g_draws.fetch_add(1, std::memory_order_relaxed); }
void RendererClear(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, float, uint32_t) { g_clears.fetch_add(1, std::memory_order_relaxed); }
void RendererResolve(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, float, uint32_t) {
  g_resolves.fetch_add(1, std::memory_order_relaxed);
}
void RendererSwap(uint32_t, uint32_t) {
  ++g_frames;
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - g_last).count();
  if (seconds >= 5.0) {
    uint64_t draws = g_draws.load();
    REXLOG_INFO("null renderer: frame {}, {:.1f} fps, {} draws a frame, {} resolves, {} clears in total", g_frames,
                double(g_frames - g_frames_at) / seconds,
                g_frames > g_frames_at ? (draws - g_draws_at) / (g_frames - g_frames_at) : 0, g_resolves.load(),
                g_clears.load());
    g_last = now;
    g_frames_at = g_frames;
    g_draws_at = draws;
  }
}
void RendererRequestCapture() {}

}  // namespace bf2n
