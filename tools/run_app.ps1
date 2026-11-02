# Installs the debug APK, copies the game into the app's files the first time (run-as; the harness's copy on the device
# is the source), starts it and brings back its log and a screenshot.
#   . tools/env.ps1; tools/run_app.ps1 [-Name run] [-Seconds 60] [-Script pads/menus.txt] [-NoInstall]
param([string]$Name = 'app', [int]$Seconds = 60, [string]$Script = '', [switch]$NoInstall)
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Pkg = 'com.lscardinali.bf2mc'
$Out = "$Root\out\runs\$Name"
New-Item -ItemType Directory -Force $Out | Out-Null
if (-not $NoInstall) {
    adb install -r -t "$Root\android\app\build\outputs\apk\debug\app-debug.apk"
    if ($LASTEXITCODE) { throw "install failed" }
}
adb shell "am force-stop $Pkg"
$have = (adb shell "run-as $Pkg ls files/game/default.xex 2>/dev/null")
if (-not $have) {
    Write-Host "copying the game into the app's files..."
    adb shell "run-as $Pkg mkdir -p files; run-as $Pkg cp -r /data/local/tmp/bf2mc/game files/"
}
adb shell "run-as $Pkg mkdir -p files/user; run-as $Pkg rm -rf files/user/logs files/user/stacks.txt files/user/script.txt"
if ($Script) {
    adb push -q "$Root\$Script" /data/local/tmp/bf2mc/script.txt
    adb shell "run-as $Pkg cp /data/local/tmp/bf2mc/script.txt files/user/script.txt"
}
adb logcat -c
adb shell "am start -n $Pkg/.MainActivity" | Out-Null
Start-Sleep $Seconds
adb shell "screencap -p /data/local/tmp/bf2mc/screen.png"; adb pull -q /data/local/tmp/bf2mc/screen.png "$Out\screen.png"
adb logcat -d -b crash > "$Out\crash.txt"
adb logcat -d -s SDL:* vulkan:* AndroidRuntime:* DEBUG:* libc:* > "$Out\logcat.txt"
adb shell "run-as $Pkg cat files/user/logs/bf2mc_001.log" > "$Out\log.txt"
adb shell "run-as $Pkg cat files/user/stacks.txt 2>/dev/null" > "$Out\stacks.txt"
