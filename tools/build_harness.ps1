# Builds the adb executables for Android arm64 into out/harness-android/bin.
#   . tools/env.ps1; tools/build_harness.ps1 [-Target smoke]
param([string]$Target = 'smoke')
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Out = "$Root\out\harness-android"
if (-not (Test-Path "$Out\build.ninja")) {
    cmake -S "$Root\harness" -B $Out -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_TOOLCHAIN_FILE=$env:ANDROID_NDK\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DANDROID_STL=c++_shared
    if ($LASTEXITCODE) { throw "configure failed" }
}
cmake --build $Out --target $Target -- -k 0
if ($LASTEXITCODE) { throw "build failed" }
