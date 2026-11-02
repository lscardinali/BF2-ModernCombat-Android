# Dot-source in a PowerShell session: . tools/env.ps1
# Puts git, the Android SDK's CMake/Ninja and adb, and the NDK on PATH.
$Root = Split-Path -Parent $PSScriptRoot
$AndroidSdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
$Ndk = Get-ChildItem "$AndroidSdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
$SdkCMake = Get-ChildItem "$AndroidSdk\cmake" -Directory | Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
$env:Path = "C:\Program Files\Git\cmd;$AndroidSdk\platform-tools;$SdkCMake\bin;$env:Path"
$env:ANDROID_NDK = $Ndk
