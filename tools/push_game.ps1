# Copies the game files the single player needs to the device, for the harness. bf2mc_mp is multiplayer (2.7 GB) and
# mpdefault.xex its executable: neither is used.
#   . tools/env.ps1; tools/push_game.ps1
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Dir = '/data/local/tmp/bf2mc/game'
adb shell "mkdir -p $Dir"
foreach ($f in 'default.xex', 'bfmc.ini', 'Data1.viv', '8', 'Audio', 'border', 'video') {
    adb push "$Root\game\$f" "$Dir/"
    if ($LASTEXITCODE) { throw "push of $f failed" }
}
