# Battlefield 2: Modern Combat for Android

A native Android port of **Battlefield 2: Modern Combat** (Xbox 360, 2006). It is built on:

- [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) (BSD-3-Clause), a static recompilation SDK for Xbox 360 games.
- [bf2-modern-combat-pc](https://github.com/batatinhafcbatatinha-pixel/bf2-modern-combat-pc) (MIT), the port to PC on that SDK.

The game's PowerPC code is recompiled to C++ on your PC, then compiled for arm64 with the Android NDK.

No game code or data is in this repository. You need your own Xbox 360 USA disc image (`default.xex` SHA-256
`a2e791b5…5484`). Do not share the APK: it would contain code generated from the game.

## Status

| Milestone | State |
|---|---|
| SDK runtime builds and links for Android arm64 | done |
| Device smoke test: guest address space, write watches, fibers, SIMDe's x86 results | done, on a Redmi phone (Unisoc T7250, 3 GB, Android 15) |
| Recompile `default.xex`, compile the generated code for arm64 | done |
| Headless harness on device: intro movies, main menu, attract movie, at the game's 30 fps | done |
| Headless harness on device: first mission, CPU-only speed in play | todo |
| APK: SDL activity, audio, gamepads, touch (1 finger A, 2 fingers START) | done |
| Vulkan renderer: the port's HLSL through glslang, intro movies on screen with sound | done |
| Vulkan renderer: menus and the first mission (In and Out), played with a pad script | done; about 6-11 fps on the Mali-G57 at half resolution, by scene |
| Vulkan 1.1 and 1.2 drivers, through VK_KHR_dynamic_rendering | path forced and run on the Mali-G57; no Adreno device tried yet |
| Home screen, in-app game import from the disc image, settings with a fill-the-screen switch | done |
| Touch controls for play | todo |
| Leaving the app and coming back, in a mission too; saves | done, see below |
| Windows test build | runs the same sources on a PC's Vulkan driver, see below |

## Layout

| Path | What |
|---|---|
| `tools/fetch_deps.ps1` | clones the SDK (bd833a2) and the PC port (f2b236b) into `third_party/`, applies the patches, downloads the SDK's Windows package for its recompiler |
| `tools/build_harness.ps1` | builds the adb executables for arm64 |
| `tools/run_smoke.ps1` | runs the smoke test on the connected device |
| `tools/recompile.ps1` | extracts the disc into `game/` and writes the generated C++ into `out/recomp` |
| `tools/push_game.ps1`, `tools/run_harness.ps1` | game files to the device; runs the game with a null renderer |
| `cmake/bf2mc.cmake` | the SDK, game code and port sources the harness and the app share |
| `harness/` | the adb executables: the smoke test, the game with a null renderer |
| `src/` | stand ins for the PC settings screen and Win32 |
| `patches/sdk-*.patch`, `patches/bf2-*.patch` | this port's changes to the SDK (on top of the PC port's FPSCR patch) and to the PC port |
| `android/` | the APK: SDL3 activity, `app/src/main/cpp` is the native host and the Vulkan renderer |
| `tools/run_app.ps1`, `tools/capture_app.ps1` | installs and runs the APK; captures one frame's draws and targets |

## Build and run

Needs Git, Android Studio (SDK, NDK 30.0.16248370, CMake 4.1.2) and Python 3.

```powershell
. tools/env.ps1
tools/fetch_deps.ps1
tools/build_harness.ps1
tools/run_smoke.ps1
```

## Installing the game on the phone

The app opens on a home screen. With no game installed it asks for the disc image through the system file picker, checks `default.xex` and extracts what the single player uses (about 4 GB) into the app's files. The multiplayer data, `mpdefault.xex` and `$SystemUpdate` stay on the disc. The `.iso` can be deleted afterwards.

## Settings

Settings is on the home screen and is saved in `user/settings.toml`. **Fill the screen** stretches the 16:9 picture to the whole display, which on a 20:9 phone widens it by about 25%. It is off by default, and it takes effect the next time the game starts. It doesn't widen the game's field of view.

## In the background, and saves

Leaving the app (home button, another app, the screen off) holds the game still. Android takes the window away, and SDL blocks its main thread until it's back. An event watch in `app_main.cpp` then stops the audio mixer and the vblank interrupt, and the present thread holds the next frame, so the game thread stops in `BeginFrame`. On the way back the present thread makes a new Vulkan surface and swapchain for the new window, and the game carries on where it was. While the app is away the process uses about 20% of one core on the Mali phone, down from about 130%.

The game saves on its own: its autosave writes progress and settings, and SAVE CAMPAIGN is in the campaign menu. It doesn't save during a mission. The SDK keeps saves under `user/<profile>/454107DB/` in the app's files, and the game finds them on the next start. Android can still kill a process that is in the background, which would lose the mission being played. While the game is away, `KeepAliveService` runs as a foreground service with a "Paused" notification. That moves the process from the cached class, which Android kills first, to perceptible (`oom_score_adj` 50), so it survives most memory pressure. The service stops when the game is back on screen.

## Windows test build

`windows/CMakeLists.txt` builds the Android app's own sources (`android/app/src/main/cpp`) into `out/windows/bf2mc.exe`, so tests on a PC exercise the renderer the phone runs. It needs Visual Studio's C++ tools, LLVM in `C:\Program Files\LLVM` and the Android SDK's CMake and Ninja. Keep game files, the generated code and the build out of version control, as for Android.

