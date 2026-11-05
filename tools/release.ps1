# Builds the signed release APK on this PC, tags the commit, pushes the tag and attaches the APK to a DRAFT GitHub
# release, which only people with write access to the repository see. The APK contains code recompiled from the game:
# don't publish the draft (NOTICE.md).
#   tools/release.ps1 -Version 1.0.0
# Needs: the game recompiled (tools/recompile.ps1), signing/keystore.properties (README, Releases), a clean main
# branch pushed to origin, and either the GitHub CLI signed in (gh auth login) or $env:GITHUB_TOKEN set to a token
# that may write the repository's contents.
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$Repo = 'lscardinali/BF2-ModernCombat-Android'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"
Set-Location $Root
$Tag = "v$Version"

# the release is built from exactly what is on GitHub
if (git status --porcelain --untracked-files=no -- . ':!android/.idea') { throw "commit or stash your changes first" }
git fetch -q origin
if ((git rev-parse HEAD) -ne (git rev-parse origin/main)) { throw "HEAD is not origin/main: push or pull first" }
if (git tag --list $Tag) { throw "$Tag exists already" }
if (-not (Test-Path signing\keystore.properties)) { throw "no signing/keystore.properties (README, Releases)" }
$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh -and -not $env:GITHUB_TOKEN) { throw "install the GitHub CLI and run gh auth login, or set GITHUB_TOKEN" }

# 1.2.3 -> 10203: a later version always has a larger code, as Android needs to install it over the last
$parts = $Version.Split('.') | ForEach-Object { [int]$_ }
$env:VERSION_CODE = [string]($parts[0] * 10000 + $parts[1] * 100 + $parts[2])
$env:VERSION_NAME = $Version
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr' }
Push-Location android
try {
    $ErrorActionPreference = 'Continue'  # Gradle writes progress to stderr
    .\gradlew.bat assembleRelease --console=plain
    if ($LASTEXITCODE) { throw "gradle failed" }
} finally { Pop-Location; $ErrorActionPreference = 'Stop' }
$Apk = "$Root\out\BF2-ModernCombat-Android-$Version.apk"
Copy-Item android\app\build\outputs\apk\release\app-release.apk $Apk -Force
$buildTools = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\build-tools" | Sort-Object Name | Select-Object -Last 1
& "$($buildTools.FullName)\apksigner.bat" verify $Apk
if ($LASTEXITCODE) { throw "the APK is not signed" }

git tag -a $Tag -m "BF2-ModernCombat-Android $Version"
git push -q origin $Tag
$notes = "Built from $(git rev-parse --short HEAD).`n`nPrivate build: it contains code recompiled from the game. Don't publish this release or share the APK (NOTICE.md)."
if ($gh) {
    gh release create $Tag $Apk --repo $Repo --draft --title $Version --notes $notes
    if ($LASTEXITCODE) { throw "gh release create failed" }
} else {
    $headers = @{ Authorization = "Bearer $env:GITHUB_TOKEN"; Accept = 'application/vnd.github+json' }
    $body = @{ tag_name = $Tag; name = $Version; body = $notes; draft = $true } | ConvertTo-Json
    $release = Invoke-RestMethod -Method Post -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers -Body $body
    $upload = $release.upload_url -replace '\{.*\}', "?name=$(Split-Path -Leaf $Apk)"
    Invoke-RestMethod -Method Post -Uri $upload -Headers $headers -ContentType 'application/vnd.android.package-archive' -InFile $Apk | Out-Null
}
Write-Host "draft release $Tag made with $(Split-Path -Leaf $Apk): https://github.com/$Repo/releases"
