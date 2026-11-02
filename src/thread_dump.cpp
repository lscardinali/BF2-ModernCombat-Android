// Stacks of every thread in the process, without root (debuggerd needs it on a release phone): a signal to each thread
// makes it walk its own stack. Recompiled functions show as sub_<guest address> (the executable is linked -rdynamic).
#include "thread_dump.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

const int kSignal = SIGRTMAX - 3;
int g_fd = -1;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;  // one stack at a time
std::atomic<int> g_done{0};

_Unwind_Reason_Code OnFrame(_Unwind_Context* context, void* arg) {
  int& depth = *static_cast<int*>(arg);
  uintptr_t pc = _Unwind_GetIP(context);
  if (!pc || depth >= 24) return _URC_END_OF_STACK;
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname) {
    dprintf(g_fd, "  #%02d %s+0x%zx\n", depth, info.dli_sname, pc - uintptr_t(info.dli_saddr));
  } else {
    dprintf(g_fd, "  #%02d %p %s\n", depth, reinterpret_cast<void*>(pc), info.dli_fname ? info.dli_fname : "?");
  }
  ++depth;
  return _URC_NO_REASON;
}

void OnSignal(int) {
  char name[32] = "?";
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(gettid()));
  int comm = open(path, O_RDONLY);
  if (comm >= 0) {
    ssize_t n = read(comm, name, sizeof(name) - 1);
    if (n > 0) name[n - 1] = 0;
    close(comm);
  }
  while (g_lock.test_and_set(std::memory_order_acquire)) {
  }
  dprintf(g_fd, "thread %d \"%s\"\n", int(gettid()), name);
  int depth = 0;
  _Unwind_Backtrace(OnFrame, &depth);
  g_lock.clear(std::memory_order_release);
  g_done.fetch_add(1);
}

void DumpOnce() {
  g_done = 0;
  int sent = 0;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return;
  while (dirent* e = readdir(dir)) {
    int tid = atoi(e->d_name);
    if (tid && tid != gettid() && syscall(SYS_tgkill, getpid(), tid, kSignal) == 0) ++sent;
  }
  closedir(dir);
  for (int i = 0; i < 200 && g_done.load() < sent; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  dprintf(g_fd, "--- %d of %d threads answered\n", g_done.load(), sent);
}

}  // namespace

void ThreadDumpAfter(const char* path, int seconds, int repeat) {
  g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (g_fd < 0) return;
  struct sigaction action{};
  action.sa_handler = OnSignal;
  action.sa_flags = SA_RESTART | SA_ONSTACK;
  sigaction(kSignal, &action, nullptr);
  std::thread([seconds, repeat] {
    for (int i = 0; i < repeat; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      dprintf(g_fd, "=== dump %d\n", i);
      DumpOnce();
    }
  }).detach();
}
