# Pushes the harness and runs the game on the device; the log comes back to out/runs/<name>/log.txt.
#   . tools/env.ps1; tools/build_harness.ps1 -Target bf2mc; tools/push_game.ps1 (once)
#   tools/run_harness.ps1 [-Name boot] [-Seconds 120] [-Script pads/menus.txt]
param([string]$Name = 'boot', [int]$Seconds = 120, [string]$Script = '')
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Dir = '/data/local/tmp/bf2mc'
$Runtime = "$Root\third_party\rexglue-sdk\out\linux-arm64\librexruntime.so"
$Cxx = "$env:ANDROID_NDK\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
adb push -q "$Root\out\harness-android\bin\bf2mc" $Runtime $Cxx "$Dir/"
$Env = "LD_LIBRARY_PATH=$Dir"
if ($Script) {
    adb push -q "$Root\$Script" "$Dir/script.txt"
    $Env += " BF2_INPUT=$Dir/script.txt"
}
$Out = "$Root\out\runs\$Name"
New-Item -ItemType Directory -Force $Out | Out-Null
adb logcat -c
adb shell "chmod +x $Dir/bf2mc; cd $Dir; rm -rf user/logs; $Env timeout $($Seconds + 15) ./bf2mc --game_data_root=$Dir/game --user_data_root=$Dir/user --seconds=$Seconds; echo exit `$?" > "$Out\stdout.txt" 2>&1
adb logcat -d -b crash > "$Out\crash.txt"
adb logcat -d -s DEBUG:* libc:* > "$Out\tombstone.txt"
Get-Content "$Out\stdout.txt" -Tail 40
