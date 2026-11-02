#pragma once
// The pad the game reads on Android. On Android only the main thread may pump SDL events: app_main.cpp reads the SDL
// gamepads and the touch screen there into a snapshot, and this driver hands the snapshot to the game's threads.
#include <atomic>
#include <cstring>

#include <rex/input/input_driver.h>

using rex::X_RESULT;
using rex::X_STATUS;

namespace bf2n {
int SplitSide();  // split_screen.cpp
}

class SdlPadDriver final : public rex::input::InputDriver {
 public:
  struct State {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  };
  static_assert(sizeof(State) <= 16);

  // two_players: the pad is a real device, which takes guest user 0 (the SDK numbers those in the order they are listed,
  // synthetic ones all feed user 0), so the next device listed is user 1. second: that next device is the second pad.
  explicit SdlPadDriver(bool two_players = false, bool second = false)
      : rex::input::InputDriver(nullptr, 0), two_players_(two_players), second_(second) {}
  // which: 0 every pad and the touch screen together, 1 and 2 the split screen's players
  static void Publish(int which, const State& s) {
    uint64_t a, b = 0;
    std::memcpy(&a, &s, 8);
    std::memcpy(&b, reinterpret_cast<const char*>(&s) + 8, sizeof(State) - 8);
    Slot(which * 2).store(a, std::memory_order_relaxed);
    Slot(which * 2 + 1).store(b, std::memory_order_relaxed);
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kId;
    info.name = "Android pad";
    info.guid = "android";
    info.synthetic = !two_players_;
    out.push_back(info);
    if (second_) {
      info.id = kSecondId;
      info.guid = "android2";
      info.synthetic = false;
      out.push_back(info);
    }
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kId && id != kSecondId) return X_ERROR_DEVICE_NOT_CONNECTED;
    const int side = bf2n::SplitSide();
    State s = Read(id == kSecondId ? 2 : side < 0 ? 0 : side + 1);
    std::memset(out, 0, sizeof(*out));
    out->packet_number = ++packet_;
    out->gamepad.buttons = s.buttons;
    out->gamepad.left_trigger = s.lt;
    out->gamepad.right_trigger = s.rt;
    out->gamepad.thumb_lx = s.lx;
    out->gamepad.thumb_ly = s.ly;
    out->gamepad.thumb_rx = s.rx;
    out->gamepad.thumb_ry = s.ry;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id != kId && id != kSecondId) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::memset(out, 0, sizeof(*out));
    out->type = 0x01;
    out->sub_type = 0x01;
    out->gamepad.buttons = 0xF3FF;
    out->gamepad.left_trigger = 0xFF;
    out->gamepad.right_trigger = 0xFF;
    out->gamepad.thumb_lx = (int16_t)0xFFC0;
    out->gamepad.thumb_ly = (int16_t)0xFFC0;
    out->gamepad.thumb_rx = (int16_t)0xFFC0;
    out->gamepad.thumb_ry = (int16_t)0xFFC0;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kId || id == kSecondId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Edge events for the title screen and menus, which read XInputGetKeystroke.
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE* out) override {
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_BAD_ARGUMENTS;
    State s = Read(0);
    uint32_t cur = s.buttons;
    if (s.lt) cur |= 1u << 16;
    if (s.rt) cur |= 1u << 17;
    uint32_t changed = cur ^ last_keys_;
    if (!changed) return X_ERROR_EMPTY;
    for (int pass = 0; pass < 2; pass++) {  // xinput order: releases first, then presses
      for (int i = 0; i < 18; i++) {
        uint32_t bit = 1u << i;
        if (!(changed & bit) || !kVk[i]) continue;
        bool down = cur & bit;
        if (down != (pass == 1)) continue;
        std::memset(out, 0, sizeof(*out));
        out->virtual_key = kVk[i];
        out->flags = down ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN : rex::input::X_INPUT_KEYSTROKE_KEYUP;
        last_keys_ = down ? (last_keys_ | bit) : (last_keys_ & ~bit);
        return X_ERROR_SUCCESS;
      }
    }
    last_keys_ = cur;
    return X_ERROR_EMPTY;
  }

 private:
  static constexpr rex::input::DeviceId kId = static_cast<rex::input::DeviceId>(0x5C21F8);
  static constexpr rex::input::DeviceId kSecondId = static_cast<rex::input::DeviceId>(0x5C21F9);
  bool two_players_ = false, second_ = false;
  // VK_PAD_* by bit of X_INPUT_GAMEPAD::buttons, then LT/RT
  static constexpr uint16_t kVk[18] = {0x5810, 0x5811, 0x5812, 0x5813, 0x5814, 0x5815, 0x5816, 0x5817, 0x5805,
                                       0x5804, 0,      0,      0x5800, 0x5801, 0x5802, 0x5803, 0x5806, 0x5807};
  static std::atomic<uint64_t>& Slot(int i) {
    static std::atomic<uint64_t> slots[6];
    return slots[i];
  }
  static State Read(int which) {
    uint64_t a = Slot(which * 2).load(std::memory_order_relaxed), b = Slot(which * 2 + 1).load(std::memory_order_relaxed);
    State s;
    std::memcpy(&s, &a, 8);
    std::memcpy(reinterpret_cast<char*>(&s) + 8, &b, sizeof(State) - 8);
    return s;
  }
  uint32_t last_keys_ = 0;
  uint32_t packet_ = 0;
};
