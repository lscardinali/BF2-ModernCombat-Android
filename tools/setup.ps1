# One-click build of the APK from your own disc, for Build-APK.cmd. Asks before installing anything, logs to
# setup-log.txt, and can be run again after a failure: finished steps are skipped.
#   tools/setup.ps1 [-Iso <path to your .iso>] [-Yes] [-NoPhone] [-Recompile]
# -Yes answers yes to every question (Android SDK licenses included); -NoPhone skips installing on a phone over USB;
# -Recompile translates the game again even if the generated code is there.
param([string]$Iso = '', [switch]$Yes, [switch]$NoPhone, [switch]$Recompile)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
Start-Transcript -Path "$Root\setup-log.txt" -Append | Out-Null

$Sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
$Packages = 'platform-tools', 'platforms;android-37.0', 'build-tools;36.0.0', 'ndk;30.0.16248370', 'cmake;4.1.2'

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Ask($question) {
    if ($Yes) { return $true }
    return (Read-Host "$question [Y/n]") -notmatch '^\s*[nN]'
}
function RefreshPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
function Winget($id, $what) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw "$what is missing, and winget isn't there to install it: install $what yourself, then run this again" }
    if (-not (Ask "$what is missing. Install it with winget ($id)?")) { throw "$what is needed" }
    winget install --id $id -e --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -and $LASTEXITCODE -ne -1978335189) { throw "winget could not install $what" }  # that code: already installed
    RefreshPath
}
function Native($what, [scriptblock]$run) {
    $ErrorActionPreference = 'Continue'  # these tools write progress to stderr
    & $run
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code) { throw "$what failed (exit code $code), see setup-log.txt" }
}

