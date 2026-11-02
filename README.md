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
| APK shell: import, audio, gamepads | todo |
| Vulkan renderer | todo |

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
| `android/` | the Android Studio project, still blank |

## Build and run

Needs Git, Android Studio (SDK, NDK 30.0.16248370, CMake 4.1.2) and Python 3.

```powershell
. tools/env.ps1
tools/fetch_deps.ps1
tools/build_harness.ps1
tools/run_smoke.ps1
```

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
- `build_harness.ps1` leaves `$ErrorActionPreference` alone: CMake writes warnings to stderr, and `Stop` turns them into failures.
