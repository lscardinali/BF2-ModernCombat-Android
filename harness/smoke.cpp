// What the SDK needs from the device, before there is a game to run: the guest address space, write faults on guest
// pages (the renderer's write watch), fibers, and the x86 semantics of the SIMDe calls the generated code uses.
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include <simde/x86/sse4.1.h>

#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread/fiber.h>

namespace {

int g_failed = 0;

void Check(const char* name, bool ok) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  std::fflush(stdout);
  if (!ok) ++g_failed;
}

// --- memory ---

std::atomic<int> g_callbacks{0};

std::pair<uint32_t, uint32_t> OnWrite(void*, uint32_t start, uint32_t length, bool) {
  g_callbacks.fetch_add(1);
  return {start & ~0xFFFu, 0x1000u};
}

void TestMemory() {
  rex::memory::Memory memory;
  bool ok = memory.Initialize();
  Check("Memory::Initialize maps the guest address space", ok);
  if (!ok) return;
  std::printf("     virtual base %p, physical base %p\n", memory.virtual_membase(), memory.physical_membase());

  uint32_t guest = memory.SystemHeapAlloc(0x20000, 0x1000, rex::memory::kSystemHeapPhysical);
  Check("physical heap allocation", guest != 0);
  if (!guest) return;
  std::printf("     guest address 0x%08X\n", guest);

  // as on the console, 0xE0000000 + x is physical x + 0x1000
  uint32_t physical = memory.GetPhysicalAddress(guest);
  std::printf("     physical address 0x%08X\n", physical);
  Check("0xE0000000 heap's physical address is offset by 0x1000", physical == (guest & 0x1FFFFFFF) + 0x1000);
  auto* p_a = memory.TranslateVirtual<uint32_t*>(0xA0000000u | physical);
  auto* p_c = memory.TranslateVirtual<uint32_t*>(0xC0000000u | physical);
  auto* p_e = memory.TranslateVirtual<uint32_t*>(guest);
  auto* p_phys = memory.TranslatePhysical<uint32_t*>(physical);
  *p_a = 0x12345678;
  Check("0xA0000000 view aliases the physical view", *p_phys == 0x12345678);
  *p_phys = 0x9ABCDEF0;
  Check("0xC0000000 view aliases the physical view", *p_c == 0x9ABCDEF0);
  Check("0xE0000000 view aliases the physical view", *p_e == 0x9ABCDEF0);
  // what the generated code does for a guest address in 0xE0000000: PPC_LOAD/PPC_STORE's own translation
  auto* p_inline = reinterpret_cast<uint32_t*>(
      memory.virtual_membase() + guest + rex::memory::detail::PhysicalHostOffset(guest));
  Check("generated code's 0xE0000000 translation agrees with the heap's", p_inline == p_e);

  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(&OnWrite, nullptr);
  Check("invalidation callback registered", handle != nullptr);
  memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
  // watches are armed on the view the page was committed through, the one the game writes through
  *p_e = 0x55AA55AA;  // faults, the handler calls OnWrite and unprotects, the store is retried
  std::printf("     %d callbacks after the first write\n", g_callbacks.load());
  Check("write to a watched page faults into the callback", g_callbacks.load() == 1);
  Check("the faulting store landed", *p_phys == 0x55AA55AA);
  *p_e = 0xAA55AA55;
  Check("second write to the page does not fault", g_callbacks.load() == 1);
  // the watch from another thread: guest threads write while the render thread arms
  memory.EnablePhysicalMemoryAccessCallbacks(physical + 0x1000, 0x1000, true, false);
  auto* p_next = memory.TranslateVirtual<uint32_t*>(guest + 0x1000);
  std::thread([&] { *p_next = 7; }).join();
  Check("write from another thread faults into the callback", g_callbacks.load() == 2 && *p_next == 7);
  // the same store as generated code emits it through a base and an index register, reversed by rev
  memory.EnablePhysicalMemoryAccessCallbacks(physical + 0x2000, 0x1000, true, false);
  auto* p_third = memory.TranslateVirtual<uint8_t*>(guest + 0x2000);
  uint32_t swapped = __builtin_bswap32(0x11223344);
  asm volatile("str %w0, [%1, %2]" : : "r"(swapped), "r"(p_third), "r"(uint64_t(4)) : "memory");
  Check("register-indexed store faults and lands",
        g_callbacks.load() == 3 && *reinterpret_cast<uint32_t*>(p_third + 4) == swapped);
  // a vector store, as the VMX128 stvx code emits it
  memory.EnablePhysicalMemoryAccessCallbacks(physical + 0x3000, 0x1000, true, false);
  auto* p_fourth = memory.TranslateVirtual<uint8_t*>(guest + 0x3000);
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(p_fourth), simde_mm_set1_epi32(0x01020304));
  Check("128-bit store faults and lands",
        g_callbacks.load() == 4 && *reinterpret_cast<uint32_t*>(p_fourth + 12) == 0x01020304);
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
}

