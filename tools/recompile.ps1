# Extracts the disc (once), then translates default.xex with the SDK's rexglue.exe. The generated C++ goes into
# out/recomp/generated/default, next to the PC port's config it was made from.
#   . tools/env.ps1; tools/recompile.ps1 [-Iso "Battlefield 2 - Modern Combat (USA).iso"]
param([string]$Iso = '')
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Port = "$Root\third_party\bf2-modern-combat-pc"
if (-not (Test-Path "$Root\game\default.xex")) {
    if (-not $Iso) { $Iso = (Get-ChildItem $Root -Filter *.iso | Select-Object -First 1).FullName }
    py -3 -I "$Port\tools\extract_iso.py" $Iso --output "$Root\game"
    if ($LASTEXITCODE) { throw "extract failed" }
}
py -3 -I "$Port\tools\validate_game.py" "$Root\game"
if ($LASTEXITCODE) { throw "unsupported game files" }
$Out = "$Root\out\recomp"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($f in 'bf2mc_config.toml', 'bf2mc_hooks.toml') { Copy-Item "$Port\project\$f" $Out -Force }
$GameFiles = "$Root\game" -replace '\\', '/'
(Get-Content "$Port\project\bf2mc_manifest.toml" -Raw).Replace('@GAME_FILES@', $GameFiles) |
    Set-Content "$Out\bf2mc_manifest.toml" -NoNewline -Encoding ascii
Push-Location $Out
try { & "$Root\out\sdk-host\win-amd64\bin\rexglue.exe" codegen bf2mc_manifest.toml }
finally { Pop-Location }
if ($LASTEXITCODE) { throw "rexglue failed" }
