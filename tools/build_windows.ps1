# Builds out/windows/bf2mc.exe with Clang and the Visual Studio libraries, from the same sources as the Android app.
#   tools/build_windows.ps1
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
$Llvm = 'C:\Program Files\LLVM\bin'
$Sdk = "$Root\out\sdk-host\win-amd64"
$Out = "$Root\out\windows"
New-Item -ItemType Directory -Force $Out | Out-Null

# the port's one change to the SDK's headers (kit.json), which the Android build gets from its patch
$kit = Get-Content "$Root\third_party\bf2-modern-combat-pc\kit.json" -Raw | ConvertFrom-Json
foreach ($p in $kit.sdk.patches) {
    $header = "$Sdk\$($p.file)"
    $text = [IO.File]::ReadAllText($header)
    if (-not $text.Contains($p.new)) {
        if (-not $text.Contains($p.old)) { throw "cannot apply $($p.name) to $header" }
        $at = $text.IndexOf($p.old)
        [IO.File]::WriteAllText($header, $text.Remove($at, $p.old.Length).Insert($at, $p.new))
    }
}

# the Vulkan SDK isn't needed: an import library for the driver loader Windows already has
$VulkanLib = "$Out\vulkan-1.lib"
if (-not (Test-Path $VulkanLib)) {
    $names = & "$Llvm\llvm-readobj.exe" --coff-exports C:\Windows\System32\vulkan-1.dll |
        Select-String '^\s*Name: (vk\w+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    @('LIBRARY vulkan-1.dll', 'EXPORTS') + $names | Set-Content "$Out\vulkan-1.def" -Encoding ascii
    & "$Llvm\llvm-dlltool.exe" -m i386:x86-64 -D vulkan-1.dll -d "$Out\vulkan-1.def" -l $VulkanLib
    if ($LASTEXITCODE) { throw "no Vulkan import library" }
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$c = ($Llvm -replace '\\', '/')
# in a batch file: vcvars needs cmd, and PowerShell 5.1 stops at the first line a tool writes to stderr
$env:Path = "$(Split-Path $vswhere);$env:Path"
@"
@call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S "$Root\windows" -B "$Out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$c/clang.exe" -DCMAKE_CXX_COMPILER="$c/clang++.exe" -DCMAKE_PREFIX_PATH="$($Sdk -replace '\\', '/')" -DBF2_VULKAN_LIB="$($VulkanLib -replace '\\', '/')" || exit /b 1
cmake --build "$Out" --target bf2mc
"@ | Set-Content "$Out\build.cmd" -Encoding ascii
$ErrorActionPreference = 'Continue'
cmd /c "`"$Out\build.cmd`" > `"$Root\out\windows-build.log`" 2>&1"
if ($LASTEXITCODE) { throw "build failed, see out/windows-build.log" }
