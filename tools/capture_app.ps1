# Builds and installs the APK, has the renderer capture one frame (user/capture), and brings back its log, its images as
# PNGs (tools/capture_to_png.py) and a screenshot into out/runs/<name>.
#   . tools/env.ps1; tools/capture_app.ps1 -Frame 400 -Seconds 25 [-Name cap] [-Script pads/menus.txt]
param([int]$Frame = 400, [int]$Seconds = 25, [string]$Name = 'cap', [string]$Script = '')
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
cmd /c "cd /d $Root\android && call $Root\android\gradlew.bat assembleDebug --console=plain > $Root\out\gradle.log 2>&1"
if ($LASTEXITCODE) {
    Write-Host "gradle failed"
    Get-Content "$Root\out\gradle.log" | Select-String ': error:|What went wrong' | Select-Object -First 12 | ForEach-Object { $_.Line }
    exit 1
}
$p = 'com.lscardinali.bf2mc'
adb install -r -t "$Root\android\app\build\outputs\apk\debug\app-debug.apk" | Out-Null
adb shell "am force-stop $p; echo $Frame > /data/local/tmp/bf2mc/capture; run-as $p cp /data/local/tmp/bf2mc/capture files/user/capture; run-as $p sh -c 'rm -f files/user/capture_* files/user/shaders/*.hlsl'"
& "$PSScriptRoot\run_app.ps1" -Name $Name -Seconds $Seconds -Script $Script -NoInstall *> "$Root\out\run.log"
$o = "$Root\out\runs\$Name"
Remove-Item "$o\capture_*" -ErrorAction SilentlyContinue
adb shell "run-as $p ls files/user | grep capture_" | ForEach-Object {
    $n = $_.Trim()
    adb shell "run-as $p cat files/user/$n > /data/local/tmp/bf2mc/$n"
    adb pull -q "/data/local/tmp/bf2mc/$n" "$o\$n"
    adb shell "rm /data/local/tmp/bf2mc/$n"
}
New-Item -ItemType Directory -Force "$o\hlsl" | Out-Null
adb shell "run-as $p ls files/user/shaders | grep -E '^(vs|ps)_.*hlsl'" | ForEach-Object {
    $n = $_.Trim()
    adb shell "run-as $p cat files/user/shaders/$n" | Set-Content "$o\hlsl\$n"
}
py -3 -I "$PSScriptRoot\capture_to_png.py" $o
