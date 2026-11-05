// Android's performance hints (ADPF, API 33): the game's threads say how much CPU each frame took against the 33.3 ms
// the game runs at, and the scheduler sets clocks and cores from that: up before a frame runs late, down when there is
// room, instead of guessing from load. A frame reports the larger of two threads' CPU time:
//   the game update, sub_82298D20 on the guest's game thread (hooked here)
//   the guest's render thread, from one swap to the next (vk_renderer.cpp)
// The present thread is in the session too. The app runs from API 31: the API links weakly (CMakeLists.txt) and every
// call is behind __builtin_available, so below 33, or where the phone has no hint support, this does nothing.
// user/no_perf_hint turns it off (app_main.cpp sets BF2_NO_PERF_HINT).
#include "generated/default/bf2mc_init.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/performance_hint.h>
#include <time.h>
#include <unistd.h>
#endif

namespace bf2n {

namespace {

#ifdef __ANDROID__
constexpr int64_t kTargetNs = 33'333'333;  // the game's 30 fps

std::mutex g_mutex;
APerformanceHintManager* g_manager = nullptr;
APerformanceHintSession* g_session = nullptr;
std::vector<int32_t> g_tids;
bool g_tids_changed = false;
std::atomic<int64_t> g_update_ns{0};

bool Enabled() {
  static const bool on = [] {
    if (std::getenv("BF2_NO_PERF_HINT")) return false;
    if (__builtin_available(android 33, *)) g_manager = APerformanceHint_getManager();
    REXLOG_INFO("perf hint: {}", g_manager ? "available" : "not on this phone");
    return g_manager != nullptr;
  }();
  return on;
}

int64_t ThreadCpuNs() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}
#endif

}  // namespace

// The calling thread joins the session, once.
void PerfHintThread() {
#ifdef __ANDROID__
  static thread_local bool joined = false;
  if (joined || !Enabled()) return;
  joined = true;
  std::lock_guard lock(g_mutex);
  g_tids.push_back(int32_t(gettid()));
  g_tids_changed = true;
#endif
}

// Render thread, at each swap: reports the frame.
void PerfHintFrame() {
#ifdef __ANDROID__
  if (!Enabled()) return;
  PerfHintThread();
  static thread_local int64_t last_cpu = ThreadCpuNs();
  const int64_t cpu = ThreadCpuNs();
  const int64_t render_ns = cpu - last_cpu;
  last_cpu = cpu;
  const int64_t actual = std::max(render_ns, g_update_ns.exchange(0, std::memory_order_relaxed));
  if (__builtin_available(android 33, *)) {  // Enabled() already means 33, the compiler wants it said here
    std::lock_guard lock(g_mutex);
    if (g_tids_changed) {
      g_tids_changed = false;
      // the game starts a new render thread for a mission: a session with a thread that has ended is refused
      std::erase_if(g_tids, [](int32_t tid) { return access(("/proc/self/task/" + std::to_string(tid)).c_str(), F_OK) != 0; });
      bool moved = false;
      if (__builtin_available(android 34, *)) {
        moved = g_session && APerformanceHint_setThreads(g_session, g_tids.data(), g_tids.size()) == 0;
        if (moved) REXLOG_INFO("perf hint: session now on {} threads", g_tids.size());
      }
      if (!moved) {
        if (g_session) APerformanceHint_closeSession(g_session);
        g_session = APerformanceHint_createSession(g_manager, g_tids.data(), g_tids.size(), kTargetNs);
        REXLOG_INFO("perf hint: session for {} threads {}", g_tids.size(), g_session ? "started" : "refused");
      }
    }
    if (g_session && actual > 0) APerformanceHint_reportActualWorkDuration(g_session, actual);
  }
#endif
}

}  // namespace bf2n

// sub_82298D20(this, f1 = dt, f2): the game update, on the guest's game thread
REX_HOOK_RAW(sub_82298D20) {
#ifdef __ANDROID__
  if (bf2n::Enabled()) {
    bf2n::PerfHintThread();
    const int64_t start = bf2n::ThreadCpuNs();
    __imp__sub_82298D20(ctx, base);
    bf2n::g_update_ns.fetch_add(bf2n::ThreadCpuNs() - start, std::memory_order_relaxed);
    return;
  }
#endif
  __imp__sub_82298D20(ctx, base);
}
