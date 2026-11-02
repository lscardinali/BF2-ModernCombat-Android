// Split screen in the campaign. The game has one human player: the controller object in the global
// below, whose +56 is the soldier it drives. Camera, HUD and input all follow that soldier, so each
// update hands the controller to the other player's soldier (sub_8219A4A0, the end of the game's own
// hotswap, type 0 = no transition), and the renderer shows the frames in turn on the two sides.
//   sub_8219D5D8(controller): the controller's update
//   sub_8219CA98(controller): the soldier a hotswap would go to now; a player without a soldier takes that one
//   +296: target of a hotswap under way. Soldier +905 bit 0x10 (sub_821A0CC0): the game's own hotswap
//   checks skip soldiers that have it.
//   +84: the pad state the game reads, current and previous (+168/+172 point into it, sub_82638EC8
//   compares them for "just pressed"). Each player keeps their own copy, or one player's held button
//   would read as a press by the other.
// BF2_SPLIT turns it on (settings.toml, app_main.cpp).
#include "generated/default/bf2mc_init.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

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
constexpr uint32_t kPadStateSize = 180;
uint8_t g_pad_state[2][kPadStateSize];
bool g_pad_saved[2] = {};
std::atomic<int> g_side{0};
std::atomic<int> g_frame_side{0};
std::atomic<int64_t> g_last_flip{0};  // steady clock ticks

// What the hand-over would store as the player's body (sub_8219A118 -> sub_821C0870, global +328): the first entry of
// the soldier's +256 list, its virtual +224, that one's virtual +84. A soldier without one can't be handed to: the game
// reads the global on every update without checking it (sub_821FACD0, reading guest 0x64).
uint32_t BodyOf(PPCContext& ctx, uint8_t* base, uint32_t soldier) {
  const uint32_t first = Load32(base, soldier + 256);
  if (!first || first == Load32(base, soldier + 260)) return 0;  // sub_821A4578 traps on an empty list
  PPCContext saved = ctx;
  ctx.r3.u64 = soldier;
  sub_821A4578(ctx, base);
  uint32_t body = ctx.r3.u32;
  if (body) {
    ctx.r3.u64 = body;
    ctx.ctr.u64 = Load32(base, Load32(base, body) + 84);
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    body = ctx.r3.u32;
  }
  ctx = saved;
  return body;
}

bool Usable(PPCContext& ctx, uint8_t* base, uint32_t soldier) {
  return soldier && !Flagged(base, soldier) && BodyOf(ctx, base, soldier);
}

void Update(PPCContext& ctx, uint8_t* base, uint32_t controller) {
  const uint32_t current = Load32(base, controller + 56);
  if (!current || !InMission(base) || Load32(base, controller + 296) || Flagged(base, current)) return;
  g_soldier[g_turn] = current;  // a hotswap the player made stays theirs
  const int next = !g_turn;
  if (!Usable(ctx, base, g_soldier[next])) {
    if (g_soldier[next]) REXLOG_INFO("split: player {} lost soldier {:08X}", next + 1, g_soldier[next]);
    g_soldier[next] = 0;
    PPCContext saved = ctx;
    sub_8219CA98(ctx, base);
    const uint32_t target = ctx.r3.u32;
    ctx = saved;
    if (target == current || !Usable(ctx, base, target)) return;
    g_soldier[next] = target;
    REXLOG_INFO("split: player {} is soldier {:08X}, the other {:08X}", next + 1, target, current);
  }
  PPCContext saved = ctx;
  ctx.r4.u64 = g_soldier[next];
  ctx.r5.u64 = 0;
  sub_8219A4A0(ctx, base);
  ctx = saved;
  uint8_t* pad = rex::memory::GuestPtr(base, Load32(base, controller + 84));
  std::memcpy(g_pad_state[g_turn], pad, kPadStateSize);
  g_pad_saved[g_turn] = true;
  if (g_pad_saved[next]) std::memcpy(pad, g_pad_state[next], kPadStateSize);
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

// The side of the frame the render thread is drawing: the game thread may have flipped since that frame began.
int SplitFrameSide() { return SplitSide() < 0 ? -1 : g_frame_side.load(std::memory_order_relaxed); }

}  // namespace bf2n

REX_HOOK_RAW(sub_82346538) {  // the render thread's frame: draws everything, then swaps
  g_frame_side.store(g_side.load(std::memory_order_relaxed), std::memory_order_relaxed);
  __imp__sub_82346538(ctx, base);
}

REX_HOOK_RAW(sub_8219D5D8) {
  const uint32_t controller = ctx.r3.u32;
  if (SplitOn() && controller == Load32(base, kHumanController)) Update(ctx, base, controller);
  __imp__sub_8219D5D8(ctx, base);
}
