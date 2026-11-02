// The game without a window: rex::ReXApp's start-up (ui/rex_app.cpp) minus its window and overlays. Usage:
//   bf2mc --game_data_root=<dir> [--user_data_root=<dir>] [--seconds=<n>]
// BF2_INPUT=<file> scripts the pad (script_input.h), as on the PC.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

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
#include "thread_dump.h"

int main(int argc, char** argv) {
  std::filesystem::path game = "game", user = "user";
  int seconds = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--game_data_root=", 0) == 0) game = a.substr(17);
    if (a.rfind("--user_data_root=", 0) == 0) user = a.substr(17);
    if (a.rfind("--seconds=", 0) == 0) seconds = std::atoi(a.c_str() + 10);
  }
  rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  auto log_config = rex::BuildLogConfig(REXCVAR_GET(log_level), {});
  log_config.app_name = "bf2mc";
  log_config.log_dir = user / "logs";
  rex::ApplyLogCvarOverrides(log_config);
  rex::InitLogging(log_config);

  rex::RuntimeConfig config;
  config.kernel_init = rex::kernel::InitializeKernel;
  config.graphics = nullptr;
  config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher) -> std::unique_ptr<rex::system::IAudioSystem> {
    return std::make_unique<bf2audio::NativeAudioSystem>(dispatcher);
  };
  std::string script = std::getenv("BF2_INPUT") ? std::getenv("BF2_INPUT") : "";
  config.input_factory = [script](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = std::make_unique<rex::input::InputSystem>(nullptr);
    if (!script.empty()) {
      auto driver = std::make_unique<ScriptInputDriver>(script);
      driver->Setup();
      input->AddDriver(std::move(driver));
    }
    return input;
  };

  std::filesystem::create_directories(user);
  rex::Runtime runtime(game, user, {}, user / "cache", {});
  rex::PPCImageInfo info = PPCImageConfig;
  auto status = runtime.Setup(info, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    rex::FlushLogging();
    return 1;
  }
  if (info.register_modules) info.register_modules(runtime.kernel_state());
  status = runtime.LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) {
    REXLOG_ERROR("Failed to load the XEX: {:08X}", status);
    rex::FlushLogging();
    return 1;
  }
  if (info.rexcrt_heap && !rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime.memory())) {
    REXLOG_ERROR("Failed to initialize the rexcrt heap");
    rex::FlushLogging();
    return 1;
  }
  bf2n::NativeGpuInstallMmio();
  static int fake_window;
  bf2n::NativeGpuSetWindow(&fake_window);  // the null renderer never looks at it

  auto main_thread = runtime.PrepareModuleLaunch();
  if (!main_thread) {
    REXLOG_ERROR("Failed to launch the module");
    rex::FlushLogging();
    return 1;
  }
  main_thread->Resume();
  REXLOG_INFO("harness: game thread started");
  if (const char* dump = std::getenv("BF2_DUMP_AFTER"); dump && *dump) {
    ThreadDumpAfter((user / "stacks.txt").c_str(), std::atoi(dump), 3);
  }
  if (seconds > 0) {
    for (int i = 0; i < seconds; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      rex::FlushLogging();
    }
    REXLOG_INFO("harness: {} seconds are up", seconds);
    rex::FlushLogging();
    std::_Exit(0);
  }
  main_thread->Wait(0, 0, 0, nullptr);
  REXLOG_INFO("harness: game thread ended");
  rex::FlushLogging();
  std::_Exit(0);
}
