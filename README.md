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
| Recompile `default.xex`, compile the generated code for arm64 | todo, needs the disc |
| Headless harness on device: reach the first mission, measure CPU-only speed | todo |
| APK shell: import, audio, gamepads | todo |
| Vulkan renderer | todo |

## Layout

| Path | What |
|---|---|
| `tools/fetch_deps.ps1` | clones the SDK (bd833a2) and the PC port (f2b236b) into `third_party/`, applies the patches, downloads the SDK's Windows package for its recompiler |
| `tools/build_harness.ps1` | builds the adb executables for arm64 |
| `tools/run_smoke.ps1` | runs the smoke test on the connected device |
| `harness/` | the adb executables: `smoke.cpp` for now |
| `patches/sdk-*.patch` | this port's changes to the SDK, on top of the PC port's FPSCR patch |
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
- `build_harness.ps1` leaves `$ErrorActionPreference` alone: CMake writes warnings to stderr, and `Stop` turns them into failures.
