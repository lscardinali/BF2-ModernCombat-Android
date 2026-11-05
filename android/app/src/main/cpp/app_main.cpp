// SDL3 main for Android: boots the recompiled game from the app's files (<files>/game) on the Vulkan renderer. This
// thread pumps the events and turns gamepads and touch into the pad (sdl_pad.h). Touch: one finger is A, two are START.
// The Windows test build (windows/CMakeLists.txt) runs this too: the game is the first game/default.xex found going up
// from the executable, and user/ is next to the executable.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
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
  if (std::getenv("BF2_NO_DIALOG")) {  // tools/run_windows.ps1: nobody there to close it
    std::fprintf(stderr, "%s\n", msg.c_str());
    return;
  }
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Battlefield 2: Modern Combat", msg.c_str(), nullptr);
}

int16_t Stick(int16_t v) { return v == -32768 ? int16_t(-32767) : v; }

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

// The folder with game/default.xex, empty if there is none.
fs::path FindGameBase(fs::path& user_base) {
  std::error_code ec;
#ifdef __ANDROID__
  for (const char* dir : {SDL_GetAndroidInternalStoragePath(), SDL_GetAndroidExternalStoragePath()}) {
    if (dir && fs::exists(fs::path(dir) / "game" / "default.xex", ec)) {
      user_base = dir;
      return dir;
    }
  }
#else
  user_base = SDL_GetBasePath();
  for (fs::path dir = user_base; !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path())
    if (fs::exists(dir / "game" / "default.xex", ec)) return dir;
#endif
  return {};
}

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
  std::error_code ec;
  fs::path user_base;
  const fs::path base = FindGameBase(user_base);
  if (base.empty()) {
#ifdef __ANDROID__
    const char* in = SDL_GetAndroidInternalStoragePath();
    Fail(std::string("The game files are missing. Copy them to\n") + (in ? in : "?") + "/game");
#else
    Fail("The game files are missing: no game/default.xex above " + user_base.string());
#endif
    return 1;
  }
  fs::current_path(user_base, ec);
  fs::path game = base / "game", user = user_base / "user";
  fs::create_directories(user, ec);

  if (fs::exists(user / "audio_log", ec)) SetEnv("BF2_AUDIO_LOG", "1");
  if (fs::exists(user / "pcm_dump", ec)) SetEnv("BF2_PCM_DUMP", (user / "mix.pcm").string());
  if (std::ifstream f(user / "draw_stacks"); f) {  // a number of seconds: maps two frames after that long
    std::string seconds;
    f >> seconds;
    SetEnv("BF2_DRAW_STACKS", seconds);
    SetEnv("BF2_DRAW_STACKS_FILE", (user / "draw_stacks.txt").string());
  }
  rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  auto log_config = rex::BuildLogConfig(REXCVAR_GET(log_level), {});
  log_config.app_name = "bf2mc";
  log_config.log_dir = user / "logs";
  rex::ApplyLogCvarOverrides(log_config);
  rex::InitLogging(log_config);
  REXLOG_INFO("android: game in {}", game.string());

#ifdef __ANDROID__
  SDL_Window* window = SDL_CreateWindow("Battlefield 2: Modern Combat", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
#else
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_Window* window = SDL_CreateWindow("Battlefield 2: Modern Combat", 1280, 720, 0);
#endif
  vk::Get().force_vulkan_11 = fs::exists(user / "force_vulkan_11", ec);
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
    input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());  // no pads without one
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
#ifndef _WIN32  // on Windows, tools/run_windows.ps1 has the debugger dump them
  if (fs::exists(user / "dump_stacks", ec)) ThreadDumpAfter((user / "stacks.txt").c_str(), 30, 4);
#endif

  // Android: the activity's window goes when the app leaves the screen, and SDL then blocks this thread in
  // SDL_PumpEvents until it is back. The watch runs as SDL queues the event, before it blocks: the present thread holds
  // the next frame, which stops the game thread, and makes a new surface on the way back. The audio mixer and the vblank
  // interrupt stop too, or the game's other threads would keep a core busy behind the home screen.
  SDL_AddEventWatch(
      [](void*, SDL_Event* e) -> bool {
        if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
          SdlPadDriver::Publish({});  // nothing held down while away
          bf2audio::SetPaused(true);
          bf2n::SetVblankPaused(true);
          vk::Get().Pause();
          rex::FlushLogging();
        } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
          vk::Get().Resume();
          bf2n::SetVblankPaused(false);
          bf2audio::SetPaused(false);
        }
        return true;
      },
      nullptr);

  for (uint64_t tick = 0;; ++tick) {
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
      if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) {
        rex::FlushLogging();
        std::_Exit(0);
      }
    }
    SDL_UpdateGamepads();
    PublishPad();
    SDL_Delay(8);
    if (tick % 120 == 0) rex::FlushLogging();
  }
}