// --- fibers ---

rex::thread::Fiber* g_main;
rex::thread::Fiber* g_worker;
uint64_t g_count = 0;

uint64_t ReadFpcr() {
  uint64_t v;
  asm volatile("mrs %0, fpcr" : "=r"(v));
  return v;
}
void WriteFpcr(uint64_t v) { asm volatile("msr fpcr, %0" : : "r"(v)); }

void Worker(void*) {
  // a real frame: locals and a callee-saved vector register across switches
  volatile double local = 1.5;
  register double d8_value asm("d8") = 2.5;
  asm volatile("" : "+w"(d8_value));
  for (;;) {
    ++g_count;
    rex::thread::Fiber::SwitchTo(g_main);
    asm volatile("" : "+w"(d8_value));
    if (local != 1.5 || d8_value != 2.5) {
      Check("fiber keeps its locals and d8", false);
      local = 1.5;
    }
  }
}

void TestFibers() {
  constexpr uint64_t kFz = 1u << 24;
  g_main = rex::thread::Fiber::ConvertCurrentThread();
  Check("ConvertCurrentThread", g_main != nullptr);
  g_worker = rex::thread::Fiber::Create(256 * 1024, &Worker, nullptr);
  Check("Create", g_worker != nullptr);

  uint64_t before = ReadFpcr();
  register double d9_value asm("d9") = 3.5;
  asm volatile("" : "+w"(d9_value));
  rex::thread::Fiber::SwitchTo(g_worker);
  // the fiber's FPCR stays its own: changing it here does not reach the worker's next run, and comes back
  WriteFpcr(before | kFz);
  for (int i = 0; i < 1000000; ++i) rex::thread::Fiber::SwitchTo(g_worker);
  Check("1000001 round trips through the fiber", g_count == 1000001);
  Check("switching restores this fiber's FPCR", ReadFpcr() == (before | kFz));
  WriteFpcr(before);
  asm volatile("" : "+w"(d9_value));
  Check("the thread keeps d9", d9_value == 3.5);
}

// --- SIMDe: x86 results the guest's translated code expects ---

float Lane0(simde__m128 v) { return simde_mm_cvtss_f32(v); }
int32_t Lane0i(simde__m128i v) { return simde_mm_cvtsi128_si32(v); }

void TestSimde() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  simde__m128 a = simde_mm_set1_ps(1.0f), n = simde_mm_set1_ps(nan);
  Check("maxps(1, NaN) returns the second operand", std::isnan(Lane0(simde_mm_max_ps(a, n))));
  Check("maxps(NaN, 1) returns the second operand", Lane0(simde_mm_max_ps(n, a)) == 1.0f);
  Check("minps(1, NaN) returns the second operand", std::isnan(Lane0(simde_mm_min_ps(a, n))));
  Check("minps(NaN, 1) returns the second operand", Lane0(simde_mm_min_ps(n, a)) == 1.0f);
  Check("cvttps(NaN) is 0x80000000", uint32_t(Lane0i(simde_mm_cvttps_epi32(n))) == 0x80000000u);
  Check("cvttps(3e9) is 0x80000000", uint32_t(Lane0i(simde_mm_cvttps_epi32(simde_mm_set1_ps(3e9f)))) == 0x80000000u);
  Check("cvttps(-3e9) is 0x80000000", uint32_t(Lane0i(simde_mm_cvttps_epi32(simde_mm_set1_ps(-3e9f)))) == 0x80000000u);
  Check("cvttps(-1.5) is -1", Lane0i(simde_mm_cvttps_epi32(simde_mm_set1_ps(-1.5f))) == -1);
  simde__m128i bytes = simde_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  simde__m128i mask = simde_mm_setr_epi8(int8_t(0x80), 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
  uint8_t out[16];
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(out), simde_mm_shuffle_epi8(bytes, mask));
  Check("pshufb zeroes on the high bit and reverses", out[0] == 0 && out[1] == 15 && out[15] == 1);
  Check("movmskps reads the sign bits",
        simde_mm_movemask_ps(simde_mm_setr_ps(-1.0f, 1.0f, -1.0f, 1.0f)) == 0b0101);
}

}  // namespace

int main() {
  std::printf("page size %ld, %ld CPUs\n", sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
  rex::InitLoggingEarly();
  TestFibers();
  TestSimde();
  TestMemory();
  std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
