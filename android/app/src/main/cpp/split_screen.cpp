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
#include <cmath>
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

constexpr uint32_t kRenderFrames = 0x8287A4FC;  // the two frame packets: +2448 the one drawn, +2452 the one written
bool g_second_pass = false;                     // game thread
std::atomic<int> g_packet_side[2] = {};
uint32_t g_controller[2] = {};                  // game thread
constexpr uint32_t kActiveUser = 0x82815D84;    // the pad the game's one input state reads (sub_823063F0)
constexpr uint32_t kRescanCounter = 0x828775C4;
uint32_t g_device = 0;
uint32_t g_input_state = 0;                     // +4 of the object sub_823063F0 runs on
double g_input_dt = 0.0;

bool SecondPassOn() {
  static const bool on = std::getenv("BF2_SPLIT2") != nullptr;
  return on;
}

// What sub_822323C8 passes to sub_821C5928: 0x8286AC18's first virtual, then +32 of what it returns.
uint32_t PlayerList(PPCContext& ctx, uint8_t* base) {
  PPCContext saved = ctx;
  const uint32_t game = Load32(base, 0x8286AC18);
  ctx.r3.u64 = game;
  ctx.ctr.u64 = Load32(base, Load32(base, game));
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  const uint32_t players = Load32(base, ctx.r3.u32 + 32);
  ctx = saved;
  return players;
}

// The input state's update, which reads the active user's pad.
void PollInput(PPCContext& ctx, uint8_t* base, uint32_t user) {
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, kActiveUser), user);
  if (!g_input_state) return;
  PPCContext saved = ctx;
  // The device itself is only read while the game scans for the active pad, which is pinned now: as in sub_823063F0.
  ctx.r3.u64 = Load32(base, 0x8284F540);
  sub_822A8D98(ctx, base);
  const uint32_t devices = ctx.r3.u32;
  auto device_call = [&](uint32_t object, uint32_t slot) {
    ctx.r3.u64 = object;
    ctx.r4.u64 = user;
    ctx.ctr.u64 = Load32(base, Load32(base, object) + slot);
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    return ctx.r3.u32;
  };
  device_call(devices, 12);
  g_device = device_call(devices, 20);
  if (g_device) device_call(g_device, 4);
  device_call(devices, 16);
  ctx = saved;
  ctx.r3.u64 = g_input_state;
  ctx.f1.f64 = g_input_dt;
  ctx.ctr.u64 = Load32(base, Load32(base, g_input_state) + 8);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  ctx = saved;
}

void SetHuman(uint8_t* base, uint32_t players, uint32_t controller) {
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, kHumanController), controller);
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, players + 4), controller);  // sub_822F7268
}

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
  if (SecondPassOn()) {
    const uint32_t frames = Load32(base, kRenderFrames);
    g_frame_side.store(g_packet_side[Load32(base, frames + 2448) & 1], std::memory_order_relaxed);
  } else {
    g_frame_side.store(g_side.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  __imp__sub_82346538(ctx, base);
}

REX_HOOK_RAW(sub_8219D5D8) {
  const uint32_t controller = ctx.r3.u32;
  if (SplitOn() && !SecondPassOn() && controller == Load32(base, kHumanController)) Update(ctx, base, controller);
  if (!SecondPassOn() || !g_controller[0]) {
    __imp__sub_8219D5D8(ctx, base);
    return;
  }
  const bool second = controller == g_controller[1];
  if (controller != g_controller[0] && !second) {
    __imp__sub_8219D5D8(ctx, base);
    return;
  }
  // The second view's pass moves no time: input was handled in the first one, and would count twice.
  if (g_second_pass) return;
  if (!second) {
    __imp__sub_8219D5D8(ctx, base);
    return;
  }
  // The second controller runs believing it is the human player, reading the other pad.
  const uint32_t players = PlayerList(ctx, base);
  const uint32_t human = Load32(base, kHumanController);
  const uint32_t active = Load32(base, kActiveUser);
  SetHuman(base, players, controller);
  PollInput(ctx, base, active == 1 ? 0 : 1);
  static int updates = 0;
  if (++updates % 120 == 0) {
    uint32_t mask = 0;
    if (g_input_state) mask = Load32(base, g_input_state + 4);
    REXLOG_INFO("split2: second pad: update {}, active {}, input buttons {:08X} sticks {:08X} {:08X}, device {:08X} user {} raw {:08X} {:08X} {:08X}", updates, Load32(base, kActiveUser), mask,
                g_input_state ? Load32(base, g_input_state + 92) : 0, g_input_state ? Load32(base, g_input_state + 96) : 0, g_device, g_device ? Load32(base, g_device + 4) : 0,
                g_device ? Load32(base, g_device + 16) : 0, g_device ? Load32(base, g_device + 20) : 0, g_device ? Load32(base, g_device + 24) : 0);
    REXLOG_INFO("split2: second soldier at {:.1f}", rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, BodyOf(ctx, base, Load32(base, controller + 56)) + 96)));
  }
  __imp__sub_8219D5D8(ctx, base);
  PollInput(ctx, base, active);
  SetHuman(base, players, human);
}