try {
    Write-Host "Battlefield 2: Modern Combat for Android: builds the APK from your own disc image." -ForegroundColor Green
    Write-Host "Needs about 30 GB free and takes an hour or more the first time."

    Step "Free space"
    $free = (Get-PSDrive ($Root.Substring(0, 1))).Free / 1GB
    Write-Host ("{0:N0} GB free on {1}:" -f $free, $Root.Substring(0, 1))
    if ($free -lt 30 -and -not (Ask "Less than 30 GB free: the build may run out of space. Go on anyway?")) { return }

    Step "Git"
    RefreshPath
    if (-not (Get-Command git -ErrorAction SilentlyContinue) -and -not (Test-Path 'C:\Program Files\Git\cmd\git.exe')) { Winget 'Git.Git' 'Git' }

    Step "Python 3"
    $py = Get-Command py -ErrorAction SilentlyContinue
    if (-not $py) { Winget 'Python.Python.3.12' 'Python 3' }

    Step "Visual C++ runtime (for the recompiler)"
    if (-not (Test-Path "$env:SystemRoot\System32\vcruntime140.dll") -or -not (Test-Path "$env:SystemRoot\System32\msvcp140.dll")) {
        Winget 'Microsoft.VCRedist.2015+.x64' 'the Visual C++ runtime'
    }

    Step "Java (for the Android build)"
    $jdk = @($env:JAVA_HOME, 'C:\Program Files\Android\Android Studio\jbr') +
        @(Get-ChildItem 'C:\Program Files\Microsoft\jdk-21*' -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName) |
        Where-Object { $_ -and (Test-Path "$_\bin\java.exe") } | Select-Object -First 1
    if (-not $jdk) {
        Winget 'Microsoft.OpenJDK.21' 'a Java 21 JDK'
        $jdk = Get-ChildItem 'C:\Program Files\Microsoft\jdk-21*' -Directory | Select-Object -Last 1 -ExpandProperty FullName
    }
    $env:JAVA_HOME = $jdk
    Write-Host "using $jdk"

    Step "Android SDK, NDK and CMake"
    $sdkmanager = "$Sdk\cmdline-tools\latest\bin\sdkmanager.bat"
    if (-not (Test-Path $sdkmanager)) {
        if (-not (Ask "Download Google's Android command-line tools into $Sdk?")) { throw "the Android SDK is needed" }
        $index = [xml](Invoke-WebRequest -UseBasicParsing https://dl.google.com/android/repository/repository2-3.xml).Content
        $package = $index.'sdk-repository'.remotePackage | Where-Object { $_.path -eq 'cmdline-tools;latest' }
        $archive = $package.archives.archive | Where-Object { $_.'host-os' -eq 'windows' } | Select-Object -First 1
        $zip = "$env:TEMP\$($archive.complete.url)"
        Invoke-WebRequest -UseBasicParsing "https://dl.google.com/android/repository/$($archive.complete.url)" -OutFile $zip
        if ((Get-FileHash $zip -Algorithm SHA1).Hash -ne $archive.complete.checksum.'#text'.ToUpper()) { throw "the command-line tools' checksum doesn't match Google's index" }
        New-Item -ItemType Directory -Force "$Sdk\cmdline-tools" | Out-Null
        Remove-Item -Recurse -Force "$Sdk\cmdline-tools\latest", "$Sdk\cmdline-tools\cmdline-tools" -ErrorAction SilentlyContinue
        Expand-Archive $zip "$Sdk\cmdline-tools" -Force
        Rename-Item "$Sdk\cmdline-tools\cmdline-tools" 'latest'
        Remove-Item $zip
    }
    $missing = $Packages | Where-Object { -not (Test-Path ("$Sdk\" + ($_ -replace ';', '\'))) }
    if ($missing) {
        Write-Host "installing: $($missing -join ', ')"
        Write-Host "The Android SDK has licenses to accept. Read them; the build can't go on without them."
        if ($Yes) { Native 'accepting the licenses' { 1..20 | ForEach-Object { 'y' } | & $sdkmanager --sdk_root="$Sdk" --licenses } }
        else { Native 'the licenses' { & $sdkmanager --sdk_root="$Sdk" --licenses } }
        Native 'sdkmanager' { & $sdkmanager --sdk_root="$Sdk" @missing }
    }
    $env:ANDROID_HOME = $Sdk
    . "$PSScriptRoot\env.ps1"

    Step "The SDK and the PC port's sources"
    if (-not (Test-Path "$Root\third_party\rexglue-sdk\.git") -or -not (Test-Path "$Root\out\sdk-host\win-amd64\bin\rexglue.exe")) {
        Native 'fetch_deps' { & "$PSScriptRoot\fetch_deps.ps1" }
    } else { Write-Host "already there" }

    Step "Your game: extract and recompile"
    $generated = "$Root\out\recomp\generated\default\sources.cmake"
    if (-not (Test-Path "$Root\game\default.xex")) {
        if (-not $Iso) { $Iso = Get-ChildItem $Root -Filter *.iso | Select-Object -First 1 -ExpandProperty FullName }
        if (-not $Iso) {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.OpenFileDialog
            $dialog.Title = 'Your Battlefield 2: Modern Combat (USA) Xbox 360 disc image'
            $dialog.Filter = 'Disc images (*.iso)|*.iso|All files (*.*)|*.*'
            if ($dialog.ShowDialog() -ne 'OK') { throw "no disc image chosen" }
            $Iso = $dialog.FileName
        }
        Write-Host "disc image: $Iso"
    }
    if ($Recompile -or -not (Test-Path "$Root\game\default.xex") -or -not (Test-Path $generated)) {
        Native 'recompile' { & "$PSScriptRoot\recompile.ps1" -Iso $Iso }
    } else { Write-Host "already recompiled (-Recompile does it again)" }

    Step "Signing key"
    if (-not (Test-Path "$Root\signing\keystore.properties")) {
        New-Item -ItemType Directory -Force "$Root\signing" | Out-Null
        $bytes = New-Object byte[] 24
        [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $password = [Convert]::ToBase64String($bytes) -replace '[+/=]', 'x'
        Native 'keytool' { & "$jdk\bin\keytool.exe" -genkeypair -keystore "$Root\signing\release.jks" -storetype PKCS12 -alias bf2mc -keyalg RSA -keysize 4096 -validity 10000 -storepass $password -keypass $password -dname "CN=BF2-ModernCombat-Android local build" }
        Set-Content "$Root\signing\keystore.properties" "storeFile=release.jks`nstorePassword=$password`nkeyAlias=bf2mc`nkeyPassword=$password" -Encoding ascii
        Write-Host "made signing\release.jks. Keep the signing folder: later builds must use the same key to install over this one." -ForegroundColor Yellow
    } else { Write-Host "using signing\keystore.properties" }

    Step "Building the APK (the first build compiles the whole game: expect a long wait)"
    Push-Location "$Root\android"
    try { Native 'the build' { .\gradlew.bat assembleRelease --console=plain } } finally { Pop-Location }
    $apk = "$Root\BF2-ModernCombat-Android.apk"
    Copy-Item "$Root\android\app\build\outputs\apk\release\app-release.apk" $apk -Force
    Write-Host "APK: $apk" -ForegroundColor Green

    Step "Phone"
    $devices = @(adb devices | Select-String '\tdevice$')
    if ($NoPhone -or -not $devices) {
        Write-Host "No phone connected over USB (with USB debugging on). Copy the APK to your phone and open it to install,"
        Write-Host "or connect the phone and run this again."
    } elseif (Ask "Install on the connected phone ($((adb shell getprop ro.product.model).Trim()))?") {
        $ErrorActionPreference = 'Continue'
        $out = (adb install -r $apk 2>&1 | ForEach-Object { "$_" }) -join [Environment]::NewLine
        $ErrorActionPreference = 'Stop'
        Write-Host $out
        if ($out -match 'INSTALL_FAILED_UPDATE_INCOMPATIBLE') {
            Write-Host "The phone has a build signed with another key. Uninstall it first (this also deletes its game files and saves), then run this again." -ForegroundColor Yellow
        }
    }
    Write-Host "`nOn the phone, open the app and choose your .iso: it copies the game files in (about 4 GB). Play with a controller." -ForegroundColor Green
} catch {
    Write-Host "`n$_" -ForegroundColor Red
    Write-Host "Fix that, then run Build-APK.cmd again: finished steps are skipped. The log is in setup-log.txt."
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