```powershell
tools/build_windows.ps1
tools/run_windows.ps1 -Name mission -Seconds 120 -Script pads/win_mission.txt -Shots 70,90 -Debugger
```

Results land in `out/runs/<Name>`. The scripted pad re-reads `out/windows/user/script.txt` when it grows, so lines appended while the game runs (`pads/win_menu.txt` stops at the title) drive it by hand. `REX_LOG_LEVEL=debug` logs the kernel calls, the XAM content ones among them. `-Debugger` runs it under cdb from the Windows SDK: `cdb.txt` has the stacks, named `sub_<guest address>`, and the guest registers of a crash. `-CdbExtra 'ba w4 0x18284FB00 "kn 14; g";'` logs every write to a guest address (guest memory starts at host `0x100000000`). `-Validate` asks for the Vulkan validation layer, which needs the Vulkan SDK installed.

## Device tiers

On phones with 4 GB of memory or less the renderer draws at half resolution, caps mipmapped textures at 256, evicts unused textures over 192 MB, uses plain multiplies and cheaper filtering, and bloom is off. Above 4 GB: full resolution, textures up to 1024, a 768 MB budget, everything on. `user/scale` and `user/max_texture` in the app's files override the first two. The target is Snapdragon 865 and newer; only the Mali-G57 phone has been tried.

## Performance hints

From Android 13, `perf_hint.cpp` reports each frame to Android's performance hint API (ADPF). It sends the larger of the game update's CPU time (`sub_82298D20`) and the render thread's, against the game's 33.3 ms. The scheduler then raises clocks and moves the threads to big cores before frames run late, and lowers them when there's room. The API links weakly and every call is behind `__builtin_available`, so the app still runs from Android 12, where the hint is skipped. When the game starts a new render thread for a mission, the session moves to it. On the Mali phone the mission is GPU-bound, and its frame rate is the same either way (11.5 fps); the hint is for CPU-bound phones. `user/no_perf_hint` turns it off for comparisons. The manifest also declares the app a game (`appCategory`, `game_mode_config.xml`) for Game Mode and vendors' game profiles.

## Measuring

Files in the app's `files/user` folder switch these on: `gpu_times` (GPU time of each pass, every 150 frames), `capture` (a frame number: its draws logged, its targets dumped, `tools/capture_app.ps1`), `dump_stacks`, `draw_stacks` (a number of seconds: the guest call stack of every draw of two frames after that long), `audio_log`, `pcm_dump`, `force_vulkan_11`.

## Notes from bring-up

- A Windows checkout turns libmspack's `cabextract/mspack` symlinks into text files, so the SDK builds lzxd.c from `libmspack/mspack`.
- bionic has no `getcontext`/`makecontext`/`swapcontext` on arm64. `fiber_posix.cpp` has a small assembly switch for Android instead, which also keeps FPCR per fiber.
- bionic has no robust mutexes. Android takes the same path as macOS, so a thread that dies holding a condition's mutex is not recovered.
- Nothing calls the SDK's `AndroidInitialize` functions, and without them shared memory falls back to `/dev/ashmem`, which is gone since API 29. They now run when the library loads.
- `Memory::EnablePhysicalMemoryAccessCallbacks` arms only the view a page was committed through. A write through another alias of the page does not fault.
- `PhysicalHostOffset` in `xmemory.h` assumes Linux has 4 KB pages, and Android counts as Linux. 16 KB-page devices will need a runtime check there; only 4 KB devices have been tried.
- `gpu_hooks.cpp` and `game_natives.cpp` hard-coded the Windows +0x1000 for addresses from 0xE0000000. On Linux the view already includes it (`PhysicalHostOffset`), so GPU callbacks were written 4 KB off and never ran: the movie player's frame queue filled and it waited forever.
- A guest thread that suspends itself after another thread has already resumed it must not sleep: on Windows the count wraps and cancels out, and the POSIX path now does the same.
- `harness/trace_hooks.cpp` (`BF2_TRACE=<s>`), `src/thread_dump.cpp` (`BF2_DUMP_AFTER=<s>`) and `tools/gen_calls.py` are how those two were found: debuggerd needs root on a release phone.
- glslang ignores `[[vk::binding]]` under HLSL register mapping, and shifts a ByteAddressBuffer as a texture, not as a storage buffer. `tools/spv_bindings.py` prints where things landed.
- The device takes 960 samples (3.75 of the game's frames) at a time. Asked for them in one burst, the game's mixer answers many with a frame of exact silence: about 40% of frames in the menus and the mission, whatever the frame rate. `audio_system.cpp` now keeps its callbacks about 5 ms apart, which leaves about 0.2%. `audio_log` reports how much the output starved.
- The SDK's timer queue, which fires the guest's kernel timers, waited with disruptorplus's spin wait: spinning, yielding and 1 ms naps, about 20% of a core on the Mali phone even with nothing due. `patches/sdk-0002-timer-queue-wait.patch` waits on a condition variable instead. disruptorplus's own blocking strategy can't be used, because it passes `wait_until`'s arguments in the wrong order.
- `build_harness.ps1` leaves `$ErrorActionPreference` alone: CMake writes warnings to stderr, and `Stop` turns them into failures.