REX_HOOK_RAW(sub_823063F0) {  // the game's pad poll
  g_input_state = Load32(base, ctx.r3.u32 + 4);
  g_input_dt = ctx.f1.f64;
  // Every 8th poll the game makes whichever pad is in use the active one: with two players, each keeps theirs.
  if (g_controller[1]) rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, kRescanCounter), 0);
  __imp__sub_823063F0(ctx, base);
}

// A second human controller, as sub_8219B998 sets one up, takes over a teammate. Building a new soldier isn't an option:
// that crashes in the middle of a mission, and one spawned while the level loads falls through the ground.
void AddSecondPlayer(PPCContext& ctx, uint8_t* base, uint32_t first, uint32_t first_soldier, uint32_t target) {
  PPCContext saved = ctx;
  const uint32_t players = PlayerList(ctx, base);
  ctx.r3.u64 = players;
  sub_821C5928(ctx, base);
  const uint32_t second = ctx.r3.u32;
  SetHuman(base, players, second);
  ctx = saved;
  ctx.r3.u64 = second;
  ctx.r4.u64 = target;
  sub_8219A118(ctx, base);
  ctx = saved;
  ctx.r3.u64 = second;
  ctx.r4.u64 = 0;
  sub_821B07B0(ctx, base);
  ctx = saved;
  ctx.r3.u64 = 28;
  ctx.r4.u64 = 0;
  sub_822BCCA8(ctx, base);
  const uint32_t helper = ctx.r3.u32;
  if (helper) {
    ctx.r4.u64 = second;
    sub_8262DD18(ctx, base);
    rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, second + 324), helper);
    ctx.r3.u64 = helper;
    sub_8262DDE8(ctx, base);
  }
  ctx = saved;
  ctx.r3.u64 = second;
  ctx.r4.u64 = second + 192;
  sub_821996E8(ctx, base);
  ctx = saved;
  sub_82635B20(ctx, base);
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, second + 328), ctx.r3.u32);
  ctx = saved;
  sub_82635B10(ctx, base);
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, second + 332), ctx.r3.u32);
  ctx = saved;
  // The hand-over took the camera and the player's body with it. One from no soldier brings them back to the first player.
  SetHuman(base, players, first);
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, first + 56), 0);
  ctx.r3.u64 = first;
  ctx.r4.u64 = first_soldier;
  sub_8219A118(ctx, base);
  ctx = saved;
  if (Load32(base, first + 56) != first_soldier)
    rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, first + 56), first_soldier);
  // Both hand-overs ran with no soldier in their controller, and sub_821FAA40 recorded that as the body's owner.
  const uint32_t controllers[2] = {first, second}, soldiers[2] = {first_soldier, target};
  for (int i = 0; i < 2; ++i) {
    const uint32_t body = BodyOf(ctx, base, soldiers[i]);
    if (!body) continue;
    rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, body + 184), controllers[i]);
    rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, body + 188), soldiers[i]);
  }
  if (Load32(base, kActiveUser) > 3) rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, kActiveUser), 0);
  g_controller[0] = first;
  g_controller[1] = second;
  REXLOG_INFO("split2: controllers {:08X} and {:08X}, soldiers {:08X} and {:08X}, second has {:08X}, first player's pad {}", first, second,
              first_soldier, target, Load32(base, second + 56), Load32(base, kActiveUser));
}

