# Sets up this PC as the self-hosted runner .github/workflows/release.yml builds on.
#   tools/setup_runner.ps1 -Token <registration token> [-Dir D:\actions-runner-bf2mc] [-GameDir <folder with default.xex>]
# The token is on GitHub: the repository's Settings > Actions > Runners > New self-hosted runner (it lasts an hour).
# Downloads GitHub's runner, checks its SHA-256 against the one published with the release, registers it with the
# label bf2mc, and gives it BF2_GAME_DIR. Then start it with run.cmd in that folder while you want releases built.
param(
    [Parameter(Mandatory)][string]$Token,
    [string]$Repo = 'https://github.com/lscardinali/BF2-ModernCombat-Android',
    [string]$Dir = 'D:\actions-runner-bf2mc',
    [string]$GameDir = ''
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $GameDir) { $GameDir = "$Root\game" }
if (-not (Test-Path "$GameDir\default.xex")) { throw "no default.xex in ${GameDir}: run tools/recompile.ps1 first, or pass -GameDir" }

New-Item -ItemType Directory -Force $Dir | Out-Null
if (-not (Test-Path "$Dir\config.cmd")) {
    $release = Invoke-RestMethod https://api.github.com/repos/actions/runner/releases/latest
    $asset = $release.assets | Where-Object { $_.name -like 'actions-runner-win-x64-*.zip' } | Select-Object -First 1
    if ($release.body -notmatch '<!-- BEGIN SHA win-x64 -->([0-9a-f]{64})<!-- END SHA win-x64 -->') {
        throw "no published SHA-256 for $($asset.name)"
    }
    $want = $Matches[1]
    $zip = Join-Path $Dir $asset.name
    Write-Host "downloading $($asset.name) ($($release.tag_name))"
    Invoke-WebRequest $asset.browser_download_url -OutFile $zip
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $want) { Remove-Item $zip; throw "SHA-256 mismatch: $got, published $want" }
    Expand-Archive $zip -DestinationPath $Dir -Force
    Remove-Item $zip
}

# what the runner's jobs see: the game files, and the JDK Gradle needs
Set-Content "$Dir\.env" "BF2_GAME_DIR=$GameDir`nJAVA_HOME=C:\Program Files\Android\Android Studio\jbr" -Encoding ascii
Push-Location $Dir
try {
    & .\config.cmd --unattended --url $Repo --token $Token --name "$env:COMPUTERNAME-bf2mc" --labels bf2mc --replace
    if ($LASTEXITCODE) { throw "runner registration failed" }
} finally { Pop-Location }
Write-Host "registered. Start it with: $Dir\run.cmd"
