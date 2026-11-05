# Battlefield 2: Modern Combat for Android

A native Android port of **Battlefield 2: Modern Combat** (Xbox 360, 2006). There is no emulator. The game's PowerPC code is recompiled to C++ on your PC, compiled for arm64 with the Android NDK, and drawn with Vulkan.

> **Credits.** This port stands on two projects. Thank you to their authors.
>
> - **[bf2-modern-combat-pc](https://github.com/batatinhafcbatatinha-pixel/bf2-modern-combat-pc)** by [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel) (MIT): the PC port this one is built from. It provides the game's hooks, the Xenos shader translator, textures, audio and XMA, and the function manifest the recompiler needs. This repository adds the Android host, a Vulkan renderer for phones, and patches on top.
> - **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** by Tom Clay and contributors (BSD-3-Clause), derived from **[Xenia](https://github.com/xenia-project/xenia)** by Ben Vanik and contributors: the static recompiler and the runtime.
>
> Other libraries and their licenses are listed in [NOTICE.md](NOTICE.md).

## You need your own game

This repository holds no game code, assets or executables. To build and play you need your own **Xbox 360 USA disc image** of the game. Its `default.xex` has the SHA-256 `a2e791b5…5484`; the build and the app both check it.

Every APK you build contains code recompiled from the game, so **the APK is for you alone: don't share it, upload it or attach it to issues.** For the same reason there are no public releases. See [NOTICE.md](NOTICE.md).

Battlefield 2: Modern Combat belongs to Electronic Arts and DICE. This project is not affiliated with or endorsed by EA, DICE or Microsoft.

## Status

| | |
|---|---|
| Boot, intro movies, menus, campaign missions | working |
| Vulkan renderer (Vulkan 1.3, or 1.1/1.2 with `VK_KHR_dynamic_rendering`) | working |
| Sound and music, movies | working |
| Game controllers (anything SDL knows: Xbox, PlayStation, Switch, generic) | working |
| Home screen, game import from the disc image, settings | working |
| Leaving the app and coming back, mid-mission too; saves | working |
| Performance hints (Android 13+), Game Mode | working |
| Touch controls for play | not yet: touch only does A (one finger) and START (two fingers), enough for menus. Play with a controller. |
| Multiplayer | not supported |

Tested on a Redmi phone (Unisoc T7250, Mali-G57, 3 GB, Android 15): menus at 30 fps, the first mission at about 6 to 11 fps at half resolution, limited by the GPU. The target is Snapdragon 865 and newer, which hasn't been tried yet. Reports from other phones are very welcome.

**Phone requirements:** arm64, Android 12 or newer, Vulkan 1.1 or newer, and about 4.5 GB free for the game files.

## Build

On Windows, with:

- Git and Python 3
- Android Studio, with the Android SDK, NDK 30.0.16248370 and CMake 4.1.2 from its SDK Manager
- your disc image, as an `.iso` in the repository folder

```powershell
. tools/env.ps1
tools/fetch_deps.ps1     # clones the SDK and the PC port into third_party/, applies patches/, downloads the recompiler
tools/recompile.ps1      # extracts the disc into game/, writes the game's C++ into out/recomp (takes a while)
cd android
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
./gradlew assembleRelease   # or assembleDebug, or open android/ in Android Studio
```

The APK lands in `android/app/build/outputs/apk/`. A release build is shrunk with R8 and its native libraries are stripped (about 50 MB). It is signed when a keystore is set up (see [Releases](#releases)), and otherwise left unsigned. A debug build keeps the symbols, for stack dumps.

`game/`, `out/` and `third_party/` are gitignored. Keep it that way.

## Installing the game on the phone

The app opens on a home screen. With no game installed, it asks for the disc image through the system file picker. It checks `default.xex`, then extracts what the campaign uses (about 4 GB) into the app's own storage. The multiplayer data stays behind, and the `.iso` can be deleted afterwards.

## Playing

- **Controllers:** Bluetooth or USB. Every pad is player 1.
- **Settings** (home screen): **Fill the screen** stretches the 16:9 picture to the edges of a wider phone. It applies the next time the game starts.
- **Saves:** the game's own autosave and SAVE CAMPAIGN (between missions; the game doesn't save mid-mission). Saves are in the app's storage under `user/`, and Android's backup and device transfer take them (about 30 KB), but not the game files.
- **Leaving the app:** the game pauses. While it's away a "Paused" notification keeps Android from killing it early, so the mission is still there when you come back. Tap the notification or the app to return.

## How it works

| Path | What |
|---|---|
| `tools/fetch_deps.ps1` | clones the SDK (bd833a2) and the PC port (f2b236b) into `third_party/`, applies `patches/`, downloads the SDK's recompiler |
| `tools/recompile.ps1` | extracts the disc into `game/` and writes the generated C++ into `out/recomp` |
| `cmake/bf2mc.cmake` | the SDK, the game code and the PC port's sources, shared by the app, the harness and the Windows build |
| `android/` | the app. `app/src/main/cpp` is the native host: SDL3 main, pads, the Vulkan renderer and the performance hints. `app/src/main/java` has the home screen, the importer, settings and the keep-alive service. |
| `patches/sdk-*.patch`, `patches/bf2-*.patch` | this port's changes to the SDK (on top of the PC port's FPSCR patch) and to the PC port |
| `src/` | stand-ins for the PC port's settings screen and for Win32 |
| `harness/`, `tools/build_harness.ps1`, `tools/run_smoke.ps1`, `tools/run_harness.ps1` | adb executables: a smoke test of the runtime, and the game with a null renderer |
| `windows/`, `tools/build_windows.ps1`, `tools/run_windows.ps1` | the app's own sources built for Windows, for testing with a debugger |
| `tools/run_app.ps1`, `tools/capture_app.ps1` | install and run the APK; capture one frame's draws and render targets |

**Renderer.** The game issues Direct3D 9-style calls on the Xbox 360's Xenos GPU. The PC port's hooks catch them, and its translator turns the game's shader microcode into HLSL. On the phone, glslang compiles that HLSL to SPIR-V, and `vk_renderer.cpp` draws with dynamic rendering. Presenting runs on its own thread, so waiting for the GPU doesn't hold the game.

**Device tiers.** With 4 GB of memory or less, the renderer draws at half resolution, caps mipmapped textures at 256, keeps unused textures under 192 MB, and uses cheaper shaders and filtering, with bloom off. Above 4 GB it runs at full resolution, with textures up to 1024 and a 768 MB budget. `user/scale` and `user/max_texture` in the app's files override the first two.

**In the background.** Android takes the window away and SDL blocks its main thread. An event watch in `app_main.cpp` stops the audio mixer and the vblank interrupt, and the present thread holds the next frame, so the game thread stops in `BeginFrame`. While it's away the process uses about 20% of one core, down from about 130%. On return, the present thread makes a new Vulkan surface and swapchain. `KeepAliveService` is a foreground service while the game is away, which moves the process from the cached class Android kills first to perceptible (`oom_score_adj` 50).

**Performance hints.** From Android 13, `perf_hint.cpp` reports each frame to ADPF: the larger of the game update's CPU time (`sub_82298D20`) and the render thread's, against the game's 33.3 ms. The scheduler then sets clocks and cores ahead of time instead of guessing. The API links weakly and every call is behind `__builtin_available`, so Android 12 just skips it. `user/no_perf_hint` turns it off for comparisons. The manifest declares the app a game for Game Mode.

## Releases

`.github/workflows/release.yml` builds the signed APK and attaches it to a **draft** release, visible only to people with write access to the repository. The APK contains the game's code, so the workflow runs on a self-hosted runner on your own PC, where your game files are, never on GitHub's machines. Don't publish the drafts.

**Signing.** Make a keystore once, keep it, and back it up. Losing it means updates can't be installed over existing installs.

```powershell
mkdir signing
keytool -genkeypair -keystore signing/release.jks -storetype PKCS12 -alias bf2mc -keyalg RSA -keysize 4096 -validity 10000
```

(`keytool` is in `C:\Program Files\Android\Android Studio\jbr\bin`.) Then write `signing/keystore.properties`:

```properties
storeFile=release.jks
storePassword=...
keyAlias=bf2mc
keyPassword=...
```

`signing/` is gitignored. Local release builds sign with it. For the workflow, add four repository secrets (Settings → Secrets and variables → Actions): `ANDROID_KEYSTORE_BASE64` (the keystore, base64: `[Convert]::ToBase64String([IO.File]::ReadAllBytes('signing/release.jks'))`), `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD`.

**Runner.** On the PC that builds, once the game is recompiled (`game/` exists):

1. On GitHub, open the repository's Settings → Actions → Runners → New self-hosted runner, and copy the token from the `config.cmd` line. It lasts an hour.
2. Run `tools/setup_runner.ps1 -Token <token>`. It downloads GitHub's runner into `D:\actions-runner-bf2mc`, checks its SHA-256 against the one GitHub publishes, and registers it with the label `bf2mc`. It also sets `BF2_GAME_DIR` (the folder with `default.xex`, `game/` by default) and `JAVA_HOME` for its jobs.
3. Start `D:\actions-runner-bf2mc\run.cmd` whenever you want releases built.
4. A public repository lets anyone open pull requests, and a pull request can bring its own workflow. Set **Settings → Actions → General → Fork pull request workflows → Require approval for all external contributors**, so nothing runs on your PC without your approval. The release workflow itself only runs for tags you push and runs you start yourself.

Push a tag to build: `git tag v1.0.0 && git push origin v1.0.0`. You can also run it from the Actions tab with a version number. The first run fetches the SDK and recompiles the game; later runs reuse both.

## Debugging

Files in the app's `files/user` folder switch these on: `gpu_times` (GPU time per pass, every 150 frames), `capture` (a frame number: its draws logged and its targets dumped, see `tools/capture_app.ps1`), `dump_stacks`, `draw_stacks` (a number of seconds: the guest call stack of every draw in two frames after that long), `audio_log`, `pcm_dump`, `force_vulkan_11`, `no_perf_hint`. `user/script.txt` is a scripted pad, as `pads/mission.txt` shows.

**Windows test build.** `windows/CMakeLists.txt` builds the app's own sources into `out/windows/bf2mc.exe`, so the renderer can be tested on a PC under a debugger. It needs Visual Studio's C++ tools, LLVM in `C:\Program Files\LLVM` and the Android SDK's CMake and Ninja.

```powershell
tools/build_windows.ps1
tools/run_windows.ps1 -Name mission -Seconds 120 -Script pads/win_mission.txt -Shots 70,90 -Debugger
```

Results land in `out/runs/<Name>`. The scripted pad re-reads `out/windows/user/script.txt` when it grows, so lines appended while the game runs drive it by hand (`pads/win_menu.txt` stops at the title). `REX_LOG_LEVEL=debug` logs the kernel calls. `-Debugger` runs the game under cdb from the Windows SDK: `cdb.txt` has the stacks, with functions named `sub_<guest address>`, and the guest registers at a crash. `-CdbExtra 'ba w4 0x18284FB00 "kn 14; g";'` logs every write to a guest address (guest memory starts at host `0x100000000`). `-Validate` turns on the Vulkan validation layer, which needs the Vulkan SDK.

## Notes from bring-up

- A Windows checkout turns libmspack's `cabextract/mspack` symlinks into text files, so the SDK builds lzxd.c from `libmspack/mspack`.
- bionic has no `getcontext`/`makecontext`/`swapcontext` on arm64. `fiber_posix.cpp` has a small assembly switch for Android instead, which also keeps FPCR per fiber.
- bionic has no robust mutexes. Android takes the same path as macOS, so a thread that dies holding a condition's mutex isn't recovered.
- Nothing called the SDK's `AndroidInitialize` functions, and without them shared memory falls back to `/dev/ashmem`, which is gone since API 29. They now run when the library loads.
- `Memory::EnablePhysicalMemoryAccessCallbacks` arms only the view a page was committed through. A write through another alias of the page doesn't fault.
- `PhysicalHostOffset` in `xmemory.h` assumes Linux has 4 KB pages, and Android counts as Linux. Phones with 16 KB pages will need a runtime check there; only 4 KB devices have been tried.
- `gpu_hooks.cpp` and `game_natives.cpp` hard-coded the Windows +0x1000 for addresses from 0xE0000000. On Linux the view already includes it, so GPU callbacks were written 4 KB off and never ran: the movie player's frame queue filled and it waited forever.
- A guest thread that suspends itself after another thread has already resumed it must not sleep. On Windows the count wraps and cancels out, and the POSIX path now does the same.
- `harness/trace_hooks.cpp` (`BF2_TRACE=<s>`), `src/thread_dump.cpp` (`BF2_DUMP_AFTER=<s>`) and `tools/gen_calls.py` are how the last two were found: debuggerd needs root on a release phone.
- glslang ignores `[[vk::binding]]` under HLSL register mapping, and shifts a ByteAddressBuffer like a texture rather than a storage buffer. `tools/spv_bindings.py` prints where things landed.
- The audio device takes 960 samples (3.75 of the game's frames) at a time. When the game's mixer was asked for them in one burst, it answered many with a frame of exact silence: about 40% of frames. `audio_system.cpp` keeps its callbacks about 5 ms apart, which brings that down to about 0.2%. `audio_log` reports how often the output starved.
- The SDK's timer queue, which fires the guest's kernel timers, waited by spinning: spin, yield and 1 ms naps, about 20% of a core with nothing due. `patches/sdk-0002-timer-queue-wait.patch` waits on a condition variable instead. disruptorplus's own blocking strategy can't be used, because it passes `wait_until`'s arguments in the wrong order.

## Contributing

**Pull requests are welcome**: fixes, performance work, touch controls, and reports from phones that haven't been tried (Adreno especially). Please:

- never include game files, generated code, APKs, or captures and dumps taken from the game
- say which phone and Android version you tested on
- keep changes to the SDK or the PC port as patches in `patches/`

Issues for bugs and test reports are just as welcome. A log from the app's `files/user/logs` helps most.

## License

This project's own code is under the [MIT License](LICENSE). The projects it builds on keep their own licenses: see [NOTICE.md](NOTICE.md). None of this grants any right to the game itself.
