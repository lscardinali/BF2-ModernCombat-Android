# Notices

## What this repository is, and what it is not

This repository is source code and build scripts only. It contains no game assets, no game executable (`default.xex`), no code generated from the game, and no SDK binaries. `tools/fetch_deps.ps1` downloads the dependencies below, and `tools/recompile.ps1` generates the game code on your own PC from your own disc.

An APK built from it is different. The game's PowerPC code is recompiled into C++ and compiled into `libmain.so`, so every APK contains code derived from Electronic Arts' game. **Do not share APKs, the generated code or the game files.** That holds for GitHub releases, workflow artifacts, issues and pull requests too. The release workflow only makes draft releases, which are visible to people with write access to the repository (see the README).

Battlefield 2: Modern Combat, its code, trademarks and assets belong to Electronic Arts and DICE. This project is not affiliated with or endorsed by Electronic Arts, DICE or Microsoft. Xbox and Xbox 360 are trademarks of Microsoft.

## This project

This project's own code, scripts and patches are under the MIT License (`LICENSE`). The patches in `patches/` change the projects below, and those changes are offered under the license of the project they change.

## Projects it builds on

| Project | Use | License |
|---|---|---|
| [bf2-modern-combat-pc](https://github.com/batatinhafcbatatinha-pixel/bf2-modern-combat-pc) by batatinhafcbatatinha-pixel | the PC port this one is built from: the game's hooks, the Xenos shader translator, textures, audio and XMA, and the function manifest the recompiler uses | MIT |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay and contributors | the static recompiler and the runtime: kernel, memory, threads, file system, input | BSD-3-Clause |
| [Xenia](https://github.com/xenia-project/xenia) by Ben Vanik and contributors | the SDK is derived from it | BSD-3-Clause |
| [SDL 3](https://github.com/libsdl-org/SDL) | window, Vulkan surface, pads, touch, audio, the Android activity | zlib |
| [glslang](https://github.com/KhronosGroup/glslang) | HLSL to SPIR-V on the phone | BSD-3-Clause and others, see its LICENSE.txt |
| [FFmpeg](https://ffmpeg.org) (Xenia's fork) | XMA audio and the movies, through `libavcodec` and `libavutil` | LGPL-2.1-or-later |
| [libmspack](https://www.cabextract.org.uk/libmspack/) | LZX decompression in the SDK | LGPL-2.1 |
| [SIMDe](https://github.com/simd-everywhere/simde) | the recompiled code's x86 vector operations on ARM | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | texture and shader hashes | BSD-2-Clause |
| [{fmt}](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog), [toml++](https://github.com/marzer/tomlplusplus), [disruptorplus](https://github.com/lewissbaker/disruptorplus) | formatting, logging, settings, the timer queue | MIT |
| [UTF8-CPP](https://github.com/nemtrif/utfcpp) | text conversion | Boost Software License 1.0 |
| [Vulkan headers](https://github.com/KhronosGroup/Vulkan-Headers) | the Vulkan API | Apache-2.0 / MIT |
| AndroidX Core and Activity | the home and settings screens | Apache-2.0 |

The full license texts are in each project's checkout under `third_party/`, which `tools/fetch_deps.ps1` creates.

FFmpeg and libmspack are under the LGPL. An APK you build links them statically into its native libraries. If you ever give an APK to anyone, the LGPL also asks you to let them rebuild it against a changed copy of those libraries. This repository's build scripts are the means to do that.
