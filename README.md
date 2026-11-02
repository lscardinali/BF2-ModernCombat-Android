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
| Split screen in the campaign | experimental, off by default, see below |
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

**Split screen (experimental)** shows two soldiers side by side in the campaign, and player 2 takes over a teammate.

## Split screen

The game has one human player: one controller object (`0x8284F7EC`) whose `+56` is the soldier it drives, and the camera, HUD and input follow that soldier. `split_screen.cpp` hands the controller to the other player's soldier on every update, through the end of the game's own hotswap, and the renderer keeps each frame for its side. The first pad drives player 1 and the second pad player 2, each with their own copy of the game's pad state. Each player gets half the frame rate. `user/split_script_player` (0 or 1) gives the scripted pad to one player, for tests without a second pad.

Known problems:

- In the test runs, with nobody playing player 2, a player's soldier was lost within seconds to a few minutes: no longer usable, probably dead, not confirmed. The next teammate takes over when the game offers one; until then the screen shows one player.
- Both halves can show the same soldier for many seconds, after a teammate has been replaced among other times. Reading the side when the frame starts did not cure it, and the cause is unknown.
- Controls feel half speed, as each player only gets every other update.
- First-person guns don't draw while the two views differ, and flipping every 30 updates shows them stuck mid-raise. Skipping `sub_821A2418`, `sub_8262F730`, `sub_821A6E20` and `sub_821A62A0` during the hand-over did not bring them back.
- These all come from handing one player back and forth, so this design is a proof of concept rather than a way to play.
- Not tried: two real pads, what the audio does, the level end.

### Spike: a real second player

Setting `BF2_SPLIT2` as well as turning split screen on tries a design without hand-overs, on the Windows build (`split_screen.cpp`). The game thread fills a frame packet (`0x8287A4FC`: +2452 written, +2448 drawn) and hands it to the render thread in `sub_82347190`. Running `sub_82298D20` a second time with no time passing builds a second packet of the same moment for the other half, and game time stays real. The camera object is at `[0x8284F8D8]+48`: +304 projection, +432 view, +624 world. `sub_8219B998` spawns a player, so a second controller from `sub_821C5928` gets a soldier of its own.

What it showed: first-person guns and animations are fine in the first view, the second view draws from its own camera, and both controllers update. It's not playable: no pad reaches the second controller yet, the second view borrows the first camera's rotation and shows no gun, both soldiers spawn in one spot, and the one run with the camera at the second soldier crashed in `sub_821A0208` (a null soldier) soon after the players died.

## Windows test build

`windows/CMakeLists.txt` builds the Android app's own sources (`android/app/src/main/cpp`) into `out/windows/bf2mc.exe`, so tests on a PC exercise the renderer and split screen the phone runs. It needs Visual Studio's C++ tools, LLVM in `C:\Program Files\LLVM` and the Android SDK's CMake and Ninja. Keep game files, the generated code and the build out of version control, as for Android.

```powershell
tools/build_windows.ps1
tools/run_windows.ps1 -Name split -Seconds 120 -Script pads/win_split.txt -Split -ScriptPlayer 1 -Shots 70,90 -Debugger
```

Results land in `out/runs/<Name>`. `-Debugger` runs it under cdb from the Windows SDK: `cdb.txt` has the stacks, named `sub_<guest address>`, and the guest registers of a crash. `-CdbExtra 'ba w4 0x18284FB00 "kn 14; g";'` logs every write to a guest address (guest memory starts at host `0x100000000`). `-Validate` asks for the Vulkan validation layer, which needs the Vulkan SDK installed.

## Device tiers

On phones with 4 GB of memory or less the renderer draws at half resolution, caps mipmapped textures at 256, evicts unused textures over 192 MB, uses plain multiplies and cheaper filtering, and bloom is off. Above 4 GB: full resolution, textures up to 1024, a 768 MB budget, everything on. `user/scale` and `user/max_texture` in the app's files override the first two. The target is Snapdragon 865 and newer; only the Mali-G57 phone has been tried.

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
- `build_harness.ps1` leaves `$ErrorActionPreference` alone: CMake writes warnings to stderr, and `Stop` turns them into failures.
