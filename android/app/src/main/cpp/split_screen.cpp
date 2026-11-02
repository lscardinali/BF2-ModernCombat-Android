// Split screen in the campaign. The game has one human player: the controller object in the global
// below, whose +56 is the soldier it drives. Camera, HUD and input all follow that soldier, so each
// update hands the controller to the other player's soldier (sub_8219A4A0, the end of the game's own
// hotswap, type 0 = no transition), and the renderer shows the frames in turn on the two sides.
//   sub_8219D5D8(controller): the controller's update
//   sub_8219CA98(controller): the soldier a hotswap would go to now; player 2 starts on that one
//   +296: target of a hotswap under way. Soldier +905 bit 0x10 (sub_821A0CC0): the game's own hotswap
//   checks skip soldiers that have it.
// BF2_SPLIT turns it on (settings.toml, app_main.cpp).
#include "generated/default/bf2mc_init.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cstdlib>

namespace {

constexpr uint32_t kHumanController = 0x8284F7EC;
constexpr uint32_t kScreenManager = 0x8285EF78;  // as input_remap.cpp: no screen node in a mission
constexpr uint32_t kManagerNode = 4216;

using Clock = std::chrono::steady_clock;

uint32_t Load32(uint8_t* base, uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a));
}

bool SplitOn() {
  static const bool on = std::getenv("BF2_SPLIT") != nullptr;
  return on;
}

bool Flagged(uint8_t* base, uint32_t soldier) {
  return (rex::memory::GuestPtr(base, soldier + 905)[0] & 0x10) != 0;
}

bool InMission(uint8_t* base) {
  const uint32_t manager = Load32(base, kScreenManager);
  return manager && Load32(base, manager + kManagerNode) == 0;
}

uint32_t g_soldier[2] = {};  // game thread
int g_turn = 0;
std::atomic<int> g_side{0};
std::atomic<int64_t> g_last_flip{0};  // steady clock ticks

void Update(PPCContext& ctx, uint8_t* base, uint32_t controller) {
  const uint32_t current = Load32(base, controller + 56);
  if (!current || !InMission(base) || Load32(base, controller + 296) || Flagged(base, current)) return;
  g_soldier[g_turn] = current;  // a hotswap the player made stays theirs
  if (!g_soldier[1] || Flagged(base, g_soldier[1])) {
    g_soldier[1] = 0;
    PPCContext saved = ctx;
    sub_8219CA98(ctx, base);
    const uint32_t target = ctx.r3.u32;
    ctx = saved;
    if (!target || target == g_soldier[0] || Flagged(base, target)) return;
    g_soldier[1] = target;
    REXLOG_INFO("split: player 2 is soldier {:08X}, player 1 {:08X}", target, g_soldier[0]);
  }
  const int next = !g_turn;
  if (!g_soldier[next] || Flagged(base, g_soldier[next])) return;
  PPCContext saved = ctx;
  ctx.r4.u64 = g_soldier[next];
  ctx.r5.u64 = 0;
  sub_8219A4A0(ctx, base);
  ctx = saved;
  g_turn = next;
  g_side.store(next, std::memory_order_relaxed);
  g_last_flip.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

}  // namespace

namespace bf2n {

// The side the next finished frame belongs to, -1 when the screen isn't split right now.
int SplitSide() {
  if (!SplitOn()) return -1;
  const auto last = Clock::duration(g_last_flip.load(std::memory_order_relaxed));
  if (Clock::now().time_since_epoch() - last > std::chrono::milliseconds(1000)) return -1;
  return g_side.load(std::memory_order_relaxed);
}

}  // namespace bf2n

REX_HOOK_RAW(sub_8219D5D8) {
  const uint32_t controller = ctx.r3.u32;
  if (SplitOn() && controller == Load32(base, kHumanController)) Update(ctx, base, controller);
  __imp__sub_8219D5D8(ctx, base);
}
