# Pushes the smoke test and the libraries it needs to the device and runs it.
#   . tools/env.ps1; tools/build_harness.ps1; tools/run_smoke.ps1
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Dir = '/data/local/tmp/bf2mc'
$Runtime = Get-ChildItem "$Root\third_party\rexglue-sdk\out\linux-arm64\librexruntime.so"
$Cxx = "$env:ANDROID_NDK\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
adb shell "mkdir -p $Dir"
adb push -q "$Root\out\harness-android\bin\smoke" $Runtime.FullName $Cxx "$Dir/"
adb shell "chmod +x $Dir/smoke; cd $Dir; LD_LIBRARY_PATH=$Dir ./smoke; echo exit `$?"
