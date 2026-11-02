# Clones the ReXGlue SDK and the BF2:MC PC port at the tested commits, applies the patches and downloads the SDK's
# prebuilt Windows package (its rexglue.exe writes the game's C++).
#   . tools/env.ps1; tools/fetch_deps.ps1
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$TP = "$Root\third_party"
New-Item -ItemType Directory -Force $TP | Out-Null

function Checkout($url, $dir, $commit) {
    if (-not (Test-Path "$dir\.git")) {
        git init -q $dir
        git -C $dir config core.autocrlf false
        git -C $dir remote add origin $url
    }
    git -C $dir fetch -q --depth 1 origin $commit
    git -C $dir checkout -q --force FETCH_HEAD
    git -C $dir clean -qfdx -e generated -e out
}

# bf2-modern-combat-pc: MIT, the port this builds on (hooks, renderer, audio)
Checkout https://github.com/batatinhafcbatatinha-pixel/bf2-modern-combat-pc "$TP\bf2-modern-combat-pc" f2b236b20c36deb74a7aaba1b45c8c50b8aea1e5
# rexglue-sdk: BSD-3, at the nightly the port pins (kit.json)
Checkout https://github.com/rexglue/rexglue-sdk "$TP\rexglue-sdk" bd833a2ab69d305f99ddf57418fc38ffec8af93d
# the submodules an Android build uses; catch2, moltenvk, vulkan-loader, dxc and tracy's server are left out
$subs = 'cli11','libmspack','FFmpeg','tomlplusplus','simde','xxHash','spdlog','fmt','utfcpp','imgui','sdl3','inja',
        'o1heap','vulkan-headers','vulkan-memory-allocator','spirv-headers','spirv-tools','glslang','tracy'
foreach ($s in $subs) {
    git -C "$TP\rexglue-sdk" -c core.autocrlf=false submodule update --init --depth 1 "thirdparty/$s"
}
# the port's one change to the SDK, then this project's Android changes
git -C "$TP\rexglue-sdk" apply --whitespace=nowarn "$TP\bf2-modern-combat-pc\patches\rexglue-fpscr-mask.patch"
foreach ($p in Get-ChildItem "$Root\patches\sdk-*.patch" -ErrorAction SilentlyContinue | Sort-Object Name) {
    git -C "$TP\rexglue-sdk" apply --whitespace=nowarn $p.FullName
}
foreach ($p in Get-ChildItem "$Root\patches\bf2-*.patch" -ErrorAction SilentlyContinue | Sort-Object Name) {
    git -C "$TP\bf2-modern-combat-pc" apply --whitespace=nowarn $p.FullName
}

# the recompiler itself, the same commit as the sources above
$kit = Get-Content "$TP\bf2-modern-combat-pc\kit.json" -Raw | ConvertFrom-Json
$host_sdk = "$Root\out\sdk-host"
if (-not (Test-Path "$host_sdk\win-amd64")) {
    New-Item -ItemType Directory -Force $host_sdk | Out-Null
    $zip = "$host_sdk\sdk.zip"
    curl.exe -fsSL -o $zip $kit.sdk.url
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $kit.sdk.sha256.ToUpper()) { throw "SDK package hash mismatch" }
    Expand-Archive $zip $host_sdk
    Remove-Item $zip
}
Write-Host "third_party ready"
