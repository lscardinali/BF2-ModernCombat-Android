// SDL3 main for Android: boots the recompiled game from the app's files (<files>/game) on the Vulkan renderer. This
// thread pumps the events and turns gamepads and touch into the pad (sdl_pad.h). Touch: one finger is A, two are START.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include "generated/default/bf2mc_init.h"
#include "native_audio/audio_system.h"
#include "native_gpu/native_gpu.h"
#include "script_input.h"
#include "sdl_pad.h"
#include "thread_dump.h"
#include "vk_context.h"
#include "vk_renderer.h"

namespace fs = std::filesystem;

namespace {

void Fail(const std::string& msg) {
  SDL_Log("%s", msg.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Battlefield 2: Modern Combat", msg.c_str(), nullptr);
}

int16_t Stick(int16_t v) { return v == -32768 ? int16_t(-32767) : v; }

void PublishPad() {
  SdlPadDriver::State s;
  int count = 0;
  static SDL_Gamepad* open[8] = {};
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count && i < 8; ++i) {
      if (!open[i] || SDL_GetGamepadID(open[i]) != ids[i]) open[i] = SDL_OpenGamepad(ids[i]);
      SDL_Gamepad* g = open[i];
      if (!g) continue;
      static const struct {
        SDL_GamepadButton b;
        uint16_t bit;
      } kMap[] = {{SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
                  {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
                  {SDL_GAMEPAD_BUTTON_START, 0x0010},       {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
                  {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
                  {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
                  {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
                  {SDL_GAMEPAD_BUTTON_WEST, 0x4000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
      for (const auto& m : kMap)
        if (SDL_GetGamepadButton(g, m.b)) s.buttons |= m.bit;
      s.lt = std::max<uint8_t>(s.lt, uint8_t(std::max(0, int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) >> 7));
      s.rt = std::max<uint8_t>(s.rt, uint8_t(std::max(0, int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) >> 7));
      if (!s.lx) s.lx = Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
      if (!s.ly) s.ly = Stick(int16_t(-int(Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)))));
      if (!s.rx) s.rx = Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
      if (!s.ry) s.ry = Stick(int16_t(-int(Stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY)))));
    }
    SDL_free(ids);
  }
  int fingers = 0;
  if (SDL_TouchID* touches = SDL_GetTouchDevices(nullptr)) {
    for (SDL_TouchID* t = touches; *t; t++) {
      int n = 0;
      if (SDL_Finger** f = SDL_GetTouchFingers(*t, &n)) SDL_free(f);
      fingers += n;
    }
    SDL_free(touches);
  }
  if (fingers == 1) s.buttons |= 0x1000;
  if (fingers >= 2) s.buttons |= 0x0010;
  SdlPadDriver::Publish(s);
}

}  // namespace

int main(int argc, char** argv) {
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    Fail(std::string("SDL failed to start: ") + SDL_GetError());
    return 1;
  }
  fs::path base;
  std::error_code ec;
  for (const char* dir : {SDL_GetAndroidInternalStoragePath(), SDL_GetAndroidExternalStoragePath()}) {
    if (dir && fs::exists(fs::path(dir) / "game" / "default.xex", ec)) {
      base = dir;
      break;
    }
  }
  if (base.empty()) {
    const char* in = SDL_GetAndroidInternalStoragePath();
    Fail(std::string("The game files are missing. Copy them to\n") + (in ? in : "?") + "/game");
    return 1;
  }
  chdir(base.c_str());
  fs::path game = base / "game", user = base / "user";
  fs::create_directories(user, ec);

  rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  auto log_config = rex::BuildLogConfig(REXCVAR_GET(log_level), {});
  log_config.app_name = "bf2mc";
  log_config.log_dir = user / "logs";
  rex::ApplyLogCvarOverrides(log_config);
  rex::InitLogging(log_config);
  REXLOG_INFO("android: game in {}", game.string());

  SDL_Window* window = SDL_CreateWindow("Battlefield 2: Modern Combat", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
  if (!window || !vk::Get().Init(window)) {
    Fail(std::string("Vulkan failed to start: ") + SDL_GetError());
    return 1;
  }
  bf2n::VulkanRendererSetup(window, user);

  rex::RuntimeConfig config;
  config.kernel_init = rex::kernel::InitializeKernel;
  config.graphics = nullptr;
  config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher) -> std::unique_ptr<rex::system::IAudioSystem> {
    return std::make_unique<bf2audio::NativeAudioSystem>(dispatcher);
  };
  // user/script.txt: a scripted pad next to the real one, for tools/run_app.ps1 (script_input.h)
  std::string script = fs::exists(user / "script.txt", ec) ? (user / "script.txt").string() : "";
  config.input_factory = [script](bool) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = std::make_unique<rex::input::InputSystem>(nullptr);
    input->AddDriver(std::make_unique<SdlPadDriver>());
    if (!script.empty()) {
      auto driver = std::make_unique<ScriptInputDriver>(script);
      driver->Setup();
      input->AddDriver(std::move(driver));
    }
    return input;
  };

  static rex::Runtime runtime(game, user, {}, user / "cache", {});
  rex::PPCImageInfo info = PPCImageConfig;
  auto status = runtime.Setup(info, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    rex::FlushLogging();
    Fail("The game failed to start, see the log in " + (user / "logs").string());
    return 1;
  }
  if (info.register_modules) info.register_modules(runtime.kernel_state());
  status = runtime.LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) {
    REXLOG_ERROR("Failed to load the XEX: {:08X}", status);
    rex::FlushLogging();
    Fail("The game failed to load, see the log in " + (user / "logs").string());
    return 1;
  }
  if (info.rexcrt_heap && !rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime.memory())) {
    REXLOG_ERROR("Failed to initialize the rexcrt heap");
    rex::FlushLogging();
    return 1;
  }
  bf2n::NativeGpuInstallMmio();
  bf2n::NativeGpuSetWindow(window);

  auto main_thread = runtime.PrepareModuleLaunch();
  if (!main_thread) {
    REXLOG_ERROR("Failed to launch the module");
    rex::FlushLogging();
    return 1;
  }
  main_thread->Resume();
  REXLOG_INFO("android: game thread started");
  if (fs::exists(user / "dump_stacks", ec)) ThreadDumpAfter((user / "stacks.txt").c_str(), 30, 4);

  for (uint64_t tick = 0;; ++tick) {
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
      if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) {
        rex::FlushLogging();
        _exit(0);
      }
    }
    SDL_UpdateGamepads();
    PublishPad();
    SDL_Delay(8);
    if (tick % 120 == 0) rex::FlushLogging();
  }
}