// Once the first player has landed: their height holds still for half a second and the game offers a teammate.
void MaybeAddSecondPlayer(PPCContext& ctx, uint8_t* base) {
  if (g_controller[1] || !InMission(base)) return;
  const uint32_t first = Load32(base, kHumanController);
  const uint32_t soldier = first ? Load32(base, first + 56) : 0;
  const uint32_t body = soldier ? BodyOf(ctx, base, soldier) : 0;
  if (!body) return;
  const float height = rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, body + 100));
  static int steady = 0;
  static float last = 0.0f;
  steady = std::fabs(height - last) < 0.01f ? steady + 1 : 0;
  last = height;
  if (steady < 30) return;
  steady = 0;
  PPCContext saved = ctx;
  ctx.r3.u64 = first;
  sub_8219CA98(ctx, base);
  const uint32_t target = ctx.r3.u32;
  ctx = saved;
  if (target && target != soldier && !Flagged(base, target) && BodyOf(ctx, base, target)) AddSecondPlayer(ctx, base, first, soldier, target);
}

// Spike (BF2_SPLIT2): the game's update once more with no time passing, for a second view of the same moment.
REX_HOOK_RAW(sub_82298D20) {
  if (!SecondPassOn() || g_second_pass) {
    __imp__sub_82298D20(ctx, base);
    return;
  }
  MaybeAddSecondPlayer(ctx, base);
  PPCContext saved = ctx;
  __imp__sub_82298D20(ctx, base);
  if (!InMission(base)) return;
  ctx = saved;
  ctx.f1.f64 = 0.0;
  const uint32_t players = PlayerList(ctx, base);
  static int logged = 0;
  if (g_controller[1] && ++logged % 120 == 0) {
    float at[2][3] = {};
    for (int p = 0; p < 2; ++p) {
      const uint32_t soldier = Load32(base, g_controller[p] + 56);
      const uint32_t body = soldier ? BodyOf(ctx, base, soldier) : 0;
      for (int i = 0; body && i < 3; ++i) at[p][i] = rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, body + 96 + i * 4));
    }
    REXLOG_INFO("split2: soldiers at {:.1f} {:.1f} {:.1f} and {:.1f} {:.1f} {:.1f}, camera {:.1f} {:.1f} {:.1f}", at[0][0], at[0][1], at[0][2], at[1][0], at[1][1], at[1][2],
                rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, Load32(base, 0x8284F8D8) + 48 + 624 + 48)),
                rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, Load32(base, 0x8284F8D8) + 48 + 624 + 52)),
                rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, Load32(base, 0x8284F8D8) + 48 + 624 + 56)));
  }
  // This pass's camera sits at the second soldier's body, looking the way the first camera looks.
  const uint32_t camera = Load32(base, 0x8284F8D8) + 48;
  auto cam = [&](uint32_t offset, int i) { return rex::memory::GuestPtr(base, camera + offset + i * 4); };
  float kept_view[3], kept_world[3], position[3];
  bool moved = false;
  if (g_controller[1]) {
    const uint32_t soldier = Load32(base, g_controller[1] + 56);
    const uint32_t body = soldier ? BodyOf(ctx, base, soldier) : 0;
    if (body) {
      for (int i = 0; i < 3; ++i) {
        position[i] = rex::memory::load_and_swap<float>(rex::memory::GuestPtr(base, body + 96 + i * 4));
        kept_view[i] = rex::memory::load_and_swap<float>(cam(432, 12 + i));
        kept_world[i] = rex::memory::load_and_swap<float>(cam(624, 12 + i));
      }
      position[1] += 1.7f;
      for (int j = 0; j < 3; ++j) {
        float dot = 0.0f;
        for (int k = 0; k < 3; ++k) dot += position[k] * rex::memory::load_and_swap<float>(cam(432, k * 4 + j));
        rex::memory::store_and_swap<float>(cam(432, 12 + j), -dot);
        rex::memory::store_and_swap<float>(cam(624, 12 + j), position[j]);
      }
      moved = true;
    }
    SetHuman(base, players, g_controller[1]);
  }
  g_second_pass = true;
  __imp__sub_82298D20(ctx, base);
  g_second_pass = false;
  if (g_controller[1]) SetHuman(base, players, g_controller[0]);
  if (moved) {
    for (int i = 0; i < 3; ++i) {
      rex::memory::store_and_swap<float>(cam(432, 12 + i), kept_view[i]);
      rex::memory::store_and_swap<float>(cam(624, 12 + i), kept_world[i]);
    }
  }
}

REX_HOOK_RAW(sub_82347190) {  // hands the packet being written to the render thread
  if (SecondPassOn()) {
    const uint32_t frames = ctx.r3.u32;
    g_packet_side[Load32(base, frames + 2452) & 1] = g_second_pass ? 1 : 0;
    if (InMission(base)) g_last_flip.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  }
  __imp__sub_82347190(ctx, base);
}
