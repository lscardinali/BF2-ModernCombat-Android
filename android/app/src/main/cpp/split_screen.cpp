// Split-screen research: the game has one human player, the controller object in the global below,
// and +56 is the soldier it drives. sub_8219D5D8(controller) is its update, sub_8219EBB0(controller,
// soldier, type) starts a hotswap and sub_8219A4A0(controller, soldier, type) hands it the soldier.
// user/split_probe logs those; user/split_test also flips between the last two soldiers every 5 s,
// straight through the hand-over, to see whether the camera and HUD simply follow.
#include "generated/default/bf2mc_init.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kHumanController = 0x8284F7EC;

uint32_t Load32(uint8_t* base, uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a));
}
float LoadF(uint8_t* base, uint32_t a) {
  const uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

bool ProbeOn() {
  static const bool on = std::getenv("BF2_SPLIT_PROBE") != nullptr;
  return on;
}
bool TestOn() {
  static const bool on = std::getenv("BF2_SPLIT_TEST") != nullptr;
  return on;
}

uint32_t g_other = 0;       // the soldier the controller left in the last hand-over
bool g_flipping = false;    // our own hand-over is running
std::chrono::steady_clock::time_point g_next_flip, g_next_pos;

// sub_821A0CC0: bit 0x10 of +905; the game's own hotswap checks skip soldiers that have it.
bool Flagged(uint8_t* base, uint32_t soldier) {
  return (rex::memory::GuestPtr(base, soldier + 905)[0] & 0x10) != 0;
}

// The position of the object the controller is attached to (+76, transform at +12), as sub_8219E9B8 reads it.
void LogPosition(PPCContext& ctx, uint8_t* base, uint32_t controller, const char* when) {
  const uint32_t object = Load32(base, controller + 76);
  if (!object) return;
  PPCContext saved = ctx;
  ctx.r3.u64 = object + 12;
  sub_8262A2F8(ctx, base);
  const uint32_t m = ctx.r3.u32;
  ctx = saved;
  REXLOG_INFO("split: {} soldier {:08X} object {:08X} at {:.1f} {:.1f} {:.1f}", when, Load32(base, controller + 56),
              object, LoadF(base, m + 48), LoadF(base, m + 52), LoadF(base, m + 56));
}

}  // namespace

REX_HOOK_RAW(sub_8219EBB0) {
  if (ProbeOn()) {
    REXLOG_INFO("split: hotswap start controller {:08X} target {:08X} type {} from {:08X}", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, uint32_t(ctx.lr));
  }
  __imp__sub_8219EBB0(ctx, base);
}

REX_HOOK_RAW(sub_8219A4A0) {
  const uint32_t controller = ctx.r3.u32, target = ctx.r4.u32, type = ctx.r5.u32;
  const uint32_t before = controller ? Load32(base, controller + 56) : 0;
  if (ProbeOn()) {
    REXLOG_INFO("split: hand-over controller {:08X} soldier {:08X} -> {:08X} type {} from {:08X}{}", controller, before,
                target, type, uint32_t(ctx.lr), g_flipping ? " (ours)" : "");
  }
  __imp__sub_8219A4A0(ctx, base);
  if (!g_flipping && before && before != target) g_other = before;
}

REX_HOOK_RAW(sub_8219D5D8) {
  const uint32_t controller = ctx.r3.u32;
  if (TestOn() && controller == Load32(base, kHumanController)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_next_pos) {
      g_next_pos = now + std::chrono::seconds(1);
      LogPosition(ctx, base, controller, "pos");
    }
    if (g_other && now >= g_next_flip) {
      g_next_flip = now + std::chrono::seconds(5);
      const uint32_t current = Load32(base, controller + 56);
      if (Flagged(base, g_other)) {
        REXLOG_INFO("split: not flipping to {:08X}: flagged", g_other);
        g_other = 0;
      } else {
        LogPosition(ctx, base, controller, "before");
        PPCContext saved = ctx;
        ctx.r4.u64 = g_other;
        ctx.r5.u64 = 0;
        g_flipping = true;
        sub_8219A4A0(ctx, base);
        g_flipping = false;
        ctx = saved;
        g_other = current;
        LogPosition(ctx, base, controller, "after");
      }
    }
  }
  __imp__sub_8219D5D8(ctx, base);
}
