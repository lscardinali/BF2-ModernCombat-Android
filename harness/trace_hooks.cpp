// BF2_TRACE=<seconds>: from that moment on, writes the guest's waits, events and thread resumes to user/trace.txt (the
// guest thread, its return address and the arguments), to read a handshake that stalls.
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/ppc/context.h>

namespace {

using Thunk = void (*)(PPCContext&, uint8_t*);
using Clock = std::chrono::steady_clock;

Thunk Sdk(const char* name) {
  void* runtime = dlopen("librexruntime.so", RTLD_NOW | RTLD_NOLOAD);
  return reinterpret_cast<Thunk>(dlsym(runtime, name));
}

const auto g_start = Clock::now();
const double g_after = [] {
  const char* e = std::getenv("BF2_TRACE");
  return e && *e ? std::atof(e) : -1.0;
}();
FILE* const g_file = g_after >= 0 ? std::fopen("user/trace.txt", "w") : nullptr;

double Now() { return std::chrono::duration<double>(Clock::now() - g_start).count(); }
bool On() { return g_file && Now() >= g_after; }

void Line(const char* what, const PPCContext& ctx, const char* phase, uint32_t result) {
  std::fprintf(g_file, "%9.3f t%d %-5s %-24s lr=%08X r3=%08X r4=%08X r5=%08X -> %08X\n", Now(), int(gettid()), phase, what,
               uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, result);
  std::fflush(g_file);
}

}  // namespace

#define TRACED(name)                                 \
  REX_HOOK_RAW(__imp__##name) {                      \
    static const Thunk sdk = Sdk("__imp__" #name);   \
    if (!On()) {                                     \
      sdk(ctx, base);                                \
      return;                                        \
    }                                                \
    PPCContext saved = ctx;                          \
    Line(#name, saved, "call", 0);                   \
    sdk(ctx, base);                                  \
    Line(#name, saved, "ret", ctx.r3.u32);           \
  }

TRACED(KeWaitForSingleObject)
TRACED(KeWaitForMultipleObjects)
TRACED(KeSetEvent)
TRACED(KeResetEvent)
TRACED(NtSetEvent)
TRACED(NtClearEvent)
TRACED(NtWaitForSingleObjectEx)
TRACED(NtWaitForMultipleObjectsEx)
TRACED(NtSuspendThread)
TRACED(NtResumeThread)

// guest functions of the stall under study: their generated bodies are __imp__sub_*
#define TRACED_SUB(name)                             \
  REX_FUNC(__imp__##name);                           \
  REX_HOOK_RAW(name) {                               \
    if (!On()) {                                     \
      __imp__##name(ctx, base);                      \
      return;                                        \
    }                                                \
    PPCContext saved = ctx;                          \
    Line(#name, saved, "call", 0);                   \
    __imp__##name(ctx, base);                        \
    Line(#name, saved, "ret", ctx.r3.u32);           \
  }

extern "C" {
REX_FUNC(__imp__sub_82391F40);
REX_HOOK_RAW(sub_82391F40) {
  static bool once = false;
  if (g_file && !once) {
    once = true;
    auto load = [base](uint32_t a) {
      uint32_t v;
      std::memcpy(&v, base + a, 4);
      return __builtin_bswap32(v);
    };
    uint32_t object = ctx.r3.u32, vtable = load(object);
    std::fprintf(g_file, "frame queue object %08X vtable %08X\n", object, vtable);
    for (uint32_t i = 0; i < 64; ++i) std::fprintf(g_file, "  vtable+%3u = %08X\n", i * 4, load(vtable + i * 4));
    std::fprintf(g_file, "  +272 = %08X +284 = %08X +44 = %08X\n", load(object + 272), load(object + 284), load(object + 44));
    std::fflush(g_file);
  }
  __imp__sub_82391F40(ctx, base);
}
TRACED_SUB(sub_82391CB0)
TRACED_SUB(sub_82391BD8)
TRACED_SUB(sub_82394DB0)
TRACED_SUB(sub_82398030)
TRACED_SUB(sub_823980C8)
TRACED_SUB(sub_82394E08)
TRACED_SUB(sub_823928C8)
}
